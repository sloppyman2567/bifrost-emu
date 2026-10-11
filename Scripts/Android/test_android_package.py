#!/usr/bin/env python3
"""Signed split installation, rollback, real Bionic and native Wayland APK gate."""

import argparse
import concurrent.futures
import hashlib
import hmac
import importlib.util
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

import android_package as package

ROOT = package.ROOT
ARGS = None


class LauncherArgumentTests(unittest.TestCase):
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


class SettingsIdentityTests(unittest.TestCase):
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
            component += f'<service android:name="{activity}$Jobs" android:permission="android.permission.BIND_JOB_SERVICE" android:exported="false"/>'
        required = ' android:isSplitRequired="true"' if not split else ""
        job_permission = '<uses-permission android:name="android.permission.RECEIVE_BOOT_COMPLETED"/>' if managed == "ManagedServices" else ""
        manifest.write_text(
            f"""<manifest xmlns:android="http://schemas.android.com/apk/res/android"
            package="org.bifrost.fixture" android:versionCode="{version}"{split_attr}>
            <uses-sdk android:minSdkVersion="25" android:targetSdkVersion="35"/>
            {job_permission}
            <application android:label="Managed Fixture" android:hasCode="{'true' if managed else 'false'}"{required}>{component}</application></manifest>"""
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
        if managed:
            classes = cls.work / (name + "-classes")
            classes.mkdir()
            dex = cls.work / (name + "-dex")
            dex.mkdir()
            subprocess.run([os.environ.get("JAVAC", "javac"), "--release", "8", "-cp",
                            str(cls.tools / "android.jar"), "-d", str(classes),
                            str(ROOT / "Tests/Host" / (activity.rsplit(".", 1)[-1] + ".java"))],
                           check=True, capture_output=True)
            subprocess.run(["java", "-cp", str(cls.tools / "d8.jar"),
                            "com.android.tools.r8.D8", "--min-api", "26", "--lib",
                            str(cls.tools / "android.jar"), "--output", str(dex),
                            *map(str, classes.rglob("*.class"))], check=True, capture_output=True)
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

    def install(self, input, state):
        return package.install(
            input,
            state,
            self.runtime,
            self.adapters,
            self.tools / "aapt2",
            self.tools / "apksigner.jar",
            self.media,
        )

    def test_signed_split_native_launch_and_persistence(self):
        state = self.work / "launch-state"
        metadata = self.install(self.bundle, state)
        self.assertEqual(metadata["apk_count"], 2)
        self.assertEqual(metadata["arm64_library_count"], 2)
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
                self.assertIn("abi=42 1.25 guest 1234567890123 1 2 3 4 5 6 7 8", result.stderr)
                self.assertIn("buffer=2.50 ok", result.stderr)
                self.assertIn("apk_bionic_fixture: ALL PASS", result.stdout)
                self.assertIn("apk_media_fixture: REAL FORMAT PASS", result.stdout)
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
            self.assertTrue(any(event["event"] == "throw" and event["operation"] == "activity.startService"
                                for event in records), "unsupported calls not traced")
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
            if starts == 3:
                # Force a runtime replacement, as a launcher upgrade does.
                import json
                stamp = app / "managed/runtime.json"
                saved = json.loads(stamp.read_text())
                saved["identity"]["schema"] = 0
                stamp.write_text(json.dumps(saved))
            status = package.launch("org.bifrost.fixture", state, ARGS.emu.resolve(), timeout=90)
            text = (app / "managed/last-run.log").read_text()
            self.assertEqual(status, 0, text[-30000:])
            self.assertIn("APP_WINDOW_IDENTITY Managed Fixture icon=", text)
            self.assertIn(f"MANAGED_FIXTURE_START {starts}", text)
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
