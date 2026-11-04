"""Provision the ARM64 Android framework on top of the verified ART root.

Adds the boot-classpath framework jars, the image's precompiled ARM64 boot
image and app_process64 (with its native closure) from the same pinned SDK
image, so Android's own runtime entry point can start the real framework.
"""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

from android_media import (ROOT, IMAGE_SHA256, image_archive, extract_system, dump,
                           without_bionic_links, install_bionic_links, BIONIC_LINKS,
                           update_inventory, digest)
import android_art
import android_super
import android_codecs

# Classpath order comes from the image's own boot image files (see
# boot_classpaths): the primary boot.oat records its jars in the
# "bootclasspath" key; the mainline extension lists its dex locations.
JAR_PATH = re.compile(rb"/(?:apex/com\.android\.[a-z0-9.]+/javalib|system/framework)/[A-Za-z0-9._-]+\.jar")
SCHEMA = 8
SHELL_SHA256 = "6726065cf17ebde172980adb2d988a48918597ca2907b613025d63d9d0ed196f"


def shell_inventory():
    return {"sha256": SHELL_SHA256, "executable": True}


# The image's shared libraries: SystemConfig's <library> declarations that name a jar in /system/framework, which an
# app's <uses-library> resolves against (FrameworkSharedLibraries). name -> (sha256 of
# /system/etc/permissions/NAME.xml, sha256 of /system/framework/NAME.jar). org.apache.http.legacy left the boot
# classpath in Android 9, so an app that still uses those classes needs it in its class loader.
SHARED_LIBRARIES = {
    "android.test.base": (
        "449eb0b5787d87d8abfaf3a3d00b6b7c908c043ccb9c8120c231ca2fbc7722bc",
        "6c4df7f402fae4c63c93790ecacb79d6a54a8e543b292ee8295924b70d9c0012"),
    "android.test.mock": (
        "58303c4148747f0cdbb6d45445d55fbdf93b99818403ef71ddaf3c79ef29c964",
        "5b23884fd8581ec4cf08e9ad35fdf046d662231aaf7cfb9aa8aaaba74572eb15"),
    "android.test.runner": (
        "c388d758b7405b8dcbaafd459a57c9f5b403f0bb3227028842e8453c9d21cdee",
        "b5bf3f9335d60df0623ccd6f88670f0a66b538286414c53954414dbd100f968f"),
    "javax.obex": (
        "81c400bb23b9345d46a7c51b011bdfcfdeafbd6b60d4c0f2caafcff644f06916",
        "055b47904a31bed68367148e9d8b9ca88d16b905583e74be2303c6f2721bac58"),
    "org.apache.http.legacy": (
        "708e3464888f4211456eea7b80d9a6f134628e47c2b8a92a850277cbdee123d2",
        "15a6318ddff7ec126d79ceb08ff7f41c8c20b40310b958fbc6112e4dfaa38aec"),
}


# The image's real media.extractor daemon, which the broker runs (CodecServices), with the libraries it cannot find in
# its linker section (the codec APEX's private copies are out of its scope). The extractor plugins are already in the
# runtime. The daemon aborts without its seccomp policy, read from the media APEX; the image's /system/etc copies are
# byte-identical to it. Bifrost's prctl(PR_SET_SECCOMP) accepts the filter without applying it, so it never reaches the host.
# android.media.SoundPool, audiofx.AudioEffect and android.renderscript load their JNI libraries by name, and RenderScript
# dlopens its CPU driver, so no DT_NEEDED graph reaches them; they ride here for the same pinned upgrade of an existing tree.
# install path -> (image path, sha256, executable).
SECCOMP_POLICIES = "apex/com.android.media/etc/seccomp_policy/"
MEDIA_EXTRACTOR = {
    "system/bin/mediaextractor": ("/system/bin/mediaextractor",
        "f1b816b4c0c1c54429f858555904d7fe864e00093726afe5221eda5483250a83", True),
    "system/lib64/libmediaextractorservice.so": ("/system/lib64/libmediaextractorservice.so",
        "d2a604f454489a6780eb6c23434895d7f681304d2a5875a5df475ca449bb1bd4", False),
    "system/lib64/libavservices_minijail.so": ("/system/lib64/libavservices_minijail.so",
        "3f8284594d2390bb1c162b3a33694e0eb0888af18c9d2ff76c34ec53e09e674a", False),
    "system/lib64/libminijail.so": ("/system/lib64/libminijail.so",
        "1ea86c7fd2fa8e7da2c73f947b56f273e61a913dc4588dee94024ef23caa69da", False),
    "system/lib64/libcap.so": ("/system/lib64/libcap.so",
        "7373f3c9a3e6f1abeca35847939ff77743f0969737c7a343dacf5ba74422397a", False),
    "system/bin/mediaserver": ("/system/bin/mediaserver",
        "5150e224e893e4b922d7287d7fb0b97e0f61b11ba4381b3b7549c62e2308fa1c", True),
    "system/lib64/libmediaplayerservice.so": ("/system/lib64/libmediaplayerservice.so",
        "3f2d5a40446e9e66a717d2f40db6fcc790bc9b97026dc00d8030a4eb53938997", False),
    "system/lib64/libstagefright_httplive.so": ("/system/lib64/libstagefright_httplive.so",
        "0157b015b572cdcffb0f26b4425394caf3af7349b422ecec4e9f245c8b2f8a7b", False),
    "system/lib64/libdrmframework.so": ("/system/lib64/libdrmframework.so",
        "99ba549ca4c05721691d1fd0adfa76c1c59e1991cd8739fa1ac0df6433375788", False),
    "system/lib64/libdrmframeworkcommon.so": ("/system/lib64/libdrmframeworkcommon.so",
        "9d977823c2c34063238ddc30de5344bf977ec01c43402cb32cac6b920bdbcec8", False),
    "system/lib64/libaudioeffect_jni.so": ("/system/lib64/libaudioeffect_jni.so",
        "b65b5c5aaa7298cf579b804bf5a3a0f3e348969016d41802c9d8d8d536caa73c", False),
    "system/lib64/librs_jni.so": ("/system/lib64/librs_jni.so",
        "cacd91627aa90c3258392f538b1de2a3bcdae8a4c0ca55c03a2a59bc56553246", False),
    "system/lib64/libRSCacheDir.so": ("/system/lib64/libRSCacheDir.so",
        "7f321554e760f002fc3efd9d702146336832c8e950ed740fa7a49eb6cc69793d", False),
    "system/lib64/libRS_internal.so": ("/system/lib64/libRS_internal.so",
        "5c21d0874b7510330f37e6e08a6cf0810fa5e0a3536ce33b3bd10e814a928f4a", False),
    "system/lib64/libRSDriver.so": ("/system/lib64/libRSDriver.so",
        "0be4814410caf4325684e5915402bf96d049e96cb5268defcb77f762c4a8cf3c", False),
    "system/lib64/libRSCpuRef.so": ("/system/lib64/libRSCpuRef.so",
        "1baeb408987e81fc262d83af0677fbe3b3176d369cffc369c45f3b2a714e6a8a", False),
    "system/lib64/libbcinfo.so": ("/system/lib64/libbcinfo.so",
        "acb731f6774361e93efb2cee5b2f3193a184a7fad6437172819044673eeb1b00", False),
    "system/lib64/libblas.so": ("/system/lib64/libblas.so",
        "6cf0d09069b5fae3a763955deef1882b4d7f432614edd7d31bac0a2f2b1b1ecd", False),
    "system/lib64/libsoundpool.so": ("/system/lib64/libsoundpool.so",
        "307a46c2a31fe98b41a8f0de0ae763baf962c5a2e487ee399070dede161fa4aa", False),
    SECCOMP_POLICIES + "mediaextractor.policy": ("/system/etc/seccomp_policy/mediaextractor.policy",
        "6cbaa21237b8664cdec18cb7a13e7103abfbf5094e9ad10c877f31a2ce8b0398", False),
    SECCOMP_POLICIES + "crash_dump.arm64.policy": ("/system/etc/seccomp_policy/crash_dump.arm64.policy",
        "0a356099c4378c4f9b6fde87d5d728dd6c6a637f338636a3615b9c815bc953da", False),
    SECCOMP_POLICIES + "code_coverage.arm64.policy": ("/system/etc/seccomp_policy/code_coverage.arm64.policy",
        "a78bd4d8339b5096d6e5e2c912e45834f87010e84c70c9ddac5c2d1e6e7f94aa", False),
}


def media_extractor_inventory():
    return {name: {"sha256": sha256, "executable": executable} for name, (_, sha256, executable) in MEDIA_EXTRACTOR.items()}


def without_media_extractor(files):
    """Validate the daemon's files and leave the image pin (they are added after it)."""
    result = dict(files)
    for name, expected in media_extractor_inventory().items():
        if name in result and result.pop(name) != expected:
            raise ValueError(f"Modified media extractor file: {name}")
    return result


def install_media_extractor(root, archive=None, system=None):
    """Add the daemon to a runtime, verified against the pins, in place like the shell and shared libraries."""
    missing = []
    for name, (_, pin, _executable) in MEDIA_EXTRACTOR.items():
        target = root / name
        if target.exists() or target.is_symlink():
            if target.is_symlink() or not target.is_file() or digest(target) != pin:
                raise ValueError(f"Modified media extractor file: {name}")
        else:
            missing.append(name)
    if not missing:
        return
    with tempfile.TemporaryDirectory(prefix=".android-extractor-") as directory:
        work = Path(directory)
        if system is None: system, _ = extract_system(image_archive(archive), work)
        for name in missing:
            image_path, pin, executable = MEDIA_EXTRACTOR[name]
            staged = work / Path(name).name
            dump(system, image_path, staged)
            if digest(staged) != pin: raise ValueError(f"{image_path} differs from the pinned image")
            staged.chmod(0o755 if executable else 0o644)
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(staged, root / name)


def shared_library_files():
    """Install path -> (image path, pinned sha256) of every shared library file."""
    files = {}
    for name, (xml, jar) in SHARED_LIBRARIES.items():
        files[f"system/etc/permissions/{name}.xml"] = (f"/system/etc/permissions/{name}.xml", xml)
        files[f"system/framework/{name}.jar"] = (f"/system/framework/{name}.jar", jar)
    return files


def shared_library_inventory():
    return {name: {"sha256": sha256, "executable": False} for name, (_, sha256) in shared_library_files().items()}


def without_shared_libraries(files):
    """Validate the shared library files and leave the image pin (the files are added after it)."""
    result = dict(files)
    for name, expected in shared_library_inventory().items():
        if name in result and result.pop(name) != expected:
            raise ValueError(f"Modified shared library file: {name}")
    return result


def without_shell(files):
    files = without_media_extractor(without_shared_libraries(without_bionic_links(files)))
    if "system/bin/sh" in files and files.pop("system/bin/sh") != shell_inventory():
        raise ValueError("Modified Android shell")
    return files


def install_shell(root, archive=None, system=None):
    target = root / "system/bin/sh"
    if target.exists() or target.is_symlink():
        if target.is_symlink() or not target.is_file() or digest(target) != SHELL_SHA256:
            raise ValueError("Modified Android shell")
        return
    with tempfile.TemporaryDirectory(prefix=".android-shell-") as directory:
        work = Path(directory)
        if system is None: system, _ = extract_system(image_archive(archive), work)
        shell = work / "sh"
        dump(system, "/system/bin/sh", shell)
        if digest(shell) != SHELL_SHA256: raise ValueError("Android shell differs from pinned image")
        shell.chmod(0o755)
        shutil.copy2(shell, target)
def protect_shared_libraries(root):
    """Make the shared library files read-only. ART refuses dex a process could write ("Writable dex file ... is not
    allowed", as for the WebView APK), and every managed root is its own copy, so this runs on each one."""
    for name in shared_library_files():
        path = root / name
        if path.is_file() and not path.is_symlink() and path.stat().st_mode & 0o222:
            path.chmod(0o444)


def install_shared_libraries(root, archive=None, system=None):
    """Add the image's shared library declarations and jars to a runtime, verified against the pins."""
    wanted = shared_library_files()
    missing = []
    for name, (_, pin) in wanted.items():
        target = root / name
        if target.exists() or target.is_symlink():
            if target.is_symlink() or not target.is_file() or digest(target) != pin:
                raise ValueError(f"Modified shared library file: {name}")
        else:
            missing.append(name)
    if not missing:
        protect_shared_libraries(root)
        return
    with tempfile.TemporaryDirectory(prefix=".android-libraries-") as directory:
        work = Path(directory)
        if system is None: system, _ = extract_system(image_archive(archive), work)
        for name in missing:
            image_path, pin = wanted[name]
            staged = work / Path(name).name
            dump(system, image_path, staged)
            if digest(staged) != pin: raise ValueError(f"{image_path} differs from the pinned image")
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(staged, root / name)
    protect_shared_libraries(root)


BOOT_IMAGE = re.compile(r"boot[A-Za-z0-9_-]*\.(art|oat|vdex)")


def boot_image_entries(system):
    """List /system/framework/arm64 as (name, symlink_target or None)."""
    listing = subprocess.run(["debugfs", "-R", "ls -p /system/framework/arm64", str(system.resolve())],
                             capture_output=True, text=True, check=True, timeout=30).stdout
    entries = []
    for line in listing.splitlines():
        # ls -p prints /inode/mode/uid/gid/name/size/.
        fields = line.strip("/").split("/")
        if len(fields) < 6 or fields[4] in (".", ".."):
            continue
        name, mode = fields[4], fields[1]
        if not BOOT_IMAGE.fullmatch(name):
            raise ValueError(f"Unexpected boot image file: {name}")
        target = None
        if mode.startswith("12"):  # symlink: the vdex files point one level up
            stat = subprocess.run(["debugfs", "-R", f"stat /system/framework/arm64/{name}",
                                   str(system.resolve())],
                                  capture_output=True, text=True, check=True, timeout=30).stdout
            found = re.search(r'Fast link dest: "([^"]+)"', stat)
            target = found.group(1) if found else ""
            if not (target.startswith("../") and BOOT_IMAGE.fullmatch(target[3:])):
                raise ValueError(f"Unexpected boot image link: {name} -> {target}")
        entries.append((name, target))
    if not any(name == "boot.art" for name, _ in entries) or len(entries) > 64:
        raise ValueError("Unexpected ARM64 boot image listing")
    return sorted(entries)


def boot_classpaths(root):
    """Return (primary, mainline) jar lists in boot image order."""
    arm64 = root / "system/framework/arm64"
    primary_text = (arm64 / "boot.oat").read_bytes()
    key = primary_text.find(b"bootclasspath\0")
    if key < 0:
        raise ValueError("boot.oat lacks a bootclasspath key")
    value = primary_text[key + len(b"bootclasspath\0"):].split(b"\0", 1)[0]
    primary = value.decode().split(":")
    if len(primary) < 5 or not all(JAR_PATH.fullmatch(j.encode()) for j in primary):
        raise ValueError("Unexpected primary boot classpath")
    mainline = []
    for oat in sorted(arm64.glob("boot-*.oat")):
        for match in JAR_PATH.findall(oat.read_bytes()):
            jar = match.decode()
            if jar not in primary and jar not in mainline:
                mainline.append(jar)
    if len(mainline) > 64:
        raise ValueError("Unexpected mainline boot classpath")
    return primary, mainline


def environment(primary, mainline):
    """Guest environment app_process64 expects from init."""
    dex2oat = ":".join(primary)
    return {"ANDROID_ROOT": "/system", "ANDROID_DATA": "/data",
            "ANDROID_ART_ROOT": "/apex/com.android.art",
            "ANDROID_I18N_ROOT": "/apex/com.android.i18n",
            "ANDROID_TZDATA_ROOT": "/apex/com.android.tzdata",
            # ART requires the mainline jars after the boot-image prefix.
            "DEX2OATBOOTCLASSPATH": dex2oat,
            "BOOTCLASSPATH": ":".join([dex2oat] + mainline)}


def extract_mainline(system, apex, work, root):
    """Extract boot jars and native libraries from one mainline APEX."""
    listing = subprocess.run(["debugfs", "-R", "ls /system/apex", str(system.resolve())],
                             capture_output=True, text=True, check=True, timeout=30).stdout
    for suffix in ("apex", "capex"):
        if f"com.android.{apex}.{suffix}" in listing.split():
            break
    else:
        raise ValueError(f"Missing APEX: {apex}")
    archive = work / f"{apex}.{suffix}"
    dump(system, f"/system/apex/com.android.{apex}.{suffix}", archive)
    if suffix == "capex":
        with zipfile.ZipFile(archive) as package:
            member = package.getinfo("original_apex")
            if member.file_size > 256 * 1024 * 1024:
                raise ValueError("Oversized decompressed APEX")
            archive = work / f"{apex}.apex"
            archive.write_bytes(package.read(member))
    with zipfile.ZipFile(archive) as package:
        member = package.getinfo("apex_payload.img")
        if member.file_size > 256 * 1024 * 1024:
            raise ValueError("Oversized APEX payload")
        image = work / f"{apex}.img"
        image.write_bytes(package.read(member))
    destination = root / "apex" / f"com.android.{apex}"
    destination.mkdir(parents=True, exist_ok=True)
    subprocess.run(["debugfs", "-R", "rdump /javalib .", str(image.resolve())],
                   cwd=destination, capture_output=True, check=True, timeout=60)
    # JNI libraries are opened by boot classes, so DT_NEEDED alone cannot
    # discover them. Some Java-only APEXes legitimately lack lib64.
    listing = subprocess.run(["debugfs", "-R", "ls -p /", str(image.resolve())],
                             capture_output=True, text=True, check=True, timeout=30).stdout
    names = {line.split("/")[5] for line in listing.splitlines()
             if len(line.split("/")) >= 7}
    if "lib64" in names:
        subprocess.run(["debugfs", "-R", "rdump /lib64 .", str(image.resolve())],
                       cwd=destination, capture_output=True, check=True, timeout=60)
        if not (destination / "lib64").is_dir():
            raise ValueError(f"Native APEX extraction failed: {apex}")
    if not (destination / "javalib").is_dir():
        raise ValueError(f"Java APEX extraction failed: {apex}")
    image.unlink()
    archive.unlink(missing_ok=True)


def framework_linker_configuration(apexes, paths):
    """Export every installed boot APEX to libnativeloader."""
    if not all(re.fullmatch(r"com\.android\.[a-z0-9.]+", name) for name in apexes):
        raise ValueError("Unsafe framework APEX name")
    namespaces = [name.replace(".", "_") for name in apexes]
    config = ("dir.art = /system/bin\n"
              "dir.art = /apex/com.android.art/bin\n"
              "dir.art = /data/local/tmp\n\n[art]\n"
              "additional.namespaces = " + ",".join(namespaces) + "\n"
              "namespace.default.isolated = true\n"
              "namespace.default.visible = true\n"
              "namespace.default.search.paths = " + ":".join(paths) + "\n"
              "namespace.default.permitted.paths = /apex:/system:/product:/data/local/tmp\n")
    for name, namespace in zip(apexes, namespaces):
        prefix = "namespace." + namespace
        config += (prefix + ".isolated = true\n" + prefix + ".visible = true\n" +
                   prefix + ".permitted.paths = /apex/" + name + "\n" +
                   prefix + ".links = default\n" +
                   prefix + ".link.default.allow_all_shared_libs = true\n")
    return config


def install(destination, art_root, archive=None):
    if destination.is_symlink():
        raise ValueError("Refusing redirected framework runtime")
    destination = destination.resolve()
    manifest = destination / "bifrost-framework.json"
    if destination.exists():
        if manifest.is_symlink() or not manifest.is_file() or manifest.stat().st_size > 1024 * 1024:
            raise ValueError("Invalid framework manifest")
        metadata = json.loads(manifest.read_text())
        if not isinstance(metadata, dict) or metadata.get("schema_version") != SCHEMA:
            raise ValueError("Unknown framework manifest; provision a fresh directory")
        actual = android_art.inventory(destination)
        actual.pop("bifrost-framework.json", None)
        if (metadata.get("image_sha256") != IMAGE_SHA256 or
                not isinstance(metadata.get("files"), dict) or
                without_shell(actual) != without_shell(metadata["files"])):
            raise ValueError("Refusing modified framework runtime")
        if metadata.get("codecs", {}).get("adapter_fingerprint") != android_codecs.fingerprint():
            raise ValueError("Codec adapters changed; provision a fresh framework directory")
        install_bionic_links(destination)
        install_shell(destination, archive)
        install_shared_libraries(destination, archive)
        install_media_extractor(destination, archive)
        return update_inventory(metadata, manifest, {**metadata["files"], **BIONIC_LINKS,
                                                     "system/bin/sh": shell_inventory(),
                                                     **shared_library_inventory(),
                                                     **media_extractor_inventory()})
    android_art.install(art_root)  # verifies the pinned ART layer
    archive = image_archive(archive)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".android-framework-", dir=destination.parent) as directory:
        work = Path(directory)
        root = work / "rootfs"
        shutil.copytree(art_root.resolve(), root, symlinks=True)
        (root / "bifrost-art.json").unlink()
        system, _ = extract_system(archive, work)
        install_shell(root, system=system)
        install_shared_libraries(root, system=system)
        install_media_extractor(root, system=system)
        for name, target in boot_image_entries(system):
            path = root / f"system/framework/arm64/{name}"
            if target is None:
                dump(system, f"/system/framework/arm64/{name}", path)
            else:
                dump(system, f"/system/framework/{target[3:]}", root / f"system/framework/{target[3:]}")
                path.parent.mkdir(parents=True, exist_ok=True)
                path.symlink_to(target)
        primary, mainline = boot_classpaths(root)
        for jar in primary:
            if jar.startswith("/system/framework/"):
                dump(system, jar, root / jar.lstrip("/"))
        for apex in sorted({jar.split("/")[2].removeprefix("com.android.") for jar in mainline}):
            if not (root / "apex" / f"com.android.{apex}" / "javalib").is_dir():
                extract_mainline(system, apex, work, root)
        missing = [jar for jar in primary + mainline if not (root / jar.lstrip("/")).is_file()]
        if missing:
            raise ValueError("Boot classpath jars missing: " + ", ".join(missing))
        # Resources.getSystem() and the software text renderer require these
        # image assets even before an Activity asks for its first window.
        dump(system, "/system/framework/framework-res.apk", root / "system/framework/framework-res.apk")
        for name in ("fonts.xml", "font_fallback.xml"):
            dump(system, "/system/etc/" + name, root / "system/etc" / name)
        subprocess.run(["debugfs", "-R", "rdump /system/fonts .", str(system.resolve())],
                       cwd=root / "system", capture_output=True, check=True, timeout=60)
        if not (root / "system/fonts").is_dir():
            raise ValueError("Framework font extraction failed")
        # init loads these after /system/build.prop. product sets
        # ro.dalvik.vm.enable_uffd_gc, which with userfaultfd selects ART's
        # CMC collector: the collector the image's boot image was built for.
        android_super.unpack(work / "super.img", work / "partitions", ["system_ext", "product", "vendor"])
        for partition in ("system_ext", "product"):
            dump(work / "partitions" / f"{partition}.img", "/etc/build.prop",
                 root / partition / "etc/build.prop")
        # The pinned image supplies its app heap policy here (192m growth,
        # 512m maximum). Omitting it makes AndroidRuntime fall back to 16m.
        dump(work / "partitions/vendor.img", "/build.prop", root / "vendor/build.prop")
        # Use the real same-image provider, including its ARM64 JNI library.
        # A feature flag alone cannot satisfy WebViewFactory's package and
        # signature verification, or its native provider initialization.
        webview = root / "product/app/webview"
        apk = webview / "webview.apk"
        dump(work / "partitions/product.img", "/app/webview/webview.apk", apk,
             max_size=256 * 1024 * 1024)
        apk.chmod(0o444)  # ART rejects writable provider DEX for an app UID.
        with zipfile.ZipFile(apk) as package:
            libraries = [member for member in package.infolist()
                         if member.filename.startswith("lib/arm64-v8a/")]
            if not libraries or len(libraries) > 16:
                raise ValueError("Unexpected WebView native library set")
            for member in libraries:
                name = member.filename.removeprefix("lib/arm64-v8a/")
                if not re.fullmatch(r"[A-Za-z0-9_.+-]+\.so", name) or member.file_size > 256 * 1024 * 1024:
                    raise ValueError("Unsafe WebView native library")
                target = webview / "lib/arm64" / name
                target.parent.mkdir(parents=True, exist_ok=True)
                with package.open(member) as source, target.open("wb") as output:
                    shutil.copyfileobj(source, output, length=1024 * 1024)
        dump(system, "/system/bin/app_process64", root / "system/bin/app_process64")
        (root / "system/bin/app_process64").chmod(0o755)
        # MediaCodec and the other android.media boot classes load this JNI
        # library dynamically. app_process64's DT_NEEDED graph cannot discover
        # it; use the real library from the same verified framework image.
        dump(system, "/system/lib64/libmedia_jni.so", root / "system/lib64/libmedia_jni.so")
        graph, preloads = android_art.native_dependencies(root, system, limit=1024)
        codecs = android_codecs.install(root, system, work)
        # app_process64 lives in /system/bin; give it the same linker section.
        config = root / "linkerconfig/ld.config.txt"
        apexes = sorted(path.name for path in (root / "apex").iterdir()
                        if path.is_dir() and path.name != "com.android.media.swcodec")
        # Preserve the ART bootstrap's single canonical library scope. Each
        # exported identity delegates there, avoiding duplicate VM globals.
        paths = ["/apex/com.android.runtime/lib64/bionic"]
        paths += [f"/apex/{name}/lib64" for name in apexes
                  if (root / "apex" / name / "lib64").is_dir()]
        paths += ["/system/lib64", "/vendor/lib64/hw"]
        service_paths = [paths[0], "/apex/com.android.media.swcodec/lib64", *paths[1:], "/vendor/lib64"]
        config.write_text("dir.codec2 = /apex/com.android.media.swcodec/bin\n" +
                          framework_linker_configuration(apexes, paths) + android_codecs.linker_section(service_paths))
        files = android_art.inventory(root)
        metadata = {"schema_version": SCHEMA,
                    "purpose": "Android framework on guest ART; system services and Activity display not implemented",
                    "image_sha256": IMAGE_SHA256,
                    "environment": environment(primary, mainline), "files": files,
                    "native_dependencies": graph, "public_preloads": preloads, "codecs": codecs}
        (root / "bifrost-framework.json").write_text(json.dumps(metadata, indent=2) + "\n")
        root.rename(destination)
    return metadata


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, default=ROOT / "build/android-framework/rootfs")
    parser.add_argument("--art-root", type=Path, default=ROOT / "build/android-art/rootfs")
    parser.add_argument("--archive", type=Path)
    args = parser.parse_args()
    try:
        metadata = install(args.destination, args.art_root, args.archive)
        print(f"Verified framework runtime: {args.destination} ({len(metadata['files'])} files)")
    except (OSError, ValueError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        parser.exit(1, f"android-framework: {error}\n")
