#!/usr/bin/env python3
"""Install signed APK sets and launch native-only Activities through guest Bionic."""

import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile

import android_preflight as preflight

ROOT = Path(__file__).resolve().parents[1]
API = 35
PACKAGE = re.compile(r"[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+")
RESERVED = {
    "libc.so",
    "libdl.so",
    "libdl_android.so",
    "libm.so",
    "libandroid.so",
    "liblog.so",
    "libEGL.so",
    "libGLESv2.so",
}
MAX_TOTAL = 2 * 1024 * 1024 * 1024


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(data)
    return value.hexdigest()


def snapshot_input(source, destination):
    with source.open("rb") as incoming, destination.open("wb") as output:
        info = os.fstat(incoming.fileno())
        if not stat.S_ISREG(info.st_mode):
            raise ValueError("Package input must be a regular file")
        if info.st_size > MAX_TOTAL:
            raise ValueError("Package input exceeds 2 GiB")
        copied = 0
        for data in iter(lambda: incoming.read(1024 * 1024), b""):
            copied += len(data)
            if copied > MAX_TOTAL:
                raise ValueError("Package input exceeds 2 GiB")
            output.write(data)


def regular_archive(archive):
    entries = preflight.check_zip(archive)
    for item in entries:
        path = PurePosixPath(item.filename)
        if (
            not item.filename
            or "\\" in item.filename
            or "\0" in item.filename
            or path.is_absolute()
            or any(x in ("", "..", ".") for x in item.filename.rstrip("/").split("/"))
        ):
            raise ValueError(f"Unsafe archive path: {item.filename!r}")
        mode = item.external_attr >> 16
        if stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR):
            raise ValueError(f"Non-regular archive member: {item.filename}")
        if item.flag_bits & 1:
            raise ValueError("Encrypted APK members are unsupported")
    return entries


def signers(path, apksigner):
    command = (
        ["java", "-jar", str(apksigner)]
        if str(apksigner).endswith(".jar")
        else [str(apksigner)]
    )
    output = preflight.run(
        command
        + [
            "verify",
            "--verbose",
            "--print-certs",
            "--min-sdk-version",
            str(API),
            "--max-sdk-version",
            str(API),
            str(path),
        ]
    )
    values = re.findall(
        r"^Signer #\d+ certificate SHA-256 digest: ([0-9a-fA-F]{64})$", output, re.M
    )
    if not values:
        raise ValueError("APK verification returned no signer certificates")
    return sorted(x.lower() for x in values)


def manifest_contract(path, aapt2):
    manifest = preflight.xmltree_nodes(
        preflight.run(
            [str(aapt2), "dump", "xmltree", "--file", "AndroidManifest.xml", str(path)]
        )
    )
    dependencies = [
        x["attributes"].get("name")
        for x in manifest["children"]
        if x["tag"] == "uses-split"
    ]
    if any(not x for x in dependencies):
        raise ValueError("Unnamed uses-split dependency")
    applications = [x for x in manifest["children"] if x["tag"] == "application"]
    required = any(
        x["attributes"].get("isSplitRequired") in ("true", "(type 0x12)0xffffffff")
        for x in applications
    )
    return dependencies, required


@contextmanager
def app_lock(directory):
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / ".install.lock").open("a+b") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        yield


def verify_runtime(runtime):
    spec = importlib.util.spec_from_file_location(
        "runtime", ROOT / "tools/fetch-android-runtime.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    metadata = json.loads((runtime / "bifrost-runtime.json").read_text())
    if (
        metadata.get("archive_sha256") != module.SHA256
        or metadata.get("files") != module.FILE_HASHES
    ):
        raise ValueError("Unrecognized Bionic runtime identity")
    for name, value in module.FILE_HASHES.items():
        path = runtime / "apex/com.android.runtime" / name
        if path.is_symlink() or digest(path) != value:
            raise ValueError(f"Modified Bionic runtime: {name}")
    return module


def verify_adapters(adapters, runtime):
    metadata = json.loads((adapters / "adapters.json").read_text())
    expected = {
        "bifrost-native",
        "libandroid.so",
        "liblog.so",
        "libEGL.so",
        "libGLESv2.so",
    }
    if (
        metadata.get("runtime_archive_sha256") != runtime.SHA256
        or set(metadata.get("files", {})) != expected
    ):
        raise ValueError("Adapters do not match the pinned runtime")
    for name, value in metadata["files"].items():
        if (adapters / name).is_symlink() or digest(adapters / name) != value:
            raise ValueError(f"Modified adapter: {name}")
    return metadata


def publish_json(path, value):
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
        temporary = Path(output.name)
        try:
            output.write((json.dumps(value, indent=2) + "\n").encode())
            output.flush()
            os.fsync(output.fileno())
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)


def install(source, state, runtime, adapters, aapt2, apksigner):
    runtime_module = verify_runtime(runtime)
    adapter_metadata = verify_adapters(adapters, runtime_module)
    state.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".apk-validation-", dir=state) as work:
        staging = Path(work)
        # Snapshot the input first: verification and installation see the same bytes.
        snapshot = staging / "input.zip"
        snapshot_input(source, snapshot)
        with zipfile.ZipFile(snapshot) as archive:
            regular_archive(archive)
        with preflight.apk_paths(snapshot, staging) as paths:
            packages = []
            certificates = None
            budget = 0
            for archive_name, path in paths:
                with zipfile.ZipFile(path) as archive:
                    entries = regular_archive(archive)
                    budget += sum(x.file_size for x in entries)
                if budget > MAX_TOTAL:
                    raise ValueError("APK set exceeds 2 GiB uncompressed")
                current = signers(path, apksigner)
                if certificates is None:
                    certificates = current
                if current != certificates:
                    raise ValueError("APK split signer certificates do not match")
                report = preflight.inspect_apk(archive_name, path, str(aapt2), staging)
                report["dependencies"], report["split_required"] = manifest_contract(
                    path, aapt2
                )
                packages.append((report, path))
            reports = [x[0] for x in packages]
            # Reuse manifest/version/provider validation from the read-only inventory.
            summary = preflight.summarize(reports)
            # Stable split filenames also make reordered APK containers idempotent.
            packages.sort(key=lambda item: item[0]["split"] or "")
            name = summary["package"]
            if not PACKAGE.fullmatch(name):
                raise ValueError("Invalid Android package name")
            base = next(x for x in reports if not x["split"])
            if not base["version_code"] or not base["version_code"].isdigit():
                raise ValueError("Invalid version code")
            if base["min_sdk"] and (
                not base["min_sdk"].isdigit() or int(base["min_sdk"]) > API
            ):
                raise ValueError(f"Package requires an Android API newer than {API}")
            splits = {x["split"] for x in reports}
            if base["split_required"] and len(packages) == 1:
                raise ValueError("Package requires split APKs")
            if any(dep not in splits for x in reports for dep in x["dependencies"]):
                raise ValueError("Required split APK dependency is missing")
            libraries = [lib for x in reports for lib in x["arm64_libraries"]]
            if any(
                lib["name"] in RESERVED or lib["soname"] in RESERVED
                for lib in libraries
            ):
                raise ValueError(
                    "Package attempts to replace a reserved runtime/adapter library"
                )
            hashes = [digest(path) for _, path in packages]
            identity = hashlib.sha256("\n".join(sorted(hashes)).encode()).hexdigest()[
                :24
            ]
            code = staging / "code"
            code.mkdir()
            (code / "lib/arm64").mkdir(parents=True)
            (code / "assets").mkdir()
            files = {}
            for index, (report, path) in enumerate(packages):
                filename = "base.apk" if not report["split"] else f"split-{index}.apk"
                shutil.copyfile(path, code / filename)
                files[filename] = digest(code / filename)
                with zipfile.ZipFile(path) as archive:
                    for item in regular_archive(archive):
                        if item.is_dir():
                            continue
                        relative = None
                        if re.fullmatch(r"lib/arm64-v8a/[^/]+\.so", item.filename):
                            relative = "lib/arm64/" + PurePosixPath(item.filename).name
                        elif item.filename.startswith("assets/"):
                            relative = item.filename
                        if not relative:
                            continue
                        target = code / relative
                        target.parent.mkdir(parents=True, exist_ok=True)
                        candidate = staging / "member"
                        with archive.open(item) as incoming, candidate.open(
                            "wb"
                        ) as output:
                            shutil.copyfileobj(incoming, output)
                        checksum = digest(candidate)
                        if relative in files and files[relative] != checksum:
                            raise ValueError(f"Conflicting split member: {relative}")
                        candidate.replace(target)
                        files[relative] = checksum
            app = state / name
            with app_lock(app):
                metadata_path = app / "package.json"
                previous = (
                    json.loads(metadata_path.read_text())
                    if metadata_path.exists()
                    else None
                )
                if previous:
                    if previous["signers"] != certificates:
                        raise ValueError("Installed package signer differs")
                    if int(base["version_code"]) < int(previous["version_code"]):
                        raise ValueError("Package downgrade is unsupported")
                root = app / "rootfs"
                if not root.exists():
                    fresh = staging / "rootfs"
                    (fresh / "apex/com.android.runtime").mkdir(parents=True)
                    for remote in runtime_module.FILES:
                        target = fresh / "apex/com.android.runtime" / remote
                        target.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(
                            runtime / "apex/com.android.runtime" / remote, target
                        )
                    (fresh / "system/bin").mkdir(parents=True)
                    (fresh / "system/lib64").mkdir()
                    (fresh / "system/bin/linker64").symlink_to(
                        "../../apex/com.android.runtime/bin/linker64"
                    )
                    for filename in adapter_metadata["files"]:
                        target = (
                            fresh
                            / (
                                "system/bin"
                                if filename == "bifrost-native"
                                else "system/lib64"
                            )
                            / filename
                        )
                        shutil.copy2(adapters / filename, target)
                    (fresh / "linkerconfig").mkdir()
                    (fresh / "linkerconfig/ld.config.txt").write_text(
                        "dir.app = /system/bin\n\n[app]\nnamespace.default.isolated = true\n"
                        "namespace.default.search.paths = /system/lib64:/apex/com.android.runtime/lib64/bionic\n"
                        "namespace.default.permitted.paths = /data/app:/system/lib64:/apex/com.android.runtime/lib64/bionic\n"
                    )
                    (fresh / "data/app").mkdir(parents=True)
                    for relative in [
                        f"data/user/0/{name}/files",
                        f"data/user/0/{name}/cache",
                        f"storage/emulated/0/Android/data/{name}/files",
                    ]:
                        (fresh / relative).mkdir(parents=True)
                    shutil.copy2(runtime / "NOTICE.html.gz", fresh / "NOTICE.html.gz")
                    fresh.rename(root)
                else:
                    for remote, value in runtime_module.FILE_HASHES.items():
                        if digest(root / "apex/com.android.runtime" / remote) != value:
                            raise ValueError("Installed runtime was modified")
                    for filename, value in adapter_metadata["files"].items():
                        target = (
                            root
                            / (
                                "system/bin"
                                if filename == "bifrost-native"
                                else "system/lib64"
                            )
                            / filename
                        )
                        if digest(target) != value:
                            raise ValueError(
                                "Installed adapters differ; create a fresh app state"
                            )
                guest_code = "/data/app/" + name + "-" + identity
                destination = root / guest_code.lstrip("/")
                if destination.exists():
                    if any(
                        digest(destination / path) != value
                        for path, value in files.items()
                    ):
                        raise ValueError("Installed code was modified")
                else:
                    for path in code.rglob("*"):
                        path.chmod(0o555 if path.is_dir() else 0o444)
                    code.rename(destination)
                    destination.chmod(0o555)
                metadata = {
                    "schema_version": 1,
                    "package": name,
                    "version_code": base["version_code"],
                    "signers": certificates,
                    "guest_code": guest_code,
                    "launcher": base["launcher"],
                    "sdk_version": API,
                    "files": files,
                    "packages": reports,
                    "runtime_files": runtime_module.FILE_HASHES,
                    "adapters": adapter_metadata,
                    "apk_count": len(packages),
                    "arm64_library_count": len(libraries),
                }
                publish_json(metadata_path, metadata)
            return metadata


def launch(package, state, emu, no_jit=False, window_plugin=None, frames=0):
    if not PACKAGE.fullmatch(package):
        raise ValueError("Invalid package name")
    app = state / package
    with app_lock(app):
        metadata = json.loads((app / "package.json").read_text())
        launcher = metadata["launcher"]
        if launcher["kind"] != "native_activity":
            raise ValueError(
                f"{launcher.get('class','No launcher')} requires guest ART/framework Activity startup; installed APK cannot yet launch"
            )
        library = launcher.get("native_library")
        entry = launcher.get("native_entrypoint")
        if (
            not library
            or not re.fullmatch(r"[A-Za-z0-9_]+", library)
            or not entry
            or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", entry)
        ):
            raise ValueError("Invalid native Activity library/entrypoint")
        relative = "lib/arm64/lib" + library + ".so"
        if relative not in metadata["files"]:
            raise ValueError("NativeActivity has no packaged ARM64 entry library")
        code = metadata["guest_code"]
        if not re.fullmatch("/data/app/" + re.escape(package) + r"-[0-9a-f]{24}", code):
            raise ValueError("Invalid installed code path")
        root = app / "rootfs"
        for path, value in metadata["files"].items():
            if digest(root / code.lstrip("/") / path) != value:
                raise ValueError(f"Installed package was modified: {path}")
        for remote, value in metadata["runtime_files"].items():
            if digest(root / "apex/com.android.runtime" / remote) != value:
                raise ValueError("Installed Bionic runtime was modified")
        for filename, value in metadata["adapters"]["files"].items():
            target = (
                root
                / ("system/bin" if filename == "bifrost-native" else "system/lib64")
                / filename
            )
            if digest(target) != value:
                raise ValueError("Installed runtime adapter was modified")
        command = [str(emu)]
        if no_jit:
            command += ["--no-jit"]
        if window_plugin:
            command += ["--android-window-plugin", str(window_plugin)]
        command += [
            "--android-runtime",
            "--android-services",
            "--rootfs",
            str(root),
            "/system/bin/bifrost-native",
            code + "/" + relative,
            entry,
            f"/data/user/0/{package}/files",
            f"/storage/emulated/0/Android/data/{package}/files",
            code + "/assets",
            str(API),
            str(frames),
        ]
    # No compiler, custom linker, synthetic TLS or host library search in launch.
    return subprocess.run(command, check=False).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-root", type=Path, default=ROOT / "android-state")
    commands = parser.add_subparsers(dest="command", required=True)
    installer = commands.add_parser("install")
    installer.add_argument("input", type=Path)
    installer.add_argument(
        "--runtime-root", type=Path, default=ROOT / "tools/android-runtime/rootfs"
    )
    installer.add_argument(
        "--adapters", type=Path, default=ROOT / "build/android-adapters"
    )
    installer.add_argument(
        "--aapt2", type=Path, default=ROOT / "tools/android-tools/aapt2"
    )
    installer.add_argument(
        "--apksigner", type=Path, default=ROOT / "tools/android-tools/apksigner.jar"
    )
    runner = commands.add_parser("run")
    runner.add_argument("package")
    runner.add_argument("--emu", type=Path, default=ROOT / "bifrost-emu")
    runner.add_argument("--no-jit", action="store_true")
    runner.add_argument("--android-window-plugin", type=Path)
    runner.add_argument(
        "--frames",
        type=int,
        default=0,
        help="bounded redraw cycles; zero runs until window close",
    )
    args = parser.parse_args()
    try:
        if args.command == "install":
            value = install(
                args.input,
                args.state_root,
                args.runtime_root,
                args.adapters,
                args.aapt2,
                args.apksigner,
            )
            print(
                f"Installed {value['package']} version {value['version_code']}: {value['apk_count']} signed APKs, {value['arm64_library_count']} ARM64 libraries"
            )
            return 0
        if not 0 <= args.frames <= 36000:
            raise ValueError("--frames must be in 0..36000")
        return launch(
            args.package,
            args.state_root,
            args.emu.resolve(),
            args.no_jit,
            args.android_window_plugin,
            args.frames,
        )
    except (
        OSError,
        ValueError,
        KeyError,
        zipfile.BadZipFile,
        subprocess.SubprocessError,
    ) as error:
        print(f"Android package: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
