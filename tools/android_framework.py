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

from android_media import ROOT, IMAGE_SHA256, image_archive, extract_system, dump
import android_art

# Classpath order comes from the image's own boot image files (see
# boot_classpaths): the primary boot.oat records its jars in the
# "bootclasspath" key; the mainline extension lists its dex locations.
JAR_PATH = re.compile(rb"/(?:apex/com\.android\.[a-z0-9.]+/javalib|system/framework)/[A-Za-z0-9._-]+\.jar")
SCHEMA = 3
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
              "namespace.default.permitted.paths = /apex:/system:/data/local/tmp\n")
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
        if metadata.get("image_sha256") != IMAGE_SHA256 or actual != metadata.get("files"):
            raise ValueError("Refusing modified framework runtime")
        return metadata
    android_art.install(art_root)  # verifies the pinned ART layer
    archive = image_archive(archive)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".android-framework-", dir=destination.parent) as directory:
        work = Path(directory)
        root = work / "rootfs"
        shutil.copytree(art_root.resolve(), root, symlinks=True)
        (root / "bifrost-art.json").unlink()
        system, _ = extract_system(archive, work)
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
        dump(system, "/system/bin/app_process64", root / "system/bin/app_process64")
        (root / "system/bin/app_process64").chmod(0o755)
        graph, preloads = android_art.native_dependencies(root, system, limit=1024)
        # app_process64 lives in /system/bin; give it the same linker section.
        config = root / "linkerconfig/ld.config.txt"
        apexes = sorted(path.name for path in (root / "apex").iterdir() if path.is_dir())
        # Preserve the ART bootstrap's single canonical library scope. Each
        # exported identity delegates there, avoiding duplicate VM globals.
        paths = ["/apex/com.android.runtime/lib64/bionic"]
        paths += [f"/apex/{name}/lib64" for name in apexes
                  if (root / "apex" / name / "lib64").is_dir()]
        paths += ["/system/lib64"]
        config.write_text(framework_linker_configuration(apexes, paths))
        files = android_art.inventory(root)
        metadata = {"schema_version": SCHEMA,
                    "purpose": "Android framework on guest ART; services and Activity launch not implemented",
                    "image_sha256": IMAGE_SHA256,
                    "environment": environment(primary, mainline), "files": files,
                    "native_dependencies": graph, "public_preloads": preloads}
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
