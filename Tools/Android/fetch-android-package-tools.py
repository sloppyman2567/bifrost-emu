#!/usr/bin/env python3
"""Fetch checksum-pinned AOSP APK signing and API-35 fixture tools."""

import base64
import hashlib
from pathlib import Path
import tempfile

import download_progress

ROOT = Path(__file__).resolve().parents[2]
BASE = "https://android.googlesource.com/platform/prebuilts/sdk/+/android-15.0.0_r1/"
FILES = {
    "d8.jar": (
        "tools/linux/lib/d8.jar",
        "875fab053a6d1bce99751d7a529fa7f80e408550275a2919d76fa4cea56d5819",
    ),
    "apksigner.jar": (
        "tools/linux/lib/apksigner.jar",
        "9469c60e5e40fc5c44a2f2338509cb6600cdf065e9b50f9fa3ca6c5be5bae6a9",
    ),
    "android.jar": (
        "35/public/android.jar",
        "ee568219aa3754977207a8fd849fdbf3a299658dd26dd0907d39ca830587f8aa",
    ),
}


def install(destination):
    destination.mkdir(parents=True, exist_ok=True)
    for name, (remote, digest) in FILES.items():
        target = destination / name
        if target.exists():
            if hashlib.sha256(target.read_bytes()).hexdigest() != digest:
                raise ValueError(f"Refusing modified tool: {target}")
            continue
        # The server sends base64 text; an interrupted download resumes from the text kept in the destination.
        def check(path, name=name, digest=digest):
            if hashlib.sha256(base64.b64decode(path.read_bytes(), validate=True)).hexdigest() != digest:
                raise ValueError(f"Hash mismatch: {name}")
        encoded = download_progress.fetch(BASE + remote + "?format=TEXT", destination / f".{name}.b64", name,
                                          48 * 1024 * 1024, check=check)
        data = base64.b64decode(encoded.read_bytes(), validate=True)
        encoded.unlink()
        with tempfile.NamedTemporaryFile(dir=destination, delete=False) as output:
            temporary = Path(output.name)
            try:
                output.write(data)
                output.flush()
                temporary.chmod(0o644)
                temporary.replace(target)
            finally:
                temporary.unlink(missing_ok=True)
    print(destination)


if __name__ == "__main__":
    install(ROOT / "Tools/android-tools")
