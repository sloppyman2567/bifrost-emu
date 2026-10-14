#!/usr/bin/env python3
"""Read-only APK/split-package inventory; never launches app code."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
MAX_APK_BYTES = 1024 * 1024 * 1024
MAX_ARCHIVE_BYTES = 2 * MAX_APK_BYTES
MAX_MEMBERS = 100000


def run(command):
    result = subprocess.run(command, capture_output=True, text=True, timeout=60,
                            env={**os.environ, "LC_ALL": "C"})
    if result.returncode:
        raise ValueError(f"{Path(command[0]).name} failed: {result.stderr.strip()[:1000]}")
    return result.stdout


def check_zip(archive):
    entries = archive.infolist()
    if len(entries) > MAX_MEMBERS or len({x.filename for x in entries}) != len(entries):
        raise ValueError("Archive has excessive or duplicate entries")
    if sum(x.file_size for x in entries) > MAX_ARCHIVE_BYTES:
        raise ValueError("Archive exceeds the 2 GiB uncompressed inspection limit")
    return entries


@contextmanager
def apk_paths(path, directory):
    """Flatten only nested APKs into generated names, never archive paths."""
    with zipfile.ZipFile(path) as archive:
        entries = check_zip(archive)
        if "AndroidManifest.xml" in archive.namelist():
            yield [(path.name, path)]
            return
        nested = [x for x in entries if x.filename.lower().endswith(".apk")]
        if not nested or len(nested) > 128:
            raise ValueError("Expected an APK or a bundle containing 1..128 APKs")
        paths = []
        for index, item in enumerate(nested):
            if item.file_size > MAX_APK_BYTES:
                raise ValueError("Nested APK exceeds the 1 GiB inspection limit")
            output = directory / f"package-{index}.apk"
            with archive.open(item) as source, output.open("wb") as destination:
                shutil.copyfileobj(source, destination)
            paths.append((item.filename, output))
        yield paths


def badging_fields(output):
    fields = {}
    for line in output.splitlines():
        label, separator, value = line.partition(":")
        if not separator:
            continue
        if label in ("package", "launchable-activity", "application"):
            fields[label] = dict(token.split("=", 1) for token in shlex.split(value)
                                 if "=" in token)
        elif re.fullmatch(r"application-icon-\d+", label):
            values = shlex.split(value)
            if len(values) == 1:
                fields.setdefault("application-icons", {})[int(label.rsplit("-", 1)[1])] = values[0]
        elif label in ("sdkVersion", "minSdkVersion", "targetSdkVersion"):
            values = shlex.split(value)
            if len(values) == 1:
                fields[label] = values[0]
    if not fields.get("package", {}).get("name"):
        raise ValueError("AAPT2 did not identify a package name")
    return fields


def xmltree_nodes(output):
    """Parse AAPT2's decoded element tree, retaining metadata ownership."""
    roots, stack = [], []
    for line in output.splitlines():
        element = re.match(r"^(\s*)E: ([\w:.-]+)", line)
        if element:
            depth = len(element[1])
            while stack and stack[-1][0] >= depth:
                stack.pop()
            node = {"tag": element[2], "attributes": {}, "children": []}
            (stack[-1][1]["children"] if stack else roots).append(node)
            stack.append((depth, node))
            continue
        attribute = re.match(r"\s*A: (.+?)(?:\(0x[0-9a-f]+\))?=(.*)$", line)
        if attribute and stack:
            name = attribute[1].split(":")[-1]
            value = attribute[2].strip()
            raw = re.search(r'\(Raw: ("(?:[^"\\]|\\.)*")\)', value)
            if raw:
                value = json.loads(raw[1])
            stack[-1][1]["attributes"][name] = value
    if len(roots) != 1 or roots[0]["tag"] != "manifest":
        raise ValueError("AAPT2 did not decode a manifest element tree")
    return roots[0]


def qualify(package, name):
    if name.startswith("."):
        return package + name
    return name if "." in name else package + "." + name


def launcher_details(package, launcher, manifest):
    if not launcher:
        return {"kind": "no_launcher"}
    applications = [x for x in manifest["children"] if x["tag"] == "application"]
    components = [x for app in applications for x in app["children"]
                  if x["tag"] in ("activity", "activity-alias")]
    name = qualify(package, launcher)
    node = next((x for x in components if qualify(package, x["attributes"].get("name", "")) == name), None)
    if node and node["tag"] == "activity-alias":
        target = node["attributes"].get("targetActivity")
        if not target:
            raise ValueError("Launcher alias has no targetActivity")
        name = qualify(package, target)
        node = next((x for x in components if x["tag"] == "activity" and
                     qualify(package, x["attributes"].get("name", "")) == name), None)
    if node is None:
        raise ValueError("Launchable activity is absent from the decoded manifest")
    metadata = {x["attributes"].get("name"): x["attributes"].get("value")
                for x in node["children"] if x["tag"] == "meta-data"}
    if name != "android.app.NativeActivity":
        return {"class": name, "kind": "managed_activity"}
    return {"class": name, "kind": "native_activity",
            "native_library": metadata.get("android.app.lib_name", "main"),
            "native_entrypoint": metadata.get("android.app.func_name", "ANativeActivity_onCreate")}



BITMAP = re.compile(r"[^\0]+\.(?:png|webp|jpe?g)", re.I)
DENSITIES = {"ldpi": 120, "mdpi": 160, "tvdpi": 213, "hdpi": 240, "xhdpi": 320,
             "xxhdpi": 480, "xxxhdpi": 640}


def resource_bitmap(resources, path):
    """Densest bitmap among the configurations of the resource that declares path."""
    groups, current = [], None
    for line in resources.splitlines():
        if re.match(r"\s*resource 0x[0-9a-f]+ ", line):
            current = []
            groups.append(current)
            continue
        entry = re.match(r"\s*\(([^)]*)\) \(file\) (\S+)", line)
        if entry and current is not None:
            current.append(entry.groups())
    group = next((x for x in groups if any(file == path for _, file in x)), [])
    def rank(item):
        qualifiers = item[0].split("-")
        density = next((DENSITIES.get(x) or int(x[:-3]) for x in qualifiers
                        if x in DENSITIES or re.fullmatch(r"\d+dpi", x)), 0)
        return ("night" not in qualifiers, density)
    bitmaps = sorted((x for x in group if BITMAP.fullmatch(x[1])), key=rank)
    return bitmaps[-1][1] if bitmaps else None


def launcher_icon(fields, path, aapt2):
    """Pick the launcher icon as a bitmap, as pre-adaptive Android launchers do."""
    activity = fields.get("launchable-activity", {}).get("icon")
    icon = activity or fields.get("application", {}).get("icon")
    if not icon:
        return None
    if not activity:
        # Badging lists the application icon per density; 65534/65535 are any/nodpi.
        dense = sorted((density % 65534, value) for density, value in fields.get("application-icons", {}).items()
                       if BITMAP.fullmatch(value))
        if dense:
            return dense[-1][1]
    if BITMAP.fullmatch(icon):
        return icon
    # An adaptive or vector icon: use the same resource's bitmap fallback, if any.
    return resource_bitmap(run([aapt2, "dump", "resources", str(path)]), icon)


def inspect_apk(name, path, aapt2, directory):
    fields = badging_fields(run([aapt2, "dump", "badging", str(path)]))
    manifest = xmltree_nodes(run([aapt2, "dump", "xmltree", "--file", "AndroidManifest.xml", str(path)]))
    identity = fields["package"]
    launcher = fields.get("launchable-activity", {}).get("name")
    report = {"archive": name, "package": identity["name"], "split": identity.get("split", ""),
              "label": fields.get("launchable-activity", {}).get("label") or fields.get("application", {}).get("label") or identity["name"],
              "icon": None if identity.get("split") else launcher_icon(fields, path, aapt2),
              "version_code": identity.get("versionCode"), "version_name": identity.get("versionName"),
              "min_sdk": fields.get("minSdkVersion", fields.get("sdkVersion")),
              "target_sdk": fields.get("targetSdkVersion"),
              "launcher": launcher_details(identity["name"], launcher, manifest),
              "dex_files": [], "abis": [], "arm64_libraries": []}
    with zipfile.ZipFile(path) as archive:
        entries = check_zip(archive)
        report["dex_files"] = [x.filename for x in entries if re.fullmatch(r"classes(?:[2-9]|[1-9][0-9]+)?\.dex", x.filename)]
        abis = set()
        for item in entries:
            match = re.fullmatch(r"lib/([^/]+)/([^/]+\.so)", item.filename)
            if not match:
                continue
            abi, filename = match.groups()
            abis.add(abi)
            if abi != "arm64-v8a":
                continue
            if item.file_size > MAX_APK_BYTES:
                raise ValueError("Native library exceeds the inspection limit")
            library = directory / "inspection-library.so"
            with archive.open(item) as source, library.open("wb") as destination:
                header = source.read(20)
                if (len(header) != 20 or header[:6] != b"\x7fELF\x02\x01" or
                        struct.unpack_from("<HH", header, 16) != (3, 183)):
                    raise ValueError(f"{item.filename} is not an AArch64 ELF64 shared library")
                destination.write(header)
                shutil.copyfileobj(source, destination)
            dynamic = run(["readelf", "--dynamic", "--wide", str(library)])
            alignment = elf_alignment(library)
            symbols = run(["readelf", "--dyn-syms", "--wide", str(library)])
            exports = []
            for line in symbols.splitlines():
                columns = line.split()
                if len(columns) >= 8 and columns[6] != "UND":
                    symbol = columns[7].split("@")[0]
                    if symbol in ("ANativeActivity_onCreate", "JNI_OnLoad"):
                        exports.append(symbol)
            sonames = re.findall(r"\(SONAME\).*?\[([^\]]+)\]", dynamic)
            report["arm64_libraries"].append({"name": filename, "bytes": item.file_size,
                "soname": sonames[0] if sonames else None,
                "needed": re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", dynamic),
                "entrypoints": sorted(set(exports)), **alignment,
                "zip_compressed": item.compress_type != zipfile.ZIP_STORED})
            library.unlink()
        report["abis"] = sorted(abis)
    return report


def elf_alignment(path):
    """Report PT_LOAD alignment, independently of the runtime AT_PAGESZ."""
    length = path.stat().st_size
    with path.open("rb") as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:6] != b"\x7fELF\x02\x01":
            raise ValueError("Invalid ELF64 header")
        offset = struct.unpack_from("<Q", header, 32)[0]
        entry_size, count = struct.unpack_from("<HH", header, 54)
        if entry_size < 56 or not 0 < count <= 4096 or offset > length or count * entry_size > length - offset:
            raise ValueError("Invalid ELF program-header table")
        loads = []
        aligned = True
        for index in range(count):
            stream.seek(offset + index * entry_size)
            kind, _, file_offset, address, _, file_size, memory_size, align = struct.unpack("<IIQQQQQQ", stream.read(56))
            if kind != 1:
                continue
            if file_offset > length or file_size > length - file_offset or file_size > memory_size:
                raise ValueError("Invalid ELF load segment")
            if align > 1 and (align & (align - 1) or address % align != file_offset % align):
                raise ValueError("Invalid ELF load alignment")
            loads.append(align)
            aligned &= align >= 16384 and address % 16384 == file_offset % 16384
        if not loads:
            raise ValueError("ELF has no load segments")
    return {"elf_load_alignments": sorted(set(loads)), "elf_16k_aligned": aligned}


def summarize(packages):
    bases = [x for x in packages if not x["split"]]
    if len(bases) != 1:
        raise ValueError("Exactly one base APK is required")
    base = bases[0]
    if any(x["package"] != base["package"] or x["version_code"] != base["version_code"] for x in packages):
        raise ValueError("Split package names/version codes do not match the base")
    splits = [x["split"] for x in packages]
    if len(splits) != len(set(splits)):
        raise ValueError("Duplicate split identities")
    libraries = [lib for pkg in packages for lib in pkg["arm64_libraries"]]
    names = [lib["name"] for lib in libraries]
    if len(names) != len(set(names)):
        raise ValueError("Conflicting ARM64 library providers across splits")
    provided = set(names) | {x["soname"] for x in libraries if x["soname"]}
    external = sorted({dep for lib in libraries for dep in lib["needed"] if dep not in provided})
    blockers = ["Integrated APK loading and an isolated Android/Bionic runtime are not implemented"]
    if not libraries:
        blockers.append("No arm64-v8a native libraries are packaged")
    if base["launcher"]["kind"] == "managed_activity":
        blockers.append("Launcher requires ART/JNI and Android framework activity startup")
    elif base["launcher"]["kind"] == "no_launcher":
        blockers.append("No launchable activity was identified")
    else:
        name = base["launcher"]["native_library"]
        target = next((x for x in libraries if x["name"] == f"lib{name}.so"), None)
        if target is None:
            blockers.append("NativeActivity's declared native library was not found")
        if base["launcher"]["native_entrypoint"] != "ANativeActivity_onCreate":
            blockers.append("Standalone --android supports only ANativeActivity_onCreate")
        elif target and "ANativeActivity_onCreate" not in target["entrypoints"]:
            blockers.append("Declared native library does not export ANativeActivity_onCreate")
    return {"package": base["package"], "min_sdk": base["min_sdk"], "target_sdk": base["target_sdk"],
            "launcher": base["launcher"], "apk_count": len(packages),
            "dex_count": sum(len(x["dex_files"]) for x in packages),
            "abis": sorted({abi for pkg in packages for abi in pkg["abis"]}),
            "arm64_library_count": len(libraries), "external_native_dependencies": external,
            "arm64_16k_aligned_count": sum(x.get("elf_16k_aligned", False) for x in libraries),
            "runnable_in_bifrost": False, "blockers": blockers}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path, help="APK, APKM or APKS file")
    parser.add_argument("--aapt2", default=str(ROOT / "Tools/android-tools/aapt2"))
    parser.add_argument("--output", type=Path, help="save the complete JSON inventory")
    args = parser.parse_args()
    try:
        if not Path(args.aapt2).is_file() and not shutil.which(args.aapt2):
            raise ValueError("AAPT2 is missing; run python3 Tools/Android/fetch-aapt2.py or use --aapt2")
        digest = hashlib.sha256()
        with args.package.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        with tempfile.TemporaryDirectory(prefix="bifrost-android-preflight-") as temporary:
            directory = Path(temporary)
            with apk_paths(args.package, directory) as paths:
                packages = [inspect_apk(name, path, args.aapt2, directory) for name, path in paths]
        report = {"schema_version": 1, "input": str(args.package.resolve()),
                  "sha256": digest.hexdigest(), "aapt2_version": run([args.aapt2, "version"]).strip(),
                  "summary": summarize(packages), "packages": packages}
        if args.output:
            args.output.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report["summary"], indent=2))
    except (OSError, ValueError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        print(f"Android preflight failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
