#!/usr/bin/env python3
"""The managed adapters export what apps import.

Bionic binds an app library's imports at dlopen, so one missing export fails the load of the whole
library even if the function is never called (YouTube's libgoogle3.so needed glGetUniformBlockIndex and
ASurfaceControl_createFromWindow). Checks the built adapters (`make android-runtime-adapters`):
  * libGLESv2.so exports every command of the OpenGL ES 2.0 to 3.2 core features in gl.xml;
  * the managed libandroid.so exports ASurfaceControl_createFromWindow.
Pass --adapters DIR for a build somewhere other than build/android-adapters.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools/CodeGen"))
import glxmlcheck


def exports(library):
    """The defined dynamic symbols of an ELF library."""
    tool = shutil.which("llvm-nm") or shutil.which("nm")
    if not tool:
        sys.exit("SKIP: no llvm-nm or nm")
    out = subprocess.run([tool, "-D", "--defined-only", str(library)], capture_output=True, text=True, check=True).stdout
    return {line.split()[-1] for line in out.splitlines() if line.split()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adapters", type=Path, default=ROOT / "build/android-adapters")
    args = parser.parse_args()
    failures = []
    for variant in ("", "managed"):
        library = args.adapters / variant / "libGLESv2.so"
        if not library.is_file():
            sys.exit(f"SKIP: {library} is not built (make android-runtime-adapters)")
        present = exports(library)
        core = glxmlcheck.core_es_commands(ROOT / "ThirdParty/GLRegistry/gl.xml")
        missing = sorted(name for name in core if name not in present)
        if missing:
            failures.append(f"{library.relative_to(args.adapters)} lacks {len(missing)} core ES commands: {', '.join(missing[:8])}")
    managed = args.adapters / "managed/libandroid.so"
    if "ASurfaceControl_createFromWindow" not in exports(managed):
        failures.append("managed/libandroid.so does not export ASurfaceControl_createFromWindow")
    if failures:
        print("\n".join("FAIL " + failure for failure in failures))
        return 1
    print(f"android_adapter_exports: ALL PASS ({len(core)} core ES commands in each libGLESv2.so, ASurfaceControl_createFromWindow)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
