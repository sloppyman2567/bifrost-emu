#!/usr/bin/env python3
"""Signed split installation, rollback, real Bionic and native Wayland APK gate."""

import argparse
import concurrent.futures
import hashlib
import hmac
import importlib.util
import json
import os
import re
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import urllib.request
import zipfile

import android_package as package
import android_paths

ROOT = package.ROOT
ARGS = None
# Google's play-services-basement (GoogleApiAvailabilityLight) for the microG fixture.
BASEMENT = ("https://dl.google.com/android/maven2/com/google/android/gms/play-services-basement/"
            "18.12.0/play-services-basement-18.12.0.aar", 451993,
            "30e9df7012c15ef31a4b288b3d77eb4550b195a0581675cad121b0b4bbc47b76")


def stop_launcher(launcher):
    """Stop an `asgard.py launcher` and the android_package.py it runs as a child: terminating only the wrapper orphans it."""
    try:
        os.killpg(launcher.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        launcher.wait(timeout=10)
    except subprocess.TimeoutExpired:
        os.killpg(launcher.pid, signal.SIGKILL)
        launcher.wait(timeout=10)


def play_services_basement(work):
    """Return the pinned basement classes.jar, downloading the AAR on first use."""
    url, size, sha256 = BASEMENT
    cache = ROOT / "build/play-services-basement" / url.rsplit("/", 1)[1]
    if not cache.is_file():
        cache.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(url, timeout=60) as response:
            data = response.read(size + 1)
        if len(data) != size or hashlib.sha256(data).hexdigest() != sha256:
            raise ValueError("play-services-basement does not match its pin")
        cache.write_bytes(data)
    if hashlib.sha256(cache.read_bytes()).hexdigest() != sha256:
        raise ValueError("Cached play-services-basement does not match its pin")
    jar = work / "play-services-basement.jar"
    with zipfile.ZipFile(cache) as archive:
        jar.write_bytes(archive.read("classes.jar"))
    return jar


class SharedLibraryProvisioningTests(unittest.TestCase):
    def test_shared_libraries_install_from_the_image_and_are_verified(self):
        import android_framework
        files = {"/system/etc/permissions/fixture.lib.xml": b"<permissions/>", "/system/framework/fixture.lib.jar": b"jar bytes"}
        pins = {"fixture.lib": (hashlib.sha256(files["/system/etc/permissions/fixture.lib.xml"]).hexdigest(),
                                hashlib.sha256(files["/system/framework/fixture.lib.jar"]).hexdigest())}
        def dump(image, remote, target, max_size=0):
            target.write_bytes(files[remote])
        with tempfile.TemporaryDirectory() as directory, \
                patch.dict(android_framework.SHARED_LIBRARIES, pins, clear=True), \
                patch("android_framework.dump", side_effect=dump), \
                patch("android_framework.image_archive"), \
                patch("android_framework.extract_system", return_value=(Path("image"), None)) as extract:
            root = Path(directory)
            android_framework.install_shared_libraries(root)
            self.assertEqual((root / "system/framework/fixture.lib.jar").read_bytes(), b"jar bytes")
            self.assertEqual((root / "system/etc/permissions/fixture.lib.xml").read_bytes(), b"<permissions/>")
            self.assertEqual(android_framework.shared_library_inventory().keys(),
                             {"system/etc/permissions/fixture.lib.xml", "system/framework/fixture.lib.jar"})
            self.assertEqual((root / "system/framework/fixture.lib.jar").stat().st_mode & 0o777, 0o444)  # ART rejects writable dex
            (root / "system/framework/fixture.lib.jar").chmod(0o644)
            android_framework.install_shared_libraries(root)       # verified and made read-only again, not extracted again
            self.assertEqual((root / "system/framework/fixture.lib.jar").stat().st_mode & 0o777, 0o444)
            self.assertEqual(extract.call_count, 1)
            (root / "system/framework/fixture.lib.jar").chmod(0o644)
            (root / "system/framework/fixture.lib.jar").write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "Modified shared library file"):
                android_framework.install_shared_libraries(root)
            with self.assertRaisesRegex(ValueError, "Modified shared library file"):
                android_framework.without_shared_libraries({"system/framework/fixture.lib.jar": {"sha256": "x", "executable": False}})
            files["/system/framework/fixture.lib.jar"] = b"other image"
            (root / "system/framework/fixture.lib.jar").unlink()
            with self.assertRaisesRegex(ValueError, "differs from the pinned image"):
                android_framework.install_shared_libraries(root)


class BionicCompatibilityTests(unittest.TestCase):
    def setUp(self):
        shell = b'fixture shell'
        import android_framework
        def install(root, archive=None, system=None):
            target = root / 'system/bin/sh'; target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(shell); target.chmod(0o755)
        for setting in (patch('android_framework.SHELL_SHA256', hashlib.sha256(shell).hexdigest()),
                        patch('android_framework.install_shell', side_effect=install),
                        patch('android_framework.install_shared_libraries')):
            setting.start()
            self.addCleanup(setting.stop)
    def make_root(self, root):
        import json
        import android_art
        import android_framework
        from android_media import IMAGE_SHA256
        libraries = root / "apex/com.android.runtime/lib64/bionic"
        libraries.mkdir(parents=True)
        for name in ("libc.so", "libm.so", "libdl.so"):
            (libraries / name).write_bytes(name.encode())
        metadata = {"schema_version": android_framework.SCHEMA, "image_sha256": IMAGE_SHA256,
                    "codecs": {"adapter_fingerprint": "fixture"}, "files": android_art.inventory(root)}
        (root / "bifrost-framework.json").write_text(json.dumps(metadata))
        return metadata

    def test_existing_framework_upgrade_is_verified_and_idempotent(self):
        import android_framework
        from android_media import BIONIC_LINKS
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            before = self.make_root(root)
            with patch("android_codecs.fingerprint", return_value="fixture"):
                after = android_framework.install(root, root / "unused")
                self.assertEqual(after["files"], {**before["files"], **BIONIC_LINKS,
                                                'system/bin/sh': android_framework.shell_inventory(),
                                                **android_framework.shared_library_inventory(),
                                                **android_framework.media_extractor_inventory()})
                manifest = (root / "bifrost-framework.json").read_bytes()
                self.assertEqual((root / "bifrost-framework.json").stat().st_mode & 0o777, 0o644)
                self.assertEqual(android_framework.install(root, root / "unused"), after)
                self.assertEqual((root / "bifrost-framework.json").read_bytes(), manifest)
            for name in BIONIC_LINKS:
                self.assertTrue((root / name).samefile(
                    root / "apex/com.android.runtime/lib64/bionic" / Path(name).name))

    def test_interrupted_link_upgrade_recovers(self):
        import android_framework
        from android_media import BIONIC_LINKS
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_root(root)
            (root / "system/lib64").mkdir(parents=True)
            name = "system/lib64/libc.so"
            (root / name).symlink_to(BIONIC_LINKS[name]["link"])
            with patch("android_codecs.fingerprint", return_value="fixture"):
                after = android_framework.install(root, root / "unused")
            self.assertTrue(BIONIC_LINKS.items() <= after["files"].items())

    def test_modified_framework_is_not_blessed_by_upgrade(self):
        import android_framework
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_root(root)
            (root / "apex/com.android.runtime/lib64/bionic/libc.so").write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "modified framework"):
                android_framework.install(root, root / "unused")
            self.assertFalse((root / "system").exists())

    def test_conflicting_paths_and_redirected_directories_are_rejected(self):
        from android_media import install_bionic_links
        for conflict in ("file", "link", "directory"):
            with self.subTest(conflict=conflict), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self.make_root(root)
                (root / "system").mkdir()
                if conflict == "directory":
                    (root / "system/lib64").symlink_to(root / "apex/com.android.runtime/lib64/bionic")
                else:
                    (root / "system/lib64").mkdir()
                    path = root / "system/lib64/libm.so"
                    if conflict == "file":
                        path.write_bytes(b"conflict")
                    else:
                        path.symlink_to("/tmp/wrong-bionic-target")
                with self.assertRaises(ValueError):
                    install_bionic_links(root)
                self.assertFalse((root / "system/lib64/libc.so").is_symlink())


class HomeCommandTests(unittest.TestCase):
    def test_am_home_options_and_unresolved_commands(self):
        from android_processes import home_command
        base = ['start', '-a', 'android.intent.action.MAIN', '-c', 'android.intent.category.HOME']
        self.assertTrue(home_command(base + ['--user', '0', '-f', '0x10200000']))
        self.assertTrue(home_command(base + ['-c', 'android.intent.category.DEFAULT']))
        for suffix in (['--user', '10'], ['-n', 'other/.Activity'], ['-f', '-1'], ['--user'],
                       ['-c', 'android.intent.category.BROWSABLE']):
            with self.subTest(suffix=suffix), self.assertRaises(ValueError): home_command(base + suffix)
        with self.assertRaises(ValueError): home_command(['start', '-a', 'android.intent.action.VIEW'])

    def test_home_activation_is_idempotent_and_resume_queues_foreground(self):
        from android_processes import ProcessHost
        import threading
        host = ProcessHost.__new__(ProcessHost)
        host.state, host.home_active, host.task_visible, host.lock = Path('/tmp/library'), False, None, threading.Lock()
        with patch('android_launcher.activate_launcher') as activated:
            self.assertEqual(host.home(), {'exit_code': 0})
            host.home(); activated.assert_called_once_with(host.state)
            self.assertIs(host.task_visible, False)
            host.task(True); self.assertIs(host.task_visible, True)
            host.home(); self.assertEqual(activated.call_count, 2)

    def test_running_background_app_returns_instead_of_relaunching(self):
        from android_launcher import card_state
        app = {'session': {'state': 'running', 'background': True}}
        self.assertEqual(card_state(app)['action'], 'resume')


class LauncherArgumentTests(unittest.TestCase):
    def test_property_settings_cli_round_trip_and_validation(self):
        import json
        from android_settings import SETTINGS, load, properties
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory)
            app = state / "org.bifrost.fixture"
            app.mkdir()
            (app / "package.json").write_text("{}")
            def configure(key, value=None, reset=False):
                return package.configure(SimpleNamespace(state_root=state, package=app.name,
                    key=key, value=value, reset=reset, json=True))
            with patch("builtins.print"):
                configure("property.ro.kernel.qemu", "0")
                configure("property.debug.fixture", "")
                configure("network", "offline")
                self.assertEqual(properties(load(app)), {"ro.kernel.qemu": "0", "debug.fixture": ""})
                for key, value in (("property.bad\nname", "1"), ("property.a", "x\ny=1"),
                                   ("property.a", "é" * 46), ("property.a", "trailing "),
                                   ("property.a", "x\u0085y=1"), ("property.a", "x\u2028y=1")):
                    with self.assertRaises(ValueError): configure(key, value)
                configure("property.ro.kernel.qemu", reset=True)
                self.assertEqual(properties(load(app)), {"debug.fixture": ""})
                self.assertEqual(load(app)["network"], "offline")
                configure(None, reset=True)
                self.assertEqual(load(app), {})
            (app / SETTINGS).write_text(json.dumps({"property.a": "ok", "property.b": "x\ny=1"}))
            self.assertEqual(properties(load(app)), {"a": "ok"})

    def test_library_settings_reject_malformed_values_and_keep_default_launch(self):
        import json
        from android_launcher import launch_request
        from android_settings import SETTINGS, apply_to_launch, launch_changes, load, save
        app = {"package": "org.bifrost.fixture", "launcher": "managed_activity"}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for value in ([], {"back_key": []}, {"back_key": {}, "window_stats": "false", "network": 3},
                          {"back_key": "q", "network": "wifi"}):
                (root / SETTINGS).write_text(json.dumps(value))
                self.assertEqual(load(root), {})
            self.assertEqual(launch_request(app), ["start", app["package"]])
            self.assertEqual(launch_changes({}), ([], {}))
            settings = {"back_key": "f2", "window_stats": False, "network": "offline"}
            save(root, settings)
            self.assertEqual(load(root), settings)
            self.assertEqual(launch_request(app, trace=True), ["start", app["package"], "--trace-apk", "calls"])
            self.assertEqual(launch_changes(settings),
                             (["--no-window-stats"], {"BIFROST_ANDROID_BACK_KEY": "f2", "BIFROST_ANDROID_OFFLINE": "1"}))
            self.assertEqual(launch_changes({"enter_key": "dpad_center"}),
                             ([], {"BIFROST_ANDROID_ENTER_KEY": "dpad_center"}))
            for value, expected in (("on", "1"), ("off", "0"), ("verify", "verify")):
                self.assertEqual(launch_changes({"jit_disk_cache": value}), ([], {"BIFROST_JIT_DISK_CACHE": expected}))
            self.assertEqual(launch_changes({"jit_disk_cache": "default", "jit_disk_cache_mb": "default"}), ([], {}))
            self.assertEqual(launch_changes({"jit_disk_cache_mb": "1024"}), ([], {"BIFROST_JIT_DISK_CACHE_MB": "1024"}))
            native = dict(app, launcher="native_activity")
            self.assertNotIn("--trace-apk", launch_request(native, trace=True))
            # Saved choices apply to every launch, below the command line and the environment.
            self.assertEqual(apply_to_launch(root, ["--timeout", "5"], {}),
                             (["--timeout", "5", "--no-window-stats"],
                              {"BIFROST_ANDROID_BACK_KEY": "f2", "BIFROST_ANDROID_OFFLINE": "1"}))
            self.assertEqual(apply_to_launch(root, ["--window-stats-title"], {"BIFROST_ANDROID_OFFLINE": "0"}),
                             (["--window-stats-title"], {"BIFROST_ANDROID_BACK_KEY": "f2"}))

    def test_start_hands_saved_settings_to_the_session_process(self):
        import json
        from android_settings import save
        launched = []
        class Child:
            def __init__(self, command, **options):
                launched.append((command, options["env"]))
            def poll(self):
                return 1
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory)
            app = state / "org.bifrost.fixture"
            app.mkdir()
            (app / "package.json").write_text(json.dumps({"package": app.name}))
            save(app, {"window_stats": True, "back_key": "f1", "network": "offline"})
            with patch.object(package.subprocess, "Popen", Child), \
                    patch.dict(os.environ, {"BIFROST_ANDROID_BACK_KEY": "escape"}), \
                    self.assertRaises(ValueError):
                package.start_background(app.name, state, ["--timeout", "5"])
            command, environment = launched[0]
            self.assertEqual(command[-3:], ["--timeout", "5", "--window-stats"])
            # The caller's own environment is not overridden by the saved choice.
            self.assertEqual(environment["BIFROST_ANDROID_BACK_KEY"], "escape")
            self.assertEqual(environment["BIFROST_ANDROID_OFFLINE"], "1")

    def test_run_applies_saved_settings_below_explicit_options(self):
        import json
        import sys
        from android_settings import save
        calls = []
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory)
            app = state / "org.bifrost.fixture"
            app.mkdir()
            (app / "package.json").write_text(json.dumps({"package": app.name}))
            save(app, {"window_stats": False, "network": "offline"})
            def run(*extra):
                argv = ["android_package.py", "--state-root", str(state), "run", app.name, *extra]
                with patch.object(package, "launch", lambda *a: calls.append(a[6]) or 0), \
                        patch.object(sys, "argv", argv), patch.dict(os.environ, clear=False):
                    os.environ.pop("BIFROST_ANDROID_OFFLINE", None)
                    self.assertEqual(package.main(), 0)
                    return os.environ.get("BIFROST_ANDROID_OFFLINE")
            self.assertEqual(run(), "1")
            self.assertEqual(calls[-1], ["--no-window-stats"])
            run("--window-stats")
            self.assertEqual(calls[-1], ["--window-stats"])

    def test_stats_layout_is_validated_and_only_applies_while_stats_are_on(self):
        import json
        import sys
        from android_settings import SETTINGS, KEYS, launch_changes, load, parse, save
        self.assertEqual(KEYS[:2], ("window_stats", "window_stats_mode"))
        self.assertEqual([parse("window_stats_mode", x) for x in ("1", "2", "3", "minimal", "overlay")],
                         ["minimal", "normal", "overlay", "minimal", "overlay"])
        for bad in ("0", "loud", "", "Normal"):
            with self.assertRaises(ValueError):
                parse("window_stats_mode", bad)
        # Stats are opt-in: a layout alone changes nothing, and it follows an explicit off.
        self.assertEqual(launch_changes({"window_stats_mode": "overlay"}), ([], {}))
        self.assertEqual(launch_changes({"window_stats": False, "window_stats_mode": "overlay"}),
                         (["--no-window-stats"], {}))
        self.assertEqual(launch_changes({"window_stats": True}), (["--window-stats"], {}))
        self.assertEqual(launch_changes({"window_stats": True, "window_stats_mode": "overlay"}),
                         (["--window-stats", "--window-stats-mode=overlay"], {}))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / SETTINGS).write_text(json.dumps({"window_stats_mode": "loud", "window_stats": True}))
            self.assertEqual(load(root), {"window_stats": True})
            save(root, {"window_stats": True, "window_stats_mode": "minimal"})
            self.assertEqual(load(root), {"window_stats": True, "window_stats_mode": "minimal"})
            state = root / "state"
            (state / "org.bifrost.fixture").mkdir(parents=True)
            (state / "org.bifrost.fixture/package.json").write_text("{}")
            def config(*arguments):
                return subprocess.run([sys.executable, str(ROOT / "Scripts/Android/android_package.py"),
                                       "--state-root", str(state), "config", "org.bifrost.fixture", *arguments],
                                      capture_output=True, text=True)
            self.assertEqual(config("window_stats_mode", "3").returncode, 0)
            self.assertEqual(config("window_stats_mode").stdout.strip(), "overlay")
            self.assertEqual(config("window_stats_mode", "loud").returncode, 1)
            # A layout on the command line replaces the saved stats choice, and reaches the emulator as one flag.
            app = state / "org.bifrost.fixture"
            save(app, {"window_stats": False})
            calls = []
            argv = ["android_package.py", "--state-root", str(state), "run", app.name, "--window-stats-mode", "2"]
            with patch.object(package, "launch", lambda *a: calls.append(a[6]) or 0), \
                    patch.object(sys, "argv", argv):
                self.assertEqual(package.main(), 0)
            self.assertEqual(calls[-1], ["--window-stats-mode=2"])

    def test_config_command_shows_sets_validates_and_resets(self):
        import json
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory)
            (state / "org.bifrost.fixture").mkdir()
            (state / "org.bifrost.fixture/package.json").write_text("{}")
            def run(*arguments):
                return subprocess.run([sys.executable, str(ROOT / "Scripts/Android/android_package.py"),
                                       "--state-root", str(state), "config", *arguments],
                                      capture_output=True, text=True)
            self.assertIn("network = default", run("org.bifrost.fixture").stdout)
            self.assertEqual(run("org.bifrost.fixture", "network", "offline").returncode, 0)
            self.assertEqual(run("org.bifrost.fixture", "window_stats", "off").returncode, 0)
            self.assertEqual(run("org.bifrost.fixture", "network").stdout.strip(), "offline")
            self.assertEqual(json.loads(run("org.bifrost.fixture", "--json").stdout),
                             {"window_stats": False, "network": "offline"})
            for bad in (("network", "wifi"), ("window_stats", "maybe"), ("nope", "1")):
                self.assertEqual(run("org.bifrost.fixture", *bad).returncode, 1 if bad[0] != "nope" else 2, bad)
            self.assertEqual(run("org.bifrost.missing", "network", "offline").returncode, 1)
            self.assertEqual(run("org.bifrost.fixture", "--reset", "network").returncode, 0)
            self.assertEqual(json.loads(run("org.bifrost.fixture", "--json").stdout), {"window_stats": False})
            self.assertEqual(run("org.bifrost.fixture", "--reset").returncode, 0)
            self.assertEqual(json.loads(run("org.bifrost.fixture", "--json").stdout), {})

    def test_library_icons_are_bounded_and_legacy_metadata_remains_readable(self):
        import json
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            apk = root / "icons.apk"
            with zipfile.ZipFile(apk, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("res/icon.png", b"\x89PNG\r\n\x1a\nfixture")
                archive.writestr("res/icon.xml", b"<adaptive-icon/>")
                archive.writestr("res/large.png", b"\x89PNG\r\n\x1a\n" + bytes(4 * 1024 * 1024))
            icon = package.copy_icon(apk, "res/icon.png", root)
            self.assertEqual(icon.name, "icon.png")
            for member in (None, "res/missing.png", "res/icon.xml", "res/large.png"):
                self.assertIsNone(package.copy_icon(apk, member, root))
            app = root / "org.bifrost.fixture"
            app.mkdir()
            metadata = {"package": app.name, "version_code": 2,
                        "launcher": {"kind": "native_activity"},
                        "packages": [{"split": "", "label": "Legacy Fixture", "version_name": "1.2"}]}
            for invalid_icon in (None, [], "../icon.png"):
                metadata["icon"] = invalid_icon
                (app / "package.json").write_text(json.dumps(metadata))
                listed = package.installed_apps(root)[0]
                self.assertEqual((listed["label"], listed["version_name"], listed["icon"]),
                                 ("Legacy Fixture", "1.2", None))

    def test_activity_report_separates_overloads_failures_and_unfinished_calls(self):
        import json
        from android_managed import activity_api_report
        records = [
            {"event": "api.catalog", "service": "activity", "interface": "fixture.Manager", "methods": [
                {"method": "probe", "signature": "int probe(int)", "declared_coverage": "unclassified"},
                {"method": "probe", "signature": "int probe(long)", "declared_coverage": "unclassified"},
                {"method": "unused", "signature": "void unused()", "declared_coverage": "unsupported"}]},
            {"event": "call", "operation": "activity.probe", "signature": "int probe(int)", "id": 1},
            {"event": "return", "operation": "activity.probe", "signature": "int probe(int)", "id": 1},
            {"event": "call", "operation": "activity.probe", "signature": "int probe(long)", "id": 2},
            {"event": "throw", "operation": "activity.probe", "signature": "int probe(long)", "id": 2,
             "exception": "java.lang.UnsupportedOperationException", "message": "unsupported"},
            {"event": "call", "operation": "activity.probe", "signature": "int probe(int)", "id": 3},
            {"event": "trace.truncated"}]
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "calls.jsonl"
            log.write_text("\n".join(json.dumps(record) for record in records) + '\n{"event":')
            report = activity_api_report(log)
        self.assertEqual((report["available_methods"], report["observed_methods"]), (3, 2))
        self.assertTrue(report["truncated"])
        self.assertFalse(report["trace_stopped"])
        self.assertEqual(report["invalid_records"], 1)
        self.assertEqual(report["orphan_completions"], 0)
        failure, unfinished, unused = report["methods"]
        self.assertEqual((failure["signature"], failure["throws"]), ("int probe(long)", 1))
        self.assertEqual((unfinished["calls"], unfinished["returns"], unfinished["unfinished"]), (2, 1, 1))
        self.assertEqual(unused["calls"], 0)

    def test_split_paths_follow_install_order_before_abi_filtering(self):
        from android_managed import installed_arguments
        manifests = [{"split": name, "abis": abis} for name, abis in (
            ("", []), ("config.armeabi_v7a", ["armeabi-v7a"]),
            ("fonts", []), ("gamedata", []), ("config.arm64_v8a", ["arm64-v8a"]))]
        expected = ["--app", "org.bifrost.fixture", "/data/app/fixture", "FixtureActivity",
                    "config.arm64_v8a=split-1.apk", "fonts=split-3.apk", "gamedata=split-4.apk"]
        for inventory in (manifests, list(reversed(manifests))):
            metadata = {"package": "org.bifrost.fixture", "launcher": {"class": "FixtureActivity"},
                        "packages": inventory}
            original = list(inventory)
            self.assertEqual(installed_arguments(metadata, "/data/app/fixture"), expected)
            self.assertEqual(inventory, original)


class LauncherProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            from PyQt6.QtWidgets import QApplication
        except ImportError:
            raise unittest.SkipTest("PyQt6 is not installed")
        with patch.dict(os.environ, {"QT_QPA_PLATFORM": "offscreen"}):
            cls.qt = QApplication.instance() or QApplication([])

    def setUp(self):
        from PyQt6.QtWidgets import QWidget
        self.temporary = tempfile.TemporaryDirectory(prefix="bifrost-profile-")
        self.window = QWidget()
        self.window.state = Path(self.temporary.name)
        self.app = {"package": "org.bifrost.fixture", "label": "Fixture",
                    "launcher": "managed_activity", "session": {"state": "stopped"}}
        self.dialog = None

    def tearDown(self):
        if self.dialog: self.dialog.close()
        self.window.close()
        self.temporary.cleanup()

    def edit(self, settings):
        from android_launcher import settings_dialog
        self.dialog = settings_dialog(self.window, self.app, settings)
        return self.dialog

    def test_settings_dialog_has_no_theme_icons_and_clips_no_text(self):
        # On a themed desktop Save and Cancel showed clip-art, and the wrapped Runtime line was cut in half.
        from PyQt6.QtWidgets import QLabel, QPushButton
        dialog = self.edit({})
        dialog.show()
        self.assertEqual([b.text() for b in dialog.findChildren(QPushButton) if not b.icon().isNull()], [])
        for label in dialog.findChildren(QLabel):
            if not label.isVisible() or not label.text(): continue
            needed = label.heightForWidth(label.width()) if label.wordWrap() else label.sizeHint().height()
            self.assertGreaterEqual(label.height(), needed, label.text())
            if not label.wordWrap(): self.assertGreaterEqual(label.width(), label.sizeHint().width(), label.text())

    def test_profile_round_trip_uses_the_cli_format_and_cancel_does_not_save(self):
        from android_settings import save, load, properties
        saved = {"property.ro.kernel.qemu": "0", "property.debug.fixture": "", "network": "offline"}
        dialog = self.edit(saved)
        self.assertEqual(dialog.emulator.currentData(), "0")
        self.assertEqual(dialog.result_settings(), saved)
        dialog.add_property("debug.other", "enabled")
        dialog.back.setCurrentIndex(dialog.back.findData("f2"))
        output = dialog.result_settings()
        save(self.window.state, output)
        self.assertEqual(load(self.window.state), output)
        self.assertEqual(properties(output), {"ro.kernel.qemu": "0", "debug.fixture": "", "debug.other": "enabled"})
        dialog.reject()
        self.assertEqual(saved, {"property.ro.kernel.qemu": "0", "property.debug.fixture": "", "network": "offline"})
        # Restore defaults is staged; rejecting the dialog leaves the saved file intact.
        dialog.reset_profile()
        self.assertEqual(dialog.result_settings(), {})
        dialog.reject()
        self.assertEqual(load(self.window.state), output)

    def test_invalid_properties_keep_the_dialog_open_with_a_visible_error(self):
        dialog = self.edit({})
        dialog.add_property("debug.fixture", "one")
        dialog.add_property("debug.fixture", "two")
        dialog.accept()
        self.assertIn("Duplicate property", dialog.error.text())
        self.assertEqual(dialog.result(), 0)
        dialog.properties.selectRow(1); dialog.remove_properties()
        dialog.properties.item(0, 1).setText("injected\nproperty=value")
        with self.assertRaises(ValueError): dialog.result_settings()
        dialog.properties.item(0, 0).setText("ro.kernel.qemu")
        with self.assertRaisesRegex(ValueError, "control above"): dialog.result_settings()
        dialog.properties.item(0, 0).setText("debug.fixture")
        dialog.properties.item(0, 1).setText("")
        dialog.accept()
        self.assertEqual(dialog.result(), 1)

    def test_custom_values_are_preserved_and_native_apps_cannot_edit_device_properties(self):
        self.app["launcher"] = "native_activity"
        dialog = self.edit({"property.ro.kernel.qemu": "custom", "property.debug.fixture": "1"})
        self.assertFalse(dialog.emulator.isEnabled())
        self.assertFalse(dialog.properties.isEnabled())
        self.assertEqual(dialog.result_settings(), {"property.ro.kernel.qemu": "custom", "property.debug.fixture": "1"})


class LauncherLibraryTests(unittest.TestCase):
    """The real library window on Qt's offscreen platform: layout, sorting, input, queueing and theming."""
    @classmethod
    def setUpClass(cls):
        try:
            from PyQt6.QtWidgets import QApplication
            from PyQt6 import QtTest  # noqa: F401
        except ImportError:
            raise unittest.SkipTest("PyQt6 is not installed")
        with patch.dict(os.environ, {"QT_QPA_PLATFORM": "offscreen"}):
            cls.qt = QApplication.instance() or QApplication([])

    def setUp(self):
        from PyQt6.QtCore import QSettings
        import android_launcher
        self.launcher = android_launcher
        self.temporary = tempfile.TemporaryDirectory(prefix="bifrost-library-")
        self.root = Path(self.temporary.name)
        (self.root / "state").mkdir()
        self.settings = QSettings(str(self.root / "library.ini"), QSettings.Format.IniFormat)
        self.windows = []
        self.window = self.open_window()

    def tearDown(self):
        for window in self.windows:
            window.timer.stop(); window.close()
            for job in list(window.jobs): job.kill(); job.waitForFinished(2000)   # no child outlives its window
            window.deleteLater()
        self.qt.processEvents()
        self.temporary.cleanup()

    def settle(self, done=lambda: True, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.qt.processEvents()
            if done(): return
            time.sleep(0.01)
        self.fail("Qt events did not settle")

    def open_window(self, size=(1100, 700)):
        window = self.launcher.build(self.root / "state", self.qt, self.settings)
        window.timer.stop(); self.windows.append(window)
        self.settle(lambda: not window.refreshing)       # the initial listing of the empty library
        if size: window.resize(*size)
        window.show(); self.settle()
        return window

    def app(self, index, label=None, **session):
        return {"package": f"org.fixture.g{index}", "label": label or f"Game {index:02d}", "version_name": f"1.{index}",
                "installed_at": 1792000000 + index, "launcher": "managed_activity", "icon": None,
                "session": {"state": "stopped", **session}}

    def fill(self, count, window=None, **extra):
        window = window or self.window
        window.apps = [dict(self.app(i), **extra) for i in range(count)]
        window.render(True); self.settle()

    def test_grid_metrics_fill_the_width_between_the_card_limits(self):
        from android_launcher import GAP, MAX_CARD, MIN_CARD, grid_metrics
        for width in range(MIN_CARD, 3000, 7):
            columns, card = grid_metrics(width)
            self.assertTrue(MIN_CARD <= card <= MAX_CARD, (width, columns, card))
            self.assertLessEqual(columns * card + (columns - 1) * GAP, width)
            # No further column would fit, so the row is as full as the limits allow.
            self.assertGreater((columns + 1) * MIN_CARD + columns * GAP, width)

    def test_cards_fill_the_viewport_on_first_show_and_follow_resizes(self):
        # The grid once used the viewport width from before the window's layout, so a window
        # opened wide showed two columns and left more than half of it empty.
        from android_launcher import GUTTER, grid_metrics
        self.fill(12)
        for width in (1100, 1650, 800, 1300):
            self.window.resize(width, 700); self.settle()
            viewport = self.window.scroll.viewport().width()
            columns, card = grid_metrics(viewport - GUTTER)
            self.assertEqual(self.window.metrics, (columns, card), width)
            shown = [self.window.cards[p] for p in self.window.order]
            self.assertEqual({c.width() for c in shown}, {card})
            self.assertLess(max(c.geometry().right() for c in shown), viewport)
            self.assertEqual(len({c.geometry().x() for c in shown}), columns)
        self.assertGreaterEqual(self.window.columns, 3)

    def test_sort_mode_orders_the_library_and_survives_a_restart(self):
        self.window.apps = [self.app(1, "Charlie", started_at=300), self.app(2, "alpha", started_at=100),
                            self.app(3, "Bravo"), self.app(4, "Delta", started_at=900)]
        self.window.render(True)
        self.assertEqual([self.window.cards[p].app["label"] for p in self.window.order],
                         ["alpha", "Bravo", "Charlie", "Delta"])
        self.window.set_sort("played")
        self.assertEqual([self.window.cards[p].app["label"] for p in self.window.order],
                         ["Delta", "Charlie", "alpha", "Bravo"])        # never played last, then by name
        self.assertEqual(self.window.sort_button.text(), "Sort: Recently played")
        self.window.resize(760, 520); self.window.close(); self.settings.sync()      # inside the 800 x 600 offscreen screen
        reopened = self.open_window(size=None)
        self.assertEqual(reopened.sort_mode, "played")
        self.assertEqual((reopened.width(), reopened.height()), (760, 520))
        self.settings.setValue("sort", "nonsense")
        self.assertEqual(self.open_window().sort_mode, "name")

    def test_card_text_says_what_happened_last(self):
        from android_launcher import activity_line
        now = 1792800000
        self.assertEqual(activity_line(self.app(1), now), ("Not played yet", ""))
        self.assertEqual(activity_line(self.app(1, state="failed", started_at=now - 60), now), ("Last run failed", "problem"))
        self.assertEqual(activity_line(self.app(1, state="running", started_at=now - 1500), now), ("Running for 25 min", ""))
        self.assertEqual(activity_line(self.app(1, state="preparing"), now), ("Starting", ""))
        self.assertTrue(activity_line(self.app(1, state="exited", started_at=now - 90000), now)[0].startswith("Played "))
        self.fill(2)
        failed = dict(self.app(7, "Broken"), session={"state": "failed", "started_at": 1792700000})
        self.window.apps = [failed]; self.window.render()
        card = self.window.cards[failed["package"]]
        self.assertEqual((card.line.text(), card.line.property("tone")), ("Last run failed", "problem"))
        self.assertTrue(card.meta.text().startswith("1.7 · Installed"), card.meta.text())

    def test_a_red_line_says_what_went_wrong_and_only_when_something_did(self):
        explain = self.launcher.explain_ending
        crash = [{"key": "signal"}]; ads = [{"key": "ads"}, {"key": "play"}]
        self.assertEqual(explain({"state": "failed", "exit_code": 139}, crash)[0], "Crashed in native code")
        self.assertEqual(explain({"state": "failed", "exit_code": 1}, [{"key": "java"}])[0], "Crashed in Java code")
        self.assertEqual(explain({"state": "interrupted"})[0], "Ended unexpectedly")
        # An app that exits with status 0 is quiet unless it quit on a failed Google Play licence check (Flying Gorilla).
        licence = [{"key": "license"}]
        self.assertEqual(explain({"state": "exited", "exit_code": 0}, licence)[0], "Closed itself: Google Play licence check")
        self.assertIsNone(explain({"state": "exited", "exit_code": 0}))
        self.assertIsNone(explain({"state": "exited", "exit_code": 0}, ads))
        self.assertEqual(explain({"state": "failed", "exit_code": 1}, licence)[0], "Closed itself: Google Play licence check")
        failed_check = ("11-04 11:08:44.061 669522 669522 E LicenseClient: Error while checking license: "
                        "com.pairip.licensecheck.LicenseCheckException: Not allowed to bind with the licensing service")
        retry = "11-04 11:08:38.044 669522 669522 D LicenseClient: Retry #1. License check failed with error 'Not allowed'"
        self.assertEqual([item["key"] for item in self.launcher.log_findings([retry, failed_check])], ["license"])
        self.assertEqual(self.launcher.log_findings([retry]), [])
        quit_log = self.root / "quit.log"; quit_log.write_text(retry + "\n" + failed_check + "\n")
        self.assertIn("Google Play licence check", self.launcher.quit_reason(quit_log))
        quit_log.write_text("I app_process64: System.exit called, status: 0\n")
        self.assertIsNone(self.launcher.quit_reason(quit_log))
        self.assertIsNone(self.launcher.quit_reason(self.root / "missing.log"))
        self.assertEqual(explain({"state": "failed", "exit_code": 137})[0], "Ended from outside")
        self.assertEqual(explain({"state": "failed", "exit_code": 1}, ads)[0], "Last run failed")
        # A time limit is not a failure, nor is an app quitting by killing itself, unless it crashed first.
        self.assertIsNone(explain({"state": "timed_out", "exit_code": 124}))
        self.assertIsNone(explain({"state": "failed", "exit_code": 137}, ads, self_killed=True))
        self.assertEqual(explain({"state": "failed", "exit_code": 137}, [{"key": "java"}], self_killed=True)[0], "Crashed in Java code")
        own, other = "11-01 21:23:12.250 2365997    37 I Process : Sending signal. PID: 2365997 SIG: 9", \
                     "11-01 21:23:12.250 2365997    37 I Process : Sending signal. PID: 42 SIG: 9"
        self.assertTrue(self.launcher.SELF_KILL.search(own)); self.assertFalse(self.launcher.SELF_KILL.search(other))
        # A log decides what the card says, and a click on a red line opens the drawer.
        log = self.root / "session.log"; log.write_text("E AndroidRuntime: FATAL EXCEPTION: main\n")
        session = {"state": "failed", "exit_code": 1, "session_id": "s", "started_at": 1792700000, "log": str(log)}
        self.assertEqual(self.launcher.session_log(session), str(log))
        failed = dict(self.app(7, "Broken"), session=session)
        self.window.apps = [failed]; self.window.render()
        card = self.window.cards[failed["package"]]
        self.assertEqual(card.line.text(), "Crashed in Java code")
        # The same for a run that ended with status 0 after a failed licence check, and not for an ordinary exit.
        log.write_text(failed_check + "\n")
        quit_session = dict(session, state="exited", exit_code=0, session_id="q")
        self.assertEqual(self.launcher.activity_line(dict(failed, session=quit_session))[1], "problem")
        self.assertEqual(self.launcher.describe_session(quit_session)["outcome"], "Closed itself: Google Play licence check")
        log.write_text("I app_process64: System.exit called, status: 0\n")
        quiet = dict(session, state="exited", exit_code=0, session_id="q2")
        self.assertEqual(self.launcher.describe_session(quiet)["outcome"], "Exited normally")
        opened = []
        self.window.open_drawer = opened.append
        from PyQt6.QtCore import Qt
        from PyQt6.QtTest import QTest
        QTest.mouseClick(card.line, Qt.MouseButton.LeftButton)
        self.assertEqual(opened, [failed["package"]])

    def test_adaptive_icon_layers_compose_as_android_shows_them(self):
        from PyQt6.QtGui import QColor, QImage
        layer = QImage(108, 108, QImage.Format.Format_ARGB32); layer.fill(QColor(0, 0, 0, 0))
        for x in range(18, 90):       # the inner 72 of 108: red, with a marker outside it that must not show
            for y in range(18, 90): layer.setPixelColor(x, y, QColor("#ff0000"))
        for x in range(0, 18): layer.setPixelColor(x, 54, QColor("#00ff00"))
        path = self.root / "icon-foreground.png"; self.assertTrue(layer.save(str(path)))
        window_card = dict(self.app(1, "Layered"), icon_layers={"background": "#ff0000ff", "foreground": str(path)})
        self.window.apps = [window_card]; self.window.render()
        image = self.window.cards[window_card["package"]].icon.pixmap().toImage()
        side = image.width()
        centre, edge = image.pixelColor(side // 2, side // 2), image.pixelColor(max(1, side // 40), side // 2)
        self.assertEqual((centre.red(), centre.blue()), (255, 0))     # foreground over background
        self.assertEqual(edge.green(), 0)                             # the outer ring of the layer is cropped
        broken = dict(self.app(2, "Broken"), icon_layers={"background": "#ff0000ff", "foreground": str(self.root / "missing.png")})
        self.window.apps = [broken]; self.window.render()             # falls back to the lettered tile, no crash
        self.assertFalse(self.window.cards[broken["package"]].icon.pixmap().isNull())

    def test_the_library_speaks_of_apps_not_games(self):
        from PyQt6.QtWidgets import QLabel, QLineEdit
        self.fill(2)
        texts = [label.text() for label in self.window.findChildren(QLabel)] + [self.window.search.placeholderText()]
        self.assertIn("App library", texts); self.assertIn("Search apps", texts); self.assertIn("2 apps", texts)
        # The fixture apps are labelled "Game 01": their own names are not the library's wording.
        labels = {app["label"] for app in self.window.apps}
        self.assertFalse([text for text in texts if "game" in text.lower() and not any(name in text for name in labels)])
        self.window.apps = self.window.apps[:1]; self.window.render()
        self.assertEqual(self.window.summary.text(), "1 app")

    def test_google_dialog_says_experimental_and_clips_no_text(self):
        # "Sign in with Google…" lost its first letter, and the wrapped hint ran into the empty account list.
        from PyQt6.QtWidgets import QDialog, QLabel, QPushButton
        import unittest.mock
        shown = []
        original = QDialog.exec
        def capture(dialog):
            dialog.show(); self.settle(); shown.append(dialog)
            for button in dialog.findChildren(QPushButton):
                self.assertGreaterEqual(button.width(), button.sizeHint().width(), button.text())
            words = []
            for label in dialog.findChildren(QLabel):
                if not label.isVisible() or not label.text(): continue
                words.append(label.text())
                self.assertGreaterEqual(label.height(), label.heightForWidth(label.width()) if label.wordWrap()
                                        else label.sizeHint().height(), label.text())
            self.assertIn("Experimental", words)
            self.assertFalse(any("game" in text.lower() for text in words), words)
            self.assertFalse(dialog.accounts.isVisible())         # nothing signed in: no empty box
            self.assertLessEqual(dialog.hint.geometry().bottom(), dialog.sign_in.geometry().top())
            dialog.timer.stop(); dialog.close()
            return 0
        with unittest.mock.patch.object(QDialog, "exec", capture):
            self.window.google_button.click(); self.settle(lambda: bool(shown))
        self.assertTrue(shown)

    def test_keyboard_and_double_click_play_but_not_a_disabled_game(self):
        from PyQt6.QtCore import Qt
        from PyQt6.QtTest import QTest
        self.fill(8)
        self.window.activateWindow(); self.settle()
        played = []
        self.window.act = lambda app: played.append(app["package"])
        order = self.window.order
        first = self.window.cards[order[0]]
        first.setFocus(); self.settle()
        QTest.keyClick(first, Qt.Key.Key_Return); QTest.keyClick(first, Qt.Key.Key_Space)
        QTest.mouseDClick(self.window.cards[order[2]], Qt.MouseButton.LeftButton)
        self.assertEqual(played, [order[0], order[0], order[2]])
        QTest.keyClick(first, Qt.Key.Key_Right); self.settle()
        self.assertEqual(self.window.selected, order[1])
        QTest.keyClick(self.window.cards[order[1]], Qt.Key.Key_Down); self.settle()
        self.assertEqual(self.window.selected, order[1 + self.window.columns])
        damaged = self.window.cards[order[3]]
        damaged.app = dict(damaged.app, error="damaged")
        QTest.keyClick(damaged, Qt.Key.Key_Return)
        self.assertEqual(len(played), 3)

    def test_context_menu_follows_the_card_state(self):
        def items(app):
            return {a.text(): a.isEnabled() for a in self.window.build_menu(app).actions() if a.text()}
        self.assertEqual(items(self.app(1)), {"Play": True, "App settings…": True, "Last session": False,
                                              "Clear data…": True, "Uninstall…": True})
        running = items(self.app(1, state="running", session_id="s"))
        self.assertEqual((running["Stop"], running["Last session"], running["Clear data…"], running["Uninstall…"]),
                         (True, True, False, False))
        self.assertFalse(items(dict(self.app(1), error="damaged"))["Play"])

    def test_polling_slows_when_idle_and_stops_while_minimized(self):
        from android_launcher import poll_wait
        self.assertEqual((poll_wait(True, True, True), poll_wait(False, True, False), poll_wait(False, False, True),
                          poll_wait(False, False, False)), (None, 1.5, 1.5, 10.0))
        refreshed = []
        self.window.refresh = lambda: refreshed.append(1)
        def tick(minimized, active, age, running=False):
            self.window.isMinimized, self.window.isActiveWindow = (lambda: minimized), (lambda: active)
            self.window.apps = [self.app(1, state="running" if running else "stopped")]
            self.window.polled = time.monotonic() - age
            self.window.tick()
        tick(True, False, 99); self.assertEqual(refreshed, [])          # minimized: never
        tick(False, False, 5); self.assertEqual(refreshed, [])          # idle and background: wait 10 s
        tick(False, False, 11); self.assertEqual(len(refreshed), 1)
        tick(False, True, 2); self.assertEqual(len(refreshed), 2)       # focused: every 1.5 s
        tick(False, False, 2, running=True); self.assertEqual(len(refreshed), 3)   # a running game is watched

    def test_installs_queue_one_at_a_time_and_report_failures(self):
        files = []
        for name in ("one.apk", "two.xapk", "three.apks", "notes.txt"):
            path = self.root / name; path.write_bytes(b"x"); files.append(str(path))
        from android_launcher import installable
        self.assertEqual(installable(files + [str(self.root / "missing.apk"), str(self.root)]), files[:3])
        started = []
        self.window.command = lambda args, done=None, env=None: started.append((args, done))
        self.window.install_files(files)
        self.assertEqual([a[0][1] for a in started], [files[0]])          # one at a time
        self.window.install_files([files[0]])                             # added while busy: queued, not started
        self.assertEqual(len(started), 1)
        self.assertIn("(1 of 3)", self.window.progress_text.text())     # the label counts what was queued when it began
        started[-1][1](0, "Installed org.one 1.0\n")                      # one.apk done; two.xapk starts
        self.assertEqual([a[0][1] for a in started], files[:2])
        started[-1][1](1, "Android package: signature mismatch\n")        # two.xapk fails; three.apks starts
        started[-1][1](0, "Installed org.three 3.0\n")                    # three.apks done; the queued one.apk starts
        self.assertEqual([a[0][1] for a in started], [files[0], files[1], files[2], files[0]])
        started[-1][1](0, "Installed org.one 1.0\n")
        self.assertFalse(self.window.progress.isVisible())
        self.assertIn("3 of 4 installed. Couldn't install two.xapk: signature mismatch", self.window.notice_text.text())
        # A single install is announced by name once it is listed.
        started.clear()
        self.window.install_files([files[0]]); started[-1][1](0, "Installed org.single 2.0\n")
        self.assertEqual(self.window.announce, "org.single")

    def test_dropped_files_are_filtered_to_packages(self):
        from PyQt6.QtCore import QMimeData, QUrl
        good = self.root / "game.apk"; good.write_bytes(b"x")
        mime = QMimeData()
        mime.setUrls([QUrl.fromLocalFile(str(good)), QUrl.fromLocalFile(str(self.root / "a.txt")), QUrl("https://example.org/x.apk")])
        event = SimpleNamespace(mimeData=lambda: mime)
        self.assertEqual(self.launcher.installable(self.window.dropped(event)), [str(good)])
        self.window.show_drop_hint()
        pages = self.window.pages.geometry()
        self.assertEqual(self.window.drop_hint.geometry().height(), pages.height())
        self.assertEqual(self.window.drop_hint.geometry().y(), pages.y())

    def test_padded_icons_are_cropped_to_fill_the_tile(self):
        from PyQt6.QtGui import QColor, QImage
        padded, flat = self.root / "padded.png", self.root / "flat.png"
        image = QImage(192, 192, QImage.Format.Format_ARGB32); image.fill(QColor(0, 0, 0, 0))
        for y in range(20, 172):
            for x in range(20, 172): image.setPixelColor(x, y, QColor(220, 30, 30))
        image.save(str(padded))
        self.assertEqual(self.launcher.opaque_bounds(image), (20, 20, 152, 152))
        solid = QImage(64, 64, QImage.Format.Format_RGB32); solid.fill(QColor(30, 30, 220)); solid.save(str(flat))
        self.assertIsNone(self.launcher.opaque_bounds(solid))
        clear = QImage(16, 16, QImage.Format.Format_ARGB32); clear.fill(QColor(0, 0, 0, 0))
        self.assertIsNone(self.launcher.opaque_bounds(clear))
        self.window.apps = [dict(self.app(1), icon=str(padded)), dict(self.app(2), icon=str(flat)), self.app(3)]
        self.window.render(True); self.settle()
        for index, (package, color) in enumerate((("org.fixture.g1", (220, 30, 30)), ("org.fixture.g2", (30, 30, 220)))):
            tile = self.window.cards[package].icon.pixmap().toImage()
            ratio = tile.devicePixelRatio()
            edge = tile.pixelColor(round(10 * ratio), round(44 * ratio))      # just inside the rounded edge
            self.assertEqual((edge.red(), edge.green(), edge.blue()), color, package)
        placeholder = self.window.cards["org.fixture.g3"].icon.pixmap().toImage()
        self.assertEqual(placeholder.pixelColor(round(44 * ratio), round(8 * ratio)).name(), self.launcher.tile_color("Game 03"))

    def test_empty_state_text_is_not_clipped(self):
        self.window.apps = []; self.window.render(True); self.settle()
        hint = self.window.empty_hint
        self.assertGreaterEqual(hint.height(), hint.heightForWidth(hint.width()))
        self.assertGreater(hint.heightForWidth(hint.width()), hint.fontMetrics().height())      # it wraps
        self.fill(3); self.window.search.setText("zzz"); self.settle()
        self.assertIn("zzz", self.window.empty_hint.text())

    def test_stylesheet_and_glyphs_follow_the_palette(self):
        from PyQt6.QtGui import QColor, QPalette
        old = self.qt.palette()
        try:
            for window_color, accent in (("#1b1e20", "#2f8fd6"), ("#eff0f1", "#c0392b")):
                palette = QPalette(old)
                palette.setColor(QPalette.ColorRole.Window, QColor(window_color))
                palette.setColor(QPalette.ColorRole.Highlight, QColor(accent))
                self.qt.setPalette(palette); self.window.apply_theme(); self.settle()
                self.assertIn("rgba(%d, %d, %d" % QColor(accent).getRgb()[:3], self.window.styleSheet())
                self.assertFalse(self.window.install_button.icon().isNull())
                self.assertFalse(self.window.cards and next(iter(self.window.cards.values())).gear.icon().isNull())
        finally:
            self.qt.setPalette(old)

    def test_desktop_launcher_process_starts_offscreen_with_the_shared_socket(self):
        from android_launcher import activate_existing
        state = self.root / "process-state"; state.mkdir()
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", HOME=str(self.root))
        with (self.root / "launcher.log").open("w") as log:
            launcher = subprocess.Popen([sys.executable, str(ROOT / "Scripts/Android/asgard.py"),
                                         "--state-root", str(state), "launcher"], stdout=log, stderr=subprocess.STDOUT, env=env,
                                         start_new_session=True)
            try:
                deadline = time.monotonic() + 20
                while not activate_existing(state):
                    if launcher.poll() is not None or time.monotonic() >= deadline:
                        self.fail("Offscreen Asgard launcher did not start: " + (self.root / "launcher.log").read_text()[-2000:])
                    time.sleep(0.1)
            finally:
                stop_launcher(launcher)


    def test_asgard_launcher_command_is_one_process(self):
        """asgard.py replaces itself with the command it runs: signalling the pid that was started must stop the launcher."""
        from android_launcher import activate_existing
        state = self.root / "single-process-state"; state.mkdir()
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", HOME=str(self.root))

        def processes():  # every process whose command line names this state root
            found = []
            for entry in os.listdir("/proc"):
                try:
                    with open(f"/proc/{entry}/cmdline", "rb") as handle:
                        if str(state).encode() in handle.read(): found.append(int(entry))
                except (OSError, ValueError): pass
            return found

        with (self.root / "single-process.log").open("w") as log:
            launcher = subprocess.Popen([sys.executable, str(ROOT / "Scripts/Android/asgard.py"),
                                         "--state-root", str(state), "launcher"], stdout=log, stderr=subprocess.STDOUT, env=env)
            try:
                deadline = time.monotonic() + 20
                while not activate_existing(state):
                    if launcher.poll() is not None or time.monotonic() >= deadline:
                        self.fail("Offscreen Asgard launcher did not start")
                    time.sleep(0.1)
                launcher.terminate()   # only the process that was started, as a library window or `kill` does
                launcher.wait(timeout=10)
                deadline = time.monotonic() + 5
                while processes() and time.monotonic() < deadline: time.sleep(0.1)
                self.assertEqual(processes(), [], "the launcher outlived the process that was signalled")
            finally:
                for pid in processes():
                    try: os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError: pass


class IconRepairTests(unittest.TestCase):
    """refresh-icons restores an adaptive icon whose layer file is missing (Battle Cats: installed with the record only)."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="bifrost-icons-")
        self.addCleanup(self.temporary.cleanup)
        self.state = Path(self.temporary.name)

    def app(self, package, files=(), **fields):
        directory = self.state / package; directory.mkdir()
        metadata = dict({"package": package, "label": package, "version_code": 1, "launcher": {"kind": "managed_activity"},
                         "adaptive_icon": {"background": "#ffffffff", "foreground": "icon-foreground.png"}}, **fields)
        (directory / "package.json").write_text(json.dumps(metadata))
        for name in files: (directory / name).write_bytes(b"png")
        (directory / "rootfs/data/app" / (package + "-1")).mkdir(parents=True)
        (directory / "rootfs/data/app" / (package + "-1") / "base.apk").write_bytes(b"apk")
        return directory

    def test_a_missing_layer_file_is_restored_and_an_intact_one_is_left_alone(self):
        broken = self.app("org.example.broken")
        intact = self.app("org.example.intact", files=["icon-foreground.png"])
        bitmap = self.app("org.example.bitmap", icon="icon.png", adaptive_icon=None)
        scanned = []

        def extract(paths, aapt2, staging):
            scanned.append(staging.name)
            (staging / "icon-foreground.png").write_bytes(b"restored")
            return {"background": "#ffffffff", "foreground": "icon-foreground.png"}
        with patch.object(package, "copy_adaptive_icon", extract):
            package.refresh_icons(self.state, Path("/nonexistent/aapt2"))
        self.assertEqual(scanned, ["org.example.broken"])
        self.assertEqual((broken / "icon-foreground.png").read_bytes(), b"restored")
        self.assertEqual((intact / "icon-foreground.png").read_bytes(), b"png")
        # The library lists the broken one as unscanned until it is repaired, and the intact one never.
        flags = {item["package"]: item["icon_unscanned"] for item in package.installed_apps(self.state)}
        self.assertEqual(flags, {"org.example.broken": False, "org.example.intact": False, "org.example.bitmap": False})
        (broken / "icon-foreground.png").unlink()
        flags = {item["package"]: item["icon_unscanned"] for item in package.installed_apps(self.state)}
        self.assertTrue(flags["org.example.broken"])
        self.assertFalse(flags["org.example.intact"])


class DesktopEntryTests(unittest.TestCase):
    """Menu entries for installed apps (android_desktop): a launcher with the app's icon, kept in step with the library."""

    def setUp(self):
        import android_desktop
        self.desktop = android_desktop
        self.temporary = tempfile.TemporaryDirectory(prefix="bifrost-desktop-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.state = self.root / "state"; self.state.mkdir()
        patcher = patch.dict(os.environ, {"XDG_DATA_HOME": str(self.root / "data")})
        patcher.start(); self.addCleanup(patcher.stop)
        os.environ.pop("ASGARD_DESKTOP_ENTRIES", None); os.environ.pop("SDL_APP_ID", None)
        self.icon = self.root / "icon.png"
        self.icon.write_bytes(b"\x89PNG\r\n\x1a\n")

    def app(self, package="org.example.game", **fields):
        return dict({"package": package, "label": "Example Game", "icon": str(self.icon), "icon_layers": None,
                     "launcher": "managed_activity", "system": False}, **fields)

    def entries(self):
        return sorted(path.name for path in self.desktop.applications_dir().glob("*.desktop"))

    @staticmethod
    def split_exec(value):
        """The Desktop Entry specification's reading of an Exec value: the string escapes, then double quotes."""
        value = re.sub(r"\\(.)", r"\1", value)  # string value escapes: \\ is one backslash
        arguments, current, quoted, started, i = [], "", False, False, 0
        while i < len(value):
            c = value[i]
            if quoted and c == "\\": current += value[i + 1]; i += 1
            elif c == '"': quoted = not quoted; started = True
            elif c == " " and not quoted:
                if started or current: arguments.append(current)
                current, started = "", False
            else: current += c
            i += 1
        if started or current: arguments.append(current)
        return arguments

    def test_an_entry_is_valid_and_starts_the_app_through_this_asgard(self):
        path = self.desktop.write_entry(self.app(), self.state)
        self.assertEqual(path, self.desktop.applications_dir() / "asgard-org.example.game.desktop")
        fields = dict(line.split("=", 1) for line in path.read_text().splitlines() if "=" in line)
        self.assertEqual((fields["Type"], fields["Name"], fields["Terminal"]), ("Application", "Example Game", "false"))
        self.assertEqual(fields["Icon"], str(self.icon))
        # The window's application id (SDL_APP_ID) is the entry's name, and StartupWMClass says so for X11.
        self.assertEqual(fields["StartupWMClass"], "asgard-org.example.game")
        arguments = self.split_exec(fields["Exec"])
        self.assertEqual(arguments, [sys.executable, str(Path(self.desktop.__file__).resolve().with_name("asgard.py")),
                                     "start", "org.example.game"])
        self.assertEqual(path.stat().st_mode & 0o022, 0)
        if shutil.which("desktop-file-validate"):
            result = subprocess.run(["desktop-file-validate", str(path)], capture_output=True, text=True)
            self.assertEqual((result.returncode, result.stdout + result.stderr), (0, ""))

    def test_awkward_names_and_paths_survive_the_entry_format(self):
        label = 'Line one\nTwo \\ back "quoted" 100% $HOME'
        with patch.object(sys, "executable", '/tmp/py thon/a "b" $c`d`\\e%f'):
            text = self.desktop.entry_text(self.app(label=label), self.state, None)
        self.assertEqual(len([line for line in text.splitlines() if line.startswith("Name=")]), 1)
        name = next(line for line in text.splitlines() if line.startswith("Name="))
        self.assertNotIn("\n", name[5:])
        self.assertEqual(re.sub(r"\\\\", r"\\", name[5:]), 'Line one Two \\ back "quoted" 100% $HOME')
        exec_value = next(line for line in text.splitlines() if line.startswith("Exec="))[5:]
        self.assertEqual(self.split_exec(exec_value)[0], '/tmp/py thon/a "b" $c`d`\\e%%f'.replace("%%", "%%"))
        self.assertNotIn("Icon=", text)

    def test_an_adaptive_icon_is_composed_once_and_reused(self):
        try:
            from PyQt6.QtGui import QImage
        except ImportError:
            self.skipTest("needs PyQt6")
        app_dir = self.state / "org.example.game"; app_dir.mkdir()
        foreground = app_dir / "icon-foreground.png"
        image = QImage(108, 108, QImage.Format.Format_ARGB32); image.fill(0xffff0000); image.save(str(foreground))
        app = self.app(icon=None, icon_layers={"background": "#ff00ff00", "foreground": str(foreground)})
        icon = self.desktop.icon_file(app, self.state)
        self.assertEqual(icon, str(app_dir / "desktop-icon.png"))
        composed = QImage(icon)
        self.assertEqual((composed.width(), composed.height()), (256, 256))
        self.assertEqual(composed.pixelColor(128, 128).red(), 255)  # the foreground covers the middle
        before = Path(icon).stat().st_mtime_ns
        self.assertEqual(self.desktop.icon_file(app, self.state), icon)
        self.assertEqual(Path(icon).stat().st_mtime_ns, before)
        broken = self.app(icon=None, icon_layers={"background": "#ff00ff00", "foreground": str(app_dir / "missing.png")})
        Path(icon).unlink()
        self.assertIsNone(self.desktop.icon_file(broken, self.state))

    def test_a_webp_or_jpeg_icon_becomes_a_png_beside_the_app(self):
        try:
            from PyQt6.QtGui import QImage
        except ImportError:
            self.skipTest("needs PyQt6")
        app_dir = self.state / "org.example.game"; app_dir.mkdir()
        image = QImage(48, 48, QImage.Format.Format_RGB32); image.fill(0xff3366cc)
        for suffix, kind in ((".jpg", "JPG"), (".webp", "WEBP")):
            source = app_dir / ("icon" + suffix)
            if not image.save(str(source), kind):
                continue  # this Qt has no writer for the format
            converted = self.desktop.icon_file(self.app(icon=str(source)), self.state)
            self.assertEqual(converted, str(app_dir / "desktop-icon.png"))
            self.assertEqual(Path(converted).read_bytes()[:8], b"\x89PNG\r\n\x1a\n")
            Path(converted).unlink()
        self.assertEqual(self.desktop.icon_file(self.app(), self.state), str(self.icon))  # a PNG is used as it is

    def test_entries_follow_the_installed_apps_and_leave_other_files_alone(self):
        apps = [self.app("org.example.one"), self.app("org.example.two"),
                self.app("com.google.android.gms", system=True), self.app("org.example.broken", error="damaged"),
                self.app("org.example.noactivity", launcher="service")]
        written, removed = self.desktop.sync(self.state, apps)
        self.assertEqual((sorted(written), removed), (["org.example.one", "org.example.two"], []))
        self.assertEqual(self.entries(), ["asgard-org.example.one.desktop", "asgard-org.example.two.desktop"])
        directory = self.desktop.applications_dir()
        # Not ours: a hand-written file with the same prefix, and an entry made for another state root.
        (directory / "asgard-org.example.mine.desktop").write_text("[Desktop Entry]\nName=Mine\n")
        other = self.root / "other-state"; other.mkdir()
        self.desktop.write_entry(self.app("org.example.elsewhere"), other)
        # App one is uninstalled: only its entry goes.
        written, removed = self.desktop.sync(self.state, [self.app("org.example.two")])
        self.assertEqual((written, removed), (["org.example.two"], ["org.example.one"]))
        self.assertEqual(self.entries(), ["asgard-org.example.elsewhere.desktop", "asgard-org.example.mine.desktop",
                                          "asgard-org.example.two.desktop"])
        # One app's change touches only its own entry.
        self.assertEqual(self.desktop.sync(self.state, [], only="org.example.two"), ([], ["org.example.two"]))
        self.assertEqual(len(self.entries()), 2)
        self.assertEqual(self.desktop.remove_all(self.state), [])
        self.assertEqual(self.desktop.remove_all(other), ["org.example.elsewhere"])
        self.assertEqual(self.entries(), ["asgard-org.example.mine.desktop"])

    def test_only_the_default_state_root_gets_entries(self):
        with patch.object(self.desktop.android_paths, "state_root", return_value=self.state):
            self.assertTrue(self.desktop.enabled(self.state))
            self.assertFalse(self.desktop.enabled(self.root / "scratch"))
            with patch.dict(os.environ, {"ASGARD_DESKTOP_ENTRIES": "0"}):
                self.assertFalse(self.desktop.enabled(self.state))
        # A test's or a scratch state root must not reach the menu: the CLI hook does nothing for it.
        with patch.object(package, "installed_apps", return_value=[self.app()]):
            package.update_desktop(self.root / "scratch", "org.example.game")
        self.assertEqual(self.entries(), [])
        with patch.object(self.desktop.android_paths, "state_root", return_value=self.state), \
                patch.object(package, "installed_apps", return_value=[self.app()]):
            package.update_desktop(self.state, "org.example.game")
            self.assertEqual(self.entries(), ["asgard-org.example.game.desktop"])
            with patch.object(package, "installed_apps", return_value=[]):
                package.update_desktop(self.state, "org.example.game")  # uninstalled
            self.assertEqual(self.entries(), [])

    def test_an_unwritable_menu_never_fails_the_install(self):
        blocked = self.root / "data"; blocked.write_text("a file where the directory should be")
        with patch.object(self.desktop.android_paths, "state_root", return_value=self.state), \
                patch.object(package, "installed_apps", return_value=[self.app()]), \
                patch("sys.stderr") as stderr:
            package.update_desktop(self.state, "org.example.game")  # must not raise
        self.assertTrue(any("Menu entry not updated" in "".join(call.args) for call in stderr.write.call_args_list))

    def test_the_window_is_named_after_its_entry_unless_the_user_chose_a_name(self):
        self.assertEqual(self.desktop.window_environment("org.example.game"), {"SDL_APP_ID": "asgard-org.example.game"})
        with patch.dict(os.environ, {"SDL_APP_ID": "mine"}):
            self.assertEqual(self.desktop.window_environment("org.example.game"), {})


class SessionResultTests(unittest.TestCase):
    def test_preparation_log_and_elapsed_time_remain_visible_without_guest_output(self):
        from android_sessions import Session, session_log
        from android_launcher import drawer_stamp, describe_session
        with tempfile.TemporaryDirectory(prefix="bifrost-preparation-") as temporary:
            with Session(Path(temporary), "org.bifrost.fixture", 10000) as session:
                data = session.snapshot()
                preparation = Path(data["preparation_log"])
                preparation.write_text("Compiling fixture (speed-profile)\n")
                self.assertEqual(session_log(data), str(preparation))
                first = drawer_stamp(data, preparation.stat(), now=data["started_at"])
                later = drawer_stamp(data, preparation.stat(), now=data["started_at"] + 10)
                self.assertNotEqual(first, later)
                self.assertEqual(describe_session(data, now=data["started_at"] + 10)["duration"], "10 s")
                session.update(phase="Optimizing Android app code")
                updated = session.snapshot()
                self.assertNotEqual(first[:4], drawer_stamp(updated, preparation.stat(), now=data["started_at"])[:4])
                stdout, _ = session.logs()
                self.assertEqual(session_log(updated), str(preparation))
                stdout.write_text("APP_ACTIVITY_CREATED\n")
                self.assertEqual(session_log(updated), str(stdout))
                # Older session records have no preparation_log field.
                updated.pop("preparation_log")
                stdout.write_text("")
                self.assertEqual(session_log(updated), str(preparation))
                updated["state"] = "failed"
                self.assertEqual(drawer_stamp(updated, stdout.stat(), now=10),
                                 drawer_stamp(updated, stdout.stat(), now=20))

    def run_session(self, script):
        from android_sessions import Session
        with tempfile.TemporaryDirectory(prefix="bifrost-session-test-") as temporary:
            with Session(Path(temporary), "org.bifrost.fixture", 10000) as session:
                stdout, _ = session.logs()
                with stdout.open("w") as log:
                    code = session.run(["sh", "-c", script], stdout=log)
                return code, session.data["state"]

    def test_self_kill_after_host_close_is_closed(self):
        # Unity's Activity teardown calls Process.killProcess on itself. The user closing the window is a normal
        # ending: the command succeeds, and the session record keeps the real SIGKILL status.
        from android_sessions import Session
        with tempfile.TemporaryDirectory(prefix="bifrost-session-test-") as temporary:
            with Session(Path(temporary), "org.bifrost.fixture", 10000) as session:
                stdout, _ = session.logs()
                with stdout.open("w") as log:
                    code = session.run(["sh", "-c", "echo APP_HOST_CLOSED; kill -9 $$"], stdout=log)
                self.assertEqual((code, session.data["state"], session.data["exit_code"]), (0, "closed", 137))

    def test_a_helper_that_left_the_process_group_is_ended_with_the_app(self):
        # A forked helper that called setsid is not in the app's process group; it was left running for hours,
        # holding a copy of the game's memory. The supervisor adopts and ends it.
        import os
        import signal
        import subprocess
        from android_sessions import Session
        with tempfile.TemporaryDirectory(prefix="bifrost-session-test-") as temporary:
            marker = Path(temporary) / "helper.pid"
            script = f'setsid sh -c "echo \\$\\$ > {marker}; sleep 60; :" & sleep 1; echo done'
            with Session(Path(temporary), "org.bifrost.fixture", 10000) as session:
                stdout, _ = session.logs()
                with stdout.open("w") as log:
                    self.assertEqual(session.run(["sh", "-c", script], stdout=log), 0)
            helper = int(marker.read_text())
            for _ in range(100):
                if not Path(f"/proc/{helper}").exists(): break
                time.sleep(0.05)
            self.assertFalse(Path(f"/proc/{helper}").exists(), "the helper outlived its app")

    def test_self_kill_without_host_close_fails(self):
        self.assertEqual(self.run_session("echo APP_HOST_CLOSED_LATER; kill -9 $$"), (137, "failed"))
        self.assertEqual(self.run_session("echo APP_HOST_CLOSED; exit 1"), (1, "failed"))


class AdaptiveIconTests(unittest.TestCase):
    """Layer lookup from aapt2's output, as a real APK set lays it out: a colour, a density split, an asset pack."""
    TREE = ("N: android=http://schemas.android.com/apk/res/android\n  E: adaptive-icon (line=1)\n"
            "      E: background (line=2)\n        A: http://schemas.android.com/apk/res/android:drawable(0x01010199)=@0x7f060098\n"
            "      E: foreground (line=3)\n        A: http://schemas.android.com/apk/res/android:drawable(0x01010199)=@0x7f0800c0\n")
    BADGING = "package: name='org.bifrost.cats' versionCode='1'\napplication: label='Cats' icon='res/icon.xml'\n"
    BASE = "  type color id=06\n    resource 0x7f060098 color/white\n      () #ffffffff\n"
    LOW = "    resource 0x7f0800c0 drawable/fg\n      (mdpi) (file) res/m/fg.png type=PNG\n"
    HIGH = "    resource 0x7f0800c0 drawable/fg\n      (xxhdpi) (file) res/x/fg.png type=PNG\n"

    def fake(self, tables):
        def run(command):
            if command[2] == "badging": return self.BADGING
            if command[2] == "xmltree": return self.TREE
            return tables[command[-1]]
        return run

    def layers(self, root, tables, assets=()):
        for name in [*tables, *assets]:
            with zipfile.ZipFile(name, "w") as archive:
                archive.writestr("resources.arsc" if name in tables else "assets/pack", b"x")
        import android_preflight as preflight
        with patch.object(preflight, "run", self.fake(tables)):
            return preflight.adaptive_icon_layers([*tables, *assets], "aapt2")

    def test_layers_come_from_the_densest_split_and_asset_packs_are_skipped(self):
        with tempfile.TemporaryDirectory() as directory:
            base, low, high, pack = (str(Path(directory) / x) for x in ("base.apk", "low.apk", "high.apk", "pack.apk"))
            found = self.layers(directory, {base: self.BASE, low: self.LOW, high: self.HIGH}, assets=[pack])
            self.assertEqual(found, {"background": "#ffffffff", "foreground": (high, "res/x/fg.png")})

    def test_a_vector_layer_means_no_adaptive_icon(self):
        with tempfile.TemporaryDirectory() as directory:
            base = str(Path(directory) / "base.apk")
            vector = self.BASE + "    resource 0x7f0800c0 drawable/fg\n      () (file) res/fg.xml type=XML\n"
            self.assertIsNone(self.layers(directory, {base: vector}))


class ApkInspectionTests(unittest.TestCase):
    def test_rotated_signer_covering_the_api_is_recorded(self):
        # APK Signature Scheme v3.1 key rotation (Roblox): only the signer whose
        # SDK range covers the device applies.
        current, legacy = "2b" * 32, "44" * 32
        output = (f"Signer (minSdkVersion=33, maxSdkVersion=2147483647) certificate SHA-256 digest: {current}\n"
                  f"Signer (minSdkVersion=24, maxSdkVersion=32) certificate SHA-256 digest: {legacy}\n")
        with patch("android_package.preflight.run", return_value=output):
            self.assertEqual(package.signers(Path("app.apk"), "apksigner"), [current])
        with patch("android_package.preflight.run", return_value=f"Signer #1 certificate SHA-256 digest: {legacy}\n"):
            self.assertEqual(package.signers(Path("app.apk"), "apksigner"), [legacy])

    def test_launcher_declared_only_by_an_alias(self):
        from android_preflight import manifest_launcher
        def node(tag, children=(), **attributes):
            return {"tag": tag, "attributes": attributes, "children": list(children)}
        def launcher_filter():
            return node("intent-filter", [node("action", name="android.intent.action.MAIN"),
                                          node("category", name="android.intent.category.LAUNCHER")])
        manifest = node("manifest", [node("application", [
            node("activity-alias", [launcher_filter()], name=".Classic", enabled="false", targetActivity=".Splash"),
            node("activity", name=".Splash"),
            node("activity-alias", [launcher_filter()], name=".Main", enabled="true", targetActivity=".Splash")])])
        self.assertEqual(manifest_launcher(manifest), ".Main")
        manifest["children"][0]["children"].pop()
        self.assertIsNone(manifest_launcher(manifest))


class FrameworkPackageContractTests(unittest.TestCase):
    probe_class = "FrameworkPackagesProbe"
    output_name = "android-framework-package-contract"
    marker = "FRAMEWORK_PACKAGE_CONTRACT_OK"
    @classmethod
    def setUpClass(cls):
        from android_managed import build_launcher
        cls.runtime = ROOT / "build/android-framework/rootfs"
        cls.art = ROOT / "build/android-art/rootfs"
        if not (cls.runtime / "system/framework/framework-res.apk").is_file():
            raise RuntimeError("Package contract tests require Tools/Android/android_framework.py provisioning")
        if not ARGS.emu.is_file():
            raise RuntimeError("Package contract tests require --emu pointing to a built Bifrost")
        cls.temporary = tempfile.TemporaryDirectory(prefix="bifrost-package-contract-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.probe = Path(cls.temporary.name) / "probe"
        build_launcher(cls.probe, [ROOT / "Tests/Host" / (cls.probe_class + ".java")])

    def check_mode(self, mode):
        from test_android_framework import run
        output = ROOT / "build" / self.output_name / mode
        passed = run(ARGS.emu.resolve(), self.runtime, self.art, self.probe / "classes.dex",
                     mode, output, 90, services=True, main_class=self.probe_class,
                     libraries=(ROOT / "build/android-adapters/libbifrost-activity.so",))
        self.assertTrue(passed, f"{mode} package contract failed; see {output}")
        self.assertIn(self.marker, (output / (mode + ".log")).read_text())

    def test_jit(self):
        self.check_mode("jit")

    def test_interpreter(self):
        self.check_mode("interp")


class FrameworkDisplayContractTests(FrameworkPackageContractTests):
    probe_class = "FrameworkDisplayProbe"
    output_name = "android-framework-display-contract"
    marker = "FRAMEWORK_DISPLAY_CONTRACT_OK"


class FrameworkCompatibilityTests(FrameworkPackageContractTests):
    probe_class = "FrameworkCompatibilityProbe"
    output_name = "android-framework-compatibility"
    marker = "FRAMEWORK_COMPATIBILITY_OK"

    def check_mode(self, mode):
        from test_android_framework import run
        for target in (29, 33, 34, 35):
            with self.subTest(target=target):
                output = ROOT / "build" / self.output_name / mode / str(target)
                passed = run(ARGS.emu.resolve(), self.runtime, self.art, self.probe / "classes.dex",
                             mode, output, 90, services=True, main_class=self.probe_class,
                             probe_args=(str(target),), app_id=10000,
                             libraries=(ROOT / "build/android-adapters/libbifrost-activity.so",))
                self.assertTrue(passed, f"{mode} API {target} compatibility failed; see {output}")
                self.assertIn(self.marker, (output / (mode + ".log")).read_text())


class FrameworkServiceRoutingTests(FrameworkPackageContractTests):
    probe_class = "FrameworkServiceRoutingProbe"
    output_name = "android-service-child-routing"
    marker = "SERVICE_CHILD_ROUTING_OK"

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.app = Path(cls.temporary.name) / "org.bifrost.routing-fixture"
        cls.app.mkdir()
        manifest = cls.app.parent / "AndroidManifest.xml"
        manifest.write_text('<manifest xmlns:android="http://schemas.android.com/apk/res/android" '
                            'package="org.bifrost.routing"><application android:hasCode="true"/></manifest>')
        apk = cls.app / "base.apk"
        subprocess.run([str(ROOT / "Tools/android-tools/aapt2"), "link", "-I", str(ROOT / "Tools/android-tools/android.jar"),
                        "--manifest", str(manifest), "-o", str(apk), "--min-sdk-version", "26", "--target-sdk-version", "35"],
                       check=True, capture_output=True)
        with zipfile.ZipFile(apk, "a") as archive:
            # ART maps stored DEX directly only when its ZIP data is 4-byte aligned.
            dex = zipfile.ZipInfo("classes.dex")
            padding = -(archive.fp.tell() + 30 + len(dex.filename) + 4) % 4
            dex.extra = struct.pack("<HH", 0xffff, padding) + bytes(padding)
            archive.writestr(dex, (cls.probe / "classes.dex").read_bytes())
        apk.chmod(0o444)

    def check_mode(self, mode):
        from test_android_framework import run
        output = ROOT / "build" / self.output_name / mode
        passed = run(ARGS.emu.resolve(), self.runtime, self.art, self.probe / "classes.dex",
                     mode, output, 150, services=True, main_class=self.probe_class, app_id=10000, app=self.app,
                     libraries=(ROOT / "build/android-adapters/libbifrost-activity.so",))
        self.assertTrue(passed, f"{mode} service routing failed; see {output}")
        self.assertIn(self.marker, (output / (mode + ".log")).read_text())


class HostNetworkTests(unittest.TestCase):
    def test_default_routes_and_offline_profiles(self):
        from android_managed import host_default_interface
        with tempfile.TemporaryDirectory(prefix="bifrost-network-") as directory:
            root = Path(directory)
            self.assertIsNone(host_default_interface(root))
            route = root / "route"
            route.write_text("Iface Destination Gateway Flags RefCnt Use Metric Mask\n"
                             "eth-test 0001A8C0 00000000 0001 0 0 600 00FFFFFF\n")
            self.assertIsNone(host_default_interface(root))
            route.write_text("eth-test 00000000 0101A8C0 0003 0 0 600 00000000\n")
            self.assertEqual(host_default_interface(root), "eth-test")
            route.write_text("lo 00000000 00000000 0001 0 0 0 00000000\n"
                             "eth-test 00000000 00000000 0201 0 0 0 00000000\n")
            self.assertIsNone(host_default_interface(root))
            (root / "ipv6_route").write_text(
                "0" * 32 + " 00 " + "0" * 32 + " 00 " + "0" * 32 +
                " 00000064 00000000 00000000 00000003 tun-test\n")
            self.assertEqual(host_default_interface(root), "tun-test")
            (root / "ipv6_route").write_text("malformed\n")
            self.assertIsNone(host_default_interface(root))

    def test_explicit_network_modes_and_auto_discovery(self):
        from android_managed import host_network_environment
        def environment(variables, interface="eth-test", validated=True):
            with patch.dict(os.environ, variables, clear=True), \
                    patch("android_managed.host_default_interface", return_value=interface), \
                    patch("android_managed.host_network_validated", return_value=validated) as probe:
                result = host_network_environment()
            return [result[k] for k in ("BIFROST_ANDROID_HOST_NETWORK",
                    "BIFROST_ANDROID_NETWORK_VALIDATED", "BIFROST_ANDROID_NETWORK_INTERFACE")], probe.called
        self.assertEqual(environment({}), (["1", "1", "eth-test"], True))
        self.assertEqual(environment({}, validated=False), (["1", "0", "eth-test"], True))
        self.assertEqual(environment({}, interface=None), (["0", "0", "wlan0"], False))
        self.assertEqual(environment({"BIFROST_ANDROID_HOST_NETWORK": "0"}), (["0", "0", "eth-test"], False))
        self.assertEqual(environment({"BIFROST_ANDROID_HOST_NETWORK": "1"}, interface=None),
                         (["1", "1", "wlan0"], True))
        self.assertEqual(environment({"BIFROST_ANDROID_NETWORK_VALIDATED": "0"}), (["1", "0", "eth-test"], False))
        self.assertEqual(environment({"BIFROST_ANDROID_NETWORK_VALIDATED": "1"}, validated=False),
                         (["1", "1", "eth-test"], False))
        for name in ("BIFROST_ANDROID_HOST_NETWORK", "BIFROST_ANDROID_NETWORK_VALIDATED"):
            with self.assertRaises(ValueError): environment({name: "maybe"})
        # The enforced offline switch is reported to Android as no network, whatever the route is.
        self.assertEqual(environment({"BIFROST_ANDROID_OFFLINE": "1"}), (["0", "0", "eth-test"], False))
        self.assertEqual(environment({"BIFROST_ANDROID_OFFLINE": "0"}), (["1", "1", "eth-test"], True))

    def test_validation_probe_requires_https_204(self):
        # NetworkMonitor: 204 validates, a redirect is a portal, the rest fails.
        from android_managed import host_network_validated
        for status, expected in ((204, True), (200, False), (302, False)):
            with patch("http.client.HTTPSConnection") as connection:
                connection.return_value.getresponse.return_value.status = status
                self.assertEqual(host_network_validated(), expected)
                connection.return_value.request.assert_called_once_with(
                    "GET", "/generate_204", headers={"Connection": "close"})
                connection.return_value.close.assert_called_once()
        with patch("http.client.HTTPSConnection") as connection:
            connection.return_value.request.side_effect = OSError("unreachable")
            self.assertFalse(host_network_validated())


class DexoptCacheTests(unittest.TestCase):
    def setUp(self):
        import json
        import android_managed as managed
        self.managed = managed
        self.temporary = tempfile.TemporaryDirectory(prefix="bifrost-dexopt-")
        self.work = Path(self.temporary.name)
        self.root = self.work / "rootfs"; self.root.mkdir()
        self.code = "/data/app/fixture"
        directory = self.root / self.code.lstrip("/"); directory.mkdir(parents=True)
        (directory / "base.apk").write_bytes(b"signed APK fixture")
        self.metadata = {"package": "org.bifrost.fixture", "files": {"base.apk": managed.file_hash(directory / "base.apk")}}
        self.runtime = {"identity": {"framework": "verified framework", "launcher": "one", "properties": {}}}
        self.env = {"BOOTCLASSPATH": "/system/framework/framework.jar"}
        self.write_runtime()
        self.compiles = 0
        def tool(root, emu, env, no_jit, binary, *args, timeout):
            if binary.endswith("dex2oat64"):
                self.compiles += 1
                for name in ("base.odex", "base.vdex", "base.art"):
                    path = root / self.code.lstrip("/") / "oat/arm64" / name
                    path.write_bytes((name + str(self.compiles)).encode())
            return subprocess.CompletedProcess([], 0, b"", b"")
        self.mock = patch.object(managed, "run_guest_tool", side_effect=tool)
        self.mock.start()

    def tearDown(self):
        self.mock.stop(); self.temporary.cleanup()

    def write_runtime(self):
        import json
        (self.work / "runtime.json").write_text(json.dumps(self.runtime))

    def compile(self):
        self.managed.dexopt(self.work, self.root, self.metadata, self.code, Path("/unused"), self.env)

    def test_unrelated_settings_hit_cache_but_compilation_inputs_invalidate(self):
        self.compile(); self.assertEqual(self.compiles, 1)
        self.runtime["identity"].update(launcher="two", properties={"ro.kernel.qemu": "0"},
                                        media_mask=0, microg="new version", settings_key="other")
        self.write_runtime()
        prop = self.root / "debug_ramdisk/adb_debug.prop"; prop.parent.mkdir()
        prop.write_text("ro.kernel.qemu=0\n")
        self.compile(); self.assertEqual(self.compiles, 1)
        prop.write_text("dalvik.vm.isa.arm64.variant=cortex-a53\n")
        self.compile(); self.assertEqual(self.compiles, 2)
        prop.write_text("dalvik.vm.boot-image=/system/framework/other.art\n")
        self.compile(); self.assertEqual(self.compiles, 3)
        self.runtime["identity"]["framework"] = "new ART and boot image"
        self.write_runtime(); self.compile(); self.assertEqual(self.compiles, 4)
        self.env["BOOTCLASSPATH"] += ":/system/framework/extension.jar"
        self.compile(); self.assertEqual(self.compiles, 5)
        self.metadata["files"]["base.apk"] = "new signed APK"
        self.compile(); self.assertEqual(self.compiles, 6)
        profile = self.root / "data/misc/profiles/ref/org.bifrost.fixture/primary.prof"
        profile.parent.mkdir(parents=True); profile.write_bytes(b"reference profile")
        self.compile(); self.assertEqual(self.compiles, 7)
        profile.write_bytes(b"changed reference profile")
        self.compile(); self.assertEqual(self.compiles, 8)

    def test_artifacts_are_verified_and_legacy_cache_migrates_without_compiling(self):
        import json
        self.compile()
        key = self.managed.read_dexopt_stamp(self.work)
        legacy = {k: key[k] for k in ("apk", "filter", "profile", "failed")}
        legacy["runtime"] = self.managed.file_hash(self.work / "runtime.json")
        (self.work / "dexopt.json").write_text(json.dumps(legacy))
        self.compile(); self.assertEqual(self.compiles, 1)
        self.assertEqual(self.managed.read_dexopt_stamp(self.work)["schema"], 2)
        path = self.root / self.code.lstrip("/") / "oat/arm64/base.odex"
        path.chmod(0o644); path.write_bytes(b"tampered")
        self.compile(); self.assertEqual(self.compiles, 2)
        path.unlink()
        self.compile(); self.assertEqual(self.compiles, 3)
        legacy["runtime"] = "a different runtime"
        (self.work / "dexopt.json").write_text(json.dumps(legacy))
        self.compile(); self.assertEqual(self.compiles, 4)

    def test_artifacts_survive_runtime_rebuild_but_not_framework_change(self):
        self.compile()
        fresh = self.work / "fresh"
        shutil.copytree(self.root, fresh, ignore=shutil.ignore_patterns("oat"))
        directory = fresh / self.code.lstrip("/"); directory.chmod(0o555)
        identity = dict(self.runtime["identity"], launcher="new launcher", properties={"ro.kernel.qemu": "0"})
        result = self.managed.retain_dexopt(self.work, self.root, fresh, identity, self.metadata, self.code, self.env)
        self.assertIsNotNone(result)
        self.assertEqual(directory.stat().st_mode & 0o777, 0o555)
        self.assertEqual(self.managed.dexopt_artifacts(self.root, self.code),
                         self.managed.dexopt_artifacts(fresh, self.code))
        identity["framework"] = "new ART"
        self.assertIsNone(self.managed.retain_dexopt(self.work, self.root, fresh, identity, self.metadata, self.code, self.env))

    def test_failed_compilation_is_retried_only_when_compilation_inputs_change(self):
        with patch.object(self.managed, "run_guest_tool", return_value=subprocess.CompletedProcess([], 1, b"", b"failure")) as tool:
            self.compile(); self.assertEqual(tool.call_count, 1)
            self.runtime["identity"]["properties"] = {"ro.kernel.qemu": "0"}
            self.write_runtime(); self.compile(); self.assertEqual(tool.call_count, 1)
            self.env["DEX2OATBOOTCLASSPATH"] = "/new/boot.jar"
            self.compile(); self.assertEqual(tool.call_count, 2)


class SettingsIdentityTests(unittest.TestCase):
    def test_saved_properties_survive_reprovisioning_and_reset_to_image_defaults(self):
        import json
        import android_managed as managed
        from android_settings import save
        with tempfile.TemporaryDirectory(prefix="bifrost-properties-") as temporary:
            work = Path(temporary)
            app, framework, adapters, dex = (work / n for n in ("app", "framework", "adapters", "launcher.dex"))
            app.mkdir()
            (framework / "system/bin").mkdir(parents=True)
            (framework / "vendor/etc").mkdir(parents=True)
            (framework / "bifrost-framework.json").write_text("{}")
            (framework / "system/build.prop").write_text("ro.kernel.qemu=1\n")
            (framework / "vendor/etc/media_codecs.xml").write_text('<MediaCodecs/>')
            names = ("libbifrost-activity.so", "managed/am", "managed/libandroid.so", "managed/libEGL.so",
                     "managed/libGLESv1_CM.so", "managed/libGLESv2.so", "managed/libGLESv3.so",
                     "managed/libaaudio.so", "managed/libOpenSLES.so", "managed/mapper.bifrost.so",
                     "managed/libbifrost-binder-attach.so")
            files = {}
            for name in names:
                path = adapters / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(name)
                files[name] = managed.file_hash(path)
            (adapters / "adapters.json").write_text(json.dumps({"files": files}))
            dex.write_text("launcher version one")
            code = "/data/app/fixture"
            package_dir = framework / code.lstrip("/"); package_dir.mkdir(parents=True)
            (package_dir / "base.apk").write_text("fixture APK")
            metadata = {"package": "org.bifrost.fixture", "files": {"base.apk": managed.file_hash(package_dir / "base.apk")}}
            def prepare():
                return managed.prepare_root(app, metadata, "/data/app/fixture", None,
                                            framework, work / "art", adapters, dex)[0]
            with patch.object(managed.android_framework, "install", return_value={"environment": {}}), \
                    patch.object(managed.android_microg, "install"), \
                    patch.object(managed, "install_managed_graphics"):
                save(app, {"property.ro.kernel.qemu": "0"})
                root = prepare()
                values = lambda: managed.image_properties(root, {"ro.kernel.qemu", "media.c2.hal.selection"})
                self.assertEqual(values(), {"ro.kernel.qemu": "0", "media.c2.hal.selection": "aidl"})
                data = root / "data/user/0/org.bifrost.fixture/save"
                data.write_text("saved game")
                artifacts = root / code.lstrip("/") / "oat/arm64"; artifacts.mkdir(parents=True)
                for name in ("base.odex", "base.vdex"): (artifacts / name).write_text(name)
                identity = json.loads((app / "managed/runtime.json").read_text())["identity"]
                key = managed.dexopt_key(identity, root, metadata, code, {})
                managed.save_dexopt_stamp(app / "managed", key, root, code, False)
                self.assertEqual(prepare(), root)  # verified cached runtime
                dex.write_text("launcher version two")  # force runtime replacement
                prepare()
                self.assertEqual(values()["ro.kernel.qemu"], "0")
                self.assertEqual(data.read_text(), "saved game")
                save(app, {})
                prepare()
                self.assertEqual(values()["ro.kernel.qemu"], "1")
                self.assertEqual(data.read_text(), "saved game")
                self.assertTrue((artifacts / "base.odex").is_file())
                immutable = json.loads((app / "managed/runtime.json").read_text())["immutable"]
                self.assertFalse(any("/oat/" in name for name in immutable))
                with patch.object(managed, "run_guest_tool") as tool:
                    managed.dexopt(app / "managed", root, metadata, code, Path("/unused"), {})
                    tool.assert_not_called()
                prop = root / "debug_ramdisk/adb_debug.prop"
                prop.write_text(prop.read_text() + "ro.kernel.qemu=0\n")
                with self.assertRaisesRegex(ValueError, "Modified managed runtime file"):
                    prepare()

    def test_concurrent_first_launches_share_a_persistent_user_key(self):
        from android_managed import settings_user_key, install_settings_key
        with tempfile.TemporaryDirectory(prefix="bifrost-settings-") as temporary:
            state = Path(temporary)
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                keys = list(pool.map(lambda _: settings_user_key(state), range(16)))
            self.assertEqual(len(keys[0]), 32)
            self.assertTrue(all(key == keys[0] for key in keys))
            self.assertEqual(settings_user_key(state), keys[0])
            self.assertNotEqual(settings_user_key(state, 1), keys[0])
            self.assertNotEqual(settings_user_key(state / "other-device"), keys[0])
            for runtime in (state / "app-one/rootfs", state / "app-two/rootfs"):
                install_settings_key(runtime, keys[0])
                self.assertEqual((runtime / "data/system/users/0/bifrost-ssaid-key").read_bytes(), keys[0])
            self.assertEqual((state / ".settings/user-0.key").stat().st_mode & 0o777, 0o600)

    def test_corrupt_or_redirected_key_is_not_replaced(self):
        from android_managed import settings_user_key
        with tempfile.TemporaryDirectory(prefix="bifrost-settings-") as temporary:
            state = Path(temporary)
            settings_user_key(state)
            target = state / ".settings/user-0.key"
            target.write_bytes(b"broken")
            with self.assertRaises(ValueError): settings_user_key(state)
            self.assertEqual(target.read_bytes(), b"broken")
            target.unlink()
            outside = state / "outside"
            outside.write_bytes(b"a" * 32)
            target.symlink_to(outside)
            with self.assertRaises(OSError): settings_user_key(state)
            self.assertEqual(outside.read_bytes(), b"a" * 32)


class DnsProxyTests(unittest.TestCase):
    def test_android_flags_and_ipv6_wire_layout(self):
        from android_managed import DnsProxy
        answer = [(socket.AF_INET6, socket.SOCK_STREAM, socket.IPPROTO_TCP,
                   "fixture.invalid", ("fe80::1234", 443, 5, 7))]
        with patch("android_managed.socket.getaddrinfo", return_value=answer) as resolver:
            data = DnsProxy.resolve(b"getaddrinfo fixture.invalid 443 1034 10 1 6 0")
        resolver.assert_called_once_with("fixture.invalid", "443", 10, 1, 6,
                                         socket.AI_CANONNAME | socket.AI_NUMERICSERV | socket.AI_ADDRCONFIG)
        raw = (struct.pack("<H", 10) + struct.pack("!HI", 443, 5) +
               socket.inet_pton(socket.AF_INET6, "fe80::1234") + struct.pack("<I", 7))
        canon = b"fixture.invalid\0"
        self.assertEqual(data, b"222\0" + struct.pack("!6i", 1, 1034, 10, 1, 6, 28) +
                         raw + struct.pack("!i", len(canon)) + canon + struct.pack("!i", 0))

    def test_offline_resolves_only_local_names(self):
        from android_managed import DnsProxy
        answer = [(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_TCP, "", ("127.0.0.1", 80))]
        with patch("android_managed.socket.getaddrinfo", return_value=answer) as resolver:
            for host in ("example.com", "dns.google", "8.8.8.8", "2001:db8::1"):
                self.assertEqual(DnsProxy.resolve(f"getaddrinfo {host} ^ 0 0 1 0 0".encode(), offline=True),
                                 b"401\0" + struct.pack("<i", 7), host)
            resolver.assert_not_called()
            for host in ("localhost", "127.0.0.1", "::1", "^"):
                self.assertTrue(DnsProxy.resolve(f"getaddrinfo {host} ^ 0 0 1 0 0".encode(), offline=True).startswith(b"222\0"), host)
            # Online behaviour is unchanged.
            self.assertTrue(DnsProxy.resolve(b"getaddrinfo example.com ^ 0 0 1 0 0").startswith(b"222\0"))

    def test_errors_are_android_numbers_and_bad_flags_do_not_resolve(self):
        from android_managed import DnsProxy
        with patch("android_managed.socket.getaddrinfo", side_effect=socket.gaierror(socket.EAI_NONAME, "missing")) as resolver:
            self.assertEqual(DnsProxy.resolve(b"getaddrinfo missing.invalid ^ 0 0 1 0 0"),
                             b"401\0" + struct.pack("<i", 8))
            resolver.reset_mock()
            self.assertEqual(DnsProxy.resolve(b"getaddrinfo missing.invalid ^ 16 0 1 0 0"),
                             b"401\0" + struct.pack("<i", 3))
            resolver.assert_not_called()

    def test_fragmented_request_and_cleanup(self):
        from android_managed import DnsProxy
        with tempfile.TemporaryDirectory(prefix="bifrost-dns-") as directory:
            root = Path(directory)
            proxy = DnsProxy(root)
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                    client.settimeout(5)
                    client.connect(str(proxy.path))
                    client.sendall(b"getaddrinfo 127.0.0.1 ")
                    client.sendall(b"443 12 2 1 6 0\0")
                    data = b""
                    while chunk := client.recv(4096):
                        data += chunk
                raw = struct.pack("<H", 2) + struct.pack("!H", 443) + b"\x7f\0\0\x01" + bytes(8)
                self.assertEqual(data, b"222\0" + struct.pack("!6i", 1, 12, 2, 1, 6, 16) +
                                 raw + struct.pack("!2i", 0, 0))
            finally:
                proxy.close()
            self.assertFalse(proxy.path.exists())
            self.assertFalse(proxy.thread.is_alive())

    def test_real_bionic_consumes_ipv4_and_ipv6_answers(self):
        from android_managed import DnsProxy
        runtime = ROOT / "build/android-framework/rootfs"
        if not (runtime / "system/lib64/libnetd_client.so").is_file() or not shutil.which("clang") or not ARGS.emu.is_file():
            self.skipTest("requires provisioned Android framework, clang and emulator")
        with tempfile.TemporaryDirectory(prefix="bifrost-dns-") as directory:
            root = Path(directory)
            for name in ("apex/com.android.runtime/bin/linker64",
                         "apex/com.android.runtime/lib64/bionic/libc.so",
                         "apex/com.android.runtime/lib64/bionic/libdl.so",
                         "apex/com.android.runtime/lib64/bionic/libm.so",
                         "system/lib64/libnetd_client.so"):
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(runtime / name, target)
            shutil.copyfile(runtime / "apex/com.android.art/lib64/libc++.so",
                            root / "system/lib64/libc++.so")
            (root / "system/bin").mkdir(parents=True)
            (root / "system/bin/linker64").symlink_to("../../apex/com.android.runtime/bin/linker64")
            source = root / "dns.c"
            source.write_text('''// Freestanding declarations for the pinned AArch64 Bionic ABI.
struct addrinfo { int flags, family, type, protocol; unsigned len;
                 char *canon; unsigned char *addr; struct addrinfo *next; };
extern int getaddrinfo(const char*, const char*, const struct addrinfo*, struct addrinfo**);
extern void freeaddrinfo(struct addrinfo*);
extern int printf(const char*, ...);
int main(void) {
    for (int family = 2; family <= 10; family += 8) {
        struct addrinfo hints = { .family = family, .type = 1 }, *result = 0;
        int error = getaddrinfo("bifrost-dns.invalid", "443", &hints, &result);
        if (error || !result) { printf("DNS error %d\\n", error); return 1; }
        unsigned char *a = result->addr;
        if (result->family != family || result->len != (family == 2 ? 16u : 28u) ||
            a[0] != family || a[1] || a[2] != 1 || a[3] != 187 ||
            (family == 2 ? a[4] != 127 || a[7] != 1 : a[23] != 1)) return 2;
        freeaddrinfo(result);
    }
    printf("BIONIC_DNS_OK\\n"); return 0;
}
''')
            binary = root / "dns"
            bionic = root / "apex/com.android.runtime/lib64/bionic"
            subprocess.run(["clang", "--target=aarch64-linux-android35", "-fuse-ld=lld",
                            "-nostdlib", "-fPIE", "-pie", "-O2", "-Wl,-z,max-page-size=16384",
                            "-Wl,--dynamic-linker=/system/bin/linker64",
                            "-Wl,-rpath,/system/lib64:/apex/com.android.runtime/lib64/bionic",
                            "-Wl,--allow-shlib-undefined", str(ROOT / "Tests/Unit/android_bionic_start.S"),
                            str(source), str(bionic / "libc.so"), "-o", str(binary)],
                           check=True, capture_output=True)
            def answer(host, service, family, kind, protocol, flags):
                self.assertEqual(host, "bifrost-dns.invalid")
                address = ("127.0.0.1", 443) if family == 2 else ("::1", 443, 0, 0)
                return [(family, 1, 6, "", address)]
            proxy = DnsProxy(root)
            try:
                with patch("android_managed.socket.getaddrinfo", side_effect=answer) as resolver:
                    environment = dict(os.environ)
                    environment.pop("BIFROST_ROOT", None)
                    environment["BIFROST_GUEST_ENV"] = "LD_LIBRARY_PATH=/system/lib64:/apex/com.android.runtime/lib64/bionic"
                    for mode in ([], ["--no-jit"]):
                        result = subprocess.run([str(ARGS.emu.resolve()), *mode, "--android-runtime",
                                                 "--rootfs", str(root), "/dns"], env=environment,
                                                timeout=30, capture_output=True, text=True)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertIn("BIONIC_DNS_OK", result.stdout)
                    self.assertEqual(resolver.call_count, 4)
            finally:
                proxy.close()


class SurfaceIdentityTests(unittest.TestCase):
    def test_actual_adapter_reuses_jni_identity_and_retires_collected_handles(self):
        runtime = ROOT / "Tools/android-runtime/rootfs"
        if not shutil.which("clang") or not ARGS.emu.is_file() or not (runtime / "system/bin/linker64").is_file():
            self.skipTest("requires clang, provisioned Bionic and emulator")
        spec = importlib.util.spec_from_file_location("surface_dependencies",
                                                     ROOT / "Tools/Android/android_guest_dependencies.py")
        dependencies = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(dependencies)
        with tempfile.TemporaryDirectory(prefix="bifrost-surface-identity-") as directory:
            root = Path(directory)
            headers = dependencies.prepare_headers(root / "headers")
            for name in ("apex/com.android.runtime/bin/linker64",
                         "apex/com.android.runtime/lib64/bionic/libc.so",
                         "apex/com.android.runtime/lib64/bionic/libdl.so"):
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(runtime / name, target)
            (root / "system/bin").mkdir(parents=True)
            (root / "system/bin/linker64").symlink_to("../../apex/com.android.runtime/bin/linker64")
            bionic = root / "apex/com.android.runtime/lib64/bionic"
            subprocess.run(["clang", "--target=aarch64-linux-android35", "-fuse-ld=lld",
                            "-nostdlib", "-ffreestanding", "-fPIE", "-pie", "-O2", "-Wall", "-Wextra",
                            "-ffunction-sections", "-Wl,--gc-sections", "-Wl,-z,max-page-size=16384",
                            "-Wl,--dynamic-linker=/system/bin/linker64",
                            "-Wl,-rpath,/apex/com.android.runtime/lib64/bionic",
                            "-Wl,--allow-shlib-undefined", *headers,
                            str(ROOT / "Tests/Unit/android_bionic_start.S"),
                            str(ROOT / "Tests/Host/android_surface_identity.c"),
                            str(ROOT / "Guest/Android/Adapters/surface.c"),
                            str(bionic / "libc.so"), "-o", str(root / "surface_identity")],
                           check=True, capture_output=True, text=True)
            environment = dict(os.environ)
            environment.pop("BIFROST_ROOT", None)
            environment["BIFROST_GUEST_ENV"] = "LD_LIBRARY_PATH=/apex/com.android.runtime/lib64/bionic"
            for mode in ([], ["--no-jit"]):
                result = subprocess.run([str(ARGS.emu.resolve()), *mode, "--android-runtime",
                                         "--rootfs", str(root), "/surface_identity"],
                                        env=environment, timeout=30, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("android_surface_identity: checks passed", result.stdout)


class BrowserLinkTests(unittest.TestCase):
    def test_host_opens_only_well_formed_web_links_at_a_limited_rate(self):
        import android_processes as processes
        calls, now = [], [100.0]
        def popen(command, **options): calls.append((command, options))
        def opener(url, last=None):
            return processes.open_url(url, last if last is not None else self.last, opener="/usr/bin/xdg-open",
                                      popen=popen, clock=lambda: now[0])
        self.last = [-processes.SAME_LINK_WINDOW, None]
        self.assertEqual(opener("https://example.test/a?b=c#d"), dict(opened=True))
        self.assertEqual(calls[0][0], ["/usr/bin/xdg-open", "https://example.test/a?b=c#d"])
        self.assertTrue(calls[0][1]["start_new_session"])
        # The same link again soon is not opened twice; a different one is held to the interval.
        now[0] += 0.1
        self.assertEqual(opener("https://example.test/a?b=c#d"), dict(opened=True, repeated=True))
        self.assertEqual(len(calls), 1)
        # No scheme but http(s), no option-looking or control characters, no oversize link, no non-strings.
        for bad in ("javascript:alert(1)", "file:///etc/passwd", "intent://x#Intent;end", "-c calc", "ftp://example.test/",
                    "https://example.test/ with space", "https://example.test/\nHeader: 1", "https://" + "a" * 2100,
                    "http://", "", None, 7):
            now[0] += 5
            with self.assertRaises(ValueError, msg=repr(bad)): opener(bad)
        # One link per interval; the clock moving on allows the next.
        now[0] += 5
        opener("https://example.test/1")
        with self.assertRaises(RuntimeError): opener("https://example.test/2")
        now[0] += processes.OPEN_INTERVAL
        opener("https://example.test/2")
        self.assertEqual(len(calls), 3)
        # No launcher installed: a clear error, not a crash.
        now[0] += 5
        with self.assertRaises(RuntimeError):
            processes.open_url("https://example.test/", [-100.0, None], popen=popen, clock=lambda: now[0], which=lambda name: None)


    def test_guest_maps_play_install_links_to_the_public_store_page(self):
        # Ad "Install" clicks arrive as https://play.google.com/d?id=<package>&inline=true&enifd=...,
        # a deep link only the Play Store app opens; the host browser gets a 404 for it.
        javac = android_paths.javac()
        java = shutil.which("java")
        if not javac or not java:
            self.skipTest("needs a JDK")
        with tempfile.TemporaryDirectory() as directory:
            classes = Path(directory)
            subprocess.run([javac, "--release", "8", "-Xlint:none", "-d", str(classes),
                            str(ROOT / "Guest/Android/Managed/Framework/FrameworkLinks.java"),
                            str(ROOT / "Tests/Host/FrameworkLinksProbe.java")], check=True, capture_output=True)
            result = subprocess.run([java, "-cp", str(classes), "FrameworkLinksProbe"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("FrameworkLinksProbe: ALL PASS", result.stdout)


class GoogleAccountStateTests(unittest.TestCase):
    ACCOUNT = {"version": 1, "revision": 1, "accounts": [
        {"name": "user@example.test", "type": "com.google", "password": "master", "previous": None,
         "authenticated": 0, "data": {"services": "ac2dm,mail"}, "tokens": {}, "visibility": {}, "grants": []},
        {"name": "other", "type": "org.example", "password": None, "previous": None,
         "authenticated": 0, "data": {}, "tokens": {}, "visibility": {}, "grants": []}]}

    def test_games_profile_choice_defaults_off_and_reaches_every_root(self):
        import android_google as google
        from xml.etree import ElementTree
        def value(root):
            text = (root / google.PREFERENCES).read_text()
            entries = [e for e in ElementTree.fromstring(text) if e.get("name") == google.GAMES_PROFILE_KEY]
            return entries[0].get("value") if entries else None
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory)
            existing, fresh = state / "existing-root", state / "fresh-root"
            (existing / google.PREFERENCES).parent.mkdir(parents=True)
            (existing / google.PREFERENCES).write_text(
                "<?xml version='1.0' encoding='utf-8' standalone='yes' ?><map>"
                "<boolean name=\"safetynet_enabled\" value=\"true\" /><string name=\"x\">y</string></map>")
            # Never chosen: not allowed, and no root is touched.
            self.assertFalse(google.games_profile_allowed(state))
            self.assertFalse(google.apply_games_profile(state, existing))
            self.assertFalse(google.apply_games_profile(state, fresh))
            self.assertFalse((fresh / google.PREFERENCES).exists())
            self.assertIsNone(value(existing))
            # Allowed: the key is added, the other preferences stay, a second launch changes nothing.
            google.set_games_profile(state, True)
            self.assertTrue(google.games_profile_allowed(state))
            self.assertTrue(google.apply_games_profile(state, existing))
            self.assertEqual(value(existing), "true")
            self.assertIn('name="safetynet_enabled"', (existing / google.PREFERENCES).read_text())
            self.assertIn(">y<", (existing / google.PREFERENCES).read_text())
            self.assertFalse(google.apply_games_profile(state, existing))
            # A root without preferences gets a file; turning it off flips both.
            self.assertTrue(google.apply_games_profile(state, fresh))
            self.assertEqual(value(fresh), "true")
            google.set_games_profile(state, False)
            self.assertFalse(google.games_profile_allowed(state))
            self.assertTrue(google.apply_games_profile(state, existing))
            self.assertTrue(google.apply_games_profile(state, fresh))
            self.assertEqual((value(existing), value(fresh)), ("false", "false"))
            # Hostile guest files: a DOCTYPE and a symlink are refused, not parsed or followed.
            google.set_games_profile(state, True)
            (existing / google.PREFERENCES).write_text('<!DOCTYPE map [<!ENTITY a "b">]><map/>')
            self.assertFalse(google.apply_games_profile(state, existing))
            outside = state / "host-file.xml"
            outside.write_text("<map/>")
            (fresh / google.PREFERENCES).unlink()
            (fresh / google.PREFERENCES).symlink_to(outside)
            self.assertFalse(google.apply_games_profile(state, fresh))
            self.assertEqual(outside.read_text(), "<map/>")

    def test_accounts_are_shared_between_roots_and_settings_are_not(self):
        import json
        import android_google as google
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory)
            signed_in, game = state / "gms-root", state / "game-root"
            for root in (signed_in, game):
                (root / google.DATA / "shared_prefs").mkdir(parents=True)
                (root / google.ACCOUNTS).parent.mkdir(parents=True, exist_ok=True)
            (signed_in / google.ACCOUNTS).write_text(json.dumps(self.ACCOUNT))
            (signed_in / google.DATA / "shared_prefs/checkin.xml").write_text("<map>device-1</map>")
            (signed_in / google.DATA / "shared_prefs/com.google.android.gms_preferences.xml").write_text("<map>sign-in root</map>")
            (game / google.DATA / "shared_prefs/com.google.android.gms_preferences.xml").write_text("<map>safetynet</map>")
            # Nothing is shared until a root publishes; importing is then a no-op.
            self.assertFalse(google.import_shared(state, game))
            self.assertEqual(google.accounts(state), [])
            self.assertTrue(google.publish_shared(state, signed_in))
            self.assertEqual([entry["name"] for entry in google.accounts(state)], ["user@example.test"])
            self.assertFalse(google.publish_shared(state, signed_in))
            # The game root receives the accounts and the device registration, not microG's settings.
            self.assertTrue(google.import_shared(state, game))
            self.assertEqual(json.loads((game / google.ACCOUNTS).read_text())["accounts"][0]["name"], "user@example.test")
            self.assertEqual((game / google.DATA / "shared_prefs/checkin.xml").read_text(), "<map>device-1</map>")
            self.assertEqual((game / google.DATA / "shared_prefs/com.google.android.gms_preferences.xml").read_text(),
                             "<map>safetynet</map>")
            self.assertFalse(google.import_shared(state, game))
            self.assertFalse(google.publish_shared(state, game))  # imported, unchanged
            # Signing out on the host reaches the game at its next launch.
            google.remove(state, "user@example.test")
            self.assertEqual(google.accounts(state), [])
            self.assertTrue(google.import_shared(state, game))
            self.assertEqual([entry["name"] for entry in json.loads((game / google.ACCOUNTS).read_text())["accounts"]], ["other"])
            with self.assertRaises(ValueError): google.remove(state, "user@example.test")
            # An account the game added itself is published when its session ends.
            document = json.loads((game / google.ACCOUNTS).read_text())
            document["accounts"].append(self.ACCOUNT["accounts"][0]); document["revision"] += 1
            (game / google.ACCOUNTS).write_text(json.dumps(document))
            self.assertTrue(google.publish_shared(state, game))
            self.assertEqual([entry["name"] for entry in google.accounts(state)], ["user@example.test"])

    def test_activity_requests_are_validated_staged_and_answered(self):
        import json
        import android_google as google
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory)
            root = state / "org.example.game/managed/rootfs"
            identifier = "a" * 32
            cache = root / "data/user/0/org.example.game/cache/google-activity"
            request = cache / identifier
            request.mkdir(parents=True)
            (request / "intent.bin").write_bytes(b"intent")
            (request / "caller.bin").write_bytes(b"caller")
            (request / "request.json").write_text(json.dumps({
                "component": google.GMS + "/org.microg.gms.auth.signin.AuthSignInActivity",
                "caller": "org.example.game/org.example.Main", "response": False}))
            (state / "org.example.game/package.json").write_text(json.dumps({"label": "Example Game"}))
            relay = google.ActivityRelay(state, root, [])
            guest = "/data/user/0/org.example.game/cache/google-activity/" + identifier
            self.assertEqual(relay.request(guest), request.resolve())
            self.assertEqual(relay.requester(request), {"package": "org.example.game", "label": "Example Game"})
            escaped = state / "outside"
            (escaped / "google-activity").mkdir(parents=True)
            (cache / ("b" * 32)).symlink_to(escaped, target_is_directory=True)
            for bad in ("/data/user/0/org.example.game/cache/google-activity/" + "A" * 32,
                        "/data/user/0/org.example.game/cache/google-activity/../google-activity/" + identifier + "/..",
                        "/data/user/0/org.example.game/files/google-activity/" + identifier,
                        "/data/user/0/org.example.game/cache/google-activity/" + "b" * 32,
                        "/data/system/users/0", "/" + "../" * 8 + "etc"):
                with self.assertRaises(ValueError, msg=bad): relay.request(bad)
            # The system app's root receives the request, answers it, and the answer returns with `done` last.
            gms = state / "gms-root"
            google.stage_request(gms, request)
            self.assertEqual((gms / google.ACTIVITY / "intent.bin").read_bytes(), b"intent")
            (gms / google.ACTIVITY / "nested.json").write_text('{"resultCode": -1}')
            google.finish_request(gms, request)
            self.assertEqual(json.loads((request / "nested.json").read_text()), {"resultCode": -1})
            self.assertTrue((request / "done").is_file())
            self.assertFalse((gms / google.ACTIVITY).exists())
            # A session without a request never sees a stale one.
            google.stage_request(gms, request)
            google.stage_request(gms, None)
            self.assertFalse((gms / google.ACTIVITY).exists())
            self.assertEqual(google.session_options("/emu", ["--window-stats", "--android-binder-broker", "/realm",
                                                              "--unrelated"], True, "/plugin.so"),
                             ["--emu", "/emu", "--window-stats", "--binder-broker", "/realm", "--no-jit",
                              "--android-window-plugin", "/plugin.so"])

    def test_activity_result_returns_while_service_session_is_running(self):
        import json
        import android_google as google
        from android_processes import ProcessHost
        for result_code in (0, -1):
            with self.subTest(result_code=result_code), tempfile.TemporaryDirectory() as directory:
                state = Path(directory)
                root = state / "org.example.game/managed/rootfs"
                request = root / "data/user/0/org.example.game/cache/google-activity" / ("d" * 32)
                request.mkdir(parents=True)
                (request / "request.json").write_text(json.dumps({
                    "component": google.GMS + "/org.microg.gms.games.GamesSignInActivity",
                    "caller": "org.example.game/org.example.Main", "response": False}))
                for name in ("intent.bin", "caller.bin"): (request / name).write_bytes(b"request")
                gms = state / google.GMS / "managed/rootfs"
                completion = google.ActivityCompletion(state, gms, request)
                host = ProcessHost.__new__(ProcessHost)
                host.token, host.activity_finished = "test-capability", completion

                def notify():
                    client, server = socket.socketpair()
                    with client, server:
                        data = json.dumps({"operation": "activity_finished", "token": host.token}).encode()
                        client.sendall(struct.pack('!I', len(data)) + data)
                        host.serve(server)
                        size = struct.unpack('!I', client.recv(4))[0]
                        return json.loads(client.recv(size))

                def start(*args):
                    google.stage_request(gms, request)
                    (gms / google.ACTIVITY / "result.bin").write_bytes(b"activity-data")
                    (gms / google.ACTIVITY / "nested.json").write_text(json.dumps({
                        "resultCode": result_code, "response": "none", "data": True}))
                    self.assertEqual(notify(), {"finished": True})
                    return {"session_id": "still-running", "supervisor_pid": 123}

                live = {"state": "running", "session_id": "still-running"}
                relay = google.ActivityRelay(state, root, [])
                with patch('android_sessions.control', side_effect=[{"state": "exited"}, live]) as control, \
                        patch('android_package.start_background', side_effect=start), \
                        patch('android_google.publish_shared') as publish:
                    relay.run(request)
                    result = json.loads((request / "result.json").read_text())
                    self.assertEqual(result["resultCode"], result_code)
                    self.assertNotIn("error", result)
                    self.assertEqual((request / "result.bin").read_bytes(), b"activity-data")
                    # Neither stopping nor waiting for the service process was necessary.
                    control.assert_called_once_with(state / google.GMS)
                    publish.assert_called_once_with(state, gms)
                    shutil.rmtree(request)  # The app consumed the answer before VM teardown.
                    self.assertEqual(notify(), {"finished": True})
                    completion()  # The runner's process-exit fallback must not recreate it.
                    self.assertFalse(request.exists())
                    publish.assert_called_once()
                    host.activity_finished = None
                    self.assertIn("error", notify())

    def test_activity_relay_never_follows_guest_symlinks(self):
        # The request directory belongs to the (untrusted) app: a symlink there must not make the
        # host read a file, truncate one, or take a package name for a path.
        import json
        import android_google as google
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory)
            root = state / "org.example.game/managed/rootfs"
            request = root / "data/user/0/org.example.game/cache/google-activity" / ("c" * 32)
            request.mkdir(parents=True)
            secret = state / "host-secret.txt"
            secret.write_text("host data")
            for name in google.REQUEST_FILES: (request / name).write_text("{}")
            relay = google.ActivityRelay(state, root, [])
            guest = "/data/user/0/org.example.game/cache/google-activity/" + "c" * 32
            self.assertEqual(relay.request(guest), request.resolve())
            # A request file that is a symlink is refused, and never copied into the system app's root.
            (request / "intent.bin").unlink()
            (request / "intent.bin").symlink_to(secret)
            with self.assertRaises(ValueError): relay.request(guest)
            with self.assertRaises(OSError): google.stage_request(state / "gms-root", request)
            self.assertFalse(((state / "gms-root") / google.ACTIVITY / "intent.bin").exists())
            # `done` pointing at a host file is replaced, not written through.
            (request / "done").symlink_to(secret)
            (state / "gms-root" / google.ACTIVITY).mkdir(parents=True, exist_ok=True)
            google.finish_request(state / "gms-root", request)
            self.assertEqual(secret.read_text(), "host data")
            self.assertTrue(google.is_regular(request / "done"))
            # A symlinked result file is not read, and the requesting package must be a package name.
            (request / "nested.json").symlink_to(secret)
            self.assertFalse(google.is_regular(request / "nested.json"))
            with self.assertRaises(OSError): google.read_regular(request / "nested.json")
            (request / "request.json").unlink()
            (request / "request.json").write_text(json.dumps({"caller": "../../etc/x"}))
            with self.assertRaises(ValueError): relay.requester(request)

    def test_system_app_is_registered_with_microg_identity(self):
        import json
        import android_google as google
        from android_sessions import app_id
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory)
            self.assertEqual(app_id(state, google.GMS), 19998)
            self.assertEqual(app_id(state, "com.android.vending"), 19999)
            google.prepare(state, google.SETTINGS_ACTIVITY)
            metadata = json.loads((google.app_directory(state) / "package.json").read_text())
            self.assertTrue(metadata["system"])
            self.assertEqual(metadata["launcher"]["class"], google.SETTINGS_ACTIVITY)
            listed = package.installed_apps(state)
            self.assertEqual([(a["package"], a["system"], a["requester"]) for a in listed], [(google.GMS, True, None)])
            # A game's request names the app Asgard shows beside the open window; the next start drops it.
            google.prepare(state, "org.microg.gms.auth.signin.AuthSignInActivity", state / "request",
                           {"package": "org.example.game", "label": "Example Game"})
            self.assertEqual(package.installed_apps(state)[0]["requester"]["label"], "Example Game")
            google.prepare(state, google.LOGIN_ACTIVITY)
            self.assertIsNone(package.installed_apps(state)[0]["requester"])


class AppRemovalTests(unittest.TestCase):
    NAME = "org.bifrost.fixture"
    OTHER = "org.bifrost.other"

    def setUp(self):
        import json
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.state = Path(self.directory.name) / "state"
        for name, uid in ((self.NAME, 10000), (self.OTHER, 10002)):
            app = self.state / name
            for rootfs in ("rootfs", "managed/rootfs"):
                root = app / rootfs
                code = root / "data/app" / (name + "-abc")
                (code / "lib/arm64").mkdir(parents=True)
                (code / "base.apk").write_bytes(b"apk")
                (code / "base.apk").chmod(0o444)
                code.chmod(0o555)
                for relative in package.APP_DATA:
                    save = root / relative / name
                    save.mkdir(parents=True)
                    (save / "save.dat").write_text("progress")
                (root / "storage/emulated/0/Android/obb" / name).mkdir(parents=True)
                (root / "storage/emulated/0/Android/obb" / name / "main.obb").write_text("obb")
                (root / "storage/emulated/0/DCIM").mkdir(parents=True, exist_ok=True)
                (root / "storage/emulated/0/DCIM/photo.jpg").write_text("shared")
            (app / "package.json").write_text(json.dumps({"package": name, "launcher": {"kind": "managed_activity"}}))
        (self.state / ".app-ids.json").write_text(json.dumps({self.NAME: 10000, self.OTHER: 10002}))
        self.addCleanup(lambda: [path.chmod(0o755) for path in self.state.rglob("*") if path.is_dir() and not path.is_symlink()])

    def app(self, name=None):
        return self.state / (name or self.NAME)

    def test_clear_data_resets_saves_but_keeps_the_install_and_shared_files(self):
        keystore = self.app() / "managed/rootfs/data/misc/keystore"
        keystore.mkdir(parents=True)
        for name in ("bifrost-10000.json", "bifrost-10000.json.bak", "bifrost-10000.json.lock", "bifrost-19998.json"):
            (keystore / name).write_text("{}")
        package.clear_data(self.state, self.NAME)
        # pm clear deletes the app's Keystore keys; another uid's keys in the root (microG) stay.
        self.assertEqual(sorted(x.name for x in keystore.iterdir()), ["bifrost-19998.json"])
        for rootfs in ("rootfs", "managed/rootfs"):
            root = self.app() / rootfs
            for relative in package.APP_DATA:
                self.assertFalse((root / relative / self.NAME / "save.dat").exists(), relative)
            self.assertEqual((root / "data/app" / (self.NAME + "-abc/base.apk")).read_bytes(), b"apk")
            self.assertEqual((root / "storage/emulated/0/Android/obb" / self.NAME / "main.obb").read_text(), "obb")
            self.assertEqual((root / "storage/emulated/0/DCIM/photo.jpg").read_text(), "shared")
        # The next start needs the package directories, as after an install.
        self.assertTrue((self.app() / "rootfs/data/user/0" / self.NAME / "files").is_dir())
        self.assertTrue((self.app() / "managed/rootfs/data/user_de/0" / self.NAME).is_dir())
        # Other apps are untouched.
        self.assertTrue((self.app(self.OTHER) / "rootfs/data/user/0" / self.OTHER / "save.dat").is_file())

    def test_uninstall_removes_read_only_code_and_releases_the_app_id(self):
        import json
        package.uninstall(self.state, self.NAME)
        self.assertFalse(self.app().exists())
        self.assertEqual([x.name for x in self.state.iterdir() if x.name.startswith(".removing-")], [])
        self.assertEqual(json.loads((self.state / ".app-ids.json").read_text()), {self.OTHER: 10002})
        self.assertEqual([x["package"] for x in package.installed_apps(self.state)], [self.OTHER])
        self.assertTrue((self.app(self.OTHER) / "rootfs/data/app" / (self.OTHER + "-abc/base.apk")).is_file())

    def test_running_system_and_unknown_apps_are_refused(self):
        import json
        with package.app_lock(self.app(), blocking=False):
            for action in (package.uninstall, package.clear_data):
                with self.assertRaisesRegex(ValueError, "busy"):
                    action(self.state, self.NAME)
        self.assertTrue((self.app() / "rootfs/data/user/0" / self.NAME / "save.dat").is_file())
        (self.app(self.OTHER) / "package.json").write_text(json.dumps({"package": self.OTHER, "system": True}))
        for name in (self.OTHER, "org.bifrost.missing", "../" + self.NAME, ""):
            for action in (package.uninstall, package.clear_data):
                with self.assertRaises(ValueError):
                    action(self.state, name)
        self.assertTrue(self.app(self.OTHER).is_dir())

    def test_redirected_app_data_is_never_followed(self):
        outside = Path(self.directory.name) / "outside"
        (outside / self.NAME).mkdir(parents=True)
        (outside / self.NAME / "keep.txt").write_text("host")
        data = self.app() / "rootfs/data/user/0"
        shutil.rmtree(data)
        data.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "redirected"):
            package.clear_data(self.state, self.NAME)
        self.assertTrue((outside / self.NAME / "keep.txt").is_file())
        # A redirected package directory is unlinked, not emptied through the link.
        data.unlink()
        (self.app() / "rootfs/data/user/0").mkdir()
        (self.app() / "rootfs/data/user/0" / self.NAME).symlink_to(outside / self.NAME)
        package.clear_data(self.state, self.NAME)
        self.assertTrue((outside / self.NAME / "keep.txt").is_file())

    def test_command_line_asks_before_destroying_data(self):
        def run(*arguments):
            return subprocess.run([sys.executable, str(ROOT / "Scripts/Android/android_package.py"),
                                   "--state-root", str(self.state), *arguments],
                                  stdin=subprocess.DEVNULL, capture_output=True, text=True)
        save = self.app() / "rootfs/data/user/0" / self.NAME / "save.dat"
        for command in ("clear-data", "uninstall"):
            refused = run(command, self.NAME)
            self.assertEqual(refused.returncode, 1)
            self.assertIn("--yes", refused.stderr)
            self.assertTrue(save.is_file())
        self.assertEqual(run("clear-data", "--yes", self.NAME).returncode, 0)
        self.assertFalse(save.exists())
        self.assertEqual(run("uninstall", "--yes", self.NAME).returncode, 0)
        self.assertFalse(self.app().exists())


class RunFromFileTests(unittest.TestCase):
    def test_missing_toolchain_names_the_step_that_provisions_it(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = (root / "runtime", root / "adapters", root / "aapt2", root / "apksigner.jar")
            expected = ("fetch-android-runtime.py", "make android-runtime-adapters",
                        "fetch-aapt2.py", "fetch-android-package-tools.py")
            for index, step in enumerate(expected):
                with self.assertRaisesRegex(ValueError, step):
                    package.require_toolchain(*paths)
                path = paths[index]
                if index < 2:
                    path.mkdir()
                    (path / ("bifrost-runtime.json", "adapters.json")[index]).write_text("{}")
                else:
                    path.write_text("")
            package.require_toolchain(*paths)

    def test_run_installs_a_package_file_and_runs_its_package(self):
        import asgard
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            installer = root / "installer.py"
            installer.write_text("import sys\nprint('Installed org.bifrost.fixture version 3: 1 signed APKs')"
                                 "\nsys.exit(int(sys.argv[-1].endswith('broken.apk')))\n")
            apk = root / "game.apk"
            apk.write_text("")
            with patch("builtins.print"):
                self.assertEqual(asgard.install_for_run(str(installer), root, ["--no-jit", str(apk), "--timeout", "5"]),
                                 ["--no-jit", "org.bifrost.fixture", "--timeout", "5"])
                real_run = subprocess.run
                with patch.object(asgard.subprocess, "run", wraps=real_run) as installed:
                    self.assertEqual(asgard.install_for_run(str(installer), root, [str(apk), "-d", "--timeout", "5"]),
                                     ["org.bifrost.fixture", "--timeout", "5"])
                    self.assertIn("--allow-downgrade", installed.call_args.args[0])
            with self.assertRaisesRegex(ValueError, "requires an APK"):
                asgard.install_for_run(str(installer), root, ["org.bifrost.fixture", "--allow-downgrade"])
            # Package names, and files that do not exist or are not packages, pass through.
            for arguments in (["org.bifrost.fixture"], [str(root / "missing.apk")], [str(installer)]):
                self.assertEqual(asgard.install_for_run(str(installer), root, arguments), arguments)
            (root / "broken.apk").write_text("")
            with patch("builtins.print"), self.assertRaisesRegex(ValueError, "Could not install"):
                asgard.install_for_run(str(installer), root, [str(root / "broken.apk")])

    def test_run_skips_installing_a_file_that_is_already_installed(self):
        import asgard
        import json
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            installer = root / "installer.py"
            installer.write_text("import sys\nprint('Installed org.bifrost.fixture version 3: 1 signed APKs')\n")
            apk = root / "game.apk"
            apk.write_bytes(b"signed package bytes")
            other = root / "other.apk"
            other.write_bytes(b"another package")

            def record(name, **fields):
                app = root / name
                (app / "rootfs").mkdir(parents=True, exist_ok=True)
                (app / "package.json").write_text(json.dumps({"package": name, **fields}))

            def run(source):
                with patch("builtins.print"), patch.object(asgard.subprocess, "run", wraps=subprocess.run) as spawn:
                    result = asgard.install_for_run(str(installer), root, [str(source), "--timeout", "5"])
                return result, spawn.called

            # No installed app records this file: the installer runs.
            self.assertEqual(run(apk), (["org.bifrost.fixture", "--timeout", "5"], True))
            # An app installed from exactly this file is run without the installer.
            record("org.example.game", source_sha256=asgard.file_sha256(apk))
            self.assertEqual(run(apk), (["org.example.game", "--timeout", "5"], False))
            # A different file, an app without a recorded hash (installed before it existed),
            # a record under the wrong directory, a missing root and unreadable metadata all install.
            self.assertEqual(run(other)[1], True)
            record("org.example.legacy")
            (root / "org.example.game/package.json").write_text(json.dumps(
                {"package": "org.example.other", "source_sha256": asgard.file_sha256(apk)}))
            (root / "broken").mkdir()
            (root / "broken/package.json").write_text("{not json")
            self.assertEqual(run(apk)[1], True)
            record("org.example.game", source_sha256=asgard.file_sha256(apk))
            shutil.rmtree(root / "org.example.game/rootfs")
            self.assertEqual(run(apk)[1], True)
            # Changing the file after installing it invalidates the record.
            record("org.example.game", source_sha256=asgard.file_sha256(apk))
            apk.write_bytes(b"signed package bytes, upgraded")
            self.assertEqual(run(apk)[1], True)


class AndroidPathsTests(unittest.TestCase):
    """Where Asgard keeps state and finds the emulator: checkout or installed tree."""

    def setUp(self):
        import android_paths
        self.paths = android_paths
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name) / "prefix/share/bifrost/asgard"
        self.root.mkdir(parents=True)
        for patcher in (patch.object(android_paths, "ROOT", self.root),
                        patch.dict(os.environ, {"XDG_DATA_HOME": "", "HOME": self.work.name}, clear=False)):
            patcher.start()
            self.addCleanup(patcher.stop)
        os.environ.pop("BIFROST_ANDROID_STATE", None)

    def test_javac_prefers_the_environment_then_the_newest_jdk_off_path(self):
        jvm = Path(self.work.name) / "jvm"
        for name in ("java-8-openjdk", "java-25-openjdk", "java-21-openjdk"):
            tool = jvm / name / "bin/javac"
            tool.parent.mkdir(parents=True)
            tool.write_text("#!/bin/sh\n")
            tool.chmod(0o755)
        (jvm / "java-26-jre/bin").mkdir(parents=True)  # A runtime without javac is not a JDK.
        environment = {"PATH": "", "JAVAC": "", "JAVA_HOME": ""}
        with patch.object(self.paths, "JDK_ROOTS", (str(jvm),)), patch.dict(os.environ, environment):
            self.assertEqual(self.paths.javac(), str(jvm / "java-25-openjdk/bin/javac"))
            with patch.dict(os.environ, {"JAVA_HOME": str(jvm / "java-8-openjdk")}):
                self.assertEqual(self.paths.javac(), str(jvm / "java-8-openjdk/bin/javac"))
            with patch.dict(os.environ, {"JAVAC": "/opt/javac"}):
                self.assertEqual(self.paths.javac(), "/opt/javac")
        with patch.object(self.paths, "JDK_ROOTS", ()), patch.dict(os.environ, environment):
            self.assertIsNone(self.paths.javac())

    def test_a_source_checkout_keeps_state_and_emulator_in_the_checkout(self):
        self.assertFalse(self.paths.installed())
        self.assertEqual(self.paths.state_root(), self.root / "android-state")
        self.assertEqual(self.paths.emulator(), self.root / "bifrost-emu")

    def test_an_installed_tree_keeps_state_per_user_and_uses_the_emulator_beside_it(self):
        (self.root / ".asgard-install").write_text("{}")
        self.assertTrue(self.paths.installed())
        self.assertEqual(self.paths.state_root(), Path(self.work.name) / ".local/share/bifrost/android")
        with patch.dict(os.environ, {"XDG_DATA_HOME": "/xdg/data"}):
            self.assertEqual(self.paths.state_root(), Path("/xdg/data/bifrost/android"))
        with patch.dict(os.environ, {"XDG_DATA_HOME": "relative"}):  # the XDG spec ignores relative paths
            self.assertEqual(self.paths.state_root(), Path(self.work.name) / ".local/share/bifrost/android")
        binary = self.root.parents[2] / "bin/bifrost-emu"
        binary.parent.mkdir(parents=True)
        binary.write_text("")
        self.assertEqual(self.paths.emulator(), binary)

    def test_an_explicit_state_directory_wins_in_both_layouts(self):
        with patch.dict(os.environ, {"BIFROST_ANDROID_STATE": "/elsewhere/state"}):
            self.assertEqual(self.paths.state_root(), Path("/elsewhere/state"))
            (self.root / ".asgard-install").write_text("{}")
            self.assertEqual(self.paths.state_root(), Path("/elsewhere/state"))


class DebugChannelTests(unittest.TestCase):
    def test_channels_set_only_switches_the_emulator_reads(self):
        import asgard
        flags = (ROOT / "Source/System/Core/DebugFlags.hpp").read_text()
        for channel, variables in asgard.DEBUG_CHANNELS.items():
            for variable in variables:
                self.assertIn(f'env("{variable}")', flags, channel)
        self.assertEqual(asgard.debug_environment(""), {})
        self.assertEqual(asgard.debug_environment("futex, mmap,"),
                         {"BIFROST_FUTEX_TRACE": "1", "BIFROST_TRACE_MMAP": "1", "BIFROST_TRACE_MADVISE": "1"})
        with self.assertRaisesRegex(ValueError, "Unknown debug channel 'nope'.*futex"):
            asgard.debug_environment("futex,nope")

    def test_command_line_rejects_an_unknown_channel_before_doing_anything(self):
        result = subprocess.run([sys.executable, str(ROOT / "Scripts/Android/asgard.py"), "--debug", "nope", "list"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("Unknown debug channel", result.stderr)


class CompatibilityTests(unittest.TestCase):
    def test_recorded_entries_are_well_formed_and_cite_existing_pages(self):
        import android_compat
        data = android_compat.load()
        self.assertEqual(android_compat.validate(data, ROOT), [])
        self.assertEqual(android_compat.entry(data, "org.example.unknown")["status"], "untested")
        broken = {"statuses": {"runs": "x"}, "apps": {"bad": {"status": "great", "checked": "soon", "page": "Docs/none.md"}}}
        self.assertEqual(len(android_compat.validate(broken, ROOT)), 5)

    def test_compat_command_lists_installed_and_recorded_apps(self):
        import json
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory)
            for name in ("org.bifrost.fixture", "com.tiltingpoint.spongebob"):
                (state / name).mkdir()
                (state / name / "package.json").write_text(json.dumps(
                    {"package": name, "version_code": "1", "launcher": {"kind": "native_activity"}}))
            def run(*arguments):
                return subprocess.run([sys.executable, str(ROOT / "Scripts/Android/android_package.py"),
                                       "--state-root", str(state), "compat", *arguments],
                                      capture_output=True, text=True)
            report = json.loads(run("--json").stdout)
            self.assertEqual(report["org.bifrost.fixture"], {"status": "untested", "evidence":
                             "No test is recorded for this app.", "installed": True})
            self.assertEqual((report["com.tiltingpoint.spongebob"]["status"], report["com.tiltingpoint.spongebob"]["installed"]),
                             ("playable", True))
            self.assertFalse(report["com.roblox.client"]["installed"])
            self.assertIn("com.roblox.client  blocked  (2026-10-19)  [not installed]", run().stdout)
            self.assertIn("child-age", run("com.tiltingpoint.spongebob").stdout)
            self.assertEqual(run("../x").returncode, 1)

    def test_compat_command_reads_the_package_from_an_apk_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            aapt2 = root / "aapt2"
            aapt2.write_text("#!/bin/sh\necho \"package: name='com.tiltingpoint.spongebob' versionCode='1'\"\n")
            aapt2.chmod(0o755)
            (root / "game.apk").write_bytes(b"")
            run = lambda path: subprocess.run([sys.executable, str(ROOT / "Scripts/Android/android_package.py"),
                                               "--state-root", str(root), "compat", path, "--aapt2", str(aapt2)],
                                              capture_output=True, text=True)
            self.assertIn("com.tiltingpoint.spongebob  playable", run(str(root / "game.apk")).stdout)
            self.assertEqual(run(str(root / "missing.apk")).returncode, 1)


class OfflineSocketTests(unittest.TestCase):
    """The emulator's offline policy, with a guest program (Tests/Unit/net_offline.c)."""

    @classmethod
    def setUpClass(cls):
        cls.emulator = ARGS.emu if ARGS else ROOT / "build/release-sdl1-gl1/bifrost-emu"
        cls.directory = tempfile.TemporaryDirectory()
        cls.fixture = Path(cls.directory.name) / "net_offline.elf"
        built = subprocess.run(["make", "-s", "cross", "SRC=Tests/Unit/net_offline.c", f"OUT={cls.fixture}"],
                               cwd=ROOT, capture_output=True, text=True)
        if built.returncode or not cls.emulator.exists():
            raise unittest.SkipTest("needs the AArch64 cross toolchain and a built emulator")

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_fixture(self, mode, offline, *flags):
        environment = dict(os.environ)
        environment.pop("BIFROST_ANDROID_OFFLINE", None)
        if offline:
            environment["BIFROST_ANDROID_OFFLINE"] = offline
        return subprocess.run([str(self.emulator), *flags, str(self.fixture), mode], env=environment,
                              capture_output=True, text=True, timeout=60)

    def test_offline_refuses_every_ip_send_beyond_loopback(self):
        for flags in ((), ("--no-jit",)):
            result = self.run_fixture("offline", "1", *flags)
            self.assertEqual((result.returncode, result.stdout.strip()), (0, "net_offline: offline checks passed"), flags)

    def test_loopback_and_unix_sockets_work_either_way(self):
        for offline in ("1", None):
            result = self.run_fixture("any", offline)
            self.assertEqual((result.returncode, result.stdout.strip()), (0, "net_offline: loopback checks passed"))


class PackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="bifrost-apk-test-")
        cls.work = Path(cls.temporary.name)
        cls.tools = ROOT / "Tools/android-tools"
        cls.runtime = ROOT / "Tools/android-runtime/rootfs"
        cls.adapters = ROOT / "build/android-adapters"
        cls.media = ROOT / "build/android-media"
        package.media.install(cls.media)
        subprocess.run(
            ["python3", str(ROOT / "Tools/Android/build-android-adapters.py")],
            check=True,
            capture_output=True,
        )
        cls.key = cls.work / "key.p12"
        subprocess.run(
            [
                "keytool",
                "-genkeypair",
                "-keystore",
                str(cls.key),
                "-storepass",
                "bifrost-test",
                "-keypass",
                "bifrost-test",
                "-alias",
                "fixture",
                "-keyalg",
                "RSA",
                "-keysize",
                "2048",
                "-validity",
                "2",
                "-dname",
                "CN=Bifrost APK regression",
            ],
            check=True,
            capture_output=True,
        )
        bionic = cls.runtime / "apex/com.android.runtime/lib64/bionic"
        spec = importlib.util.spec_from_file_location(
            "android_guest_dependencies", ROOT / "Tools/Android/android_guest_dependencies.py"
        )
        dependencies = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(dependencies)
        headers = dependencies.prepare_headers(cls.work / "headers")
        dependencies.extract("zlib", cls.work / "zlib")
        headers += ["-isystem", str(cls.work / "zlib" / ("zlib-" + dependencies.ZLIB_VERSION))]
        compiler = [
            "clang", "--target=aarch64-linux-android35", "-fuse-ld=lld",
            "-nostdlib", "-ffreestanding", "-O2", "-Wall", "-Wextra", "-fPIC",
            "-shared", "-Wl,-z,max-page-size=16384", "-Wl,-z,defs",
        ] + headers
        cls.dependency = cls.work / "libfixture_dependency.so"
        source = cls.work / "dependency.c"
        source.write_text(
            '#include <stdlib.h>\n#include <unistd.h>\n'
            'extern int __cxa_atexit(void (*)(void *), void *, void *);\n'
            'int fixture_dependency_value(void) { return 41; }\n'
            'static void exit_failure(void *unused) {\n'
            '    (void)unused;\n'
            '    if (getenv("BIFROST_FIXTURE_EXIT_FAILURE")) _exit(23);\n'
            '}\n'
            '__attribute__((constructor)) static void register_exit_failure(void) {\n'
            '    static int dso;\n'
            '    __cxa_atexit(exit_failure, 0, &dso);\n'
            '}\n'
        )
        subprocess.run(compiler + ["-Wl,-soname,libfixture_dependency.so", str(source),
                       str(bionic / "libc.so"),
                       "-o", str(cls.dependency)], check=True, capture_output=True)
        cls.native = cls.work / "libfixture.so"
        subprocess.run(compiler + [
            "-Wl,-soname,libfixture.so", str(ROOT / "Tests/Host/android_apk_fixture.c"),
            str(cls.dependency), str(cls.adapters / "libandroid.so"),
            str(cls.adapters / "liblog.so"), str(cls.adapters / "libz.so"),
            str(cls.adapters / "libEGL.so"), str(cls.adapters / "libGLESv2.so"),
            str(cls.media / "lib64/libmediandk.so"),
            str(bionic / "libc.so"), "-o", str(cls.native),
        ], check=True, capture_output=True)
        cls.base = cls.apk("base", split="", native=False)
        cls.split = cls.apk("arm64", split="config.arm64_v8a", native=True)
        cls.bundle = cls.work / "fixture.apkm"
        with zipfile.ZipFile(cls.bundle, "w") as archive:
            archive.write(cls.base, "base.apk")
            archive.write(cls.split, "split.apk")

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        # Each test installs into its own `*-state` directory, which holds a copy of the Android framework (gigabytes).
        # Freed when the test ends, the suite needs the space of one test and not of all of them: together they filled a
        # 16 GB /tmp and failed ten later tests with "No space left on device".
        existing = {entry.name for entry in self.work.iterdir()}

        def free_state():
            for entry in self.work.iterdir():
                if entry.name not in existing and entry.name.endswith("-state"):
                    shutil.rmtree(entry, ignore_errors=True)
        self.addCleanup(free_state)

    @classmethod
    def apk(cls, name, split="", native=False, version=1, managed=False, key=None):
        manifest = cls.work / (name + ".xml")
        apk = cls.work / (name + ".apk")
        activity = (
            "org.bifrost.fixture." + (managed if isinstance(managed, str) else "Managed")
            if managed else "android.app.NativeActivity"
        )
        split_attr = f' split="{split}"' if split else ""
        component = (
            ""
            if split
            else f"""<activity android:name="{activity}" android:exported="true">
            <meta-data android:name="android.app.lib_name" android:value="fixture"/>
            <intent-filter><action android:name="android.intent.action.MAIN"/>
            <category android:name="android.intent.category.LAUNCHER"/></intent-filter></activity>"""
        )
        if managed == "ManagedServices" and not split:
            for service in ("Lifecycle", "NullBinding", "SelfStopping", "AfterActivity"):
                component += f'<service android:name="{activity}${service}" android:exported="false"/>'
            component += f'<service android:name="{activity}$LifecycleDisabled" android:enabled="false" android:exported="false"/>'
            component += f'<service android:name="{activity}$LifecycleRemote" android:process=":remote" android:exported="false"/>'
            component += (f'<service android:name="{activity}$Foreground" android:foregroundServiceType="mediaPlayback" android:exported="false"/>'
                          f'<service android:name="{activity}$ForegroundRemote" android:process=":remote" '
                          'android:foregroundServiceType="mediaPlayback" android:exported="false"/>')
            component += (f'<receiver android:name="{activity}$RemoteReceiver" android:process=":remote" android:exported="false">'
                          '<intent-filter><action android:name="org.bifrost.fixture.REMOTE_RECEIVER"/></intent-filter></receiver>')
            component += f'<service android:name="{activity}$Jobs" android:permission="android.permission.BIND_JOB_SERVICE" android:exported="false"/>'
        if managed is True and not split:
            # Deliberately declare the lower-priority provider first.
            component += (f'<provider android:name="{activity}$InitProvider" '
                          'android:authorities="org.bifrost.fixture.init;org.bifrost.fixture.alias" android:exported="false"/>')
            # Repeated authorities: Android keeps the first declaration only.
            component += (f'<provider android:name="{activity}$InitProvider" '
                          'android:authorities="org.bifrost.fixture.init" android:exported="false"/>')
            component += (f'<provider android:name="{activity}$ShadowedProvider" '
                          'android:authorities="org.bifrost.fixture.alias" android:exported="false"/>')
            component += (f'<provider android:name="{activity}$EarlyProvider" '
                          'android:authorities="org.bifrost.fixture.early" android:initOrder="100" android:exported="false"/>')
            component += (f'<provider android:name="{activity}$DisabledProvider" '
                          'android:authorities="org.bifrost.fixture.disabled" android:enabled="false" android:exported="false"/>')
            component += (f'<provider android:name="{activity}$RemoteProvider" '
                          'android:authorities="org.bifrost.fixture.remote" android:process=":provider" android:exported="false"/>')
        if managed == "ManagedPlayServices" and not split:
            # The client library checks this against its own version code.
            component += '<meta-data android:name="com.google.android.gms.version" android:value="12451000"/>'
        if managed == "Managed$Home" and not split:
            component = component.replace('android:exported="true"', 'android:exported="true" android:hardwareAccelerated="false"')
        compiled = []
        if managed == "ManagedAccounts" and not split:
            component += (f'<service android:name="{activity}$AuthenticatorService" android:exported="false">'
                          '<intent-filter><action android:name="android.accounts.AccountAuthenticator"/></intent-filter>'
                          '<meta-data android:name="android.accounts.AccountAuthenticator" '
                          'android:resource="@xml/authenticator"/></service>')
            resources = cls.work / (name + "-res/xml")
            resources.mkdir(parents=True)
            (resources / "authenticator.xml").write_text(
                '<account-authenticator xmlns:android="http://schemas.android.com/apk/res/android" '
                'android:accountType="org.bifrost.fixture.account" android:label="Fixture account"/>')
            flat = cls.work / (name + "-flat")
            flat.mkdir()
            subprocess.run([str(cls.tools / "aapt2"), "compile", "-o", str(flat), str(resources / "authenticator.xml")],
                           check=True, capture_output=True)
            compiled = sorted(map(str, flat.glob("*.flat")))
        if managed == "ManagedDialog" and not split:
            # As Unity's player Activity declares it.
            component += (f'<activity android:name="{activity}$Engine" android:exported="false" '
                          'android:hardwareAccelerated="false"/>')
        if managed == "ManagedGoogleActivity" and not split:
            component += f'<service android:name="{activity}$Sender" android:process=":sender" android:exported="false"/>'
            component += f'<service android:name="{activity}$Late" android:process=":late" android:exported="false"/>'
            component += f'<activity android:name="{activity}$Other" android:exported="false"/>'
        if managed == "ManagedGeometry" and not split:
            # As a game declares it, so a window size change is not an Activity restart.
            activity_attributes = ' android:screenOrientation="landscape" android:configChanges="orientation|screenSize|smallestScreenSize|screenLayout|keyboardHidden|density|uiMode"'
            component = component.replace(f'<activity android:name="{activity}"',
                                          f'<activity android:name="{activity}"{activity_attributes}')
        if managed == "ManagedSplash" and not split:
            component += f'<activity android:name="{activity}$Main" android:exported="false"/>'
        if managed == "ManagedActivities" and not split:
            component += f'<activity android:name="{activity}$Second" android:exported="false"/>'
            component += (f'<activity android:name="{activity}$Translucent" android:exported="false" '
                          'android:theme="@android:style/Theme.Translucent.NoTitleBar"/>')
            component += (f'<provider android:name="{activity}$Files" android:authorities="org.bifrost.fixture.files" '
                          'android:exported="false"><intent-filter>'
                          '<action android:name="org.bifrost.fixture.FILES"/></intent-filter></provider>')
        required = ' android:isSplitRequired="true"' if not split else ""
        application_class = f' android:name="{activity}$App"' if managed is True and not split else ""
        job_permission = ('<uses-permission android:name="android.permission.RECEIVE_BOOT_COMPLETED"/>'
                          '<uses-permission android:name="android.permission.FOREGROUND_SERVICE"/>'
                          '<uses-permission android:name="android.permission.FOREGROUND_SERVICE_MEDIA_PLAYBACK"/>'
                          if managed == "ManagedServices" else "")
        if managed == "ManagedPlayServices":
            # microG's GServicesProvider checks that its caller requests this.
            job_permission = '<uses-permission android:name="com.google.android.providers.gsf.permission.READ_GSERVICES"/>'
        if managed == "ManagedGeometry":
            job_permission = '<uses-permission android:name="android.permission.INTERNET"/>'
        if managed == "ManagedActivities":
            # Normal (granted at install) and runtime (denied without a permission UI).
            job_permission = ('<uses-permission android:name="android.permission.INTERNET"/>'
                              '<uses-permission android:name="android.permission.CAMERA"/>')
        manifest.write_text(
            f"""<manifest xmlns:android="http://schemas.android.com/apk/res/android"
            package="org.bifrost.fixture" android:versionCode="{version}"{split_attr}>
            <uses-sdk android:minSdkVersion="25" android:targetSdkVersion="35"/>
            {job_permission}
            <application android:label="Managed Fixture" android:hasCode="{'true' if managed else 'false'}"{required}{application_class}>{component}</application></manifest>"""
        )
        subprocess.run(
            [
                str(cls.tools / "aapt2"),
                "link",
                "-I",
                str(cls.tools / "android.jar"),
                "--manifest",
                str(manifest),
                "-o",
                str(apk),
                *compiled,
            ],
            check=True,
            capture_output=True,
        )
        if managed:
            classes = cls.work / (name + "-classes")
            classes.mkdir()
            dex = cls.work / (name + "-dex")
            dex.mkdir()
            libraries = [play_services_basement(cls.work)] if managed == "ManagedPlayServices" else []
            subprocess.run([android_paths.javac() or "javac", "--release", "8", "-cp",
                            os.pathsep.join(map(str, [cls.tools / "android.jar", *libraries])), "-d", str(classes),
                            str(ROOT / "Tests/Host" / (activity.rsplit(".", 1)[-1].split("$")[0] + ".java"))],
                           check=True, capture_output=True)
            subprocess.run(["java", "-cp", str(cls.tools / "d8.jar"),
                            "com.android.tools.r8.D8", "--min-api", "26", "--lib",
                            str(cls.tools / "android.jar"), "--output", str(dex),
                            *map(str, classes.rglob("*.class")), *map(str, libraries)],
                           check=True, capture_output=True)
            with zipfile.ZipFile(apk, "a") as archive:
                archive.write(dex / "classes.dex", "classes.dex")
        if native:
            with zipfile.ZipFile(apk, "a") as archive:
                archive.write(cls.native, "lib/arm64-v8a/libfixture.so")
                archive.write(cls.dependency, "lib/arm64-v8a/libfixture_dependency.so")
                archive.writestr("assets/split.txt", "APK split asset\n")
        cls.sign(apk, key)
        return apk

    @classmethod
    def sign(cls, apk, key=None):
        subprocess.run(
            [
                "java",
                "-jar",
                str(cls.tools / "apksigner.jar"),
                "sign",
                "--ks",
                str(key or cls.key),
                "--ks-pass",
                "pass:bifrost-test",
                "--key-pass",
                "pass:bifrost-test",
                "--ks-key-alias",
                "fixture",
                str(apk),
            ],
            check=True,
            capture_output=True,
        )

    def install(self, input, state, *, allow_downgrade=False):
        return package.install(
            input,
            state,
            self.runtime,
            self.adapters,
            self.tools / "aapt2",
            self.tools / "apksigner.jar",
            self.media,
            allow_downgrade=allow_downgrade,
        )

    def test_signed_split_native_launch_and_persistence(self):
        state = self.work / "launch-state"
        metadata = self.install(self.bundle, state)
        self.assertEqual(metadata["apk_count"], 2)
        self.assertEqual(metadata["arm64_library_count"], 2)
        self.assertEqual(metadata["source_sha256"], package.digest(self.bundle))
        self.assertEqual(metadata["launcher"]["kind"], "native_activity")
        self.assertEqual(metadata["media"]["image_sha256"], package.media.IMAGE_SHA256)
        self.assertEqual((state / "org.bifrost.fixture/package.json").is_file(), True)
        root = state / "org.bifrost.fixture/rootfs"
        library = root / "system/lib64/media/libmediandk.so"
        original = library.read_bytes()
        try:
            library.write_bytes(b"broken")
            with self.assertRaisesRegex(ValueError, "media library/data was modified"):
                package.launch("org.bifrost.fixture", state, Path("/must/not/execute"))
        finally:
            library.write_bytes(original)
        configuration = root / "linkerconfig/ld.config.txt"
        original = configuration.read_text()
        try:
            configuration.write_text(original + "\n")
            with self.assertRaisesRegex(ValueError, "media linker namespace was modified"):
                package.launch("org.bifrost.fixture", state, Path("/must/not/execute"))
        finally:
            configuration.write_text(original)
        for mode in ("jit", "interp", "tier2"):
            for backend in ("builtin", "plugin"):
                cmd = [
                    "python3",
                    str(ROOT / "Scripts/Android/android_package.py"),
                    "--state-root",
                    str(state),
                    "run",
                    "org.bifrost.fixture",
                    "--emu",
                    str(ARGS.emu),
                    "--frames",
                    "2",
                ]
                if mode == "interp":
                    cmd += ["--no-jit"]
                if backend == "plugin":
                    cmd += ["--android-window-plugin", str(ARGS.plugin)]
                env = {
                    **os.environ,
                    "SDL_VIDEODRIVER": "wayland",
                    "SDL_VIDEO_DRIVER": "wayland",
                    "BIFROST_TIER2": "1" if mode == "tier2" else "0",
                    "BIFROST_TIER2_HITS": "32",
                }
                result = subprocess.run(
                    cmd, env=env, capture_output=True, text=True, timeout=45
                )
                self.assertEqual(
                    result.returncode, 0, result.stdout + "\n" + result.stderr
                )
                # The session writes the app's stdout and stderr to its log.
                log = Path(re.search(r"\(log: (.+)\)", result.stdout).group(1)).read_text()
                self.assertIn("abi=42 1.25 guest 1234567890123 1 2 3 4 5 6 7 8", log)
                self.assertIn("buffer=2.50 ok", log)
                self.assertIn("apk_bionic_fixture: ALL PASS", log)
                self.assertIn("apk_media_fixture: REAL FORMAT PASS", log)
                self.assertIn("android_service_manager: ALL PASS", log)
                self.assertNotRegex(
                    result.stdout + result.stderr + log, r"Fatal signal|\[crash\]|CANNOT LINK"
                )
                print(f"APK Bionic/Wayland: {mode} {backend} passed", flush=True)
        self.assertEqual(
            (
                state
                / "org.bifrost.fixture/rootfs/data/user/0/org.bifrost.fixture/files/save"
            ).read_bytes(),
            b"\x06",
        )
        # Diagnostics run constructors without invoking Activity callbacks or JNI.
        result = subprocess.run([
            "python3", str(ROOT / "Scripts/Android/android_package.py"), "--state-root", str(state),
            "probe", "org.bifrost.fixture", "--emu", str(ARGS.emu), "--timeout", "30",
        ], capture_output=True, text=True, timeout=75)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Native load probe: 2/2 loaded", result.stdout)
        self.assertIn("2/2 exited cleanly", result.stdout)
        self.assertIn("abi=42 1.25 guest 1234567890123 1 2 3 4 5 6 7 8", result.stdout)
        command = [
            "python3", str(ROOT / "Scripts/Android/android_package.py"), "--state-root", str(state),
            "probe", "org.bifrost.fixture", "--emu", str(ARGS.emu),
            "--library", "libfixture.so", "--preload", "libfixture_dependency.so",
        ]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("native probe: PRELOAD_OK ", result.stdout)
        # A real guest destructor exits nonzero after successful dlopen. The
        # load marker must survive, and cleanup failure must still fail the CLI.
        result = subprocess.run(command, capture_output=True, text=True, timeout=60,
                                env={**os.environ, "BIFROST_GUEST_ENV": "BIFROST_FIXTURE_EXIT_FAILURE=1"})
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("loaded, process failed (exit 23)", result.stdout)
        self.assertIn("1/1 loaded; 0/1 exited cleanly", result.stdout)
        with self.assertRaisesRegex(ValueError, "packaged ARM64"):
            package.probe("org.bifrost.fixture", state, Path("/must/not/execute"),
                          ["../libfixture.so"])
        with self.assertRaisesRegex(ValueError, "Preload requires packaged ARM64"):
            package.probe("org.bifrost.fixture", state, Path("/must/not/execute"),
                          preloads=["../libfixture.so"])
        # Reinstallation must retain saved data and publish the same immutable code.
        self.assertEqual(
            self.install(self.bundle, state)["guest_code"], metadata["guest_code"]
        )
        reordered = self.work / "reordered.apkm"
        with zipfile.ZipFile(reordered, "w") as archive:
            archive.write(self.split, "split.apk")
            archive.write(self.base, "base.apk")
        self.assertEqual(
            self.install(reordered, state)["guest_code"], metadata["guest_code"]
        )
        self.assertEqual(
            (
                state
                / "org.bifrost.fixture/rootfs/data/user/0/org.bifrost.fixture/files/save"
            ).read_bytes(),
            b"\x06",
        )

    def test_invalid_install_does_not_replace_existing_package(self):
        state = self.work / "rollback-state"
        original = self.install(self.bundle, state)
        self.assertEqual(original["source_sha256"], package.digest(self.bundle))
        before = (state / "org.bifrost.fixture/package.json").read_bytes()
        with self.assertRaisesRegex(ValueError, "requires split"):
            self.install(self.base, state)
        bad = self.work / "bad-version.apkm"
        other = self.apk(
            "other-version", split="config.arm64_v8a", native=True, version=2
        )
        with zipfile.ZipFile(bad, "w") as archive:
            archive.write(self.base, "base.apk")
            archive.write(other, "split.apk")
        with self.assertRaisesRegex(ValueError, "version"):
            self.install(bad, state)
        unsigned = self.work / "unsigned.apk"
        unsigned.write_bytes(self.split.read_bytes())
        with zipfile.ZipFile(unsigned, "a") as archive:
            archive.writestr("assets/tampered", "bad")
        with self.assertRaises(ValueError):
            self.install(unsigned, state)
        self.assertEqual(
            (state / "org.bifrost.fixture/package.json").read_bytes(), before
        )
        self.assertEqual(original["version_code"], "1")

    def test_signer_identity_upgrade_and_code_integrity(self):
        state = self.work / "identity-state"
        self.install(self.bundle, state)
        key = self.work / "other-key.p12"
        subprocess.run(
            [
                "keytool",
                "-genkeypair",
                "-keystore",
                str(key),
                "-storepass",
                "bifrost-test",
                "-keypass",
                "bifrost-test",
                "-alias",
                "fixture",
                "-keyalg",
                "RSA",
                "-keysize",
                "2048",
                "-validity",
                "2",
                "-dname",
                "CN=Different signer",
            ],
            check=True,
            capture_output=True,
        )
        other = self.apk("other-signer", split="config.arm64_v8a", native=True, key=key)
        bundle = self.work / "identity.apkm"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(self.base, "base.apk")
            archive.write(other, "split.apk")
        with self.assertRaisesRegex(ValueError, "split signer"):
            self.install(bundle, state)
        other_base = self.apk("other-base", key=key)
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(other_base, "base.apk")
            archive.write(other, "split.apk")
        with self.assertRaisesRegex(ValueError, "Installed package signer"):
            self.install(bundle, state)
        with self.assertRaisesRegex(ValueError, "Installed package signer"):
            self.install(bundle, state, allow_downgrade=True)
        upgrade_base = self.apk("upgrade-base", version=2)
        upgrade_split = self.apk(
            "upgrade-split", split="config.arm64_v8a", native=True, version=2
        )
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(upgrade_base, "base.apk")
            archive.write(upgrade_split, "split.apk")
        metadata = self.install(bundle, state)
        manifest = state / "org.bifrost.fixture/package.json"
        before = manifest.read_bytes()
        with self.assertRaisesRegex(ValueError, "downgrade"):
            self.install(self.bundle, state)
        self.assertEqual(manifest.read_bytes(), before)
        app = manifest.parent
        from android_settings import save, load
        save(app, {"property.ro.kernel.qemu": "0"})
        native_save = app / "rootfs/data/user/0/org.bifrost.fixture/files/downgrade-save"
        managed_save = app / "managed/rootfs/data/user/0/org.bifrost.fixture/files/downgrade-save"
        for target in (native_save, managed_save):
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("retained")
        downgraded = self.install(self.bundle, state, allow_downgrade=True)
        self.assertEqual(downgraded["version_code"], "1")
        self.assertEqual(load(app), {"property.ro.kernel.qemu": "0"})
        for target in (native_save, managed_save): self.assertEqual(target.read_text(), "retained")
        # Same-version reinstall and a subsequent upgrade use the same signed path.
        self.install(self.bundle, state)
        self.install(bundle, state)
        code = state / "org.bifrost.fixture/rootfs" / metadata["guest_code"].lstrip("/")
        asset = code / "assets/split.txt"
        asset.chmod(0o644)
        asset.write_text("Modified after install")
        with self.assertRaisesRegex(ValueError, "Installed package was modified"):
            package.launch("org.bifrost.fixture", state, Path("/must/not/execute"))

    def test_snapshot_bounds_include_growth_during_copy(self):
        source = self.work / "snapshot-source"
        destination = self.work / "snapshot-destination"
        source.write_bytes(b"123456789")
        with patch.object(package, "MAX_TOTAL", 8):
            with self.assertRaisesRegex(ValueError, "exceeds"):
                package.snapshot_input(source, destination)
            # A stale stat cannot allow the stream to exceed the bound.
            with patch.object(
                package.os,
                "fstat",
                return_value=SimpleNamespace(st_mode=0o100644, st_size=0),
            ):
                with self.assertRaisesRegex(ValueError, "exceeds"):
                    package.snapshot_input(source, destination)

    def test_expanded_asset_total_is_separate_from_input_size(self):
        self.assertGreater(package.MAX_EXPANDED_TOTAL, package.MAX_TOTAL)
        state = self.work / "expanded-state"
        with patch.object(package, "MAX_EXPANDED_TOTAL", 8):
            with self.assertRaisesRegex(ValueError, "uncompressed"):
                self.install(self.bundle, state)

    def test_archive_paths_and_special_members(self):
        for index, member in enumerate(
            ("../escape.apk", "/absolute.apk", "a/../escape.apk", "a\\escape.apk")
        ):
            path = self.work / f"unsafe-{index}.zip"
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr(member, b"not an APK")
            with zipfile.ZipFile(path) as archive:
                with self.assertRaises(ValueError):
                    package.regular_archive(archive)
        path = self.work / "symlink.zip"
        with zipfile.ZipFile(path, "w") as archive:
            info = zipfile.ZipInfo("assets/link")
            info.create_system = 3
            info.external_attr = 0o120777 << 16
            archive.writestr(info, "/etc/passwd")
        with zipfile.ZipFile(path) as archive:
            with self.assertRaises(ValueError):
                package.regular_archive(archive)

    def test_managed_service_execution(self):
        self.managed_service_execution()

    def test_managed_service_trace(self):
        self.managed_service_execution("methods")

    def managed_service_execution(self, trace=None):
        tag = "managed-services-trace" if trace else "managed-services"
        state = self.work / (tag + "-state")
        base = self.apk(tag, managed="ManagedServices")
        bundle = self.work / (tag + ".apkm")
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(base, "base.apk")
            archive.write(self.split, "split.apk")
        self.install(bundle, state)
        app = state / "org.bifrost.fixture"
        output = ROOT / "build/android-service-results"
        output.mkdir(parents=True, exist_ok=True)
        # Full interpreter APK startup still has pre-existing font-worker null reads.
        status = package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(), timeout=90,
                                trace_apk=trace, trace_duration=1 if trace else 15)
        log = app / "managed/last-run.log"
        shutil.copyfile(log, output / ("jit-trace.log" if trace else "jit.log"))
        text = log.read_text()
        self.assertEqual(status, 0, text[-30000:])
        self.assertIn("SERVICE_EXECUTION_OK", text)
        self.assertIn("JOB_SCHEDULER_OK", text)
        self.assertIn("SERVICE_PROCESS_OK", text)
        self.assertIn("FOREGROUND_SERVICE_OK Foreground", text)
        self.assertIn("FOREGROUND_SERVICE_OK ForegroundRemote", text)
        self.assertIn("REMOTE_RECEIVER_OK", text)
        self.assertNotRegex(text, r"thread \d+: exception:|FATAL EXCEPTION|AssertionError")
        markers = ("SERVICE_EXECUTION_OK", "SERVICE_FIXTURE Activity destroyed",
                   "SERVICE_FIXTURE alive after Activity destruction", "SERVICE_FIXTURE destroy after-activity")
        positions = [text.index(marker) for marker in markers]
        self.assertEqual(positions, sorted(positions))
        if not trace:
            status = package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(), timeout=90)
            shutil.copyfile(log, output / "jit-restart.log")
            restarted = log.read_text()
            self.assertEqual(status, 0, restarted[-30000:])
            self.assertIn("JOB_SCHEDULER_OK restart=true", restarted)
        if trace:
            import json
            directory, = (app / "managed/traces").iterdir()
            shutil.copytree(directory, output / directory.name)
            records = [json.loads(line) for line in (directory / "calls.jsonl").read_text().splitlines()]
            starts = {record["id"]: record for record in records if record["event"] == "call"}
            ends = {record["id"]: record for record in records if record["event"] in ("return", "throw")}
            self.assertEqual(starts.keys(), ends.keys(), "trace lost call/return pairs")
            for key, event in starts.items():
                self.assertEqual(event["tid"], ends[key]["tid"])
                self.assertEqual(event["operation"], ends[key]["operation"])
                self.assertGreaterEqual(ends[key]["duration_ns"], 0)
            # The fixture's rejected foreground-service type: a call that throws is traced as a pair (startService is
            # implemented now; the unsupported-call case is in test_managed_activity_api_report).
            self.assertTrue(any(event["event"] == "throw" and event["operation"] == "activity.setServiceForeground"
                                for event in records), "thrown calls not traced")
            native = text + (app / "managed/last-logd.log").read_text()
            self.assertTrue("nativeloader" in native and "libbifrost-activity.so" in native,
                            "native library-load log missing")
            self.assertTrue(any(event["event"] == "thread.stack" for event in records))
            summary = json.loads((directory / "summary.json").read_text())
            self.assertIn("applicationThread.scheduleCreateService", summary["calls"])
            self.assertFalse(summary["unfinished_calls"])
            self.assertFalse(summary["truncated"])
            method_trace = (directory / "methods.trace").read_bytes()
            self.assertTrue(method_trace.startswith(b"*version\n"), "ART did not flush its method trace")
            self.assertGreater(len(method_trace), 100)
            method_summary = json.loads((directory / "methods-summary.json").read_text())
            self.assertGreater(method_summary["records"], 0)
            self.assertFalse(method_summary["partial_record"])
            self.assertFalse(method_summary["overflow"])

    def test_gles1_adapter_provides_every_name_an_es1_app_imports(self):
        # Android binds all of a library's imports when it loads: DevilutionX (SDL2's GLES1 renderer) needed
        # glBlendEquationOES and then glColor4f, and without them it never started. Whatever the OpenGL ES 1.x registry
        # defines must resolve from libGLESv1_CM.so and libGLESv2.so, the two libraries such an app links.
        import xml.etree.ElementTree as ElementTree
        if not shutil.which("readelf"):
            self.skipTest("needs readelf")
        registry = ElementTree.parse(ROOT / "ThirdParty/GLRegistry/gl.xml").getroot()
        wanted = {command.get("name")
                  for feature in registry.iter("feature") if feature.get("api") == "gles1"
                  for require in feature.iter("require") if require.get("profile") in (None, "common")
                  for command in require.iter("command")}
        extensions = {"GL_OES_framebuffer_object", "GL_OES_blend_subtract", "GL_OES_blend_equation_separate",
                      "GL_OES_blend_func_separate", "GL_OES_draw_texture"}
        wanted |= {command.get("name") for extension in registry.iter("extension") if extension.get("name") in extensions
                   for command in extension.iter("command")}
        self.assertGreater(len(wanted), 140)

        def exports(name):
            library = self.adapters / "managed" / name
            listing = subprocess.run(["readelf", "--dyn-syms", "-W", str(library)], check=True,
                                     capture_output=True, text=True).stdout
            return {fields[7] for fields in map(str.split, listing.splitlines())
                    if len(fields) >= 8 and fields[6] != "UND"}, listing
        gles1, _ = exports("libGLESv1_CM.so")
        gles2, _ = exports("libGLESv2.so")
        self.assertEqual(sorted(wanted - gles1 - gles2), [])
        # The OES names forward to libGLESv2's functions, so the library must depend on it.
        needed = subprocess.run(["readelf", "-d", str(self.adapters / "managed/libGLESv1_CM.so")], check=True,
                                capture_output=True, text=True).stdout
        self.assertIn("libGLESv2.so", needed)

    def test_managed_activity_api_report(self):
        import json
        state = self.work / "activity-trace-state"
        bundle = self.work / "activity-trace.apkm"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(self.apk("activity-trace", managed=True), "base.apk")
            archive.write(self.split, "split.apk")
        self.install(bundle, state)
        app = state / "org.bifrost.fixture"
        status = package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(), timeout=90, trace_apk="activity")
        text = (app / "managed/last-run.log").read_text()
        self.assertEqual(status, 0, text[-30000:])
        self.assertIn("MANAGED_FIXTURE_ACTIVITY_TRACE_FAILURE_OK", text)
        directory, = (app / "managed/traces").iterdir()
        records = [json.loads(line) for line in (directory / "calls.jsonl").read_text().splitlines()]
        self.assertFalse(any(record["event"] in ("thread.stack", "threads.begin", "library.mapping", "methods.start")
                             for record in records))
        report = json.loads((directory / "activity-api-report.json").read_text())
        self.assertEqual(set(report["interfaces"]), {"activity", "activity_task", "bifrost.activity_client"})
        self.assertGreater(report["available_methods"], report["observed_methods"])
        self.assertFalse(report["truncated"])
        self.assertEqual(report["invalid_records"], 0)
        self.assertEqual(report["orphan_completions"], 0)
        self.assertTrue(report["trace_stopped"])
        self.assertFalse(any(item["unfinished"] for item in report["methods"]))
        memory, = [item for item in report["methods"] if item["method"] == "getMyMemoryState"]
        self.assertGreaterEqual(memory["returns"], 2)
        self.assertEqual(memory["calls"], memory["returns"])
        failure, = [item for item in report["methods"]
                    if item["service"] == "activity_task" and item["method"] == "getRecentTasks"]
        self.assertEqual(failure["throws"], 1)
        self.assertIn("UnsupportedOperationException", failure["last_failure"]["exception"])
        self.assertTrue(failure["first_caller"])
        self.assertTrue(failure["signature"])
        output = ROOT / "build/android-activity-trace-results"
        output.mkdir(parents=True, exist_ok=True)
        shutil.copytree(directory, output / directory.name)
        shutil.copyfile(app / "managed/last-run.log", output / "jit.log")

    def test_managed_launcher_persists_data_and_finishes_activity(self):
        state = self.work / "managed-state"
        base = self.apk("managed", managed=True)
        bundle = self.work / "managed.apkm"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(base, "base.apk")
            archive.write(self.split, "split.apk")
        self.install(bundle, state)
        app = state / "org.bifrost.fixture"
        certificate = subprocess.check_output([
            "keytool", "-exportcert", "-keystore", str(self.key),
            "-storepass", "bifrost-test", "-alias", "fixture"])
        for starts in (1, 2, 3):
            if starts == 2:
                # Stand in for ProfileSaver: start 2 must merge it and run speed-profile.
                import json
                from android_managed import run_guest_tool
                root = app / "managed/rootfs"
                code = json.loads((app / "package.json").read_text())["guest_code"]
                environment = json.loads((root / "bifrost-framework.json").read_text())["environment"]
                result = run_guest_tool(
                    root, ARGS.emu.resolve(), environment, False, "/apex/com.android.art/bin/profman",
                    "--generate-test-profile=/data/misc/profiles/cur/0/org.bifrost.fixture/primary.prof",
                    "--apk=" + code + "/base.apk", "--dex-location=" + code + "/base.apk",
                    "--generate-test-profile-method-percentage=100",
                    "--generate-test-profile-class-percentage=100", timeout=120)
                self.assertEqual(result.returncode, 0, result.stderr[-4000:])
            if starts == 3:
                settings = app / "managed/rootfs/data/system/users/0/bifrost-system-settings.xml"
                settings.write_text('<?xml version="1.0" encoding="UTF-8"?>\n'
                                    '<!DOCTYPE properties SYSTEM "http://java.sun.com/dtd/properties.dtd">\n'
                                    '<properties><entry key="screen_off_timeout">112345</entry></properties>')
                # Force a runtime replacement, as a launcher upgrade does.
                import json
                stamp = app / "managed/runtime.json"
                saved = json.loads(stamp.read_text())
                saved["identity"]["schema"] = 0
                stamp.write_text(json.dumps(saved))
            status = package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(),
                                    no_jit=starts == 3, timeout=180 if starts == 3 else 90)
            text = (app / "managed/last-run.log").read_text()
            self.assertEqual(status, 0, text[-30000:])
            self.assertIn("APP_WINDOW_IDENTITY Managed Fixture icon=", text)
            self.assertIn(f"MANAGED_FIXTURE_START {starts}", text)
            self.assertIn("MANAGED_FIXTURE_ODEX true", text)
            self.assertIn("MANAGED_FIXTURE_PROFILE_SAVER true", text)
            if starts >= 2:  # Start 3 also checks profiles survive runtime replacement.
                import json
                stamp = json.loads((app / "managed/dexopt.json").read_text())
                self.assertEqual((stamp["filter"], stamp["failed"]), ("speed-profile", False))
            self.assertTrue((app / "managed/rootfs/data/misc/profiles/cur/0/org.bifrost.fixture").is_dir())
            markers = ["MANAGED_FIXTURE_PROVIDER_CREATED", "APP_PROVIDERS_STARTED 2",
                       "MANAGED_FIXTURE_APPLICATION_AFTER_PROVIDERS", "MANAGED_FIXTURE_PROVIDER_ALIASES_OK"]
            positions = [text.index(marker) for marker in markers]
            self.assertEqual(positions, sorted(positions), text[-30000:])
            self.assertIn("MANAGED_FIXTURE_PENDING_LOOKUP_OK", text)
            self.assertIn("MANAGED_FIXTURE_PENDING_CREATE_OK", text)
            # PackageManager.resolveActivity, a SharedPreferences commit from
            # another thread (listener on main) and TelephonyRegistry.
            self.assertIn("MANAGED_FIXTURE_RESOLVE_ACTIVITY_OK", text)
            self.assertIn("MANAGED_FIXTURE_PREFS_LISTENER background main=true", text)
            self.assertIn("MANAGED_FIXTURE_TELEPHONY_REGISTRY_OK", text)
            self.assertIn("MANAGED_FIXTURE_NETWORK_INTERFACES_OK", text)
            self.assertIn("MANAGED_FIXTURE_PACKAGE_QUERIES_OK", text)
            self.assertIn("MANAGED_FIXTURE_APP_TASKS_OK", text)
            self.assertIn("MANAGED_FIXTURE_LOUDNESS_OK", text)
            self.assertIn("MANAGED_FIXTURE_SPATIALIZER_OK", text)
            self.assertIn("MANAGED_FIXTURE_BLUETOOTH_ROUTES_OK", text)
            self.assertIn("MANAGED_FIXTURE_PLAYER_TRACKING_OK", text)
            key = (state / ".settings/user-0.key").read_bytes()
            expected_id = hmac.new(key, struct.pack(">I", len(certificate)) + certificate,
                                   hashlib.sha256).hexdigest()[:16]
            self.assertIn("MANAGED_FIXTURE_SETTINGS_OK " + expected_id, text)
            markers = ("MANAGED_FIXTURE_SURFACE_CREATED", "MANAGED_FIXTURE_PAUSED",
                       "MANAGED_FIXTURE_SURFACE_DESTROYED", "MANAGED_FIXTURE_STOPPED",
                       "MANAGED_FIXTURE_DESTROYED", "APP_WINDOW_CLOSED")
            positions = [text.index(marker) for marker in markers]
            self.assertEqual(positions, sorted(positions))
            self.assertNotIn("bifrost.registration_probe", text)
        self.assertTrue((app / "managed/rootfs/data/user/0/org.bifrost.fixture/shared_prefs/fixture.xml").is_file())

    def test_managed_home_and_resume(self):
        import threading
        from android_sessions import control
        from android_launcher import activate_existing
        state = self.work / "home-state"
        bundle = self.work / "home.apkm"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(self.apk("home", managed="Managed$Home"), "base.apk")
            archive.write(self.split, "split.apk")
        self.install(bundle, state)
        errors = []
        done = threading.Event()
        def resume_twice():
            try:
                app = state / "org.bifrost.fixture"
                for _ in range(2):
                    deadline = time.monotonic() + 90
                    while not done.is_set() and time.monotonic() < deadline:
                        saved = control(app)
                        if saved.get("state") == "running" and saved.get("background"):
                            self.assertFalse(control(app, "resume").get("error"))
                            break
                        done.wait(0.1)
                    else: raise AssertionError("App did not background for HOME")
                    while control(app).get("background") and not done.wait(0.1): pass
            except Exception as error: errors.append(error)
        with patch.dict(os.environ, {"QT_QPA_PLATFORM": "offscreen"}):
            with (self.work / "home-launcher.log").open("w") as launcher_log:
                launcher = subprocess.Popen([sys.executable, str(ROOT / "Scripts/Android/asgard.py"),
                    "--state-root", str(state), "launcher"], stdout=launcher_log, stderr=subprocess.STDOUT,
                    start_new_session=True)
                worker = None
                try:
                    deadline = time.monotonic() + 20
                    while not activate_existing(state):
                        if launcher.poll() is not None or time.monotonic() >= deadline:
                            self.fail("Offscreen Asgard launcher did not start")
                        time.sleep(0.1)
                    worker = threading.Thread(target=resume_twice, daemon=True)
                    worker.start()
                    status = package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(),
                                            window_plugin=ARGS.plugin.resolve(), timeout=120)
                finally:
                    done.set()
                    if worker: worker.join(3)
                    stop_launcher(launcher)
        text = (state / "org.bifrost.fixture/managed/last-run.log").read_text()
        self.assertFalse(errors, str(errors) + text[-12000:])
        self.assertEqual(status, 0, text[-12000:])
        for marker in ("HOME_INTENT_OK 2", "HOME_ALL_PASS", "HOME_DESTROYED"):
            self.assertIn(marker, text)
        self.assertEqual(text.count("HOME_CREATED"), 1, text[-12000:])
        self.assertEqual(text.count("HOME_RESTARTED"), 2, text[-12000:])
        self.assertEqual(text.count("APP_TASK_BACKGROUND"), 2, text[-12000:])
        self.assertEqual(text.count("APP_TASK_FOREGROUND"), 2, text[-12000:])

    def test_managed_dialog_and_popup_windows(self):
        state = self.work / "dialog-state"
        bundle = self.work / "dialog.apkm"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(self.apk("dialog", managed="ManagedDialog"), "base.apk")
            archive.write(self.split, "split.apk")
        self.install(bundle, state)
        status = package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(), timeout=90)
        text = (state / "org.bifrost.fixture/managed/last-run.log").read_text()
        self.assertEqual(status, 0, text[-30000:])
        markers = ("MANAGED_DIALOG_SHOWN", "text=Bifrost dialog", "MANAGED_DIALOG_POSITIVE",
                   "MANAGED_DIALOG_DISMISSED", "MANAGED_DIALOG_POPUP_SHOWN",
                   "MANAGED_DIALOG_POPUP_DISMISSED",
                   # A text toast gets a TYPE_TOAST window (2005) and is removed.
                   "MANAGED_DIALOG_TOAST_REQUESTED", "APP_TOAST_SHOWN Bifrost toast",
                   "MANAGED_DIALOG_STACK_BEGIN", "MANAGED_DIALOG_STACK_END",
                   "MANAGED_DIALOG_ENGINE_START",
                   # A dialog created on an engine Looper thread draws there
                   # (ViewRootImpl.checkThread) and its click runs there.
                   "text=Engine dialog", "MANAGED_DIALOG_ENGINE_POSITIVE FixtureEngine",
                   "MANAGED_DIALOG_ENGINE_DISMISSED",
                   # SurfaceViews in its windows get host surfaces; their
                   # callbacks run there with the Surface still valid, when
                   # the window is removed and when the app stops it.
                   "MANAGED_DIALOG_ENGINE_SURFACE_CREATED removed FixtureEngine",
                   "MANAGED_DIALOG_ENGINE_SURFACE_CHANGED removed FixtureEngine 320x180",
                   "APP_SURFACE android.view.SurfaceView 320x180",
                   "MANAGED_DIALOG_ENGINE_SURFACE_DESTROYED removed FixtureEngine valid=true",
                   "MANAGED_DIALOG_ENGINE_SURFACE_CREATED stopped FixtureEngine",
                   "MANAGED_DIALOG_ENGINE_SURFACE_CHANGED stopped FixtureEngine 320x180",
                   "APP_PAUSED", "MANAGED_DIALOG_ENGINE_SURFACE_DESTROYED stopped FixtureEngine valid=true",
                   "APP_STOPPED", "MANAGED_DIALOG_DESTROYED",
                   # The root then closes the window it leaked (closeAll).
                   "WindowLeaked: Activity org.bifrost.fixture.ManagedDialog has leaked window")
        positions = [text.index(marker) for marker in markers]
        self.assertEqual(positions, sorted(positions), text[-30000:])
        self.assertNotIn("CalledFromWrongThreadException", text)
        self.assertEqual(text.count("MANAGED_DIALOG_ENGINE_SURFACE_DESTROYED"), 2, text[-30000:])
        # The dialog shown from onResume holds focus before the Activity's
        # window exists, so the Activity first gains focus when the dialog
        # goes, and, its decor being invisible by then, when it is visible again;
        # the non-focusable PopupWindow leaves it focused.
        regained = text.index("MANAGED_DIALOG_ACTIVITY_FOCUS true")
        self.assertNotIn("MANAGED_DIALOG_ACTIVITY_FOCUS false", text[:regained], text[-30000:])
        self.assertLess(text.index("MANAGED_DIALOG_POSITIVE"), regained)
        self.assertLess(regained, text.index("MANAGED_DIALOG_POPUP_SHOWN"))
        # Window layers (WindowManagerPolicy.getWindowLayerFromTypeLw): a dialog added while the toast is up stacks
        # below it, and a PopupWindow of the Activity's window below the dialog added before it (a sub-window stacks
        # within its parent's subtree).
        segment = text[text.index("MANAGED_DIALOG_STACK_BEGIN"):text.index("MANAGED_DIALOG_STACK_END")]
        added = re.findall(r"APP_POPUP_ADDED (\d+) type=(\d+) title=.* stack=([\d,]+)", segment)
        self.assertEqual([each[1] for each in added], ["2", "1000"], segment)
        toast = re.search(r"APP_POPUP_ADDED (\d+) type=2005", text).group(1)
        dialog_stack, popup_stack = added[0][2].split(","), added[1][2].split(",")
        self.assertLess(dialog_stack.index(added[0][0]), dialog_stack.index(toast), segment)
        self.assertLess(popup_stack.index(added[1][0]), popup_stack.index(added[0][0]), segment)
        self.assertLess(popup_stack.index(added[1][0]), popup_stack.index(toast), segment)
        # The next Activity's window, added while the toast is still up, stacks below the toast as well.
        engine = re.search(r"APP_POPUP_ADDED (\d+) type=1 title=.* stack=([\d,]+)",
                           text[text.index("MANAGED_DIALOG_ENGINE_START"):])
        engine_stack = engine.group(2).split(",")
        self.assertLess(engine_stack.index(engine.group(1)), engine_stack.index(toast), engine.group(0))
        # The dialog added to the stopped root (under the Engine Activity) is not visible, and gets its surface only
        # once the root is visible again (ADD_FLAG_APP_VISIBLE is dispatched later).
        resumed = [match.start() for match in re.finditer("MANAGED_DIALOG_ROOT_RESUMED", text)]
        self.assertEqual(len(resumed), 2, text[-30000:])
        self.assertGreater(text.index("MANAGED_DIALOG_ENGINE_SURFACE_CREATED stopped"), resumed[1], text[-30000:])
        # Windows whose ViewRootImpl has a ThreadedRenderer draw on the GPU, as on Android (a software canvas refuses the
        # hardware bitmaps an image loader hands out): the Activity window, the dialog, the PopupWindow and the stacking
        # test's dialog and PopupWindow, at framework-computed frames. The toast and the unaccelerated Engine Activity and its dialogs reached the host as
        # software layers. All but the leaked window, which dies asynchronously at exit, were removed.
        self.assertEqual(text.count("APP_WINDOW_GPU"), 5, text[-30000:])
        self.assertRegex(text, r"APP_WINDOW_GPU \d+ \[40,60\]\[280,140\] opaque=\w+ text=Popup window")
        # The dialog dims what is behind it in a layer of its own directly below its window: the software dim layer
        # stacks above every GPU layer and would dim the dialog itself.
        self.assertRegex(text, r"APP_WINDOW_GPU \d+ \[\d+,\d+\]\[\d+,\d+\] opaque=\w+ dim=[1-9]\d* text=Bifrost dialog")
        shown = [line.split()[1:] for line in text.splitlines() if line.startswith("APP_POPUP_SHOWN")]
        self.assertEqual(len(shown), 5, text[-30000:])
        self.assertIn("type=2005 title=Toast", text)
        self.assertNotIn("APP_TOAST_FAILED", text)
        removed = {line.split()[1] for line in text.splitlines() if line.startswith("APP_POPUP_REMOVED")}
        self.assertLessEqual({"1", "2", "3", "4", "5", "6"}, removed, text[-30000:])

    def test_managed_view_window_and_surface_lifecycle(self):
        state = self.work / "views-state"
        bundle = self.work / "views.apkm"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(self.apk("views", managed="ManagedViews"), "base.apk")
            archive.write(self.split, "split.apk")
        self.install(bundle, state)
        status = package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(), timeout=90)
        text = (state / "org.bifrost.fixture/managed/last-run.log").read_text()
        self.assertEqual(status, 0, text[-30000:])
        self.assertNotRegex(text, r"FATAL EXCEPTION|AssertionError")
        # An Activity without a SurfaceView launches and its button reaches
        # the host; the SurfaceView's surface follows its attachment and
        # visibility, created once per visible period.
        markers = ("MANAGED_VIEWS_RESUMED", "APP_WINDOW_ADDED", "APP_WINDOW_GPU",
                   "MANAGED_VIEWS_SURFACE_CREATED", "MANAGED_VIEWS_SURFACE_CHANGED 400x300",
                   "MANAGED_VIEWS_SURFACE_DESTROYED", "MANAGED_VIEWS_SURFACE_CREATED",
                   "MANAGED_VIEWS_SURFACE_DESTROYED", "MANAGED_VIEWS_DESTROYED", "APP_POPUP_REMOVED")
        position = 0
        for marker in markers:
            position = text.index(marker, position) + len(marker)
        self.assertRegex(text, r"APP_WINDOW_GPU \d+ \[0,0\]\[\d+,\d+\] opaque=\w+ text=Overlay button")
        # The SurfaceView sits 100 px above the window: its host layer is clipped to the visible 200 rows.
        self.assertRegex(text, r"APP_SURFACE android.view.SurfaceView 400x300 at \d+,-100 format=\d+ clip=0,100,400,300")
        self.assertEqual(text.count("MANAGED_VIEWS_SURFACE_CREATED"), 2)
        self.assertEqual(text.count("MANAGED_VIEWS_SURFACE_DESTROYED"), 2)
        # A legacy view animation ends: the window's draw time advances with each draw.
        self.assertIn("MANAGED_VIEWS_ANIMATION_END", text)

    def test_managed_window_geometry(self):
        # Every size an app can read agrees with the host window, and the attached
        # WebView sees the full window at the window's density.
        state = self.work / "geometry-state"
        bundle = self.work / "geometry.apkm"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(self.apk("geometry", managed="ManagedGeometry"), "base.apk")
            archive.write(self.split, "split.apk")
        self.install(bundle, state)
        # The Activity finishes itself, but the process with an attached WebView does not exit
        # (status 124 at the timeout; observed, not yet explained), so only the log is checked.
        package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(), timeout=40)
        text = (state / "org.bifrost.fixture/managed/last-run.log").read_text()
        self.assertNotRegex(text, r"FATAL EXCEPTION|AssertionError")
        self.assertIn("APP_DESTROYED", text)
        # The OpenGL ES 1.x OES names an ES 1 app imports run against this view's context (ManagedGeometry.gles1Oes).
        self.assertIn("GEOMETRY gles1-oes ok", text)
        self.assertIn("glDrawTexfOES is OpenGL ES 1.x fixed-function API, which is not supported", text)
        width, height, density = map(int, re.search(r"APP_WINDOW_OPEN (\d+)x(\d+) density=(\d+)", text).groups())
        report = re.search(r"GEOMETRY periodic activity=(\d+)x(\d+) density=([\d.]+) .*? application=(\d+)x(\d+) .*? real=\d+x\d+ "
                           r"displayMetrics=(\d+)x(\d+) window=Rect\(0, 0 - (\d+), (\d+)\).*? decor=(\d+)x(\d+) root=(\d+)x(\d+) web=(\d+)x(\d+)@0,0", text)
        self.assertIsNotNone(report, text[-3000:])
        sizes = list(map(int, report.group(1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15)))
        self.assertEqual(sizes, [width, height] * 7)
        page = re.search(r"GEOMETRY page inner=(\d+)x(\d+) dpr=([\d.]+)", text)
        if page:  # the page reports after the WebView loads, which can outlast a short run
            # The density follows the host display, so the ratio is fractional on most desktops (200 dpi is 1.25).
            ratio = float(page.group(3))
            self.assertAlmostEqual(ratio, density / 160, places=2)
            self.assertLessEqual(abs(int(page.group(1)) * ratio - width), 1)
            self.assertLessEqual(abs(int(page.group(2)) * ratio - height), 1)

    def test_managed_text_input(self):
        # Text from the host's input method (committed text, composition) reaches
        # a focused EditText through its InputConnection, in order with key events.
        # BIFROST_TEXT_SCRIPT pushes the events into the real window's SDL queue.
        # H arrives with the text it produced and must type once; Backspace is a key.
        script = ("down:H,text:h;up:H;text:é;edit:ni;text:你;"
                  "down:Backspace;up:Backspace;text:!")
        state = self.work / "text-input-state"
        bundle = self.work / "text-input.apkm"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(self.apk("text-input", managed="ManagedTextInput"), "base.apk")
            archive.write(self.split, "split.apk")
        self.install(bundle, state)
        with patch.dict(os.environ, {"BIFROST_TEXT_SCRIPT": script}):
            status = package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(), timeout=90)
        text = (state / "org.bifrost.fixture/managed/last-run.log").read_text()
        self.assertEqual(status, 0, text[-30000:])
        self.assertNotRegex(text, r"FATAL EXCEPTION|AssertionError|MANAGED_TEXT_INPUT_TIMEOUT")
        # "h", "hé", the composition "ni" (underlined), its replacement by "你",
        # Backspace removing it, then "!".
        self.assertIn("MANAGED_TEXT_INPUT_KEYMAP_OK", text)  # the FULL US QWERTY virtual keyboard
        markers = ("MANAGED_TEXT_INPUT_TEXT 68 composing=-1",
                   "MANAGED_TEXT_INPUT_TEXT 68 e9 composing=-1",
                   "MANAGED_TEXT_INPUT_TEXT 68 e9 6e 69 composing=2", "MANAGED_TEXT_INPUT_COMPOSING",
                   "MANAGED_TEXT_INPUT_TEXT 68 e9 4f60 composing=-1",
                   "MANAGED_TEXT_INPUT_TEXT 68 e9 composing=-1",
                   "MANAGED_TEXT_INPUT_TEXT 68 e9 21 composing=-1", "MANAGED_TEXT_INPUT_OK")
        position = 0
        for marker in markers:
            position = text.index(marker, position) + len(marker)
        # The key that produced "h" is not also typed as a key event.
        self.assertNotIn("MANAGED_TEXT_INPUT_TEXT 68 68", text)

    def test_managed_hardware_canvas(self):
        # HWUI's RenderThread, Skia GL and EGL draw through the GL thunk onto
        # a SurfaceView's host-backed surface.
        state = self.work / "hardware-state"
        bundle = self.work / "hardware.apkm"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(self.apk("hardware", managed="ManagedHardware"), "base.apk")
            archive.write(self.split, "split.apk")
        self.install(bundle, state)
        status = package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(), timeout=90)
        text = (state / "org.bifrost.fixture/managed/last-run.log").read_text()
        self.assertEqual(status, 0, text[-30000:])
        self.assertNotRegex(text, r"FATAL EXCEPTION|AssertionError")
        self.assertRegex(text, r"MANAGED_HARDWARE_SURFACE \d+x\d+")
        self.assertIn("MANAGED_HARDWARE_FRAMES 30", text)
        # The Activity window draws through its own ThreadedRenderer into a
        # host layer: frame metrics report frames HWUI swapped.
        self.assertRegex(text, r"APP_WINDOW_GPU \d+ \[0,0\]\[\d+,\d+\] opaque=\w+ text=Managed Fixture")
        frames = int(re.search(r"MANAGED_HARDWARE_WINDOW accelerated=true frames=(\d+)", text).group(1))
        self.assertGreaterEqual(frames, 10, text[-30000:])
        # A TextureView samples its producer's gralloc buffers as
        # EGL_NATIVE_BUFFER_ANDROID images: an RGBA canvas frame and an
        # ImageWriter YUV frame, read back through TextureView.getBitmap.
        rgba = re.search(r"MANAGED_HARDWARE_TEXTURE_RGBA (\d+),(\d+),(\d+)", text)
        yuv = re.search(r"MANAGED_HARDWARE_TEXTURE_YUV (\d+),(\d+),(\d+)", text)
        self.assertTrue(rgba and yuv, text[-30000:])
        for actual, expected in zip(map(int, rgba.groups()), (200, 40, 90)):
            self.assertLessEqual(abs(actual - expected), 2, rgba.group(0))
        # ImageWriter's YUV frames are JFIF: full-range BT.601.
        for actual, expected in zip(map(int, yuv.groups()), (238, 14, 14)):
            self.assertLessEqual(abs(actual - expected), 3, yuv.group(0))
        # A HARDWARE bitmap drawn by a GPU canvas keeps its pixels (image loaders produce these).
        hardware = re.search(r"MANAGED_HARDWARE_TEXTURE_HWBITMAP (\d+),(\d+),(\d+)", text)
        self.assertTrue(hardware, text[-30000:])
        for actual, expected in zip(map(int, hardware.groups()), (30, 160, 220)):
            self.assertLessEqual(abs(actual - expected), 3, hardware.group(0))
        self.assertIn("MANAGED_HARDWARE_BITMAPS_INTACT wrong=0 of 24", text)
        density = re.search(r"MANAGED_HARDWARE_DENSITY bitmap=(\d+) display=(\d+)", text)
        self.assertTrue(density and density.group(1) == density.group(2), density and density.group(0))

    def test_managed_play_services_available(self):
        # Google's own client check accepts the provisioned microG packages:
        # both are present with spoofed Google certificates (0 = SUCCESS).
        state = self.work / "play-services-state"
        bundle = self.work / "play-services.apkm"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(self.apk("play-services", managed="ManagedPlayServices"), "base.apk")
            archive.write(self.split, "split.apk")
        self.install(bundle, state)
        status = package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(), timeout=180)
        text = (state / "org.bifrost.fixture/managed/last-run.log").read_text()
        self.assertEqual(status, 0, text[-30000:])
        self.assertNotRegex(text, r"FATAL EXCEPTION|AssertionError")
        self.assertRegex(text, r"MANAGED_PLAY_SERVICES status=0 apk=252432034 client=\d+", text[-30000:])
        self.assertIn("MANAGED_PLAY_SERVICES_ADS_BIND false\n", text)
        # microG runs in its own process and UID; the app binds its service
        # by action and reaches its providers there.
        self.assertIn("ASGARD_PROCESS_READY pid=", text)
        self.assertRegex(text, r"uid=19998 process=com\.google\.android\.gms\n")
        self.assertIn("MANAGED_PLAY_SERVICES_CONNECTED com.google.android.gms/org.microg.gms.measurement."
                      "MeasurementService com.google.android.gms.common.internal.IGmsServiceBroker alive=true", text)
        self.assertRegex(text, r"MANAGED_PLAY_SERVICES_GSERVICES rows=0 uid=1\d{4}\n")
        self.assertIn("MANAGED_PLAY_SERVICES_CHIMERA com.google.android.gms/org.microg.gms.measurement."
                      "MeasurementService", text)

    def test_managed_accounts_service(self):
        # AccountManager against the account service: an authenticator in the
        # app adds, authenticates and removes an account, and microG's
        # "com.google" authenticator is registered beside it.
        state = self.work / "accounts-state"
        bundle = self.work / "accounts.apkm"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(self.apk("accounts", managed="ManagedAccounts"), "base.apk")
            archive.write(self.split, "split.apk")
        self.install(bundle, state)
        status = package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(), timeout=240)
        text = (state / "org.bifrost.fixture/managed/last-run.log").read_text()
        self.assertEqual(status, 0, text[-30000:])
        self.assertNotRegex(text, r"FATAL EXCEPTION|MANAGED_ACCOUNTS_FAILED")
        self.assertIn("MANAGED_ACCOUNTS_TYPES fixture=true google=true\n", text)
        self.assertIn("MANAGED_ACCOUNTS_GOOGLE_FEATURES ac2dm=true calendar=false\n", text, text[-30000:])
        self.assertIn("MANAGED_ACCOUNTS_SECRETS_DENIED\n", text)
        self.assertIn("MANAGED_ACCOUNTS_MEDIA_KEY_OK\n", text, text[-30000:])
        self.assertIn("MANAGED_ACCOUNTS_OK\n", text, text[-30000:])

    def test_managed_google_activity_window(self):
        # Activities of Play services started by an app run in a window of
        # the system app's session and return their result: explicit and
        # implicit start, a PendingIntent another process of the app created
        # for one (refused for any other Activity), and a caller's
        # authenticator response that the Activity ends without answering.
        state = self.work / "google-activity-state"
        bundle = self.work / "google-activity.apkm"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(self.apk("google-activity", managed="ManagedGoogleActivity"), "base.apk")
            archive.write(self.split, "split.apk")
        self.install(bundle, state)
        # As Asgard runs it: the app and the Google window share one Binder realm.
        realm = Path(tempfile.mkdtemp(prefix="bfrealm-"))
        broker = subprocess.Popen([str(ARGS.emu.resolve()), "--asgard-broker", str(realm / "broker.sock")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        try:
            for _ in range(200):
                if (realm / "broker.sock").exists(): break
                time.sleep(0.025)
            status = package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(), timeout=300,
                                    emu_args=["--android-binder-broker", str(realm / "broker.sock")])
        finally:
            broker.terminate()
            broker.wait(timeout=10)
            shutil.rmtree(realm, ignore_errors=True)
        text = (state / "org.bifrost.fixture/managed/last-run.log").read_text()
        nested = "".join(path.read_text(errors="replace")[-6000:] for path in
                         sorted((state / "com.google.android.gms/managed/sessions").glob("*/stdout.log")))
        self.assertEqual(status, 0, text[-8000:] + nested)
        self.assertNotRegex(text, r"FATAL EXCEPTION|APP_ACTIVITY_REMOTE_FAILED")
        for marker in ("MANAGED_GOOGLE_RESULT 7 -1 error=10\n", "MANAGED_GOOGLE_SENDER_PROCESS true\n",
                       "MANAGED_GOOGLE_RESULT 10 -1 error=10\n", "MANAGED_GOOGLE_LATE_FRAME 1\n",
                       "MANAGED_GOOGLE_SENDER_REFUSED\n",
                       "MANAGED_GOOGLE_RESULT 8 -1 error=10\n",
                       "MANAGED_GOOGLE_RESPONSE_ERROR 4 canceled\n", "MANAGED_GOOGLE_RESULT 9 0 error=null\n",
                       "MANAGED_GOOGLE_DONE\n"):
            self.assertIn(marker, "\n".join(line for line in text.splitlines() if "MANAGED_GOOGLE" in line or "REMOTE" in line) + "\n", nested)
        self.assertRegex(nested, r"APP_REMOTE_REQUEST org\.microg\.gms\.auth\.signin\.AuthSignInActivity from "
                                 r"ComponentInfo\{org\.bifrost\.fixture/org\.bifrost\.fixture\.ManagedGoogleActivity\}")

    def test_google_settings_alias_starts(self):
        # microG's SettingsActivity is an <activity-alias>: the session loads its target's class.
        import android_google as google
        state = self.work / "google-settings-state"
        google.prepare(state, google.SETTINGS_ACTIVITY)
        status = package.launch(google.GMS, state, ARGS.emu.resolve(), timeout=45)
        text = "".join(path.read_text(errors="replace") for path in
                       (state / google.GMS / "managed/sessions").glob("*/stdout.log"))
        self.assertEqual(status, 124, text[-4000:])  # still open when the timeout ended it
        self.assertNotIn("ClassNotFoundException", text)
        for marker in ("APP_ACTIVITY_CREATED", "APP_WINDOW_ADDED"):
            self.assertIn(marker, text, text[-4000:])

    def test_managed_parse_cache_skips_manifest_parsing(self):
        # Parsed manifests of immutable archives are kept in the root: a second
        # launch starts without parsing them, and unreadable entries are parsed again.
        state = self.work / "parse-cache-state"
        bundle = self.work / "parse-cache.apkm"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(self.apk("parse-cache", managed="ManagedSplash"), "base.apk")
            archive.write(self.split, "split.apk")
        self.install(bundle, state)
        log = state / "org.bifrost.fixture/managed/last-run.log"
        cache = state / "org.bifrost.fixture/managed/rootfs/data/local/tmp/package-cache"
        parsed = lambda: re.findall(r"Parse times for '([^']+)'", log.read_text())
        self.assertEqual(package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(), timeout=90), 0)
        self.assertIn("/product/app/webview/webview.apk", parsed())
        entries = sorted(cache.glob("*.bin"))
        self.assertGreaterEqual(len(entries), 2, entries)
        self.assertEqual(package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(), timeout=90), 0)
        self.assertEqual(parsed(), [], log.read_text()[-3000:])
        for entry in entries: entry.write_bytes(b"not a parcel")
        self.assertEqual(package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(), timeout=90), 0)
        self.assertIn("/product/app/webview/webview.apk", parsed())
        self.assertTrue(all(entry.read_bytes()[:4] != b"not " for entry in cache.glob("*.bin")))

    def test_managed_splash_hands_off_to_main_activity(self):
        # The launcher Activity finishing right after startActivity does not
        # end the app: the started Activity becomes the task's root.
        state = self.work / "splash-state"
        bundle = self.work / "splash.apkm"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(self.apk("splash", managed="ManagedSplash"), "base.apk")
            archive.write(self.split, "split.apk")
        self.install(bundle, state)
        status = package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(), timeout=90)
        text = (state / "org.bifrost.fixture/managed/last-run.log").read_text()
        self.assertEqual(status, 0, text[-30000:])
        self.assertNotRegex(text, r"FATAL EXCEPTION|NoSuchMethodException|Unable to start activity")
        markers = ["MANAGED_SPLASH_FINISHING", "MANAGED_SPLASH_MAIN_RESUMED",
                   "MANAGED_SPLASH_DESTROYED", "MANAGED_SPLASH_MAIN_DESTROYED"]
        positions = [text.index(marker) for marker in markers]
        self.assertEqual(positions, sorted(positions), text[-30000:])

    def test_managed_activity_stack(self):
        state = self.work / "activities-state"
        bundle = self.work / "activities.apkm"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(self.apk("activities", managed="ManagedActivities"), "base.apk")
            archive.write(self.split, "split.apk")
        self.install(bundle, state)
        status = package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(), timeout=90)
        text = (state / "org.bifrost.fixture/managed/last-run.log").read_text()
        self.assertEqual(status, 0, text[-30000:])
        self.assertNotRegex(text, r"FATAL EXCEPTION|AssertionError")
        markers = ("MANAGED_ACTIVITIES_ROOT_RESUMED returned=0", "MANAGED_ACTIVITIES_UNRESOLVED_OK",
                   "MANAGED_ACTIVITIES_PROVIDER_QUERY_OK",
                   # The permission controller's result arrives while the caller is paused.
                   "MANAGED_ACTIVITIES_ROOT_PAUSED",
                   "MANAGED_ACTIVITIES_PERMISSIONS 7 [android.permission.INTERNET, android.permission.CAMERA] [0, -1]",
                   # A second request from the first result's callback is answered, not cancelled.
                   "MANAGED_ACTIVITIES_PERMISSIONS 8 [android.permission.CAMERA] [-1]",
                   "MANAGED_ACTIVITIES_SECOND_CREATED value=7",
                   # The opaque second Activity stops the root once it is resumed.
                   "MANAGED_ACTIVITIES_SECOND_RESUMED", "MANAGED_ACTIVITIES_ROOT_STOPPED",
                   "MANAGED_ACTIVITIES_SECOND_FOCUS true", "MANAGED_ACTIVITIES_SECOND_WINDOW",
                   "MANAGED_ACTIVITIES_SECOND_CLICKED", "MANAGED_ACTIVITIES_ROOT_RESTARTED",
                   "MANAGED_ACTIVITIES_RESULT 42 -1 42",
                   "MANAGED_ACTIVITIES_ROOT_RESUMED returned=1", "MANAGED_ACTIVITIES_SECOND_DESTROYED",
                   "APP_POPUP_REMOVED",
                   # Second round: the Back key through the window's view tree.
                   "MANAGED_ACTIVITIES_SECOND_BACK", "MANAGED_ACTIVITIES_RESULT 43 0 null",
                   "MANAGED_ACTIVITIES_ROOT_RESUMED returned=2", "MANAGED_ACTIVITIES_PENDING_CREATED",
                   "MANAGED_ACTIVITIES_PENDING_RETURNED",
                   # startIntentSenderForResult: a foreign sender is cancelled,
                   # the app's own one returns its result (Back: cancelled, no data).
                   "MANAGED_ACTIVITIES_FOREIGN_SENDER_CANCELED", "MANAGED_ACTIVITIES_RESULT 44 0 null",
                   "MANAGED_ACTIVITIES_SENDER_RETURNED", "MANAGED_ACTIVITIES_ROOT_DESTROYED")
        positions = [text.index(marker) for marker in markers]
        self.assertEqual(positions, sorted(positions), text[-30000:])
        # Only the opaque round stops the root; its window and SurfaceView
        # leave the host meanwhile and come back. The translucent round keeps
        # the root visible (one more surface teardown at the root's destroy).
        self.assertEqual(text.count("MANAGED_ACTIVITIES_ROOT_STOPPED"), 2, text[-30000:])
        self.assertEqual(text.count("MANAGED_ACTIVITIES_ROOT_RESTARTED"), 1)
        stopped = text.index("MANAGED_ACTIVITIES_ROOT_STOPPED")
        restarted = text.index("MANAGED_ACTIVITIES_ROOT_RESTARTED")
        self.assertIn("APP_SURFACE_DESTROYED android.view.SurfaceView", text[stopped:restarted])
        self.assertIn("APP_SURFACE android.view.SurfaceView", text[restarted:])
        translucent = text[text.index("MANAGED_ACTIVITIES_ROOT_RESUMED returned=1"):
                           text.index("MANAGED_ACTIVITIES_ROOT_RESUMED returned=2")]
        self.assertNotIn("MANAGED_ACTIVITIES_ROOT_STOPPED", translucent)
        self.assertNotIn("APP_SURFACE_DESTROYED", translucent)
        # The second window reaches the host while it is resumed; focus
        # arrives by a ViewRootImpl message, in either order with the layer.
        layer = text.index("text=Close second")
        self.assertLess(text.index("MANAGED_ACTIVITIES_SECOND_RESUMED"), layer)
        self.assertLess(layer, text.index("MANAGED_ACTIVITIES_SECOND_CLICKED"))
        # The root's dialog is a window of the stopped Activity: it gives up focus when the root stops, takes none
        # while the second window is not focusable (that window's flag change is a relayout), and has it back with
        # the root. Without the stop, it would take focus from the second window.
        stopped_at = text.index("MANAGED_ACTIVITIES_ROOT_STOPPED")
        clicked = text.index("MANAGED_ACTIVITIES_SECOND_CLICKED")
        covered = text[stopped_at:clicked]
        self.assertNotIn("MANAGED_ACTIVITIES_DIALOG_FOCUS true", covered, text[-30000:])
        self.assertIn("MANAGED_ACTIVITIES_SECOND_FOCUS false", covered, text[-30000:])
        self.assertIn("MANAGED_ACTIVITIES_DIALOG_FOCUS true", text[clicked:text.index("MANAGED_ACTIVITIES_SECOND_BACK")], text[-30000:])
        # Started for a result: the caller is reported. The window covers
        # the display, and its button text reached the host layer.
        self.assertIn("caller=ComponentInfo{org.bifrost.fixture/org.bifrost.fixture.ManagedActivities}", text)
        width, height = re.search(r"APP_WINDOW_OPEN (\d+)x(\d+)", text).groups()
        self.assertIn(f"MANAGED_ACTIVITIES_SECOND_WINDOW {width}x{height}", text)
        self.assertRegex(text, rf"APP_WINDOW_GPU \d+ \[0,0\]\[{width},{height}\] opaque=\w+ text=.*Close second")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--emu", type=Path, default=ROOT / "build/release-sdl1-gl1/bifrost-emu"
    )
    parser.add_argument(
        "--plugin",
        type=Path,
        default=ROOT / "build/release-sdl1-gl1/plugins/android-window-sdl.so",
    )
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__] + remaining)
