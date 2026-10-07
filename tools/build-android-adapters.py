#!/usr/bin/env python3
"""Build ARM64 guest adapter DSOs, a Bionic NativeActivity launcher, and the
managed-app adapters plus ActivityProbe JNI bridge."""

import argparse
import hashlib
import importlib.util
import json
import re
from pathlib import Path
import subprocess
import tempfile
from android_guest_dependencies import build_zlib, prepare_headers

ROOT = Path(__file__).resolve().parents[1]


def shim(library, names, hidden=()):
    """Assembly for lazy thunk exports; `library` names the thunk table.
    Names in `hidden` get the local-only label thunk_<name>, for C wrappers."""
    lines = [".text"]
    for name in sorted(set(names)):
        label = "thunk_" + name if name in hidden else name
        # Lazy atomic cache per export. Preserve every AAPCS64 argument, including
        # SIMD and variadic arguments, before resolving the existing shared thunk.
        lines += [
            f".global {label}",
            *([f".hidden {label}"] if name in hidden else []),
            f".type {label},%function",
            f"{label}:",
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
            f".size {label},.-{label}",
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
        common += prepare_headers(staging / "headers")
        dependencies = build_zlib(staging, bionic, common)
        for library, names in groups.items():
            if library == "liblog.so":
                names = ["__android_log_write"]
            assembly = staging / (library + ".S")
            assembly.write_text(shim(library, names))
            inputs = [str(assembly)]
            if library == "libandroid.so":
                inputs += [
                    str(ROOT / "guest/android/assets.c"),
                    str(bionic / "libc.so"),
                ]
            if library == "liblog.so":
                inputs += [str(ROOT / "guest/android/log.c"), str(bionic / "libc.so")]
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
                str(bionic / "libdl_android.so"),
                "-Wl,--allow-shlib-undefined",
                "-o",
                str(staging / "bifrost-native"),
            ],
            check=True,
        )
        # ActivityProbe's play-mode JNI bridge (scripts/test_android_framework.py --play).
        subprocess.run(common + ["-fPIC", "-shared", "-Wl,-soname,libbifrost-activity.so",
                                 str(ROOT / "guest/android/activity_window.c"), str(bionic / "libc.so"),
                                 "-o", str(staging / "libbifrost-activity.so")], check=True)
        names = list(groups) + ["bifrost-native", "libbifrost-activity.so", "libz.so",
                                "libz.so.LICENSE"]
        # Managed apps run inside the image's real framework, whose libraries
        # import Android-private EGL/GLES/libandroid symbols. Each managed
        # adapter keeps its thunked exports first in Bionic's breadth-first
        # lookup and depends on the real library, which the runner installs
        # under the same-length soname re_<name> (lib -> re_).
        managed = staging / "managed"
        managed.mkdir()
        thunked_android = [n for n in groups["libandroid.so"]
                           if not n.startswith("AConfiguration_")]  # real AAssetManager
        # Share the AudioThunk export registry. Managed AAudio streams must
        # stay on the host backend, rather than fall back to Android AudioTrack.
        aa_exports = re.findall(r"REG\(aa_libs,\s*(\w+)\)",
                               (ROOT / "src/frost_graphics/audio_thunk.cpp").read_text())
        # aaudio.c runs data callbacks on its own Bionic thread, like libaaudio;
        # it wraps open/close and drives the private bifrost_aaudio_* calls.
        aa_hidden = {"AAudioStreamBuilder_openStream", "AAudioStream_close"}
        aa_hidden.update(n for n in aa_exports if n.startswith("bifrost_"))
        assembly = staging / "managed-libaaudio.so.S"
        assembly.write_text(shim("libaaudio.so", aa_exports, aa_hidden))
        subprocess.run(common + ["-fPIC", "-shared", "-Wl,-soname,libaaudio.so",
                                 str(assembly), str(ROOT / "guest/android/aaudio.c"),
                                 str(bionic / "libc.so"), "-o", str(managed / "libaaudio.so")],
                       check=True)
        names.append("managed/libaaudio.so")
        for soname, table, exports in (
            ("libandroid.so", "libandroid.so", thunked_android),
            ("libEGL.so", "libEGL.so", groups["libEGL.so"]),
            ("libGLESv2.so", "libGLESv2.so", groups["libGLESv2.so"]),
            ("libGLESv3.so", "libGLESv2.so", groups["libGLESv2.so"]),
        ):
            real = "re_" + soname[3:]
            stub = staging / real
            subprocess.run(common + ["-fPIC", "-shared", "-Wl,-soname," + real,
                                     "-Wno-unused-command-line-argument",
                                     "-x", "assembler", "/dev/null", "-o", str(stub)],
                           check=True)
            assembly = staging / ("managed-" + soname + ".S")
            assembly.write_text(shim(table, exports))
            subprocess.run(common + ["-fPIC", "-shared", "-Wl,-soname," + soname,
                                     "-Wl,--no-as-needed", str(assembly), str(stub),
                                     "-o", str(managed / soname)], check=True)
            names.append("managed/" + soname)
        metadata = {
            "schema_version": 1,
            "runtime_archive_sha256": module.SHA256,
            "dependencies": dependencies,
            "files": {
                name: hashlib.sha256((staging / name).read_bytes()).hexdigest()
                for name in names
            },
        }
        (output / "managed").mkdir(exist_ok=True)
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
