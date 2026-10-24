#!/usr/bin/env python3
"""install-asgard.py: first install, incremental update, removal of stale files, read-only trees.

Runs the installer from a small fake checkout (the script locates its root from its own path),
so it needs no Android provisioning and writes only to a temporary directory."""
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

INSTALLER = Path(__file__).resolve().parents[1] / "Setup/install-asgard.py"


class AsgardInstallTests(unittest.TestCase):
    def setUp(self):
        self.work = Path(tempfile.mkdtemp(prefix="bifrost-asgard-install."))
        self.addCleanup(self.cleanup)
        self.checkout = self.work / "checkout"
        spec = {}
        for name in ("Scripts/Android/asgard.py", "Scripts/Android/test_skip.py", "Tools/Android/android_art.py",
                     "Tools/Android/helper.cpp", "Tools/android-tools/aapt2", "Tools/android-tools/apksigner.jar",
                     "Tools/android-runtime/rootfs/a", "Guest/Android/Managed/Launcher.java",
                     "Tests/Unit/android_bionic_start.S", "build/android-framework/rootfs/system/lib64/libc.so",
                     "build/android-art/rootfs/apex/x", "build/android-adapters/adapters.json",
                     "build/android-managed-launcher/classes.dex", "build/microg/gms.apk"):
            path = self.checkout / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("v1 " + name)
            spec[name] = path
        (self.checkout / "build/android-framework/rootfs/system/bin").mkdir(parents=True)
        os.symlink("../lib64/libc.so", self.checkout / "build/android-framework/rootfs/system/bin/linker")
        (self.checkout / "build/android-art/rootfs/empty-dir").mkdir()
        # A read-only directory, as the framework image has.
        (self.checkout / "build/android-art/rootfs/apex").chmod(0o555)
        self.addCleanup(lambda: (self.checkout / "build/android-art/rootfs/apex").chmod(0o755))
        (self.checkout / "Scripts/Setup").mkdir(parents=True)
        shutil.copy2(INSTALLER, self.checkout / "Scripts/Setup/install-asgard.py")
        self.prefix = self.work / "prefix"
        self.tree = self.prefix / "share/bifrost/asgard"

    def cleanup(self):
        for directory, _, _ in os.walk(self.work):
            os.chmod(directory, 0o755)
        shutil.rmtree(self.work, ignore_errors=True)

    def run_installer(self, *args, expect=0):
        result = subprocess.run([sys.executable, str(self.checkout / "Scripts/Setup/install-asgard.py"),
                                 "--prefix", str(self.prefix), *args], capture_output=True, text=True)
        self.assertEqual(result.returncode, expect, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def test_first_install_copies_the_runtime_set_and_nothing_else(self):
        self.run_installer()
        self.assertEqual((self.tree / "Scripts/Android/asgard.py").read_text(), "v1 Scripts/Android/asgard.py")
        self.assertEqual(os.readlink(self.tree / "build/android-framework/rootfs/system/bin/linker"), "../lib64/libc.so")
        self.assertTrue((self.tree / "build/android-art/rootfs/empty-dir").is_dir())
        self.assertFalse((self.tree / "Scripts/Android/test_skip.py").exists())   # tests are not installed
        self.assertFalse((self.tree / "Tools/Android/helper.cpp").exists())       # build-only sources neither
        self.assertFalse((self.tree / "Scripts/Setup").exists())
        self.assertEqual(json.loads((self.tree / ".asgard-install").read_text())["complete"], True)
        wrapper = self.prefix / "bin/asgard"
        self.assertTrue(os.access(wrapper, os.X_OK))
        self.assertIn("share/bifrost/asgard/Scripts/Android/asgard.py", wrapper.read_text())
        self.assertFalse(list(self.prefix.rglob(".*.new.*")))                     # no temporary files left

    def test_an_unchanged_install_copies_nothing(self):
        self.run_installer()
        self.assertIn("0 to copy", self.run_installer())

    def test_dry_run_writes_nothing(self):
        output = self.run_installer("--dry-run")
        self.assertIn("to copy", output)
        self.assertFalse(self.prefix.exists())

    def test_update_copies_only_what_changed_and_removes_what_went_away(self):
        self.run_installer()
        untouched = self.tree / "build/microg/gms.apk"
        before = untouched.stat()
        (self.checkout / "Scripts/Android/asgard.py").write_text("v2 longer content")          # edited
        (self.checkout / "Scripts/Android/new_module.py").write_text("new")                      # added
        (self.checkout / "Tools/Android/android_art.py").unlink()                                # deleted
        shutil.rmtree(self.checkout / "build/android-art/rootfs/empty-dir")
        output = self.run_installer()
        self.assertIn("2 to copy", output)
        self.assertIn("2 to remove", output)
        self.assertEqual((self.tree / "Scripts/Android/asgard.py").read_text(), "v2 longer content")
        self.assertEqual((self.tree / "Scripts/Android/new_module.py").read_text(), "new")
        self.assertFalse((self.tree / "Tools/Android/android_art.py").exists())
        self.assertFalse((self.tree / "build/android-art/rootfs/empty-dir").exists())
        after = untouched.stat()
        self.assertEqual((before.st_ino, before.st_mtime_ns), (after.st_ino, after.st_mtime_ns))

    def test_a_modified_install_is_repaired_even_when_read_only(self):
        self.run_installer()
        (self.tree / "Scripts/Android/asgard.py").write_text("tampered")
        (self.tree / "Scripts/Android/stray.py").write_text("x")
        for directory, _, _ in os.walk(self.tree):  # a system install's directories
            os.chmod(directory, 0o555)
        self.run_installer()
        self.assertEqual((self.tree / "Scripts/Android/asgard.py").read_text(), "v1 Scripts/Android/asgard.py")
        self.assertFalse((self.tree / "Scripts/Android/stray.py").exists())

    def test_size_alone_and_modification_time_alone_each_trigger_an_update(self):
        self.run_installer()
        size_only = self.tree / "Tools/android-tools/aapt2"
        stamp = size_only.stat()
        os.chmod(size_only, stat.S_IMODE(stamp.st_mode) | 0o200)
        size_only.write_text("shorter")
        os.utime(size_only, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))     # same time, different size
        os.chmod(size_only, stat.S_IMODE(stamp.st_mode))
        self.assertIn("1 to copy", self.run_installer())
        self.assertEqual(size_only.read_text(), "v1 Tools/android-tools/aapt2")
        newer = self.checkout / "Tools/android-tools/apksigner.jar"        # same size, newer time
        stamp = newer.stat()
        os.utime(newer, ns=(stamp.st_atime_ns, stamp.st_mtime_ns + 5_000_000_000))
        self.assertIn("1 to copy", self.run_installer())
        self.assertEqual((self.tree / "Tools/android-tools/apksigner.jar").stat().st_mtime_ns, newer.stat().st_mtime_ns)

    def test_verify_finds_same_size_edits_that_size_and_time_cannot(self):
        self.run_installer()
        target = self.tree / "Tools/android-tools/aapt2"
        before = target.stat()
        mode = stat.S_IMODE(before.st_mode)
        os.chmod(target, mode | 0o200)
        target.write_text("v9 " + "Tools/android-tools/aapt2")                                  # same length
        os.utime(target, ns=(before.st_atime_ns, before.st_mtime_ns))
        os.chmod(target, mode)
        self.assertIn("0 to copy", self.run_installer())
        self.assertIn("1 to copy", self.run_installer("--verify"))
        self.assertEqual(target.read_text(), "v1 Tools/android-tools/aapt2")

    def test_an_unprovisioned_checkout_names_the_missing_pieces(self):
        shutil.rmtree(self.checkout / "build/android-adapters")
        output = self.run_installer(expect=1)
        self.assertIn("build/android-adapters", output)
        self.assertIn("make android-runtime-adapters", output)
        self.assertFalse(self.tree.exists())

    def test_uninstall_removes_the_install_and_refuses_foreign_directories(self):
        self.run_installer()
        self.run_installer("--uninstall")
        self.assertFalse(self.tree.exists())
        self.assertFalse((self.prefix / "bin/asgard").exists())
        foreign = self.prefix / "share/bifrost/asgard"
        foreign.mkdir(parents=True)
        (foreign / "keep").write_text("mine")
        self.run_installer("--uninstall", expect=1)
        self.run_installer(expect=1)
        self.assertEqual((foreign / "keep").read_text(), "mine")

    def test_a_symlinked_install_location_is_never_followed(self):
        self.prefix.joinpath("share/bifrost").mkdir(parents=True)
        os.symlink(self.checkout, self.tree)
        self.run_installer(expect=1)
        self.run_installer("--uninstall", expect=1)
        self.assertTrue((self.checkout / "Scripts/Android/asgard.py").exists())


if __name__ == "__main__":
    unittest.main()
