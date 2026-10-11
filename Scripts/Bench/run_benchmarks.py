#!/usr/bin/env python3
"""Run repeatable guest benchmarks and save machine-readable results."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shlex
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_BENCHES = ["mips", "memcpy", "sort", "matrix", "fib"]
BENCHES = {
    "mips": ("Tests/Apps/bench_mips.elf", r"done: acc=(0x[0-9a-fA-F]+)", "Tests/Apps/bench_mips.c"),
    "memcpy": ("Tests/Apps/bench_memcpy.elf", r"(VALID: memcpy contents)", "Tests/Apps/bench_memcpy.c"),
    "sort": ("Tests/Apps/bench_sort.elf", r"(VALID: sorted output)", "Tests/Apps/bench_sort.c"),
    "matrix": ("Tests/Apps/bench_matrix.elf", r"(VALID: matrix result)", "Tests/Apps/bench_matrix.c"),
    "fib": ("Tests/Apps/bench_fib.elf", r"fib\(35\) = (9227465)\b", "Tests/Apps/bench_fib.c"),
    # CoreMark is an optional, ignored guest fixture because its source is
    # not vendored. Select it explicitly after provisioning Tests/Unit/coremark.elf.
    "coremark": ("Tests/Unit/coremark.elf", None, None),
}
# Fixed iteration count: must stay above CoreMark's 10-second minimum.
# crcfinal depends on it (0x33ff at 110000, checked against QEMU).
COREMARK_ITERATIONS = "110000"
COREMARK_CRC = ("0xe714", "0x1fd7", "0x8e3a", "0x33ff")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_text(command: list[str]) -> str:
    try:
        result = subprocess.run(command, check=False, capture_output=True, text=True, timeout=5)
        return (result.stdout or result.stderr).splitlines()[0]
    except (OSError, subprocess.TimeoutExpired, IndexError):
        return "unavailable"


def cpu_model() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text(errors="replace").splitlines():
            if line.lower().startswith("model name") or line.lower().startswith("hardware"):
                return line.split(":", 1)[-1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def git_metadata() -> dict[str, object]:
    def git(*args: str) -> str:
        try:
            return subprocess.run(
                ["git", *args], cwd=ROOT, check=True, capture_output=True, text=True, timeout=5
            ).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            return "unknown"

    status = git("status", "--porcelain")
    return {
        "revision": git("rev-parse", "HEAD"),
        "working_tree_dirty": status not in ("", "unknown"),
        "working_tree_status": status.splitlines() if status not in ("", "unknown") else [],
    }


def validate_output(name: str, output: str) -> str:
    if re.search(r"(?:^|\b)(?:ERROR|FAIL):", output, re.IGNORECASE | re.MULTILINE):
        raise RuntimeError("guest reported ERROR/FAIL")
    if name == "coremark":
        expected = {"seedcrc": "0xe9f5", "[0]crclist": "0xe714",
                    "[0]crcmatrix": "0x1fd7", "[0]crcstate": "0x8e3a",
                    "[0]crcfinal": COREMARK_CRC[3]}
        for label, value in expected.items():
            if not re.search(r"^\s*" + re.escape(label) + r"\s*:\s*" + value + r"\s*$", output, re.M | re.I):
                raise RuntimeError(f"CoreMark output has an incorrect or missing {label}")
        if (not re.search(r"^Iterations\s*:\s*" + COREMARK_ITERATIONS + r"\s*$", output, re.M)
                or "Correct operation validated." not in output
                or "CoreMark 1.0" not in output
                or "ERROR!" in output or "Errors detected" in output):
            raise RuntimeError(f"CoreMark must validate exactly {COREMARK_ITERATIONS} iterations")
        return ",".join(COREMARK_CRC)
    elf, expected, _source = BENCHES[name]
    del elf
    if expected is None:
        raise RuntimeError(f"no output validator is configured for {name}")
    match = re.search(expected, output)
    if not match:
        raise RuntimeError(f"expected result marker not found: {expected}")
    return match.group(1)


def run_sample(emulator: Path, guest: Path, mode: str, timeout: int) -> tuple[float, str, str]:
    command = [str(emulator)]
    if mode == "interp":
        command.append("--no-jit")
    command.append(str(guest))
    if guest.name == "coremark.elf":
        command.extend(["0", "0", "0", COREMARK_ITERATIONS])
    started = time.perf_counter()
    try:
        result = subprocess.run(
            command,
            cwd=ROOT,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        partial = exc.stdout or ""
        raise RuntimeError(f"timed out after {timeout}s\n{partial}") from exc
    elapsed = time.perf_counter() - started
    if result.returncode != 0:
        raise RuntimeError(f"emulator exited with {result.returncode}\n{result.stdout}")
    bench_name = guest.stem
    if guest.parent.name == "Apps" and bench_name.startswith("bench_"):
        bench_name = bench_name[len("bench_") :]
    stable = validate_output(bench_name, result.stdout)
    return elapsed, stable, result.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", default="./bifrost-emu", help="emulator executable (default: ./bifrost-emu)")
    parser.add_argument("--mode", choices=("jit", "interp", "both"), default="jit")
    parser.add_argument("--bench", action="append", choices=tuple(BENCHES), help="benchmark to run; repeat to select several")
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--timeout", type=int, default=300, help="per-run timeout in seconds")
    parser.add_argument("--output", help="JSON result path (default: bench-results/<UTC timestamp>.json)")
    args = parser.parse_args()

    if args.warmups < 0 or args.runs < 1 or args.timeout < 1:
        parser.error("--warmups must be >= 0; --runs and --timeout must be >= 1")

    emulator = Path(args.emulator)
    if not emulator.is_absolute():
        emulator = ROOT / emulator
    emulator = emulator.resolve()
    if not emulator.is_file() or not os.access(emulator, os.X_OK):
        print(f"error: emulator is not executable: {emulator}", file=sys.stderr)
        return 2

    selected = args.bench or list(DEFAULT_BENCHES)
    if not args.bench and (ROOT / BENCHES["coremark"][0]).is_file():
        selected.append("coremark")
    modes = ("jit", "interp") if args.mode == "both" else (args.mode,)
    for name in selected:
        guest = ROOT / BENCHES[name][0]
        if not guest.is_file():
            print(f"error: missing {guest.relative_to(ROOT)}; run make setup-tests first", file=sys.stderr)
            return 2

    try:
        version = command_text([str(emulator), "--version"])
        cxx = shlex.split(os.environ.get("CXX", "c++"))
        metadata = {
            "schema_version": 1,
            "created_utc": datetime.now(timezone.utc).isoformat(),
            "repository": git_metadata(),
            "emulator": {
                "path": str(emulator),
                "sha256": sha256_file(emulator),
                "version": version,
                "build_config": (emulator.parent / ".build-config").read_text().splitlines()
                if (emulator.parent / ".build-config").is_file()
                else None,
            },
            "host": {
                "system": platform.system(),
                "release": platform.release(),
                "machine": platform.machine(),
                "cpu_model": cpu_model(),
                "logical_cpus": os.cpu_count(),
                "compiler": command_text(cxx + ["--version"]),
                "python": platform.python_version(),
            },
            "execution": {
                "mode": args.mode,
                "warmups": args.warmups,
                "runs": args.runs,
                "timeout_seconds": args.timeout,
                "bifrost_environment": {
                    key: value for key, value in sorted(os.environ.items()) if key.startswith("BIFROST_")
                },
            },
            "benchmarks": [],
        }

        for name in selected:
            binary_name, _expected, source_name = BENCHES[name]
            guest = ROOT / binary_name
            entry: dict[str, object] = {
                "name": name,
                "guest_binary": binary_name,
                "guest_arguments": ["0", "0", "0", COREMARK_ITERATIONS] if name == "coremark" else [],
                "guest_sha256": sha256_file(guest),
                "source": source_name,
                "source_sha256": sha256_file(ROOT / source_name) if source_name else None,
                "modes": [],
            }
            print(f"[{name}]", flush=True)
            for mode in modes:
                for index in range(args.warmups):
                    _elapsed, _stable, _output = run_sample(emulator, guest, mode, args.timeout)
                    print(f"  {mode} warmup {index + 1}/{args.warmups}", flush=True)
                elapsed_samples: list[float] = []
                stable_results: list[str] = []
                output = ""
                for index in range(args.runs):
                    elapsed, stable, output = run_sample(emulator, guest, mode, args.timeout)
                    elapsed_samples.append(elapsed)
                    stable_results.append(stable)
                    print(f"  {mode} run {index + 1}/{args.runs}: {elapsed:.4f}s", flush=True)
                if len(set(stable_results)) != 1:
                    raise RuntimeError(f"{name}/{mode} produced inconsistent results: {stable_results}")
                mode_result = {
                    "mode": mode,
                    "elapsed_seconds": elapsed_samples,
                    "median_seconds": statistics.median(elapsed_samples),
                    "min_seconds": min(elapsed_samples),
                    "max_seconds": max(elapsed_samples),
                    "stable_result": stable_results[0],
                    "last_stdout": output,
                }
                entry["modes"].append(mode_result)
            metadata["benchmarks"].append(entry)

        timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        output_path = Path(args.output) if args.output else Path("bench-results") / f"bench-{timestamp}.json"
        if not output_path.is_absolute():
            output_path = ROOT / output_path
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(json.dumps(metadata, indent=2) + "\n")
        print(f"Results written to {output_path}")
        return 0
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"benchmark failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
