#!/usr/bin/env python3
"""Assemble the small, freestanding AArch64 programs under test/*.s."""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def remove_asm_comment(line: str) -> str:
    """These fixtures use ';' for comments; keep semicolons inside strings."""
    in_string = False
    escaped = False
    for index, char in enumerate(line):
        if char == '"' and not escaped:
            in_string = not in_string
        elif char == ";" and not in_string:
            return line[:index].rstrip() + "\n"
        if char == "\\" and not escaped:
            escaped = True
        else:
            escaped = False
    return line


def main() -> int:
    if len(sys.argv) != 2 or not sys.argv[1]:
        print(f"usage: {Path(sys.argv[0]).name} AARCH64_CC", file=sys.stderr)
        return 2
    compiler = sys.argv[1]
    fixtures = sorted((ROOT / "test").glob("*.s"))
    if not fixtures:
        print("error: no test/*.s fixtures found", file=sys.stderr)
        return 1

    built = 0
    # os.replace must stay on the destination filesystem. /tmp may be a
    # separate mount, so build beside the fixtures for atomic publication.
    with tempfile.TemporaryDirectory(prefix=".bifrost-asm-tests-", dir=ROOT / "test") as temp_name:
        temp_dir = Path(temp_name)
        for source in fixtures:
            output = source.with_suffix(".elf")
            if output.is_file() and output.stat().st_mtime_ns > source.stat().st_mtime_ns:
                continue

            lines = source.read_text(encoding="utf-8").splitlines(keepends=True)
            if not any(re.match(r"\s*\.global\s+_start\b", line) for line in lines):
                lines.insert(0, ".global _start\n")
            assembly = temp_dir / f"{source.stem}.S"
            assembly.write_text("".join(remove_asm_comment(line) for line in lines), encoding="utf-8")
            temp_output = temp_dir / f"{source.stem}.elf"
            command = [
                compiler,
                "-nostdlib",
                "-static",
                "-no-pie",
                "-Wl,-e,_start",
                "-o",
                str(temp_output),
                str(assembly),
            ]
            try:
                subprocess.run(command, cwd=ROOT, check=True)
            except (OSError, subprocess.CalledProcessError) as exc:
                print(f"error: failed to build {source.relative_to(ROOT)}: {exc}", file=sys.stderr)
                return 1
            os.replace(temp_output, output)
            built += 1

    print(f"Built {built} freestanding AArch64 assembly test binaries.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
