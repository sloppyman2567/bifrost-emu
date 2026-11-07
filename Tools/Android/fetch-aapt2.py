#!/usr/bin/env python3
"""Install a pinned official Linux AAPT2; no Android runtime is installed."""
import io
import os
from pathlib import Path
import tempfile
import zipfile

import download_progress

VERSION = "8.7.3-12006047"
SHA256 = "028916a040b58f34c9c89984e1bc48f8da6581662014f2b66391860def8b9c36"
URL = ("https://dl.google.com/dl/android/maven2/com/android/tools/build/aapt2/"
       f"{VERSION}/aapt2-{VERSION}-linux.jar")


def main():
    destination = Path(__file__).resolve().parent.parent / "android-tools"
    destination.mkdir(exist_ok=True)
    jar = download_progress.fetch(URL, destination / f"aapt2-{VERSION}-linux.jar", "AAPT2", 8 * 1024 * 1024,
                                  check=download_progress.sha256_check(SHA256, "AAPT2 archive"), timeout=30)
    archive = jar.read_bytes()
    jar.unlink()
    with zipfile.ZipFile(io.BytesIO(archive)) as source:
        for name in ("aapt2", "NOTICE"):
            with tempfile.NamedTemporaryFile(dir=destination, delete=False) as temporary:
                path = Path(temporary.name)
                try:
                    temporary.write(source.read(name))
                    temporary.flush()
                    os.chmod(path, 0o755 if name == "aapt2" else 0o644)
                    os.replace(path, destination / name)
                finally:
                    path.unlink(missing_ok=True)
    print(destination / "aapt2")


if __name__ == "__main__":
    main()
