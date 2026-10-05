#!/usr/bin/env python3
"""Build ARM64 guest adapter DSOs and a Bionic NativeActivity launcher."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def shim(library, names):
    lines = [".text"]
    for name in sorted(set(names)):
        # Lazy atomic cache per export. Preserve every AAPCS64 argument, including
        # SIMD and variadic arguments, before resolving the existing shared thunk.
        lines += [
            f".global {name}",
            f".type {name},%function",
            f"{name}:",
            f"adrp x16,.Lptr_{name}",
            f"add x16,x16,:lo12:.Lptr_{name}",
            "ldar x17,[x16]",
            f"cbnz x17,.Lcall_{name}",
            "sub sp,sp,#208",
            "stp x0,x1,[sp,#0]",
            "stp x2,x3,[sp,#16]",
            "stp x4,x5,[sp,#32]",
            "stp x6,x7,[sp,#48]",
            "stp x8,x30,[sp,#64]",
            "stp q0,q1,[sp,#80]",
            "stp q2,q3,[sp,#112]",
            "stp q4,q5,[sp,#144]",
            "stp q6,q7,[sp,#176]",
            "adrp x0,.Llibrary",
            "add x0,x0,:lo12:.Llibrary",
            f"adrp x1,.Lname_{name}",
            f"add x1,x1,:lo12:.Lname_{name}",
            "mov x8,#0x100d",
            "svc #0",
            "cmp x0,#0",
            f"b.le .Lmissing_{name}",
            "mov x17,x0",
            f"adrp x16,.Lptr_{name}",
            f"add x16,x16,:lo12:.Lptr_{name}",
            "stlr x17,[x16]",
            "ldp x0,x1,[sp,#0]",
            "ldp x2,x3,[sp,#16]",
            "ldp x4,x5,[sp,#32]",
            "ldp x6,x7,[sp,#48]",
            "ldp x8,x30,[sp,#64]",
            "ldp q0,q1,[sp,#80]",
            "ldp q2,q3,[sp,#112]",
            "ldp q4,q5,[sp,#144]",
            "ldp q6,q7,[sp,#176]",
            "add sp,sp,#208",
            f".Lcall_{name}:",
            "br x17",
            f".Lmissing_{name}:",
            "mov x0,#2",
            f"adrp x1,.Lerror_{name}",
            f"add x1,x1,:lo12:.Lerror_{name}",
            f"mov x2,#.Lerror_end_{name}-.Lerror_{name}",
            "mov x8,#64",
            "svc #0",
            "mov x0,#126",
            "mov x8,#94",
            "svc #0",
            "brk #1",
            f".size {name},.-{name}",
        ]
    lines += [".section .rodata", f'.Llibrary: .asciz "{library}"']
    for name in sorted(set(names)):
        lines += [
            f'.Lname_{name}: .asciz "{name}"',
            f'.Lerror_{name}: .ascii "Android adapter: missing {library}:{name}\\n"',
            f".Lerror_end_{name}:",
        ]
    lines += [".bss", ".balign 8"]
    for name in sorted(set(names)):
        lines += [f".Lptr_{name}: .quad 0"]
    lines += ['.section .note.GNU-stack,"",%progbits']
    return "\n".join(lines) + "\n"


def build(runtime, output):
    spec = importlib.util.spec_from_file_location(
        "runtime", ROOT / "tools/fetch-android-runtime.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.install(runtime)
    bionic = runtime / "apex/com.android.runtime/lib64/bionic"
    common = [
        "clang",
        "--target=aarch64-linux-android35",
        "-fuse-ld=lld",
        "-nostdlib",
        "-ffreestanding",
        "-O2",
        "-Wall",
        "-Wextra",
        "-I" + str(ROOT / "guest/android"),
        "-Wl,-z,max-page-size=16384",
        "-Wl,-z,defs",
    ]
    groups = {
        name: [] for name in ["libandroid.so", "liblog.so", "libEGL.so", "libGLESv2.so"]
    }
    for line in (ROOT / "tools/opgen/thunk_dp.txt").read_text().splitlines():
        fields = line.split()
        if len(fields) < 6 or fields[0].startswith("#"):
            continue
        name, group = fields[:2]
        library = {
            "ANDROID": (
                "liblog.so" if name.startswith("__android_log_") else "libandroid.so"
            ),
            "EGL": "libEGL.so",
            "GLES": "libGLESv2.so",
        }.get(group)
        if library:
            groups[library].append(name)
    # Core GL readback uses the same ABI in GLES. The Android shim resolver
    # routes it to the existing GL readback/marshalling implementation.
    groups["libGLESv2.so"].append("glReadPixels")
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=".android-adapters-", dir=output.parent
    ) as work:
        staging = Path(work)
        for library, names in groups.items():
            assembly = staging / (library + ".S")
            assembly.write_text(shim(library, names))
            inputs = [str(assembly)]
            if library == "libandroid.so":
                inputs += [
                    str(ROOT / "guest/android/assets.c"),
                    str(bionic / "libc.so"),
                ]
            subprocess.run(
                common
                + ["-fPIC", "-shared", "-Wl,-soname," + library]
                + inputs
                + ["-o", str(staging / library)],
                check=True,
            )
        subprocess.run(
            common
            + [
                "-fPIE",
                "-pie",
                "-Wl,--dynamic-linker=/system/bin/linker64",
                "-Wl,-rpath,/system/lib64:/apex/com.android.runtime/lib64/bionic",
                str(ROOT / "ctest/android_bionic_start.S"),
                str(ROOT / "guest/android/native_launcher.c"),
                str(staging / "libandroid.so"),
                str(bionic / "libc.so"),
                str(bionic / "libdl.so"),
                "-Wl,--allow-shlib-undefined",
                "-o",
                str(staging / "bifrost-native"),
            ],
            check=True,
        )
        names = list(groups) + ["bifrost-native"]
        metadata = {
            "schema_version": 1,
            "runtime_archive_sha256": module.SHA256,
            "files": {
                name: hashlib.sha256((staging / name).read_bytes()).hexdigest()
                for name in names
            },
        }
        for name in names:
            (staging / name).replace(output / name)
        manifest = staging / "adapters.json"
        manifest.write_text(json.dumps(metadata, indent=2) + "\n")
        manifest.replace(output / "adapters.json")
    print(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--runtime-root", type=Path, default=ROOT / "tools/android-runtime/rootfs"
    )
    parser.add_argument("--output", type=Path, default=ROOT / "build/android-adapters")
    args = parser.parse_args()
    build(args.runtime_root, args.output)
