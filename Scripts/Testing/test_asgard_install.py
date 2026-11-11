#!/usr/bin/env python3
"""install-asgard.py: first install, incremental update, removal of stale files, read-only trees,
provisioning checks and the microG download.

Runs the installer from a small fake checkout (the script locates its root from its own path),
so it needs no Android provisioning and writes only to a temporary directory. It stays offline:
the fake checkout's android_microg.py is the real one with its pins and URL pointing at small
files in a local directory, so its real archive() "downloads" them through a file:// URL."""
import hashlib
import importlib.util
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
INSTALLER = REPOSITORY / "Scripts/Setup/install-asgard.py"
GMS, VENDING = "gms-test.apk", "vending-test.apk"
RELEASE = {GMS: b"microG services " * 8, VENDING: b"companion " * 5}


class AsgardInstallTests(unittest.TestCase):
    def setUp(self):
        self.work = Path(tempfile.mkdtemp(prefix="bifrost-asgard-install."))
        self.addCleanup(self.cleanup)
        self.checkout = self.work / "checkout"
        spec = {}
        for name in ("Scripts/Android/asgard.py", "Scripts/Android/test_skip.py", "Tools/Android/android_old.py",
                     "Tools/Android/helper.cpp", "Tools/android-tools/aapt2", "Tools/android-tools/apksigner.jar",
                     "Tools/android-runtime/rootfs/a", "Guest/Android/Managed/Launcher.java",
                     "Tests/Unit/android_bionic_start.S",
                     "build/android-art/rootfs/apex/x", "build/android-art/rootfs/bifrost-art.json",
                     "build/android-adapters/adapters.json", "build/android-managed-launcher/classes.dex"):
            path = self.checkout / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("v1 " + name)
            spec[name] = path
        self.install_framework_tool()
        os.symlink("../lib64/libc.so", self.checkout / "build/android-framework/rootfs/system/bin/linker")
        (self.checkout / "build/android-art/rootfs/empty-dir").mkdir()
        # A read-only directory, as the framework image has.
        (self.checkout / "build/android-art/rootfs/apex").chmod(0o555)
        self.addCleanup(lambda: (self.checkout / "build/android-art/rootfs/apex").chmod(0o755))
        (self.checkout / "Scripts/Setup").mkdir(parents=True)
        shutil.copy2(INSTALLER, self.checkout / "Scripts/Setup/install-asgard.py")
        self.prefix = self.work / "prefix"
        self.tree = self.prefix / "share/bifrost/asgard"
        self.microg = self.checkout / "build/microg"
        self.install_microg_tool()
        self.microg.mkdir(parents=True)
        for name, content in RELEASE.items():
            (self.microg / name).write_bytes(content)

    def install_microg_tool(self):
        """The real android_microg.py and its imports, pinned to the local release directory."""
        self.releases = self.work / "releases"
        self.releases.mkdir()
        for name, content in RELEASE.items():
            (self.releases / name).write_bytes(content)
        pins = {"com.google.android.gms": ("GmsCore", GMS, len(RELEASE[GMS]), hashlib.sha256(RELEASE[GMS]).hexdigest(), 19998),
                "com.android.vending": ("Phonesky", VENDING, len(RELEASE[VENDING]), hashlib.sha256(RELEASE[VENDING]).hexdigest(), 19999)}
        source = (REPOSITORY / "Tools/Android/android_microg.py").read_text()
        source, urls = re.subn(r"^URL = .*$", f"URL = {self.releases.as_uri() + '/'!r}", source, flags=re.M)
        source, packages = re.subn(r"^PACKAGES = \{.*?^\}$", f"PACKAGES = {pins!r}", source, flags=re.M | re.S)
        self.assertEqual((urls, packages), (1, 1))  # never reach the real URL
        self.tool = self.checkout / "Tools/Android/android_microg.py"
        self.tool.write_text(source)
        for name in ("android_media.py", "android_super.py", "download_progress.py"):
            shutil.copy2(REPOSITORY / "Tools/Android" / name, self.checkout / "Tools/Android" / name)

    def install_framework_tool(self):
        """The real android_framework.py and its imports, with a framework tree that is current for them: the
        manifest pins this version's in-place additions, whose files are placeholders (the check reads no contents)."""
        for name in ("android_framework.py", "android_art.py", "android_codecs.py", "android_guest_dependencies.py",
                     "android_media.py", "android_super.py", "download_progress.py", "file_digests.py"):
            shutil.copy2(REPOSITORY / "Tools/Android" / name, self.checkout / "Tools/Android" / name)
        launchers = self.checkout / "Guest/Android/Launchers"
        launchers.mkdir(parents=True)
        for name in ("codec2_store.c", "codec2_store.S", "media_resource.c", "media_log.c"):
            shutil.copy2(REPOSITORY / "Guest/Android/Launchers" / name, launchers / name)
        self.pins = json.loads(self.framework_tool(
            "print(json.dumps({'schema': f.SCHEMA, 'image': f.IMAGE_SHA256, 'codecs': c.fingerprint(), "
            "'files': f.in_place_inventory()}))").stdout)
        self.framework = self.checkout / "build/android-framework/rootfs"
        for name, value in self.pins["files"].items():
            path = self.framework / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if "link" in value:
                os.symlink(value["link"], path)
            else:
                path.write_text("placeholder " + name)
        self.write_framework_manifest(self.pins["files"])

    def write_framework_manifest(self, files, fingerprint=None):
        (self.framework / "bifrost-framework.json").write_text(json.dumps(
            {"schema_version": self.pins["schema"], "image_sha256": self.pins["image"],
             "codecs": {"adapter_fingerprint": fingerprint or self.pins["codecs"]}, "files": files}))

    def framework_tool(self, code, root=None, expect=0):
        """Run `code` with android_framework (f) and android_codecs (c) imported from a checkout or an install."""
        result = subprocess.run([sys.executable, "-B", "-c", "import json, sys\nsys.path.insert(0, 'Tools/Android')\n"
                                 "import android_framework as f, android_codecs as c\n" + code],
                                cwd=root or self.checkout, capture_output=True, text=True)
        self.assertEqual(result.returncode, expect, result.stdout + result.stderr)
        return result

    def forbid_downloads(self):
        """Fail the run if the installer reaches archive() at all."""
        with self.tool.open("a") as stream:
            stream.write("\ndef archive(*args, **kwargs):\n    raise SystemExit('archive() was called')\n")

    def snapshot(self):
        """Every path under the test directory, to prove that a run changed nothing."""
        state = {}
        for directory, directories, files in os.walk(self.work):
            for name in directories + files:
                info = os.lstat(os.path.join(directory, name))
                state[os.path.join(directory, name)] = (info.st_mode, info.st_size, info.st_mtime_ns)
        return state

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

    def test_installed_files_are_readable_by_everyone_and_writable_only_by_the_owner(self):
        private = self.checkout / "build/android-framework/rootfs/bifrost-framework.json"
        private.chmod(0o600)                       # an inventory rewritten through a temporary file
        tool = self.checkout / "Tools/android-tools/aapt2"
        tool.chmod(0o700)
        shared = self.microg / GMS
        shared.chmod(0o666)
        self.run_installer()
        mode = lambda name: stat.S_IMODE((self.tree / name).stat().st_mode)
        self.assertEqual(mode("build/android-framework/rootfs/bifrost-framework.json"), 0o644)
        self.assertEqual(mode("Tools/android-tools/aapt2"), 0o755)
        self.assertEqual(mode(f"build/microg/{GMS}"), 0o644)
        self.assertIn("0 to copy", self.run_installer())
        installed = self.tree / "build/android-framework/rootfs/bifrost-framework.json"
        installed.chmod(0o600)                     # an install made before modes were normalized
        self.assertIn("1 to copy", self.run_installer())
        self.assertEqual(mode("build/android-framework/rootfs/bifrost-framework.json"), 0o644)

    def test_the_install_says_when_the_launcher_cannot_start(self):
        stub = self.work / "python/PyQt6"
        stub.mkdir(parents=True)
        (stub / "__init__.py").write_text("raise ImportError('no Qt')\n")
        os.environ["PYTHONPATH"] = str(stub.parent)
        self.addCleanup(os.environ.pop, "PYTHONPATH")
        self.assertIn("needs PyQt6", self.run_installer())
        (stub / "__init__.py").write_text("")
        (stub / "QtWidgets.py").write_text("")
        self.assertNotIn("needs PyQt6", self.run_installer())

    def test_an_unchanged_install_copies_nothing(self):
        self.run_installer()
        self.assertIn("0 to copy", self.run_installer())

    def test_dry_run_writes_nothing(self):
        before = self.snapshot()
        output = self.run_installer("--dry-run")
        self.assertIn("to copy", output)
        self.assertNotIn("would download", output)
        self.assertFalse(self.prefix.exists())
        self.assertEqual(self.snapshot(), before)  # not even __pycache__ from importing android_microg

    def test_dry_run_reports_missing_microg_without_downloading(self):
        self.run_installer()                       # so the plan lists only microG (the listing is cut at 20)
        shutil.rmtree(self.microg)
        self.forbid_downloads()
        before = self.snapshot()
        output = self.run_installer("--dry-run")
        self.assertIn(f"would download microG {GMS}", output)
        self.assertIn(f"would download microG {VENDING}", output)
        self.assertIn(f"update build/microg/{GMS}", output)
        self.assertEqual(self.snapshot(), before)

    def test_dry_run_leaves_a_wrong_size_apk_alone(self):
        (self.microg / GMS).write_bytes(b"short")
        self.forbid_downloads()
        before = self.snapshot()
        output = self.run_installer("--dry-run")
        self.assertIn(f"would download microG {GMS}", output)
        self.assertIn(f"5 bytes, pinned at {len(RELEASE[GMS])}", output)
        self.assertNotIn(f"would download microG {VENDING}", output)
        self.assertEqual(self.snapshot(), before)

    def test_dry_run_does_not_call_an_installed_apk_stale_when_the_checkout_lacks_it(self):
        self.run_installer()
        shutil.rmtree(self.microg)
        output = self.run_installer("--dry-run")
        self.assertIn("2 to copy", output)
        self.assertIn("0 to remove", output)

    def assert_microg_installed(self):
        for name, content in RELEASE.items():
            self.assertEqual((self.microg / name).read_bytes(), content)
            self.assertEqual((self.tree / "build/microg" / name).read_bytes(), content)
        self.assertEqual(json.loads((self.tree / ".asgard-install").read_text())["complete"], True)

    def test_a_real_install_downloads_missing_microg(self):
        (self.microg / GMS).unlink()
        self.run_installer()
        self.assert_microg_installed()

    def test_a_real_install_fills_an_empty_microg_directory(self):
        shutil.rmtree(self.microg)
        self.microg.mkdir()                        # this passed the old directory check
        self.run_installer()
        self.assert_microg_installed()

    def test_a_real_install_creates_the_microg_directory(self):
        shutil.rmtree(self.microg)
        self.run_installer()
        self.assert_microg_installed()

    def test_a_real_install_replaces_empty_and_wrong_size_apks(self):
        (self.microg / GMS).write_bytes(b"")
        (self.microg / VENDING).write_bytes(b"truncated")
        self.run_installer()
        self.assert_microg_installed()

    def test_same_size_wrong_content_is_rejected_by_the_hash_and_installs_nothing(self):
        (self.microg / GMS).write_bytes(b"x" * len(RELEASE[GMS]))
        output = self.run_installer(expect=1)
        self.assertIn("SHA-256 mismatch", output)
        self.assertFalse(self.tree.exists())

    def test_a_failed_download_is_reported_and_installs_nothing(self):
        shutil.rmtree(self.microg)
        shutil.rmtree(self.releases)
        output = self.run_installer(expect=1)
        self.assertIn("cannot provision microG", output)
        self.assertNotIn("Traceback", output)
        self.assertFalse(self.tree.exists())
        self.assertEqual(list(self.microg.iterdir()), [])

    def test_a_truncated_download_keeps_no_partial_file(self):
        (self.microg / GMS).unlink()
        (self.releases / GMS).write_bytes(b"short")
        self.assertIn("cannot provision microG", self.run_installer(expect=1))
        self.assertEqual([path.name for path in self.microg.iterdir()], [VENDING])
        self.assertFalse(self.tree.exists())

    def test_what_a_killed_download_left_behind_is_not_installed(self):
        (self.microg / "tmpabc123").write_bytes(b"partial")
        self.run_installer()
        self.assertFalse((self.tree / "build/microg/tmpabc123").exists())

    def test_uninstall_needs_no_microg(self):
        self.run_installer()
        shutil.rmtree(self.microg)
        self.forbid_downloads()
        self.run_installer("--uninstall")
        self.assertFalse(self.tree.exists())
        self.assertFalse(self.microg.exists())

    def test_the_real_pins_have_the_shape_the_installer_reads(self):
        sys.dont_write_bytecode = True
        sys.path.insert(0, str(REPOSITORY / "Tools/Android"))
        self.addCleanup(sys.path.remove, str(REPOSITORY / "Tools/Android"))
        spec = importlib.util.spec_from_file_location("android_microg", REPOSITORY / "Tools/Android/android_microg.py")
        microg = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(microg)
        self.assertEqual(set(microg.PACKAGES), {"com.google.android.gms", "com.android.vending"})
        for _, name, size, sha256, _ in microg.PACKAGES.values():
            self.assertRegex(name, r"\.apk\Z")
            self.assertGreater(size, 1 << 20)
            self.assertRegex(sha256, r"\A[0-9a-f]{64}\Z")

    def test_empty_or_truncated_runtime_trees_are_rejected(self):
        self.forbid_downloads()                    # and these are found before any download
        for name, build in (("build/android-framework/rootfs/bifrost-framework.json", "android_framework.py"),
                            ("build/android-art/rootfs/bifrost-art.json", "android_art.py")):
            manifest = self.checkout / name
            content = manifest.read_text()
            manifest.unlink()                      # a tree that never finished
            output = self.run_installer(expect=1)
            self.assertIn(f"missing {name}", output)
            self.assertIn(f"rm -rf {Path(name).parent}, then python3 Tools/Android/{build}", output)
            self.assertIn("make provision-asgard", output)
            manifest.write_text("")                # a manifest that is itself empty
            self.assertIn(f"missing {name}", self.run_installer(expect=1))
            manifest.write_text(content)
            self.assertFalse(self.tree.exists())

    def test_an_empty_runtime_directory_is_rejected(self):
        tree = self.checkout / "build/android-framework/rootfs"
        shutil.rmtree(tree)
        tree.mkdir()
        self.assertIn("missing build/android-framework/rootfs/bifrost-framework.json", self.run_installer(expect=1))
        self.assertFalse(self.tree.exists())

    def test_check_prefix_changes_nothing(self):
        before = self.snapshot()
        self.run_installer("--check-prefix")
        self.assertEqual(self.snapshot(), before)

    def test_check_prefix_accepts_our_own_read_only_install(self):
        self.run_installer()
        for directory, _, _ in os.walk(self.tree):
            os.chmod(directory, 0o555)
        self.run_installer("--check-prefix")

    def test_a_foreign_install_location_fails_the_check_and_stops_before_downloading(self):
        self.tree.mkdir(parents=True)
        self.forbid_downloads()
        self.assertIn("not an install made by this script", self.run_installer("--check-prefix", expect=1))
        self.assertIn("not an install made by this script", self.run_installer(expect=1))

    @unittest.skipIf(os.geteuid() == 0, "root can write anywhere")
    def test_a_prefix_owned_by_someone_else_stops_before_downloading(self):
        self.prefix = Path("/proc/bifrost-test-prefix")
        shutil.rmtree(self.microg)
        self.forbid_downloads()
        for arguments in (("--check-prefix",), ()):
            output = self.run_installer(*arguments, expect=1)
            self.assertIn("cannot write to /proc", output)
            self.assertIn("/usr/local needs sudo", output)
        self.assertFalse(self.microg.exists())

    def test_make_provision_asgard_follows_the_setup_order_and_builds_art_once(self):
        make = subprocess.run(["make", "--no-print-directory", "-n", "provision-asgard", "PREFIX=/opt/x", "DESTDIR=/stage"],
                              cwd=REPOSITORY, capture_output=True, text=True)
        self.assertEqual(make.returncode, 0, make.stderr)
        steps = ["install-asgard.py --prefix \"/stage/opt/x\" --check-prefix", "fetch-android-runtime.py", "fetch-aapt2.py",
                 "fetch-android-package-tools.py", "build-android-adapters.py", "android_managed.py build",
                 "android_framework.py", "android_microg.py"]
        place = 0
        for step in steps:
            place = make.stdout.index(step, place)
        self.assertRegex(make.stdout[place:], r"install-asgard\.py --prefix \"/stage/opt/x\"\n")
        art = [line for line in make.stdout.splitlines() if "android_art.py" in line]
        self.assertEqual(art, ["[ -f build/android-art/rootfs/bifrost-art.json ] || python3 Tools/Android/android_art.py"])

    def test_update_copies_only_what_changed_and_removes_what_went_away(self):
        self.run_installer()
        untouched = self.tree / "build/microg" / GMS
        before = untouched.stat()
        (self.checkout / "Scripts/Android/asgard.py").write_text("v2 longer content")          # edited
        (self.checkout / "Scripts/Android/new_module.py").write_text("new")                      # added
        (self.checkout / "Tools/Android/android_old.py").unlink()                                # deleted
        shutil.rmtree(self.checkout / "build/android-art/rootfs/empty-dir")
        output = self.run_installer()
        self.assertIn("2 to copy", output)
        self.assertIn("2 to remove", output)
        self.assertEqual((self.tree / "Scripts/Android/asgard.py").read_text(), "v2 longer content")
        self.assertEqual((self.tree / "Scripts/Android/new_module.py").read_text(), "new")
        self.assertFalse((self.tree / "Tools/Android/android_old.py").exists())
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
        self.assertIn("make provision-asgard", output)
        self.assertFalse(self.tree.exists())

    def test_a_framework_older_than_its_scripts_is_refused_before_anything_is_copied(self):
        self.forbid_downloads()
        extractor = self.framework / "system/bin/mediaextractor"
        extractor.unlink()                         # provisioned before the scripts pinned this file
        output = self.run_installer(expect=1)
        self.assertIn("missing build/android-framework/rootfs/system/bin/mediaextractor", output)
        self.assertIn("python3 Tools/Android/android_framework.py  (in place)", output)
        extractor.write_text("placeholder")
        files = dict(self.pins["files"])
        del files["system/lib64/libsoundpool.so"]  # on disk but not yet in the inventory
        self.write_framework_manifest(files)
        self.assertIn("missing build/android-framework/rootfs/system/lib64/libsoundpool.so", self.run_installer(expect=1))
        self.write_framework_manifest(self.pins["files"], fingerprint="0" * 64)
        output = self.run_installer(expect=1)
        self.assertIn("build/android-framework/rootfs is out of date (Codec adapters changed", output)
        self.assertIn("rm -rf build/android-framework/rootfs, then python3 Tools/Android/android_framework.py", output)
        self.assertFalse(self.tree.exists())

    def test_an_installed_framework_is_never_upgraded_in_place(self):
        self.run_installer()
        (self.tree / "build/android-framework/rootfs/system/bin/mediaextractor").unlink()
        for directory, _, _ in os.walk(self.tree):  # a system install's directories
            os.chmod(directory, 0o555)
        before = self.snapshot()
        error = self.framework_tool("f.install(f.Path('build/android-framework/rootfs'), f.Path('build/android-art/rootfs'))",
                                    root=self.tree, expect=1).stderr
        self.assertIn("ValueError: This Asgard install's Android framework is older than its scripts "
                      "(it lacks system/bin/mediaextractor)", error)
        self.assertIn("sudo make install-asgard", error)
        self.assertEqual(self.snapshot(), before)  # never reaches the image download below ROOT/build

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
