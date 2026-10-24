#!/usr/bin/env python3
"""Install the AArch64 shared libraries that games need into a guest rootfs.

Usage: fetch-game-libs.py ROOTFS [--suite trixie] [--mirror URL] [--list | --check]

Downloads pinned Debian arm64 packages (OpenAL, SDL2_ttf, Vorbis, libpng, libjpeg, libcurl, ...)
and their dependencies, and unpacks only their shared libraries into ROOTFS/usr/lib. Nothing is
shipped with bifrost-emu and no game data is fetched.

Trust chain: the suite's Release file (over HTTPS) gives the SHA-256 of Packages.xz, which gives the
SHA-256 and size of every .deb; each download is checked against it. The Release signature is not
verified (no gpg is required), so this relies on HTTPS to the mirror.

Libraries that bifrost-emu serves from the host (audio, GL, windowing: ALSA, PulseAudio, SDL2, Mesa,
X11, Wayland, ...) and the C runtime already in the rootfs are never installed: a real copy would
shadow the host-backed one. The install fails if a package needs a newer glibc than the rootfs has.

Run on a rootfs that already has game libraries, it only replaces what changed. --check downloads no
packages: it compares ROOTFS/etc/bifrost-game-libs.txt with the suite's current index and exits 0 when
the install is up to date, 3 when it is missing or stale.
"""
import argparse
import hashlib
import io
import lzma
import gzip
import os
import re
import sys
import tarfile
import urllib.request

# What games link against directly; their dependencies are resolved from the package index.
ROOT_PACKAGES = [
    "libopenal1", "libsdl2-ttf-2.0-0", "libvorbisfile3", "libvorbis0a", "libogg0",
    "libpng16-16t64", "libjpeg62-turbo", "libcurl4t64", "libopenhmd0", "libfreetype6",
    "libsdl2-image-2.0-0", "libsdl2-mixer-2.0-0", "libsndfile1", "libopus0", "libopusfile0", "libmpg123-0t64",
    "libwebp7", "libsqlite3-0", "libxml2", "libtiff6",
]

# Never installed: the C runtime is the rootfs's own, and these are backed by host thunks.
EXCLUDED = re.compile(
    r"^(libc6|libc-bin|libgcc-s1|libstdc\+\+6|gcc-.*-base|libasound2.*|libpulse.*|pulseaudio.*|libpipewire.*|"
    r"libsndio.*|libjack.*|libportaudio.*|libsdl2-2\.0-0|libsdl2-[0-9].*|libgl1.*|libglx.*|libglvnd.*|libegl.*|"
    r"libgles.*|libgbm.*|libdrm.*|libvulkan.*|libx11.*|libxcb.*|libxau.*|libxdmcp.*|libwayland.*|libdbus.*|"
    r"libsystemd.*|libudev.*|debconf.*|cdebconf.*|dpkg|perl-base|ca-certificates|openssl)$")

CHUNK = 1 << 20


def fetch(url, timeout=120):
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return response.read()


def parse_packages(text):
    packages, provides = {}, {}
    for stanza in text.split("\n\n"):
        fields, key = {}, None
        for line in stanza.split("\n"):
            if line.startswith((" ", "\t")) and key:
                fields[key] += "\n" + line.strip()
            elif ":" in line:
                key, value = line.split(":", 1)
                fields[key] = value.strip()
        name = fields.get("Package")
        if not name:
            continue
        packages[name] = fields
        for clause in fields.get("Provides", "").split(","):
            virtual = clause.strip().split(" ")[0]
            if virtual:
                provides.setdefault(virtual, []).append(name)
    return packages, provides


def version_tuple(text):
    return tuple(int(part) for part in re.findall(r"\d+", text)[:3])


def rootfs_glibc_version(rootfs):
    for relative in ("lib/libc.so.6", "lib64/libc.so.6", "usr/lib/libc.so.6"):
        path = os.path.join(rootfs, relative)
        if os.path.isfile(path):
            with open(path, "rb") as f:
                match = re.search(rb"release version (\d+\.\d+)", f.read())
            if match:
                return match.group(1).decode()
    return None


def clause_names(clause):
    """Package names of one 'a (>= 1) | b' alternatives clause."""
    return [re.sub(r"[ (:].*", "", alternative.strip()) for alternative in clause.split("|")]


def resolve(roots, packages, provides, log):
    chosen, queue = {}, list(roots)
    while queue:
        name = queue.pop()
        if name in chosen:
            continue
        if name not in packages:
            sys.exit(f"error: package {name} is not in the suite's index")
        chosen[name] = packages[name]
        for clause in packages[name].get("Depends", "").split(","):
            if not clause.strip():
                continue
            allowed = [d for d in clause_names(clause) if not EXCLUDED.match(d)]
            if not allowed:
                log(f"  skip {clause.strip()} (provided by the rootfs or the host)")
                continue
            candidates = []
            for dependency in allowed:
                candidates += [dependency] if dependency in packages else provides.get(dependency, [])
            if not candidates:
                log(f"  skip {clause.strip()} (not in the index)")
                continue
            queue.append(candidates[0])
    return chosen


def check_glibc(chosen, rootfs, glibc):
    have = version_tuple(glibc)
    for name, fields in sorted(chosen.items()):
        for match in re.finditer(r"libc6 \(>= ([\d.]+)", fields.get("Depends", "")):
            if version_tuple(match.group(1)) > have:
                sys.exit(f"error: {name} needs glibc >= {match.group(1)} but the rootfs has {glibc}; "
                         f"use an older --suite")


def read_ar(data):
    """Members of an ar archive (.deb) as {name: bytes}."""
    if data[:8] != b"!<arch>\n":
        raise ValueError("not an ar archive")
    members, offset = {}, 8
    while offset + 60 <= len(data):
        header = data[offset:offset + 60]
        name = header[:16].decode().strip().rstrip("/")
        size = int(header[48:58].decode())
        members[name] = data[offset + 60:offset + 60 + size]
        offset += 60 + size + (size & 1)
    return members


def unpack_libraries(deb, rootfs):
    members = read_ar(deb)
    name = next((n for n in members if n.startswith("data.tar")), None)
    if name is None:
        raise ValueError("no data.tar in package")
    blob = members[name]
    if name.endswith(".xz"):
        blob = lzma.decompress(blob)
    elif name.endswith(".gz"):
        blob = gzip.decompress(blob)
    elif not name.endswith(".tar"):
        raise ValueError(f"unsupported package compression {name}")
    destination = os.path.join(rootfs, "usr", "lib")
    written = []
    with tarfile.open(fileobj=io.BytesIO(blob)) as tar:
        for member in tar:
            # Only shared libraries: (usr/)lib/aarch64-linux-gnu/..., flattened into usr/lib.
            match = re.match(r"^\./?(?:usr/)?lib/aarch64-linux-gnu/(.+)$", "./" + member.name.lstrip("./"))
            if not match or member.isdir() or "pkgconfig" in match.group(1):
                continue
            relative = os.path.normpath(match.group(1))
            if relative.startswith("..") or os.path.isabs(relative):
                continue
            target = os.path.join(destination, relative)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            if os.path.lexists(target):
                os.remove(target)
            if member.issym():
                link = member.linkname
                resolved = os.path.normpath(os.path.join(os.path.dirname(relative), link))
                if os.path.isabs(link) or resolved.startswith(".."):
                    continue  # a link out of the library directory
                os.symlink(link, target)
            elif member.isreg():
                with tar.extractfile(member) as source, open(target, "wb") as out:
                    while True:
                        chunk = source.read(CHUNK)
                        if not chunk:
                            break
                        out.write(chunk)
                os.chmod(target, 0o755 if member.mode & 0o111 else 0o644)
            else:
                continue
            written.append(relative)
    return written


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("rootfs")
    parser.add_argument("--suite", default="trixie", help="Debian suite to take packages from (default: %(default)s)")
    parser.add_argument("--mirror", default="https://deb.debian.org/debian")
    parser.add_argument("--list", action="store_true", help="resolve and print the packages, download nothing")
    parser.add_argument("--check", action="store_true",
                        help="report whether ROOTFS's game libraries match the suite (exit 0 current, 3 not)")
    args = parser.parse_args()

    rootfs = os.path.abspath(args.rootfs)
    glibc = rootfs_glibc_version(rootfs)
    if not glibc:
        sys.exit(f"error: {rootfs} has no glibc libc.so.6 (build the rootfs first)")
    log = (lambda text: None) if not args.list else print

    release = fetch(f"{args.mirror}/dists/{args.suite}/Release").decode()
    entry = re.search(r"^ ([0-9a-f]{64})\s+(\d+)\s+main/binary-arm64/Packages\.xz$", release, re.M)
    if not entry:
        sys.exit(f"error: no main/binary-arm64/Packages.xz in the {args.suite} Release file")
    packed = fetch(f"{args.mirror}/dists/{args.suite}/main/binary-arm64/Packages.xz", timeout=300)
    if hashlib.sha256(packed).hexdigest() != entry.group(1) or len(packed) != int(entry.group(2)):
        sys.exit("error: Packages.xz does not match the hash in the Release file")
    packages, provides = parse_packages(lzma.decompress(packed).decode())

    chosen = resolve(ROOT_PACKAGES, packages, provides, log)
    check_glibc(chosen, rootfs, glibc)
    if args.list:
        for name in sorted(chosen):
            print(f"{name} {chosen[name]['Version']}")
        return 0

    manifest_path = os.path.join(rootfs, "etc", "bifrost-game-libs.txt")
    installed = {}
    if os.path.isfile(manifest_path):
        with open(manifest_path) as f:
            for line in f:
                parts = line.split()
                if len(parts) == 3 and not line.startswith("#"):
                    installed[parts[0]] = parts[2]
    wanted = {name: chosen[name]["SHA256"] for name in chosen}
    if args.check:
        added, changed, removed = (sorted(wanted.keys() - installed.keys()),
                                   sorted(n for n in wanted.keys() & installed.keys() if wanted[n] != installed[n]),
                                   sorted(installed.keys() - wanted.keys()))
        if not (added or changed or removed):
            print(f"game libraries up to date ({len(wanted)} packages)")
            return 0
        print(f"game libraries need an update: {len(added)} new, {len(changed)} changed, {len(removed)} dropped")
        return 3

    manifest = []
    for name in sorted(chosen):
        fields = chosen[name]
        if installed.get(name) == fields["SHA256"]:
            print(f"  {name} {fields['Version']}: up to date")
            manifest.append(f"{name} {fields['Version']} {fields['SHA256']}")
            continue
        deb = fetch(f"{args.mirror}/{fields['Filename']}", timeout=300)
        if len(deb) != int(fields["Size"]) or hashlib.sha256(deb).hexdigest() != fields["SHA256"]:
            sys.exit(f"error: {name} does not match the package index (size or SHA-256)")
        files = unpack_libraries(deb, rootfs)
        print(f"  {name} {fields['Version']}: {len(files)} library files")
        manifest.append(f"{name} {fields['Version']} {fields['SHA256']}")
    with open(manifest_path, "w") as f:
        f.write(f"# Debian {args.suite} arm64 packages unpacked by fetch-game-libs.py\n")
        f.write("\n".join(manifest) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
