#!/usr/bin/env python3
"""What an Asgard checkout needs before it can install and run apps, and how to get it.

`make android-provision` is the full recipe. This module names each of its steps so that the library and
`asgard setup` can say what is missing in plain words and run just the missing steps, instead of failing at
the first one with a command line to type. An installed tree (Scripts/Setup/install-asgard.py) is a complete
copy with no sources to build from, so for it a missing step means "install again", never a command here.

  asgard setup                # run the missing steps, each one resumes where an interrupted download stopped
  asgard setup --check        # list what is missing; exit status 1 when anything is
  asgard setup --check --json # the same for the library window
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
MARKER = ".asgard-install"
# The host tools the steps run (Makefile's ASGARD_TOOLS), found by Scripts/Setup/find-host-tools.sh wherever they are.
HOST_TOOLS = ("python3", "java", "javac", "keytool", "clang", "ld.lld", "debugfs", "readelf", "curl")


class Step:
    """One part of the setup: `outputs` are paths below the root that exist when it is done; `commands` make them."""

    def __init__(self, key, title, why, outputs, commands):
        self.key, self.title, self.why, self.outputs, self.commands = key, title, why, tuple(outputs), tuple(commands)

    def command_line(self):
        return " && ".join(" ".join(["python3" if part == sys.executable else part for part in command])
                           for command in self.commands)


PY = sys.executable
# In the order of the Makefile's android-provision.
STEPS = (
    Step("runtime", "Android runtime", "the Android C library (Bionic) every app runs on",
         ["Tools/android-runtime/rootfs/bifrost-runtime.json"],
         [[PY, "Tools/Android/fetch-android-runtime.py"]]),
    Step("tools", "APK tools", "aapt2 and apksigner, which read and check an app's package before it is installed",
         ["Tools/android-tools/aapt2", "Tools/android-tools/apksigner.jar"],
         [[PY, "Tools/Android/fetch-aapt2.py"], [PY, "Tools/Android/fetch-android-package-tools.py"]]),
    Step("adapters", "Runtime adapters", "the small native pieces that connect an app to the emulator",
         ["build/android-adapters/adapters.json"],
         [[PY, "Tools/Android/build-android-adapters.py"]]),
    Step("launcher", "App launcher", "the Java program that starts an app's Activity",
         ["build/android-managed-launcher/classes.dex"],
         [[PY, "Scripts/Android/android_managed.py", "build"]]),
    Step("framework", "Android framework", "the Android 15 framework and runtime (ART) apps run on",
         ["build/android-framework/rootfs/bifrost-framework.json"],
         [[PY, "Tools/Android/android_framework.py"]]),
    Step("art", "Android runtime (ART) image", "the compiled Java runtime image",
         ["build/android-art/rootfs/bifrost-art.json"],
         [[PY, "Tools/Android/android_art.py"]]),
    Step("microg", "Google services stand-in (microG)", "the sign-in and Play services stand-in apps use",
         [], [[PY, "Tools/Android/android_microg.py"]]),
)


def installed(root=ROOT):
    return (Path(root) / MARKER).is_file()


def microg_files(root):
    """[(path, pinned size)] of the microG release APKs, from the tool that downloads them."""
    sys.path.insert(0, str(Path(root) / "Tools/Android"))
    try:
        import android_microg
        return [(Path(root) / "build/microg" / name, size) for _, name, size, _, _ in android_microg.PACKAGES.values()]
    except (ImportError, OSError):
        return []
    finally:
        sys.path.pop(0)


def done(step, root):
    root = Path(root)
    if step.key == "microg":
        files = microg_files(root)
        return bool(files) and all(path.is_file() and path.stat().st_size == size for path, size in files)
    return all((root / output).exists() for output in step.outputs)


def missing(root=ROOT):
    """The steps whose results are not there yet, in the order to run them."""
    return [step for step in STEPS if not done(step, root)]


def can_run(root=ROOT):
    """Whether this tree has the sources the steps build from: a checkout does, an installed copy does not."""
    return (Path(root) / "Makefile").is_file() and not installed(root)


def status(root=ROOT):
    todo = missing(root)
    return {"missing": [{"id": step.key, "title": step.title, "why": step.why, "command": step.command_line()}
                        for step in todo],
            "can_set_up": bool(todo) and can_run(root), "installed": installed(root)}


def environment(root):
    """The steps' environment: find-host-tools.sh links tools it found off PATH into build/host-bin, as the Makefile does."""
    env = dict(os.environ)
    env["PATH"] = env.get("PATH", "") + os.pathsep + str(Path(root) / "build/host-bin")
    return env


def run_setup(root=ROOT, out=print, runner=subprocess.run):
    """Run the missing steps in order and stop at the first that fails. Returns a process exit status."""
    root = Path(root)
    todo = missing(root)
    if not todo:
        out("Asgard is set up.")
        return 0
    if not can_run(root):
        out("This copy of Asgard is incomplete (" + ", ".join(step.title for step in todo) + " missing) and has no "
            "sources to build them from. Install it again from the source checkout: make install-asgard")
        return 1
    env = environment(root)
    tools = runner(["bash", str(root / "Scripts/Setup/find-host-tools.sh"), *HOST_TOOLS], cwd=root, env=env)
    if tools.returncode:
        out("Some tools the setup needs are missing (listed above). Install them with your distribution's package "
            "manager, then run the setup again.")
        return 1
    for step in todo:
        out(f"== {step.title}: {step.why}")
        for command in step.commands:
            result = runner(command, cwd=root, env=env)
            if result.returncode:
                out(f"Couldn't set up {step.title}: {Path(command[1] if len(command) > 1 else command[0]).name} "
                    f"stopped with status {result.returncode} (the reason is above). Run the setup again once that is "
                    "fixed; steps already done are kept and an interrupted download resumes.")
                return 1
        if not done(step, root):
            out(f"{step.title} did not appear after its setup finished; the output above shows why.")
            return 1
    out("Asgard is set up.")
    return 0


def main(argv=None, root=ROOT):
    parser = argparse.ArgumentParser(prog="asgard setup", description=__doc__.split("\n\n")[0])
    parser.add_argument("--check", action="store_true", help="only list what is missing")
    parser.add_argument("--json", action="store_true", help="with --check: machine-readable, exit status 0")
    args = parser.parse_args(argv)
    if args.json and not args.check:
        parser.error("--json goes with --check")
    if args.check:
        report = status(root)
        if args.json:
            print(json.dumps(report))
            return 0
        if not report["missing"]:
            print("Asgard is set up.")
            return 0
        print("Not set up yet:")
        for item in report["missing"]:
            print(f"  {item['title']}: {item['why']}")
        print("Run: python3 Scripts/Android/asgard.py setup" if report["can_set_up"]
              else "Install Asgard again from the source checkout: make install-asgard")
        return 1
    return run_setup(root)


if __name__ == "__main__":
    raise SystemExit(main())
