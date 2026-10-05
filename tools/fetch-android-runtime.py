#!/usr/bin/env python3
"""Install the pinned AOSP Bionic bootstrap fixture (not a complete ART runtime)."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REVISION = "8aeb37cca394ce39c1311744c60960cbd466aa77"
SHA256 = "83bf0dce249728dae48149b80d28b48115c54adad95a352120d58a6ac669d1fc"
URL = (f"https://android.googlesource.com/platform/prebuilts/runtime/+/{REVISION}/"
       "mainline/runtime/apex/com.android.runtime-arm64.apex?format=TEXT")
FILES = ("bin/linker64", "lib64/bionic/libc.so", "lib64/bionic/libdl.so",
         "lib64/bionic/libdl_android.so", "lib64/bionic/libm.so")
FILE_HASHES = {
    "bin/linker64": "dc36b5feed26752e8f0758cff8d6dcdd63b1434c03d6692b58ba5c09399b1d16",
    "lib64/bionic/libc.so": "9f1f93bbd186af5c40a93f6ca4425bfdce82b7f3b9c730e59bb5c7fa598593cf",
    "lib64/bionic/libdl.so": "7e215e0e68372d779492438d1a8e0e53e63154812d506af830e6c3dbcaeb0607",
    "lib64/bionic/libdl_android.so": "a04282cb138f39d0c1226c5771e10e124a79f1c1522659c915d827415f85af69",
    "lib64/bionic/libm.so": "74ec94e6aecf17ab8d56d2da151dd0756e21e8f53e567815cbf43c5266224c5b",
}


def install(destination, archive=None):
    if destination.exists():
        manifest = destination / "bifrost-runtime.json"
        if manifest.is_file():
            metadata = json.loads(manifest.read_text())
            runtime = destination / "apex/com.android.runtime"
            if (metadata.get("archive_sha256") == SHA256 and
                metadata.get("files") == FILE_HASHES and
                all((runtime / name).is_file() and hashlib.sha256((runtime / name).read_bytes()).hexdigest() == digest
                    for name, digest in metadata["files"].items())):
                print(f"Verified existing Bionic bootstrap in {destination}")
                return
        raise ValueError(f"Refusing to replace an existing or modified runtime: {destination}")
    if not shutil.which("debugfs"):
        raise ValueError("debugfs (e2fsprogs) is required to read the pinned APEX payload")
    if archive:
        if archive.stat().st_size > 32 * 1024 * 1024:
            raise ValueError("Runtime archive exceeds the download bound")
        data = archive.read_bytes()
    else:
        with urllib.request.urlopen(URL, timeout=60) as response:
            encoded = response.read(32 * 1024 * 1024 + 1)
        if len(encoded) > 32 * 1024 * 1024:
            raise ValueError("Runtime download exceeds the download bound")
        data = base64.b64decode(encoded, validate=True)
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise ValueError("Pinned runtime SHA-256 mismatch")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".android-bootstrap-", dir=destination.parent) as work:
        temporary = Path(work)
        apex = temporary / "runtime.apex"
        apex.write_bytes(data)
        staging = temporary / "rootfs"
        runtime = staging / "apex/com.android.runtime"
        with zipfile.ZipFile(apex) as package:
            # Only these fixed members are read; never extract archive paths.
            payload = package.getinfo("apex_payload.img")
            if payload.file_size > 32 * 1024 * 1024:
                raise ValueError("Oversized APEX payload")
            (temporary / "payload.img").write_bytes(package.read(payload))
            notice = package.read("assets/NOTICE.html.gz")
        hashes = {}
        for index, name in enumerate(FILES):
            generated = f"file-{index}"
            result = subprocess.run(["debugfs", "-R", f"dump /{name} {generated}", "payload.img"],
                                    cwd=temporary, capture_output=True, timeout=30)
            output = temporary / generated
            if result.returncode or not output.is_file():
                raise ValueError(f"Cannot read {name} from the pinned payload")
            header = output.read_bytes()[:20]
            if header[:6] != b"\x7fELF\x02\x01" or header[18:20] != b"\xb7\x00":
                raise ValueError(f"Runtime file is not ARM64 ELF64: {name}")
            target = runtime / name
            target.parent.mkdir(parents=True, exist_ok=True)
            output.rename(target)
            target.chmod(0o755 if name.startswith("bin/") else 0o644)
            hashes[name] = hashlib.sha256(target.read_bytes()).hexdigest()
            if hashes[name] != FILE_HASHES[name]:
                raise ValueError(f"Extracted runtime SHA-256 mismatch: {name}")
        (staging / "system/bin").mkdir(parents=True)
        (staging / "system/bin/linker64").symlink_to("../../apex/com.android.runtime/bin/linker64")
        (staging / "data/local/tmp").mkdir(parents=True)
        (staging / "linkerconfig").mkdir()
        (staging / "linkerconfig/ld.config.txt").write_text(
            "dir.bootstrap = /data/local/tmp\n\n[bootstrap]\n"
            "namespace.default.isolated = true\n"
            "namespace.default.search.paths = /apex/com.android.runtime/lib64/bionic\n"
            "namespace.default.permitted.paths = /data/local/tmp:/apex/com.android.runtime/lib64/bionic\n")
        (staging / "NOTICE.html.gz").write_bytes(notice)
        (staging / "bifrost-runtime.json").write_text(json.dumps({
            "purpose": "Bionic bootstrap only; ART/framework not included",
            "aosp_tag": "android-15.0.0_r1", "prebuilts_revision": REVISION,
            "url": URL, "archive_sha256": SHA256, "files": hashes,
            "guest_page_size": 4096,
        }, indent=2) + "\n")
        staging.rename(destination)
    print(f"Installed pinned Bionic bootstrap in {destination}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, default=ROOT / "tools/android-runtime/rootfs")
    parser.add_argument("--archive", type=Path, help="use an already downloaded, hash-checked APEX")
    args = parser.parse_args()
    try:
        install(args.destination, args.archive)
    except (OSError, ValueError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        parser.exit(1, f"android-runtime: {error}\n")
