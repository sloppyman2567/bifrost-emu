#!/usr/bin/env python3
"""Signed split installation, rollback, real Bionic and native Wayland APK gate."""

import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

import android_package as package

ROOT = package.ROOT
ARGS = None


class PackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="bifrost-apk-test-")
        cls.work = Path(cls.temporary.name)
        cls.tools = ROOT / "tools/android-tools"
        cls.runtime = ROOT / "tools/android-runtime/rootfs"
        cls.adapters = ROOT / "build/android-adapters"
        subprocess.run(
            ["python3", str(ROOT / "tools/build-android-adapters.py")],
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
            "android_guest_dependencies", ROOT / "tools/android_guest_dependencies.py"
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
        source.write_text("int fixture_dependency_value(void) { return 41; }\n")
        subprocess.run(compiler + ["-Wl,-soname,libfixture_dependency.so", str(source),
                       "-o", str(cls.dependency)], check=True, capture_output=True)
        cls.native = cls.work / "libfixture.so"
        subprocess.run(compiler + [
            "-Wl,-soname,libfixture.so", str(ROOT / "ctest/host/android_apk_fixture.c"),
            str(cls.dependency), str(cls.adapters / "libandroid.so"),
            str(cls.adapters / "liblog.so"), str(cls.adapters / "libz.so"),
            str(cls.adapters / "libEGL.so"), str(cls.adapters / "libGLESv2.so"),
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

    @classmethod
    def apk(cls, name, split="", native=False, version=1, managed=False, key=None):
        manifest = cls.work / (name + ".xml")
        apk = cls.work / (name + ".apk")
        activity = (
            "org.bifrost.fixture.Managed" if managed else "android.app.NativeActivity"
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
        required = ' android:isSplitRequired="true"' if not split else ""
        manifest.write_text(
            f"""<manifest xmlns:android="http://schemas.android.com/apk/res/android"
            package="org.bifrost.fixture" android:versionCode="{version}"{split_attr}>
            <uses-sdk android:minSdkVersion="25" android:targetSdkVersion="35"/>
            <application android:hasCode="false"{required}>{component}</application></manifest>"""
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
            ],
            check=True,
            capture_output=True,
        )
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

    def install(self, input, state):
        return package.install(
            input,
            state,
            self.runtime,
            self.adapters,
            self.tools / "aapt2",
            self.tools / "apksigner.jar",
        )

    def test_signed_split_native_launch_and_persistence(self):
        state = self.work / "launch-state"
        metadata = self.install(self.bundle, state)
        self.assertEqual(metadata["apk_count"], 2)
        self.assertEqual(metadata["arm64_library_count"], 2)
        self.assertEqual(metadata["launcher"]["kind"], "native_activity")
        self.assertEqual((state / "org.bifrost.fixture/package.json").is_file(), True)
        for mode in ("jit", "interp", "tier2"):
            for backend in ("builtin", "plugin"):
                cmd = [
                    "python3",
                    str(ROOT / "scripts/android_package.py"),
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
                    "BIFROST_TIER2": "1" if mode == "tier2" else "0",
                    "BIFROST_TIER2_HITS": "32",
                }
                result = subprocess.run(
                    cmd, env=env, capture_output=True, text=True, timeout=45
                )
                self.assertEqual(
                    result.returncode, 0, result.stdout + "\n" + result.stderr
                )
                self.assertIn("abi=42 1.25 guest 1234567890123 1 2 3 4 5 6 7 8", result.stderr)
                self.assertIn("buffer=2.50 ok", result.stderr)
                self.assertIn("apk_bionic_fixture: ALL PASS", result.stdout)
                self.assertIn("android_service_manager: ALL PASS", result.stdout)
                self.assertNotRegex(
                    result.stdout + result.stderr, r"Fatal signal|\[crash\]|CANNOT LINK"
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
            "python3", str(ROOT / "scripts/android_package.py"), "--state-root", str(state),
            "probe", "org.bifrost.fixture", "--emu", str(ARGS.emu), "--timeout", "30",
        ], capture_output=True, text=True, timeout=75)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Native load probe: 2/2 loaded", result.stdout)
        self.assertIn("abi=42 1.25 guest 1234567890123 1 2 3 4 5 6 7 8", result.stdout)
        with self.assertRaisesRegex(ValueError, "packaged ARM64"):
            package.probe("org.bifrost.fixture", state, Path("/must/not/execute"),
                          ["../libfixture.so"])
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

    def test_managed_launcher_is_installed_but_rejected_before_execution(self):
        state = self.work / "managed-state"
        base = self.apk("managed", managed=True)
        bundle = self.work / "managed.apkm"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.write(base, "base.apk")
            archive.write(self.split, "split.apk")
        self.install(bundle, state)
        with self.assertRaisesRegex(ValueError, "ART/framework"):
            package.launch("org.bifrost.fixture", state, Path("/must/not/execute"))


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
