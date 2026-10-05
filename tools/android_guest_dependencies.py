"""Build genuine guest dependencies using pinned upstream sources/Bionic headers."""

import hashlib
import gzip
import io
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
BIONIC_REVISION = "361ba86734fb2821a6adcfdf775db8abd04e0de0" # android-15.0.0_r1
ZLIB_VERSION = "1.3.2"
ARCHIVES = {
    "zlib": (
        "https://zlib.net/fossils/zlib-1.3.2.tar.gz",
        "bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16",
    ),
    "include": (
        f"https://android.googlesource.com/platform/bionic/+archive/{BIONIC_REVISION}/libc/include.tar.gz",
        "666f5d2a7b0c4931d069a8856a229e02723acba864505687ba411c004fa9a593",
    ),
    "uapi": (
        f"https://android.googlesource.com/platform/bionic/+archive/{BIONIC_REVISION}/libc/kernel/uapi.tar.gz",
        "47f50e8db29884a09d3ad4055f92db3a95ff5119688514b7672892f0bdea810f",
    ),
    "android": (
        f"https://android.googlesource.com/platform/bionic/+archive/{BIONIC_REVISION}/libc/kernel/android.tar.gz",
        "6c5c2ef512baf9649b3f0c0d1a09a2965f2c7378692168f1d4c9f68276872099",
    ),
    "ndk": (
        "https://android.googlesource.com/platform/frameworks/native/+archive/f7274fca5e36082674740bc6c976f73c4578d009/include/android.tar.gz",
        "2cf6bf139b0b26997bdbdb76e081caa58372f01fb60792a656abdc80f9d4dea7",
    ),
    "window": (
        "https://android.googlesource.com/platform/frameworks/native/+archive/f7274fca5e36082674740bc6c976f73c4578d009/libs/nativewindow/include/android.tar.gz",
        "728f2bfd14a860982ed6bd149dfe9860af11d32bd929b824af6a916b5139145f",
    ),
    "rect": (
        "https://android.googlesource.com/platform/frameworks/native/+archive/f7274fca5e36082674740bc6c976f73c4578d009/libs/arect/include/android.tar.gz",
        "099db416513348f19360c6f0d21ac0e1dc30f573e782cd3fe59cd9ca16ca8457",
    ),
    "jni": (
        "https://android.googlesource.com/platform/libnativehelper/+archive/3ca43dfe2bf4613852df0303531fa8ecf4b7063c/include_jni.tar.gz",
        "5834335dd71af809ad606f25fea8dcb88d4c47cccfc9b0d2426b578076eaf5b0",
    ),
    "log": (
        "https://android.googlesource.com/platform/system/logging/+archive/54d3fa266d2123c0b076c646fae60d049311052a/liblog/include/android.tar.gz",
        "516fe848bf798ad7a027369e4a51a526fbb319afd2bdf99ca23c39dc92e37d10",
    ),
}


def extract(name, destination):
    url, expected = ARCHIVES[name]
    cache = ROOT / "build/android-dependencies"
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / (expected + ".tar.gz")
    if archive.exists():
        if archive.stat().st_size > 4 * 1024 * 1024:
            raise ValueError(f"Oversized cached Android dependency: {name}")
        data = archive.read_bytes()
    else:
        for attempt in range(3):
            try:
                request_url = url if not attempt else url + f"?retry={attempt}"
                with urllib.request.urlopen(request_url, timeout=30) as response:
                    data = response.read(4 * 1024 * 1024 + 1)
                break
            except urllib.error.HTTPError as error:
                if error.code not in (429, 500, 502, 503, 504) or attempt == 2:
                    raise
                time.sleep(1)
    if len(data) > 4 * 1024 * 1024:
        raise ValueError(f"Oversized Android dependency: {name}")
    with gzip.GzipFile(fileobj=io.BytesIO(data)) as compressed:
        raw = compressed.read(64 * 1024 * 1024 + 1)
    if len(raw) > 64 * 1024 * 1024:
        raise ValueError(f"Oversized extracted Android dependency: {name}")
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as source:
        members = source.getmembers()
        if len(members) > 8192 or sum(item.size for item in members) > 64 * 1024 * 1024:
            raise ValueError(f"Oversized extracted Android dependency: {name}")
        names = set()
        contents = hashlib.sha256()
        for item in sorted(members, key=lambda member: member.name):
            path = Path(item.name)
            if (path.is_absolute() or ".." in path.parts or item.name in names or
                    not (item.isfile() or item.isdir())):
                raise ValueError(f"Unsafe Android dependency member: {item.name}")
            names.add(item.name)
            if item.isfile():
                contents.update(item.name.encode("utf-8") + b"\0")
                contents.update(item.size.to_bytes(8, "big"))
                contents.update(source.extractfile(item).read())
        # Gitiles inserts the request time into archive metadata. Pin names,
        # lengths and file bytes instead; retain upstream's published archive
        # checksum for the static zlib release. Never trust a cache unverified.
        actual = contents.hexdigest() if "+archive/" in url else hashlib.sha256(data).hexdigest()
        if actual != expected:
            raise ValueError(f"Pinned Android dependency SHA-256 mismatch: {name}")
        if not archive.exists():
            with tempfile.NamedTemporaryFile(dir=cache, delete=False) as stream:
                temporary = Path(stream.name)
                try:
                    stream.write(data)
                    stream.close()
                    temporary.replace(archive)
                finally:
                    temporary.unlink(missing_ok=True)
        source.extractall(destination, filter="data")


def prepare_headers(directory):
    for name in ("include", "uapi", "android", "jni"):
        extract(name, directory / name)
    for name in ("ndk", "window", "rect", "log"):
        extract(name, directory / "include/android")
    includes = []
    for path in (directory / "include", directory / "uapi",
                 directory / "uapi/asm-arm64", directory / "android/uapi", directory / "jni"):
        includes += ["-isystem", str(path)]
    return includes


def build_zlib(staging, bionic, compiler):
    directory = staging / "dependencies"
    extract("zlib", directory)
    source = directory / ("zlib-" + ZLIB_VERSION)
    files = "adler32 crc32 deflate infback inffast inflate inftrees trees zutil compress uncompr gzclose gzlib gzread gzwrite".split()
    subprocess.run(compiler + [
        "-fPIC", "-shared", "-DHAVE_UNISTD_H", "-DHAVE_HIDDEN",
        "-Wl,-soname,libz.so",
    ] + [str(source / (name + ".c")) for name in files] + [
        str(bionic / "libc.so"), "-o", str(staging / "libz.so"),
    ], check=True)
    shutil.copy2(source / "LICENSE", staging / "libz.so.LICENSE")
    return {"zlib_version": ZLIB_VERSION, "bionic_headers_revision": BIONIC_REVISION,
            "archives": {name: {"url": url,
                                "files_sha256" if "+archive/" in url else "sha256": digest}
                         for name, (url, digest) in ARCHIVES.items()}}
