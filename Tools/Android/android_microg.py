"""Provision pinned microG Services and its Play Store companion as system apps.

microG (https://github.com/microg/GmsCore) reimplements Google Play services
under the real package names. Release v0.3.17.252432 is pinned by size and
SHA-256; both hashes match microG's signed F-Droid index (repository
certificate SHA-256 9BD06727E62796C0130EB6DAB39B73157451582CBD138E86C468ACC395D14165,
the fingerprint microG's download page publishes).

Each APK requests android.permission.FAKE_PACKAGE_SIGNATURE and declares
"fake-signature" meta-data; FrameworkPackages reports that certificate as
microG's framework patches do. UIDs are reserved in android_sessions.app_id.
"""
from pathlib import Path
import re
import shutil
import tempfile
import urllib.request
import zipfile

from android_media import ROOT, copy_bytes, digest

VERSION = "v0.3.17.252432"
URL = f"https://github.com/microg/GmsCore/releases/download/{VERSION}/"
# package: (priv-app directory, release asset, size, SHA-256, UID)
PACKAGES = {
    "com.google.android.gms": ("GmsCore", "com.google.android.gms-252432034.apk", 108113833,
                               "e5e6cba078bdec812b5450599350e5c1a12c8b7438da5bf77123af170f8c1a35", 19998),
    "com.android.vending": ("Phonesky", "com.android.vending-84022634.apk", 4210169,
                            "9b8616011cc8f026bcbbe094df850794abb9d6d9f23214d7a8f8c4d1033ea012", 19999),
}
UIDS = {package: value[4] for package, value in PACKAGES.items()}
LIBRARY = re.compile(r"[A-Za-z0-9_.+-]+\.so\Z")


def archive(package, cache=ROOT / "build/microg"):
    """Return the verified release APK, downloading it on first use."""
    _, name, size, sha256, _ = PACKAGES[package]
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / name
    if not path.exists():
        with tempfile.NamedTemporaryFile(dir=cache, delete=False) as stream:
            temporary = Path(stream.name)
            try:
                with urllib.request.urlopen(URL + name, timeout=60) as response:
                    copy_bytes(response, stream, size, f"microG {name}")
                    if response.read(1):
                        raise ValueError(f"Oversized microG download: {name}")
                stream.close()
                if digest(temporary) != sha256:
                    raise ValueError(f"microG SHA-256 mismatch: {name}")
                temporary.chmod(0o644)  # NamedTemporaryFile is 0600; installs share this file
                temporary.replace(path)
            finally:
                temporary.unlink(missing_ok=True)
    if path.stat().st_size != size or digest(path) != sha256:
        raise ValueError(f"Pinned microG SHA-256 mismatch: {name}")
    return path


def install(root):
    """Install both packages under root/system/priv-app; return their identity."""
    identity = {}
    for package, (directory, _, _, sha256, _) in PACKAGES.items():
        source = archive(package)
        target = root / "system/priv-app" / directory
        target.mkdir(parents=True, exist_ok=True)
        apk = target / f"{directory}.apk"
        shutil.copyfile(source, apk)
        apk.chmod(0o444)  # ART rejects writable DEX for an app UID.
        # extractNativeLibs="true": PackageManager extracts lib/<abi>/ at install.
        with zipfile.ZipFile(apk) as contents:
            libraries = [member for member in contents.infolist()
                         if member.filename.startswith("lib/arm64-v8a/")]
            if len(libraries) > 32:
                raise ValueError(f"Unexpected microG native library set: {package}")
            for member in libraries:
                name = member.filename.removeprefix("lib/arm64-v8a/")
                if not LIBRARY.fullmatch(name) or member.file_size > 256 * 1024 * 1024:
                    raise ValueError(f"Unsafe microG native library: {member.filename}")
                library = target / "lib/arm64" / name
                library.parent.mkdir(parents=True, exist_ok=True)
                with contents.open(member) as stream, library.open("wb") as output:
                    shutil.copyfileobj(stream, output, length=1024 * 1024)
        identity[package] = sha256
    return {"version": VERSION, "packages": identity}
