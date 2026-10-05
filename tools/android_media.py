"""Provision real ARM64 Media NDK libraries from a checksum-pinned AOSP image.

The SDK/NDK link stubs are never installed. Platform dependencies live in their
own linker namespace; only libmediandk.so is exposed to the app namespace.
"""

import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
URL = "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-35_r02.zip"
IMAGE_SIZE = 769099654
IMAGE_SHA256 = "1e6ae4fca6449c07278a333311f0ef29c5101d1e3e18ba48fbe9bd2dc5006926"
FILES_SHA256 = "e892e694d4bf7c6654cd2459fadfce8d64488c43413a7b18efab49dc17d907fa"
BIONIC = {"libc.so", "libm.so", "libdl.so", "libdl_android.so"}
LIBRARY = re.compile(r"[A-Za-z0-9_.+@-]+\.so\Z")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify_files(files):
    if not isinstance(files, dict) or len(files) != 156:
        raise ValueError("Invalid Media NDK file set")
    serialized = json.dumps(files, sort_keys=True, separators=(",", ":")).encode()
    if hashlib.sha256(serialized).hexdigest() != FILES_SHA256:
        raise ValueError("Media NDK file set does not match the pinned image")


def verify(root):
    manifest = root / "media.json"
    if manifest.is_symlink() or manifest.stat().st_size > 128 * 1024:
        raise ValueError("Invalid Media NDK manifest")
    metadata = json.loads(manifest.read_text())
    if metadata.get("schema_version") != 1 or metadata.get("image_sha256") != IMAGE_SHA256:
        raise ValueError("Unknown Android media image")
    files = metadata.get("files")
    verify_files(files)
    for name, value in files.items():
        path = root / name
        if path.is_symlink() or digest(path) != value:
            raise ValueError(f"Modified Media NDK file: {name}")
    return metadata


def needed(path):
    with path.open("rb") as stream:
        header = stream.read(20)
    if header[:6] != b"\x7fELF\x02\x01" or header[18:20] != b"\xb7\0":
        raise ValueError(f"Media dependency is not ARM64 ELF64: {path.name}")
    result = subprocess.run(["readelf", "--wide", "--dynamic", str(path)],
                            capture_output=True, text=True, check=True, timeout=30)
    return re.findall(r"\(NEEDED\).*\[([^\]]+)\]", result.stdout)


def dump(image, remote, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    # debugfs parses its command separately from shell argv. Use a fixed safe
    # basename and cwd so workspace paths containing spaces need no quoting.
    subprocess.run(["debugfs", "-R", f"dump {remote} {target.name}", str(image.resolve())],
                   cwd=target.parent, capture_output=True, check=True, timeout=30)
    # debugfs can return zero when a requested inode is absent.
    if not target.is_file() or target.stat().st_size > 64 * 1024 * 1024:
        raise ValueError(f"Cannot extract bounded Android file: {remote}")


def copy_bytes(source, output, length):
    while length:
        block = source.read(min(length, 1024 * 1024))
        if not block:
            raise ValueError("Truncated Android image")
        output.write(block)
        length -= len(block)


def image_archive(archive):
    if archive is None:
        cache = ROOT / "build/android-media-source"
        cache.mkdir(parents=True, exist_ok=True)
        archive = cache / "arm64-v8a-35_r02.zip"
        if not archive.exists():
            with tempfile.NamedTemporaryFile(dir=cache, delete=False) as stream:
                temporary = Path(stream.name)
                try:
                    with urllib.request.urlopen(URL, timeout=60) as response:
                        copy_bytes(response, stream, IMAGE_SIZE)
                        if response.read(1):
                            raise ValueError("Oversized Android image download")
                    stream.close()
                    if digest(temporary) != IMAGE_SHA256:
                        raise ValueError("Android image SHA-256 mismatch")
                    temporary.replace(archive)
                finally:
                    temporary.unlink(missing_ok=True)
    if archive.stat().st_size != IMAGE_SIZE or digest(archive) != IMAGE_SHA256:
        raise ValueError("Pinned Android image SHA-256 mismatch")
    return archive


def extract_system(archive, temporary):
    """Extract the fixed system partition from an already verified SDK archive."""
    # The pinned emulator disk is GPT containing a dynamic super partition.
    # Read its bounded partition table rather than mounting host filesystems.
    with zipfile.ZipFile(archive) as package:
        member = package.getinfo("arm64-v8a/system.img")
        if member.file_size != 1622147072:
            raise ValueError("Unexpected Android system disk size")
        disk = temporary / "disk.img"
        with package.open(member) as source, disk.open("wb") as output:
            copy_bytes(source, output, member.file_size)
        notice = package.getinfo("arm64-v8a/NOTICE.txt")
        if notice.file_size > 8 * 1024 * 1024:
            raise ValueError("Oversized Android notice")
        notice_data = package.read(notice)
    with disk.open("rb") as source:
        source.seek(512)
        header = source.read(512)
        if header[:8] != b"EFI PART":
            raise ValueError("Missing Android GPT header")
        table, count, stride = struct.unpack_from("<QII", header, 72)
        if count > 128 or stride != 128 or table * 512 + count * stride > disk.stat().st_size:
            raise ValueError("Invalid Android GPT partition table")
        source.seek(table * 512)
        partitions = [source.read(stride) for _ in range(count)]
        selected = [item for item in partitions
                    if item[56:128].decode("utf-16le").rstrip("\0") == "super"]
        if len(selected) != 1:
            raise ValueError("Missing Android super partition")
        start, end = struct.unpack_from("<QQ", selected[0], 32)
        if start > end or (end + 1) * 512 > disk.stat().st_size:
            raise ValueError("Invalid Android super partition bounds")
        source.seek(start * 512)
        image = temporary / "super.img"
        with image.open("wb") as output:
            copy_bytes(source, output, (end - start + 1) * 512)
    disk.unlink()
    partitions = temporary / "partitions"
    partitions.mkdir()
    subprocess.run(["lpunpack", "-p", "system", str(image), str(partitions)],
                   capture_output=True, check=True, timeout=120)
    system = partitions / "system.img"
    return system, notice_data


def install(destination, archive=None):
    if destination.exists():
        return verify(destination)
    for tool in ("debugfs", "lpunpack", "readelf"):
        if not shutil.which(tool):
            raise ValueError(f"{tool} is required to extract real Android media libraries")
    archive = image_archive(archive)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".android-media-", dir=destination.parent) as work:
        temporary = Path(work)
        staging = temporary / "bundle"
        staging.mkdir()
        system, notice = extract_system(archive, temporary)
        (staging / "NOTICE.txt").write_bytes(notice)
        apex = temporary / "i18n.apex"
        dump(system, "/system/apex/com.android.i18n.apex", apex)
        with zipfile.ZipFile(apex) as package:
            member = package.getinfo("apex_payload.img")
            if member.file_size > 64 * 1024 * 1024:
                raise ValueError("Oversized i18n payload")
            i18n = temporary / "i18n.img"
            i18n.write_bytes(package.read(member))
        i18n_libraries = {"libandroidicu.so", "libicuuc.so", "libicui18n.so"}
        queue = ["libmediandk.so"]
        seen = set()
        while queue:
            name = queue.pop()
            if name in seen or name in BIONIC:
                continue
            if not LIBRARY.fullmatch(name) or len(seen) >= 256:
                raise ValueError("Invalid or oversized Media NDK dependency graph")
            seen.add(name)
            target = staging / "lib64" / name
            if name in i18n_libraries:
                dump(i18n, "/lib64/" + name, target)
            else:
                dump(system, "/system/lib64/" + name, target)
            queue.extend(needed(target))
        dump(i18n, "/etc/icu/icudt75l.dat", staging / "icu/icudt75l.dat")
        files = {str(path.relative_to(staging)): digest(path)
                 for path in sorted(staging.rglob("*")) if path.is_file()}
        verify_files(files)
        metadata = {"schema_version": 1, "url": URL, "image_sha256": IMAGE_SHA256,
                    "purpose": "Real Media NDK dependencies; Android media services not included",
                    "files": files}
        (staging / "media.json").write_text(json.dumps(metadata, indent=2) + "\n")
        staging.rename(destination)
    return metadata


def linker_configuration():
    return (
        "additional.namespaces = media\n"
        "namespace.default.links = media\n"
        "namespace.default.link.media.shared_libs = libmediandk.so\n"
        "namespace.media.isolated = true\n"
        "namespace.media.search.paths = /system/lib64/media\n"
        "namespace.media.permitted.paths = /system/lib64/media:/apex/com.android.i18n\n"
        "namespace.media.links = default\n"
        "namespace.media.link.default.shared_libs = libc.so:libm.so:libdl.so:libdl_android.so\n"
    )


def guest_path(name):
    if name.startswith("lib64/"):
        return "system/lib64/media/" + name.removeprefix("lib64/")
    if name.startswith("icu/"):
        return "apex/com.android.i18n/etc/" + name
    if name == "NOTICE.txt":
        return "system/lib64/media/NOTICE.txt"
    raise ValueError("Invalid media bundle path")


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/android-media")
    parser.add_argument("--archive", type=Path)
    args = parser.parse_args()
    metadata = install(args.output, args.archive)
    print(f"Verified real Android Media NDK: {len(metadata['files']) - 2} libraries in {args.output}")
