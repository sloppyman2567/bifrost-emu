"""Provision a separate, same-image ARM64 ART bootstrap (not an Android framework)."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

from android_media import (ROOT, URL, IMAGE_SHA256, image_archive, extract_system, dump, needed,
                           digest, LIBRARY, without_bionic_links, install_bionic_links,
                           update_bionic_inventory)

APEXES = {
    "art": "capex", "runtime": "apex", "i18n": "apex",
    "conscrypt": "capex", "tzdata": "apex", "os.statsd": "apex", "adbd": "capex",
}
FILES_SHA256 = "380cddb785f861e0c04e7c84307280968010b024a405dd557fde5e2f1a89bd7b"


def native_dependencies(root, system, limit=512):
    """Close DT_NEEDED plus libnativeloader's configured public preloads."""
    for name in ("public.libraries.txt", "llndk.libraries.txt"):
        dump(system, "/system/etc/" + name, root / "system/etc" / name)
    preloads = []
    for line in (root / "system/etc/public.libraries.txt").read_text().splitlines():
        fields = line.split("#", 1)[0].split()
        if not fields or "nopreload" in fields or "32" in fields:
            continue
        if not LIBRARY.fullmatch(fields[0]):
            raise ValueError("Unsafe ART public library name")
        preloads.append(fields[0])
    candidates = {}
    queue = []
    for path in sorted(root.rglob("*")):
        if path.is_file() and not path.is_symlink():
            with path.open("rb") as stream:
                elf = stream.read(4) == b"\x7fELF"
            if elf:
                queue.append(path)
                if path.name.endswith(".so"):
                    candidates.setdefault(path.name, path)

    def require(name):
        if not LIBRARY.fullmatch(name):
            raise ValueError(f"Unsafe ART dependency: {name}")
        if name not in candidates:
            if len(candidates) >= limit:
                raise ValueError("Oversized ART dependency graph")
            target = root / "system/lib64" / name
            dump(system, "/system/lib64/" + name, target)
            candidates[name] = target
            queue.append(target)

    for name in preloads:
        require(name)
    seen, graph = set(), {}
    while queue:
        path = queue.pop()
        if path in seen:
            continue
        seen.add(path)
        dependencies = needed(path)
        graph[str(path.relative_to(root))] = dependencies
        for name in dependencies:
            require(name)
    return graph, preloads


def linker_configuration(paths):
    """Export boot APEX identities while sharing one canonical native scope.

    This standalone bootstrap has no Android linkerconfig service. Delegating
    the APEX namespaces to default avoids loading a second libart/libcore and
    splitting their VM globals when libnativeloader opens boot-class JNI code.
    """
    namespaces = ["com_android_" + name.replace(".", "_") for name in APEXES]
    config = (
        "dir.art = /apex/com.android.art/bin\n"
        "dir.art = /data/local/tmp\n\n[art]\n"
        "additional.namespaces = " + ",".join(namespaces) + "\n"
        "namespace.default.isolated = true\n"
        "namespace.default.visible = true\n"
        "namespace.default.search.paths = " + ":".join(paths) + "\n"
        "namespace.default.permitted.paths = /apex:/system:/data/local/tmp\n")
    for name, namespace in zip(APEXES, namespaces):
        prefix = "namespace." + namespace
        config += (
            prefix + ".isolated = true\n" + prefix + ".visible = true\n" +
            prefix + ".permitted.paths = /apex/com.android." + name + "\n" +
            prefix + ".links = default\n" +
            prefix + ".link.default.allow_all_shared_libs = true\n")
    return config


def verify_inventory(files):
    if isinstance(files, dict):
        files = without_bionic_links(files)
    if not isinstance(files, dict) or len(files) != 529:
        raise ValueError("Invalid ART file inventory")
    encoded = json.dumps(files, sort_keys=True, separators=(",", ":")).encode()
    if hashlib.sha256(encoded).hexdigest() != FILES_SHA256:
        raise ValueError("ART file inventory differs from the pinned SDK image")


def inventory(root):
    result = {}
    for path in sorted(root.rglob("*")):
        name = str(path.relative_to(root))
        if path.is_symlink():
            result[name] = {"link": str(path.readlink())}
        elif path.is_file():
            result[name] = {"sha256": digest(path), "executable": bool(path.stat().st_mode & 0o111)}
        elif not path.is_dir():
            raise ValueError(f"Unexpected ART file type: {name}")
    return result


def unpack_apex(system, name, suffix, work, root):
    archive = work / (name + "." + suffix)
    dump(system, f"/system/apex/com.android.{name}.{suffix}", archive)
    if suffix == "capex":
        with zipfile.ZipFile(archive) as package:
            member = package.getinfo("original_apex")
            if member.file_size > 128 * 1024 * 1024:
                raise ValueError("Oversized decompressed APEX")
            archive = work / (name + ".apex")
            archive.write_bytes(package.read(member))
    with zipfile.ZipFile(archive) as package:
        member = package.getinfo("apex_payload.img")
        if member.file_size > 128 * 1024 * 1024:
            raise ValueError("Oversized APEX payload")
        image = work / (name + ".img")
        image.write_bytes(package.read(member))
    destination = root / "apex" / ("com.android." + name)
    destination.mkdir(parents=True)
    # The archive is pinned, and debugfs writes into a fresh private staging
    # directory. No mounts, privilege, or host Android libraries are required.
    subprocess.run(["debugfs", "-R", "rdump / .", str(image.resolve())],
                   cwd=destination, capture_output=True, check=True, timeout=60)
    if not (destination / "apex_manifest.pb").is_file():
        raise ValueError(f"APEX extraction failed: {name}")
    shutil.rmtree(destination / "lost+found", ignore_errors=True)


class ObsoleteRuntime(ValueError):
    """An existing tree that no later change can make usable: only a fresh provision fixes it."""


REPLACE_HINT = "run it again with --replace-obsolete (that deletes the old tree and downloads the Android image again)"


def obsolete_reason(destination):
    """Why an existing runtime tree has to be regenerated, judged from its manifest alone (no file is read), or None.
    A tree whose manifest cannot be read, or is not ours, is not judged here: install() refuses it."""
    manifest = destination / "bifrost-art.json"
    if destination.is_symlink() or manifest.is_symlink() or not manifest.is_file() or manifest.stat().st_size > 256 * 1024:
        return None
    try:
        metadata = json.loads(manifest.read_text())
    except ValueError:
        return None
    if not isinstance(metadata, dict) or metadata.get("schema_version") != 2:
        return "its manifest is from an older format"
    if metadata.get("image_sha256") != IMAGE_SHA256:
        return "it was built from another Android image"
    return None


def discard(destination):
    """Delete a runtime tree the image made read-only. Only a real directory goes: never a link, never a path that
    holds no manifest (the callers have judged it obsolete from that manifest)."""
    if destination.is_symlink() or not destination.is_dir():
        raise ValueError(f"Refusing to delete {destination}: not a runtime directory")
    for directory, _, _ in os.walk(destination):  # does not follow links
        path = Path(directory)
        if not path.is_symlink():
            path.chmod(path.stat().st_mode | 0o700)
    shutil.rmtree(destination)


def install(destination, archive=None, replace_obsolete=False):
    if destination.is_symlink():
        raise ValueError("Refusing redirected ART runtime")
    destination = destination.resolve()
    if replace_obsolete and destination.exists():
        reason = obsolete_reason(destination)
        if reason:
            print(f"Replacing the ART runtime at {destination}: {reason}")
            discard(destination)
    if destination.exists():
        manifest = destination / "bifrost-art.json"
        if manifest.is_symlink() or not manifest.is_file() or manifest.stat().st_size > 256 * 1024:
            raise ValueError("Invalid ART manifest")
        metadata = json.loads(manifest.read_text())
        if not isinstance(metadata, dict) or metadata.get("schema_version") != 2:
            raise ObsoleteRuntime(f"Obsolete or unknown ART manifest; {REPLACE_HINT}")
        if metadata.get("image_sha256") != IMAGE_SHA256:
            raise ObsoleteRuntime(f"The ART runtime was built from another Android image; {REPLACE_HINT}")
        verify_inventory(metadata.get("files"))
        actual = inventory(destination)
        actual.pop("bifrost-art.json", None)
        # Writable data and fixture files are deliberately outside this root.
        if without_bionic_links(actual) != without_bionic_links(metadata["files"]):
            raise ValueError("Refusing modified ART runtime")
        return update_bionic_inventory(destination, metadata, manifest)
    for tool in ("debugfs", "readelf"):
        if not shutil.which(tool):
            raise ValueError(f"Required ART extraction tool: {tool}")
    archive = image_archive(archive)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".android-art-", dir=destination.parent) as directory:
        work = Path(directory)
        root = work / "rootfs"
        root.mkdir()
        system, notice = extract_system(archive, work)
        for name, suffix in APEXES.items():
            unpack_apex(system, name, suffix, work, root)
        (root / "NOTICE.txt").write_bytes(notice)
        (root / "system/bin").mkdir(parents=True)
        (root / "system/bin/linker64").symlink_to("../../apex/com.android.runtime/bin/linker64")
        dump(system, "/system/build.prop", root / "system/build.prop")
        # Loaded by libartpalette at runtime, absent from DT_NEEDED.
        dump(system, "/system/lib64/libartpalette-system.so", root / "system/lib64/libartpalette-system.so")
        # DT_NEEDED alone misses libraries opened by libnativeloader at VM
        # startup. Preserve the image's public list and preload its closure.
        graph, preloads = native_dependencies(root, system)
        paths = ["/apex/com.android.runtime/lib64/bionic", "/apex/com.android.art/lib64",
                 "/apex/com.android.i18n/lib64", "/apex/com.android.conscrypt/lib64",
                 "/apex/com.android.os.statsd/lib64", "/apex/com.android.adbd/lib64", "/system/lib64"]
        (root / "linkerconfig").mkdir()
        (root / "linkerconfig/ld.config.txt").write_text(linker_configuration(paths))
        install_bionic_links(root)
        files = inventory(root)
        verify_inventory(files)
        metadata = {"schema_version": 2, "purpose": "ART bootstrap only; framework startup not implemented",
                    "url": URL, "image_sha256": IMAGE_SHA256, "guest_page_size": 4096,
                    "files": files, "native_dependencies": graph,
                    "public_preloads": preloads}
        (root / "bifrost-art.json").write_text(json.dumps(metadata, indent=2) + "\n")
        root.rename(destination)
    return metadata


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, default=ROOT / "build/android-art/rootfs")
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--replace-obsolete", action="store_true",
                        help="delete an existing tree whose manifest is from another format or image, then provision it again")
    args = parser.parse_args()
    try:
        metadata = install(args.destination, args.archive, args.replace_obsolete)
        print(f"Verified ART runtime: {args.destination} ({len(metadata['files'])} files)")
    except (OSError, ValueError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        parser.exit(1, f"android-art: {error}\n")
