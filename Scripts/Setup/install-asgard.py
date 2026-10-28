#!/usr/bin/env python3
"""Install Asgard (the Android layer) where `bifrost-emu app.apk` finds it from any directory.

Usage:
  ./Scripts/Setup/install-asgard.py                 # for your user: ~/.local
  ./Scripts/Setup/install-asgard.py --prefix DIR    # DIR/share/bifrost/asgard and DIR/bin/asgard
  ./Scripts/Setup/install-asgard.py --system        # /usr/local (needs root)
  ./Scripts/Setup/install-asgard.py --dry-run       # list what would change, write nothing
  ./Scripts/Setup/install-asgard.py --verify        # also compare file contents, not just size and time
  ./Scripts/Setup/install-asgard.py --uninstall     # remove it from the chosen prefix

This checkout must already be provisioned for Android (see Docs/Android/ANDROID_APK.md). The
install is a self-contained copy of what Asgard reads (about 1 GiB: scripts, signing/dex tools,
the framework and ART images, adapters, launcher, microG) under <prefix>/share/bifrost/asgard,
plus <prefix>/bin/asgard. Run it again after updating the checkout: only files that differ
(size, modification time, or link target) are copied, each replaced atomically so a running
session keeps the file it opened, and files that no longer exist in the checkout are removed.
App state is not part of the install: it lives in ~/.local/share/bifrost/android, or in
BIFROST_ANDROID_STATE (see Docs/Android/ANDROID_APK.md).
"""
import argparse
import compileall
import fnmatch
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MARKER = ".asgard-install"
WRAPPER_TAG = "# installed by install-asgard.py"

# Everything Asgard reads under its root at run time, kept at the same relative paths because
# the scripts locate their root as the parent of Scripts/. (path, ignore patterns)
FILES = [
    ("Scripts/Android", ("__pycache__", "*.pyc", "test_*", "run_*.sh")),
    ("Tools/Android", ("__pycache__", "*.pyc", "*.cpp")),
    ("Tools/android-tools", ()),
    ("Tools/android-runtime", ()),
    ("Guest/Android", ()),
    ("Tests/Unit/android_bionic_start.S", ()),
    ("build/android-framework/rootfs", ()),
    ("build/android-art/rootfs", ()),
    ("build/android-adapters", ()),
    ("build/android-managed-launcher", ()),
    ("build/microg", ()),
]
# How to create what is missing from an unprovisioned checkout.
PROVISIONING = {
    "Tools/android-runtime": "python3 Tools/Android/fetch-android-runtime.py",
    "Tools/android-tools": "python3 Tools/Android/fetch-aapt2.py && python3 Tools/Android/fetch-android-package-tools.py",
    "build/android-adapters": "make android-runtime-adapters",
    "build/android-managed-launcher": "make android-managed-launcher",
    "build/android-framework/rootfs": "python3 Tools/Android/android_framework.py",
    "build/android-art/rootfs": "python3 Tools/Android/android_art.py",
    "build/microg": "start any managed app once (microG downloads on first use)",
}
REQUIRED = ("Tools/android-tools/aapt2", "Tools/android-tools/apksigner.jar",
            "build/android-adapters/adapters.json", "build/android-managed-launcher/classes.dex")
# Written by the install itself; never part of the sync or deleted by it.
GENERATED = ("__pycache__",)


def sha256(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def installed_mode(mode):
    """Readable by everyone who runs the install, writable only by its owner: a system install
    copies a user's checkout, where generated files can be private to that user (0600)."""
    mode = stat.S_IMODE(mode) & ~0o022
    return mode | 0o444 | (0o111 if mode & stat.S_IXUSR else 0)


def ignored(name, patterns):
    return any(fnmatch.fnmatch(name, pattern) for pattern in patterns)


def scan(base, patterns):
    """Yield (relative path, lstat result) for a file or tree, skipping ignored names."""
    info = base.lstat()
    yield Path(), info
    if stat.S_ISDIR(info.st_mode):
        for child in sorted(base.iterdir()):
            if not ignored(child.name, patterns):
                for relative, child_info in scan(child, patterns):
                    yield Path(child.name) / relative, child_info


def source_plan():
    """{relative path: lstat} of every file, link and directory that belongs in the install."""
    plan = {}
    for name, patterns in FILES:
        for relative, info in scan(ROOT / name, patterns):
            plan[Path(name) / relative] = info
    return plan


def differs(relative, info, destination, verify):
    """Whether the installed copy of `relative` must be rewritten."""
    target = destination / relative
    try:
        installed = target.lstat()
    except FileNotFoundError:
        return True
    if stat.S_ISDIR(info.st_mode):
        return not stat.S_ISDIR(installed.st_mode)
    if stat.S_ISLNK(info.st_mode):
        return not stat.S_ISLNK(installed.st_mode) or os.readlink(target) != os.readlink(ROOT / relative)
    if not stat.S_ISREG(installed.st_mode) or installed.st_size != info.st_size \
            or stat.S_IMODE(installed.st_mode) != installed_mode(info.st_mode):
        return True
    if installed.st_mtime_ns != info.st_mtime_ns:
        return True
    return verify and sha256(ROOT / relative) != sha256(target)


def writable(path):
    """Let us change a directory the image made read-only; the final modes are restored afterwards."""
    mode = stat.S_IMODE(path.lstat().st_mode)
    if not mode & stat.S_IWUSR:
        path.chmod(mode | stat.S_IWUSR)


def remove(path):
    writable(path.parent)
    if path.is_symlink() or not path.is_dir():
        path.unlink()
        return
    for directory, _, _ in os.walk(path):  # does not follow symlinks
        writable(Path(directory))
    shutil.rmtree(path)


def installed_extras(destination, wanted):
    """Installed paths under the synced trees that the checkout no longer provides."""
    stale = []
    for name, _ in FILES:
        top = destination / name
        if not top.is_dir() or top.is_symlink():
            continue
        for directory, directories, files in os.walk(top):
            directories[:] = [d for d in directories if d not in GENERATED]
            for entry in directories + files:
                relative = (Path(directory) / entry).relative_to(destination)
                if relative not in wanted:
                    stale.append(relative)
    return [path for path in stale if not any(parent in stale for parent in path.parents)]


def replace_file(relative, info, destination):
    source, target = ROOT / relative, destination / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    writable(target.parent)
    temporary = target.with_name(f".{target.name}.new.{os.getpid()}")
    try:
        if stat.S_ISLNK(info.st_mode):
            temporary.symlink_to(os.readlink(source))
        else:
            shutil.copy2(source, temporary)
            temporary.chmod(installed_mode(info.st_mode))
        if target.is_dir() and not target.is_symlink():
            remove(target)
        os.replace(temporary, target)
    finally:
        if temporary.exists() or temporary.is_symlink():
            temporary.unlink()


def bulk_copy(name, patterns, destination):
    """First install of a tree: let cp share blocks where the filesystem can, else copy."""
    source, target = ROOT / name, destination / name
    target.parent.mkdir(parents=True, exist_ok=True)
    if not patterns and source.is_dir():
        fast = subprocess.run(["cp", "-a", "--reflink=auto", "--", str(source), str(target)],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if fast.returncode == 0:
            return
        shutil.rmtree(target, ignore_errors=True)


def sync(destination, plan, verify, dry_run):
    changed = [(relative, info) for relative, info in plan.items()
               if not stat.S_ISDIR(info.st_mode) and differs(relative, info, destination, verify)]
    missing_dirs = [relative for relative, info in plan.items()
                    if stat.S_ISDIR(info.st_mode) and differs(relative, info, destination, verify)]
    stale = installed_extras(destination, set(plan))
    copy_bytes = sum(info.st_size for _, info in changed if stat.S_ISREG(info.st_mode))
    print(f"{len(changed)} to copy ({copy_bytes / 2**20:.1f} MiB), {len(stale)} to remove, "
          f"{len(plan) - len(changed) - len(missing_dirs)} unchanged")
    for relative, _ in changed[:20]:
        print(f"  update {relative}")
    if len(changed) > 20:
        print(f"  ... and {len(changed) - 20} more")
    for relative in stale[:20]:
        print(f"  remove {relative}")
    if dry_run:
        return len(changed), len(stale)
    destination.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(destination).free
    if free < copy_bytes * 1.05 + 2**26:
        sys.exit(f"error: not enough free space ({free / 2**30:.1f} GiB free, "
                 f"{copy_bytes / 2**30:.1f} GiB to copy)")
    for name, patterns in FILES:  # a tree that is not installed yet is cheapest to copy in one go
        if not (destination / name).exists() and (ROOT / name).is_dir():
            bulk_copy(name, patterns, destination)
    for relative, info in changed:
        if differs(relative, info, destination, verify):
            replace_file(relative, info, destination)
    for relative in stale:
        remove(destination / relative)
    # Directories last: restore read-only modes after everything inside is in place.
    for relative, info in sorted(plan.items(), key=lambda item: len(item[0].parts), reverse=True):
        if stat.S_ISDIR(info.st_mode):
            (destination / relative).mkdir(parents=True, exist_ok=True)
            (destination / relative).chmod(installed_mode(info.st_mode))
    return len(changed), len(stale)


def wrapper_text():
    return ("#!/bin/sh\n" + WRAPPER_TAG + "\n"
            'here=$(dirname "$(readlink -f "$0")")\n'
            'exec python3 "$here/../share/bifrost/asgard/Scripts/Android/asgard.py" "$@"\n')


def write_wrapper(prefix):
    bindir = prefix / "bin"
    bindir.mkdir(parents=True, exist_ok=True)
    wrapper = bindir / "asgard"
    if not wrapper.is_file() or wrapper.read_text() != wrapper_text():
        wrapper.write_text(wrapper_text())
    wrapper.chmod(0o755)
    return wrapper


def write_marker(tree, **fields):
    """Replace the marker atomically; the tree may be read-only from a previous install."""
    writable(tree)
    temporary = tree / f".{MARKER}.new"
    temporary.write_text(json.dumps({"format": 1, **fields}) + "\n")
    os.replace(temporary, tree / MARKER)


def install(prefix, verify, dry_run):
    missing = [(name, PROVISIONING.get(name, "")) for name, _ in FILES if not (ROOT / name).exists()]
    if not missing:
        missing = [(name, "re-run the provisioning steps in Docs/Android/ANDROID_APK.md")
                   for name in REQUIRED if not (ROOT / name).exists()]
    if missing:
        print("error: this checkout is not fully provisioned for Android:", file=sys.stderr)
        for name, how in missing:
            print(f"  missing {name}" + (f"  ->  {how}" if how else ""), file=sys.stderr)
        sys.exit(1)
    tree = prefix / "share/bifrost/asgard"
    if tree.is_symlink() or (tree.exists() and not (tree / MARKER).is_file()):
        sys.exit(f"refusing to update {tree}: it is not an install made by this script")
    marker = tree / MARKER
    if marker.is_file() and json.loads(marker.read_text() or "{}").get("complete") is False:
        print("note: the previous run did not finish; this run completes it")
    if not dry_run:
        tree.mkdir(parents=True, exist_ok=True)
        write_marker(tree, complete=False)
    changed, removed = sync(tree, source_plan(), verify, dry_run)
    if dry_run:
        return
    try:  # only a label: absent without git or outside a checkout (an installed package's setup tree)
        revision = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                                  capture_output=True, text=True).stdout.strip()
    except OSError:
        revision = ""
    write_marker(tree, complete=True, source_revision=revision or None, updated_at=int(time.time()))
    if changed or removed:
        compileall.compile_dir(tree / "Scripts/Android", quiet=2)
        compileall.compile_dir(tree / "Tools/Android", quiet=2)
    wrapper = write_wrapper(prefix)
    print(f"Asgard installed in {tree}\nCommand: {wrapper}\n"
          "bifrost-emu app.apk finds it automatically (see Docs/Android/ANDROID_APK.md).")


def uninstall(prefix):
    tree = prefix / "share/bifrost/asgard"
    wrapper = prefix / "bin/asgard"
    if tree.is_symlink() or (tree.exists() and not (tree / MARKER).is_file()):
        sys.exit(f"refusing to remove {tree}: it is not an install made by this script")
    if tree.exists():
        remove(tree)
        print(f"removed {tree}")
    else:
        print(f"nothing to remove at {tree}")
    if wrapper.is_file() and WRAPPER_TAG in wrapper.read_text():
        wrapper.unlink()
        print(f"removed {wrapper}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    where = parser.add_mutually_exclusive_group()
    where.add_argument("--user", action="store_true", help="install under ~/.local (default)")
    where.add_argument("--system", action="store_true", help="install under /usr/local")
    where.add_argument("--prefix", type=Path, help="install under DIR/share/bifrost and DIR/bin")
    parser.add_argument("--dry-run", action="store_true", help="list what would change and write nothing")
    parser.add_argument("--verify", action="store_true", help="compare contents as well as size and time")
    parser.add_argument("--uninstall", action="store_true")
    args = parser.parse_args()
    prefix = Path("/usr/local") if args.system else (args.prefix or Path.home() / ".local").expanduser()
    try:
        if args.uninstall:
            uninstall(prefix)
        else:
            install(prefix, args.verify, args.dry_run)
    except PermissionError as error:
        sys.exit(f"error: {error}\n  Run it as the user (or with sudo) that owns {prefix}.")


if __name__ == "__main__":
    main()
