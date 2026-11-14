#!/usr/bin/env python3
"""Install Asgard (the Android layer) where `bifrost-emu app.apk` finds it from any directory.

Usage:
  ./Scripts/Setup/install-asgard.py                 # for your user: ~/.local
  ./Scripts/Setup/install-asgard.py --prefix DIR    # DIR/share/bifrost/asgard and DIR/bin/asgard
  ./Scripts/Setup/install-asgard.py --system        # /usr/local (needs root)
  ./Scripts/Setup/install-asgard.py --dry-run       # list what would be built and what would change, write nothing
  ./Scripts/Setup/install-asgard.py --no-build      # install only what is already built; stop if anything is missing
  ./Scripts/Setup/install-asgard.py --verify        # also compare file contents, not just size and time
  ./Scripts/Setup/install-asgard.py --check-prefix  # only check that the prefix can be installed to
  ./Scripts/Setup/install-asgard.py --uninstall     # remove it from the chosen prefix

In a checkout, the install first builds what the checkout lacks or has out of date (the app launcher after its
Java changed, the adapters after theirs, a framework tree older than its scripts, anything not yet downloaded:
`make android-provision`, which keeps what is current), so updating is `git pull` and this. That runs as the
checkout's owner when this runs as root, so `sudo make install-asgard` leaves no root-owned files in build/.
The pinned microG APKs are downloaded here when they are missing or the wrong size; --dry-run only reports
them. The marker also records the emulator build this install belongs with (BIFROST_BUILT_EMULATOR, which the Makefile
exports, or the checkout's bifrost-emu), so that `asgard` can warn when another build runs it. The install is a self-contained copy of what Asgard reads (about 1 GiB: scripts, signing/dex tools,
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
import pwd
import shutil
import stat
import subprocess
import sys
import time
from http.client import HTTPException
from pathlib import Path
from types import SimpleNamespace

sys.dont_write_bytecode = True  # importing android_microg must not write into the checkout

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
    ("build/microg", ("tmp*",)),  # tmp*: what a killed download leaves behind
]
MICROG = "build/microg"
# How to create what is missing from an unprovisioned checkout.
PROVISIONING = {
    "Tools/android-runtime": "python3 Tools/Android/fetch-android-runtime.py",
    "Tools/android-tools": "python3 Tools/Android/fetch-aapt2.py && python3 Tools/Android/fetch-android-package-tools.py",
    "build/android-adapters": "make android-runtime-adapters",
    "build/android-managed-launcher": "make android-managed-launcher",
    "build/android-framework/rootfs": "python3 Tools/Android/android_framework.py --replace-obsolete  (builds ART too)",
    "build/android-art/rootfs": "python3 Tools/Android/android_art.py --replace-obsolete",
}
# Written last, when a tree is complete. The tools that write them refuse to touch a tree without one.
MANIFESTS = ("build/android-art/rootfs/bifrost-art.json", "build/android-framework/rootfs/bifrost-framework.json")
REQUIRED = ("Tools/android-tools/aapt2", "Tools/android-tools/apksigner.jar",
            "build/android-adapters/adapters.json", "build/android-managed-launcher/classes.dex", *MANIFESTS)
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


def source_plan(pending=None):
    """{relative path: lstat} of every file, link and directory that belongs in the install.
    `pending` ({name: (size, why)}) adds the microG APKs a real run would download first."""
    plan = {}
    for name, patterns in FILES:
        if (ROOT / name).exists():
            for relative, info in scan(ROOT / name, patterns):
                plan[Path(name) / relative] = info
    if pending:
        plan.setdefault(Path(MICROG), SimpleNamespace(st_mode=stat.S_IFDIR | 0o755))
        for name, (size, _) in pending.items():
            plan[Path(MICROG) / name] = SimpleNamespace(st_mode=stat.S_IFREG | 0o644, st_size=size, st_mtime_ns=-1)
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


def provisioning_hint(name):
    """The command that creates `name`, a tree of FILES or a file inside one."""
    for tree, how in PROVISIONING.items():
        if name == tree or name.startswith(tree + "/"):
            return how
    return ""


def fresh_provisioning(name):
    """The command that rebuilds a tree whose manifest guards it from being changed in place: the tool deletes the
    old tree itself when its manifest says it can no longer be used, and downloads the Android image again."""
    return provisioning_hint(name)


def checkout_tool(name):
    """A module of this checkout's Tools/Android (never an installed copy's)."""
    tools = str(ROOT / "Tools/Android")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    return __import__(name)


def stale_framework():
    """[(problem, how to fix it)] when the framework tree is older than the scripts copied with it. The installed
    scripts would add the missing files at the first launch, below a root that is read-only by then. Checks
    paths and the manifest only, never file contents."""
    tree = ROOT / "build/android-framework/rootfs"
    try:
        missing = checkout_tool("android_framework").missing_additions(tree)
    except (OSError, ValueError) as error:
        return [(f"{tree.relative_to(ROOT)} is out of date ({error})", fresh_provisioning(MANIFESTS[1]))]
    if not missing:
        return []
    more = f" and {len(missing) - 1} more" if len(missing) > 1 else ""
    return [(f"missing {tree.relative_to(ROOT)}/{missing[0]}{more}",
             "python3 Tools/Android/android_framework.py --replace-obsolete  (in place)")]


def missing_provisioning():
    """[(problem, how to fix it)]: trees that do not exist, required files that are absent or empty, then a framework
    tree that is older than the scripts."""
    missing = [(f"missing {name}", PROVISIONING.get(name, "")) for name, _ in FILES
               if name != MICROG and not (ROOT / name).exists()]
    if missing:
        return missing
    for name in REQUIRED:
        path = ROOT / name
        if not path.is_file() or path.stat().st_size == 0:
            missing.append((f"missing {name}", fresh_provisioning(name) if name in MANIFESTS else provisioning_hint(name)))
    return missing or stale_framework()


def owner_options():
    """subprocess options that run a command as the checkout's owner when this runs as root (sudo make install-asgard):
    the build trees stay that user's, and so do the caches the build tools write. Empty otherwise."""
    info = ROOT.stat()
    if os.geteuid() != 0 or info.st_uid == 0:
        return {}
    try:
        owner = pwd.getpwuid(info.st_uid)
    except KeyError:
        return {}
    env = {name: value for name, value in os.environ.items()
           if not name.startswith(("XDG_", "SUDO_"))}
    env.update(HOME=owner.pw_dir, USER=owner.pw_name, LOGNAME=owner.pw_name)
    return {"user": info.st_uid, "group": info.st_gid, "env": env,
            "extra_groups": os.getgrouplist(owner.pw_name, info.st_gid)}


def build_missing(dry_run):
    """Build what this checkout lacks or has out of date, with Scripts/Android/android_provisioning.py: the same steps
    as `make android-provision` and `asgard setup`, each of which keeps what is current. An installed copy's setup tree
    has no sources and no Makefile, and is left to the check in missing_provisioning()."""
    script = ROOT / "Scripts/Android/android_provisioning.py"
    if not script.is_file() or not (ROOT / "Makefile").is_file():
        return
    command = [sys.executable, "-B", str(script)] + (["--check"] if dry_run else [])
    sys.stdout.flush()
    result = subprocess.run(command, cwd=ROOT, **owner_options())
    if result.returncode and not dry_run:
        sys.exit("error: could not build what this checkout needs for Android (the reason is above); nothing was "
                 "installed.\n  Fix that and run this again: steps already done are kept, and an interrupted "
                 "download resumes.\n  --no-build installs what is built and stops at what is missing.")


def paired_emulator():
    """(path, sha256) of the emulator this install belongs with, or None: BIFROST_BUILT_EMULATOR (the Makefile exports
    its build target; the package's wrapper names /usr/bin/bifrost-emu) or else the checkout's own bifrost-emu. It is the
    build `make install` copies, not whatever is installed now, so that updating only Asgard is noticed too (see
    Scripts/Android/android_pairing.py)."""
    named = os.environ.get("BIFROST_BUILT_EMULATOR")
    path = (ROOT / named) if named else ROOT / "bifrost-emu"  # an absolute `named` keeps itself
    try:
        with path.open("rb") as stream:
            return path, hashlib.file_digest(stream, "sha256").hexdigest()
    except OSError:
        return None


SOURCE_SUFFIXES = (".cpp", ".hpp", ".h", ".c", ".inc")


def newer_source(emulator):
    """The first C or C++ source or header under Source/, Include/ or Plugins/ that is newer than `emulator`, or None."""
    limit = emulator.stat().st_mtime_ns
    for top in ("Source", "Include", "Plugins"):
        for directory, _, files in os.walk(ROOT / top):
            for name in files:
                if name.endswith(SOURCE_SUFFIXES) and (Path(directory) / name).stat().st_mtime_ns > limit:
                    return Path(directory) / name
    return None


def microg_tool():
    return checkout_tool("android_microg")


def microg_problems(microg):
    """{name: (pinned size, why it is unusable)} for the pinned APKs, judged by name and size.
    The content is archive()'s to check, which reads every byte."""
    problems = {}
    for _, name, size, _, _ in microg.PACKAGES.values():
        path = ROOT / MICROG / name
        try:
            actual = path.lstat()
        except FileNotFoundError:
            problems[name] = (size, "missing")
            continue
        if not stat.S_ISREG(actual.st_mode):
            problems[name] = (size, "not a regular file")
        elif actual.st_size != size:
            problems[name] = (size, f"{actual.st_size} bytes, pinned at {size}")
    return problems


def fetch_microg(microg):
    """Download what is missing or the wrong size and verify all of it, before anything is copied."""
    try:
        for package in microg.PACKAGES:
            microg.archive(package, ROOT / MICROG)
    except (OSError, ValueError, HTTPException) as error:
        sys.exit(f"error: cannot provision microG: {error}\n"
                 f"  Nothing was installed. Check {ROOT / MICROG} and the network, then run this again.")


def check_prefix(prefix, writing=True):
    """Stop before anything long happens if this prefix cannot take the install."""
    tree = prefix / "share/bifrost/asgard"
    if tree.is_symlink() or (tree.exists() and not (tree / MARKER).is_file()):
        sys.exit(f"refusing to update {tree}: it is not an install made by this script")
    if not writing:
        return
    for wanted in (tree, prefix / "bin"):
        existing = wanted
        while not existing.exists():
            existing = existing.parent
        # A directory of ours may be read-only (see writable()); we can still change that.
        if not os.access(existing, os.W_OK | os.X_OK) and existing.stat().st_uid != os.geteuid():
            sys.exit(f"error: cannot write to {existing}, so nothing can be installed under {prefix}.\n"
                     "  Choose a prefix you own, such as PREFIX=$HOME/.local, or run as the user that owns it "
                     "(/usr/local needs sudo).")


def install(prefix, verify, dry_run, build=True):
    if build:
        build_missing(dry_run)
    missing = missing_provisioning()
    if missing:
        print("error: this checkout is not fully provisioned for Android:", file=sys.stderr)
        for problem, how in missing:
            print(f"  {problem}" + (f"  ->  {how}" if how else ""), file=sys.stderr)
        print("  (make provision-asgard runs every step)", file=sys.stderr)
        sys.exit(1)
    check_prefix(prefix, writing=not dry_run)
    tree = prefix / "share/bifrost/asgard"
    microg = microg_tool()
    pending = microg_problems(microg)
    if dry_run:
        for name, (size, why) in pending.items():
            print(f"would download microG {name} ({size / 2**20:.1f} MiB): {why}")
    else:
        fetch_microg(microg)
        pending = None
    marker = tree / MARKER
    if marker.is_file() and json.loads(marker.read_text() or "{}").get("complete") is False:
        print("note: the previous run did not finish; this run completes it")
    if not dry_run:
        tree.mkdir(parents=True, exist_ok=True)
        write_marker(tree, complete=False)
    changed, removed = sync(tree, source_plan(pending), verify, dry_run)
    if dry_run:
        return
    try:  # only a label: absent without git or outside a checkout (an installed package's setup tree)
        revision = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                                  capture_output=True, text=True).stdout.strip()
    except OSError:
        revision = ""
    pair = paired_emulator()
    newer = newer_source(pair[0]) if pair and (ROOT / "Makefile").is_file() else None
    if newer:
        print(f"note: {pair[0]} is older than {newer.relative_to(ROOT)}. Run make first, then install again, so that the "
              "emulator and Asgard come from the same sources.", file=sys.stderr)
    emulator = {"sha256": pair[1], "path": str(pair[0])} if pair else None
    write_marker(tree, complete=True, source_revision=revision or None, updated_at=int(time.time()), emulator=emulator)
    if changed or removed:
        compileall.compile_dir(tree / "Scripts/Android", quiet=2)
        compileall.compile_dir(tree / "Tools/Android", quiet=2)
    wrapper = write_wrapper(prefix)
    print(f"Asgard installed in {tree}\nCommand: {wrapper}\n"
          "bifrost-emu app.apk finds it automatically (see Docs/Android/ANDROID_APK.md).")
    try:
        import PyQt6.QtWidgets  # noqa: F401
    except ImportError:
        print("note: the desktop launcher (asgard launcher) needs PyQt6, which is not installed for this Python:\n"
              "  Scripts/Setup/install-deps.sh installs it, or pip install PyQt6.\n"
              "  Everything else, including bifrost-emu app.apk, works without it.")


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
    parser.add_argument("--no-build", action="store_true",
                        help="do not build what is missing or out of date first; stop at what is missing")
    parser.add_argument("--verify", action="store_true", help="compare contents as well as size and time")
    parser.add_argument("--check-prefix", action="store_true",
                        help="check that this prefix can be installed to, change nothing and stop")
    parser.add_argument("--uninstall", action="store_true")
    args = parser.parse_args()
    prefix = Path("/usr/local") if args.system else (args.prefix or Path.home() / ".local").expanduser()
    try:
        if args.check_prefix:
            check_prefix(prefix)
        elif args.uninstall:
            uninstall(prefix)
        else:
            install(prefix, args.verify, args.dry_run, build=not args.no_build)
    except PermissionError as error:
        sys.exit(f"error: {error}\n  Run it as the user (or with sudo) that owns {prefix}.")


if __name__ == "__main__":
    main()
