#!/usr/bin/env python3
"""Install signed APK sets and launch native or managed Activities through guest Bionic."""

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
import signal
import stat
import subprocess
import sys
import tempfile
import time
import uuid
import zipfile

import android_paths
import android_preflight as preflight
import android_settings

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools/Android"))
import android_media as media
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
    "libz.so",
    "libmediandk.so",
}
# The input snapshot stays at 2 GiB (asgard.MAX_PACKAGE_BYTES mirrors it); Play asset packs can
# expand past that once their APK members are unpacked.
MAX_TOTAL = 2 * 1024 * 1024 * 1024
MAX_EXPANDED_TOTAL = 4 * 1024 * 1024 * 1024


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(data)
    return value.hexdigest()


def snapshot_input(source, destination):
    if not source.exists():
        raise ValueError(f"No such file: {source}")
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
    # Key rotation (APK Signature Scheme v3.1) labels each signer with the SDK
    # range it covers; only the signer covering API applies on this device.
    values += [
        digest for low, high, digest in re.findall(
            r"^Signer \(minSdkVersion=(\d+), maxSdkVersion=(\d+)\) certificate SHA-256 digest: "
            r"([0-9a-fA-F]{64})$", output, re.M)
        if int(low) <= API <= int(high)
    ]
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
def app_lock(directory, blocking=True):
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / ".install.lock").open("a+b") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | (0 if blocking else fcntl.LOCK_NB))
        except BlockingIOError:
            raise ValueError("App is busy: " + directory.name) from None
        yield


def verify_runtime(runtime):
    spec = importlib.util.spec_from_file_location(
        "runtime", ROOT / "Tools/Android/fetch-android-runtime.py"
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
        "libz.so",
        "libz.so.LICENSE",
    }
    managed = {"libbifrost-activity.so", "managed/libandroid.so", "managed/libEGL.so",
               "managed/libGLESv1_CM.so", "managed/libGLESv2.so", "managed/libGLESv3.so", "managed/libaaudio.so",
               "managed/libOpenSLES.so", "managed/mapper.bifrost.so",
               "managed/vulkan.bifrost.so", "managed/am",
               "managed/libbifrost-binder-attach.so"}
    files = set(metadata.get("files", {}))
    if (
        metadata.get("runtime_archive_sha256") != runtime.SHA256
        or files not in (expected, expected | managed)
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


def app_linker_configuration(has_media=False):
    return (
        "dir.app = /system/bin\n\n[app]\nnamespace.default.isolated = true\n"
        "namespace.default.search.paths = /system/lib64:/apex/com.android.runtime/lib64/bionic\n"
        "namespace.default.permitted.paths = /data/app:/system/lib64:/apex/com.android.runtime/lib64/bionic\n"
        + (media.linker_configuration() if has_media else "")
    )


def require_toolchain(runtime, adapters, aapt2, apksigner):
    """Name the provisioning step for whatever installation needs and is missing."""
    for path, what, step in (
            (runtime / "bifrost-runtime.json", "Android runtime", "python3 Tools/Android/fetch-android-runtime.py"),
            (adapters / "adapters.json", "Android runtime adapters", "make android-runtime-adapters"),
            (aapt2, "aapt2", "python3 Tools/Android/fetch-aapt2.py"),
            (apksigner, "apksigner", "python3 Tools/Android/fetch-android-package-tools.py")):
        if not path.exists():
            raise ValueError(f"{what} not provisioned ({path} is missing). Run: {step}")


def install(source, state, runtime, adapters, aapt2, apksigner, media_root=None, *, allow_downgrade=False):
    require_toolchain(runtime, adapters, aapt2, apksigner)
    runtime_module = verify_runtime(runtime)
    adapter_metadata = verify_adapters(adapters, runtime_module)
    adapter_metadata = dict(adapter_metadata, files={
        name: value for name, value in adapter_metadata["files"].items()
        if not name.startswith("managed/")})
    media_metadata = media.verify(media_root) if media_root is not None else None
    state.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".apk-validation-", dir=state) as work:
        staging = Path(work)
        # Snapshot the input first: verification and installation see the same bytes.
        snapshot = staging / "input.zip"
        snapshot_input(source, snapshot)
        source_sha256 = digest(snapshot)
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
                if budget > MAX_EXPANDED_TOTAL:
                    raise ValueError("APK set exceeds 4 GiB uncompressed")
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
            preflight.require_arm64(abi for report in reports for abi in report["abis"])
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
            # What a device shows: the adaptive icon, whose bitmap fallback can be unrelated art
            # (FNAF Mobile Port's is the Catrobat template's panda).
            base_path = next(p for r, p in packages if not r["split"])
            adaptive = copy_adaptive_icon([base_path, *(p for r, p in packages if r["split"])], str(aapt2), staging)
            icon = None if adaptive else copy_icon(base_path, base.get("icon"), staging)
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
                    if int(base["version_code"]) < int(previous["version_code"]) and not allow_downgrade:
                        raise ValueError("Package downgrade requires --allow-downgrade")
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
                        app_linker_configuration(bool(media_metadata))
                    )
                    if media_metadata:
                        for filename in media_metadata["files"]:
                            target = fresh / media.guest_path(filename)
                            target.parent.mkdir(parents=True, exist_ok=True)
                            shutil.copy2(media_root / filename, target)
                        verify_media(fresh, media_metadata)
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
                    installed_media = previous.get("media") if previous else None
                    if installed_media != media_metadata:
                        raise ValueError("Installed media bundle differs; create a fresh app state")
                    if media_metadata:
                        verify_media(root, media_metadata)
                    for remote, value in runtime_module.FILE_HASHES.items():
                        if digest(root / "apex/com.android.runtime" / remote) != value:
                            raise ValueError("Installed runtime was modified")
                    # An update brings this build's adapters. Files that still match the
                    # adapters recorded at the earlier install are that earlier Bifrost's,
                    # unmodified, and are replaced (a Bifrost update rebuilds them, so
                    # refusing would leave every installed app unable to update); anything
                    # else was changed in the app root and is refused.
                    recorded = (previous or {}).get("adapters", {}).get("files", {})
                    upgrades = []
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
                        current = digest(target) if target.is_file() and not target.is_symlink() else None
                        if current == value:
                            continue
                        if (current is not None and current == recorded.get(filename)) or \
                                (current is None and not target.is_symlink() and filename not in recorded):
                            upgrades.append((filename, target))
                            continue
                        raise ValueError(
                            "Installed adapters were modified; create a fresh app state"
                        )
                    for filename, target in upgrades:
                        replacement = target.with_name("." + target.name + ".update")
                        shutil.copy2(adapters / filename, replacement)
                        if digest(replacement) != adapter_metadata["files"][filename]:
                            replacement.unlink()
                            raise ValueError("Runtime adapter changed while it was being installed")
                        replacement.replace(target)
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
                for old in app.glob("icon.*"):
                    if old.name != (icon.name if icon else None):
                        old.unlink()
                if icon:
                    icon.replace(app / icon.name)
                metadata = {
                    "schema_version": 1,
                    "package": name,
                    "label": base["label"],
                    "version_code": base["version_code"],
                    "version_name": base["version_name"],
                    "icon": icon.name if icon else None,
                    "adaptive_icon": adaptive,
                    "icon_version": ICON_VERSION,
                    "source_sha256": source_sha256,
                    "installed_at": int(time.time()),
                    "signers": certificates,
                    "guest_code": guest_code,
                    "launcher": base["launcher"],
                    "sdk_version": API,
                    "files": files,
                    "packages": reports,
                    "runtime_files": runtime_module.FILE_HASHES,
                    "adapters": adapter_metadata,
                    "media": media_metadata,
                    "apk_count": len(packages),
                    "arm64_library_count": len(libraries),
                }
                publish_json(metadata_path, metadata)
            return metadata


ICON_TYPES = ((b"\x89PNG\r\n\x1a\n", ".png"), (b"\xff\xd8\xff", ".jpg"), (b"RIFF", ".webp"))


def copy_adaptive_icon(paths, aapt2, staging):
    """Copy an adaptive icon's layers for the library to compose; metadata names a colour or a copied file per layer."""
    try:
        layers = preflight.adaptive_icon_layers(paths, aapt2)
    except (OSError, ValueError, subprocess.SubprocessError):
        return None
    if not layers:
        return None
    result = {}
    for name, layer in layers.items():
        if isinstance(layer, (str, float)):
            result[name] = layer
            continue
        copied = copy_icon(layer[0], layer[1], staging, "icon-" + name)
        if copied is None:
            return None
        result[name] = copied.name
    return result


# 2: an adaptive icon wins over its bitmap fallback. An older record is scanned again by refresh-icons.
ICON_VERSION = 2
ICON_LAYER_FILE = re.compile(r"icon-(?:background|foreground)\.(?:png|jpg|webp)")


def missing_icon_layers(app, metadata):
    """Whether the metadata names an adaptive-icon layer file that is not in the app's directory (an install that wrote the
    record but not the file); the library could not draw such an icon."""
    adaptive = metadata.get("adaptive_icon")
    return isinstance(adaptive, dict) and any(
        isinstance(value, str) and ICON_LAYER_FILE.fullmatch(value) and not (app / value).is_file()
        for value in adaptive.values())


def icon_unscanned(app, metadata):
    """Whether refresh-icons has work here: a record from before ICON_VERSION, or a layer file gone missing."""
    return not metadata.get("system") and (metadata.get("icon_version", 0) < ICON_VERSION
                                           or missing_icon_layers(app, metadata))


def refresh_icons(state, aapt2):
    """Give apps installed before adaptive icons were read their layers, and restore layers whose files are missing. Scanned
    once: the result is saved either way."""
    for app in sorted(state.iterdir()):
        metadata_path = app / "package.json"
        if not PACKAGE.fullmatch(app.name) or not metadata_path.is_file():
            continue
        try:
            metadata = json.loads(metadata_path.read_text())
            if not icon_unscanned(app, metadata):
                continue
            with app_lock(app):
                installed = sorted((app / "rootfs/data/app").glob(app.name + "-*/*.apk"), key=lambda p: (p.name != "base.apk", p.name))
                metadata["adaptive_icon"] = copy_adaptive_icon(installed, aapt2, app) if installed else None
                if metadata["adaptive_icon"]: metadata["icon"] = None
                metadata["icon_version"] = ICON_VERSION
                temporary = metadata_path.with_suffix(".json.tmp")
                temporary.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
                temporary.replace(metadata_path)
        except (OSError, ValueError, KeyError) as error:
            print(f"{app.name}: {error}", file=sys.stderr)


def copy_icon(apk, member, staging, stem="icon"):
    """Copy the launcher bitmap for library display; a missing or odd icon is not an error."""
    if not member:
        return None
    with zipfile.ZipFile(apk) as archive:
        try:
            item = archive.getinfo(member)
        except KeyError:
            return None
        if item.file_size > 4 * 1024 * 1024:
            return None
        data = archive.read(item)
    kind = next((suffix for magic, suffix in ICON_TYPES if data.startswith(magic)
                 and (suffix != ".webp" or data[8:12] == b"WEBP")), None)
    if kind is None:
        return None
    output = staging / (stem + kind)
    output.write_bytes(data)
    return output


def verify_media(root, metadata):
    if metadata.get("schema_version") != 1 or metadata.get("image_sha256") != media.IMAGE_SHA256:
        raise ValueError("Unknown installed media image")
    media.verify_files(metadata.get("files"))
    for name, value in metadata["files"].items():
        path = root / media.guest_path(name)
        if path.is_symlink() or digest(path) != value:
            raise ValueError("Installed media library/data was modified")
    configuration = (root / "linkerconfig/ld.config.txt").read_text()
    if configuration != app_linker_configuration(True):
        raise ValueError("Installed media linker namespace was modified")


def verify_installed(package, app, metadata):
    """Shared integrity gate for Activity launch and native-library diagnostics."""
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
    if metadata.get("media"):
        verify_media(root, metadata["media"])
    return code, root


def launch(package, state, emu, no_jit=False, window_plugin=None, frames=0,
           emu_args=(), timeout=0, trace_apk=None, trace_after=0, trace_duration=15, session_id=None):
    if not PACKAGE.fullmatch(package):
        raise ValueError("Invalid package name")
    app = state / package
    from android_sessions import Session, app_id
    with app_lock(app, blocking=False):
        metadata = json.loads((app / "package.json").read_text())
        # Installed before installs were refused for this.
        preflight.require_arm64(abi for report in metadata.get("packages", []) for abi in report.get("abis", []))
        uid = app_id(state, package)
        with Session(app, package, uid, session_id) as session:
            return launch_session(package, app, metadata, emu, no_jit, window_plugin, frames,
                                  emu_args, timeout, trace_apk, trace_after, trace_duration, session)


def launch_session(package, app, metadata, emu, no_jit, window_plugin, frames,
                   emu_args, timeout, trace_apk, trace_after, trace_duration, session):
    session.update(phase="Verifying installed files")
    launcher = metadata["launcher"]
    if launcher["kind"] == "managed_activity":
        if frames:
            raise ValueError("--frames is a NativeActivity redraw bound; use --timeout for managed apps")
        # A provisioned system app (microG) runs from its priv-app directory in the managed root.
        code, root = (metadata["guest_code"], None) if metadata.get("system") \
            else verify_installed(package, app, metadata)
        from android_managed import launch_managed
        return launch_managed(app, metadata, code, root, emu, no_jit,
                              window_plugin, emu_args, timeout, trace_apk, trace_after, trace_duration, session)
    if trace_apk is not None or trace_after or trace_duration != 15:
        raise ValueError("APK framework/method tracing requires a managed Activity")
    if launcher["kind"] != "native_activity":
        raise ValueError("Installed package has no supported launcher Activity")
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
    code, root = verify_installed(package, app, metadata)
    command = [str(emu), *emu_args]
    if no_jit:
        command += ["--no-jit"]
    if window_plugin:
        command += ["--android-window-plugin", str(window_plugin)]
    command += [
        "--android-runtime",
        "--android-services",
        "--android-app-id", str(session.data["uid"]),
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
    log_path, _ = session.logs()
    print(f"Launching {package} (log: {log_path})", flush=True)
    import android_desktop
    with log_path.open("wb") as log:
        return session.run(command, stdout=log, env=dict(os.environ, **android_desktop.window_environment(package)),
                           timeout=timeout)


def probe(package, state, emu, libraries=None, no_jit=False, timeout=45, preloads=None):
    """Run real Bionic dlopen/constructors in a fresh process per installed DSO."""
    if not PACKAGE.fullmatch(package):
        raise ValueError("Invalid package name")
    if not 1 <= timeout <= 300:
        raise ValueError("Probe timeout must be in 1..300 seconds")
    app = state / package
    with app_lock(app):
        metadata = json.loads((app / "package.json").read_text())
        code, root = verify_installed(package, app, metadata)
        available = sorted(
            path.removeprefix("lib/arm64/")
            for path in metadata["files"]
            if path.startswith("lib/arm64/") and path.endswith(".so")
        )
        selected = list(dict.fromkeys(libraries)) if libraries else available
        if not selected or any(name not in available for name in selected):
            raise ValueError("Probe requires packaged ARM64 library names")
        prerequisites = list(dict.fromkeys(preloads or []))
        if any(name not in available for name in prerequisites):
            raise ValueError("Preload requires packaged ARM64 library names")
    failures = 0
    loaded_count = 0
    for library in selected:
        command = [str(emu)] + (["--no-jit"] if no_jit else []) + [
            "--android-runtime", "--android-services", "--rootfs", str(root),
            "/system/bin/bifrost-native", "--probe", code + "/lib/arm64/" + library,
        ] + [code + "/lib/arm64/" + name for name in prerequisites]
        # Constructors can print arbitrarily much. Spool instead of accumulating
        # subprocess output in memory; show only a bounded diagnostic tail.
        with tempfile.TemporaryFile() as output:
            with subprocess.Popen(command, stdout=output, stderr=output, start_new_session=True) as process:
                try:
                    status = process.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    status = 124
                finally:
                    # A constructor can fork. End the entire diagnostic session,
                    # including descendants, before advancing to the next DSO.
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait()
            length = output.tell()
            output.seek(0)
            fatal = False
            loaded = False
            carry = b""
            for block in iter(lambda: output.read(65536), b""):
                text = carry + block
                fatal |= bool(re.search(rb"Fatal signal|\[crash\]|FORTIFY:|CANNOT LINK", text))
                loaded |= b"native probe: LOAD_OK " in text
                carry = text[-32:]
            output.seek(max(0, length - 8192))
            tail = output.read().decode("utf-8", errors="replace")
        success = status == 0 and not fatal and loaded
        loaded_count += loaded
        failures += not success
        if success:
            result = "loaded, exited cleanly"
        else:
            result = "loaded, process failed" if loaded else "load failed"
        print(f"{library}: {result} (exit {status})", flush=True)
        print(tail, end="" if tail.endswith("\n") else "\n", flush=True)
    print(f"Native load probe: {loaded_count}/{len(selected)} loaded; "
          f"{len(selected) - failures}/{len(selected)} exited cleanly; JNI/framework startup not attempted")
    return 1 if failures else 0


def update_desktop(state, package=None):
    """Keep the menu entries in step with the installed apps (see android_desktop). An entry is a convenience: failing to
    write one never fails an install or an uninstall."""
    import android_desktop
    if not android_desktop.enabled(state):
        return
    try:
        android_desktop.sync(state, installed_apps(state), only=package)
    except OSError as error:
        print(f"Menu entry not updated: {error}", file=sys.stderr)


def installed_apps(state):
    from android_sessions import control
    result = []
    if not state.is_dir(): return result
    for app in sorted(state.iterdir()):
        if not PACKAGE.fullmatch(app.name) or not (app / "package.json").is_file(): continue
        try:
            metadata = json.loads((app / "package.json").read_text())
            if metadata["package"] != app.name: raise ValueError("Package directory mismatch")
            session = control(app)
            # Records from before label/icon capture still carry the base APK report.
            base = next((x for x in metadata.get("packages", []) if not x.get("split")), {})
            icon = metadata.get("icon")
            adaptive = metadata.get("adaptive_icon")
            layers = {name: value if re.fullmatch(r"#[0-9a-f]{8}", value) else str(app / value)
                      for name, value in adaptive.items()
                      if isinstance(value, str)
                      and re.fullmatch(r"#[0-9a-f]{8}|icon-(?:background|foreground)\.(?:png|jpg|webp)", value)
                      } if isinstance(adaptive, dict) else {}
            result.append({"package": app.name, "label": metadata.get("label") or base.get("label") or app.name,
                           "version": metadata["version_code"],
                           "version_name": metadata.get("version_name") or base.get("version_name"),
                           "icon": str(app / icon) if isinstance(icon, str) and re.fullmatch(r"icon\.(?:png|jpg|webp)", icon)
                                   and (app / icon).is_file() else None,
                           "icon_layers": layers if len(layers) == 2 else None,
                           "icon_inset": adaptive.get("foreground_inset", 0) if isinstance(adaptive, dict) else 0,
                           "icon_unscanned": icon_unscanned(app, metadata),
                           "installed_at": metadata.get("installed_at"), "system": bool(metadata.get("system")),
                           "requester": metadata.get("requester"),
                           "launcher": metadata["launcher"]["kind"], "session": session})
        except (OSError, ValueError, KeyError) as error:
            result.append({"package": app.name, "label": app.name, "error": str(error),
                           "session": {"state": "invalid"}})
    return result


def installed_labels(state):
    """{package: title} for every installed app, read from the records without probing sessions."""
    labels = {}
    if not state.is_dir(): return labels
    for app in sorted(state.iterdir()):
        if not PACKAGE.fullmatch(app.name) or not (app / "package.json").is_file(): continue
        try:
            metadata = json.loads((app / "package.json").read_text())
            base = next((x for x in metadata.get("packages", []) if not x.get("split")), {})
            labels[app.name] = metadata.get("label") or base.get("label") or ""
        except (OSError, ValueError, AttributeError, TypeError):
            labels[app.name] = ""
    return labels


def resolve_package(state, name, fuzzy=True):
    """The installed package `name` stands for: the exact name, else (unless `fuzzy` is off) the one app whose
    package name or title contains it, ignoring case, spaces and punctuation. Anything else says what to try."""
    if PACKAGE.fullmatch(name) and (state / name / "package.json").is_file():
        return name
    if Path(name).suffix.lower() in (".apk", ".apkm", ".apks", ".xapk"):
        raise ValueError(f"No such file: {name}")
    apps = installed_labels(state)
    if not apps:
        raise ValueError(f"Package is not installed: {name}. No apps are installed yet: asgard install FILE")
    squash = lambda text: re.sub(r"[^a-z0-9]", "", text.lower())
    wanted = squash(name)
    found = [package for package, label in apps.items()
             if wanted and (wanted in squash(package) or wanted in squash(label))]
    if len(found) > 1:  # "2048" in two names: an app whose title is exactly the words given wins
        exact = [package for package in found if wanted in (squash(apps[package]), squash(package.rsplit(".", 1)[-1]))]
        found = exact if len(exact) == 1 else found
    if fuzzy and len(found) == 1:
        print(f"Using {found[0]}" + (f" ({apps[found[0]]})" if apps[found[0]] else ""), file=sys.stderr)
        return found[0]
    import difflib
    guesses = found or difflib.get_close_matches(name, list(apps), n=3, cutoff=.5) or \
        [package for label in difflib.get_close_matches(name, list(apps.values()), n=3, cutoff=.5) if label
         for package, title in apps.items() if title == label]
    if guesses:
        shown = ", ".join(f"{package}" + (f" ({apps[package]})" if apps[package] else "") for package in guesses)
        if len(found) > 1:
            raise ValueError(f"{name!r} matches more than one app: {shown}")
        raise ValueError(f"Package is not installed: {name}. Did you mean {shown}?" +
                         ("" if fuzzy else " (this command needs the full package name)"))
    raise ValueError(f"Package is not installed: {name}. See: asgard list")


# App-owned locations in a guest root (credential and device storage, ART profiles, the
# app-specific external directories), each holding one directory per package. OBBs are kept:
# Android 10+ clears them, but nothing here could fetch them again.
APP_DATA = ("data/user/0", "data/user_de/0", "data/misc/profiles/cur/0", "data/misc/profiles/ref",
            "storage/emulated/0/Android/data", "storage/emulated/0/Android/media")


@contextmanager
def idle_app(state, package):
    """Yield an installed, non-system app's directory while no session can start or run."""
    app = state / package
    if not PACKAGE.fullmatch(package) or not (app / "package.json").is_file():
        raise ValueError("Package is not installed: " + package)
    if app.is_symlink():
        raise ValueError("Refusing redirected app state: " + package)
    if json.loads((app / "package.json").read_text()).get("system"):
        raise ValueError("System app cannot be removed: " + package)
    # A session holds this lock from preparation to exit, so it is also the running check.
    with app_lock(app, blocking=False):
        yield app


def remove_tree(path):
    if path.is_symlink():
        path.unlink()
    elif path.exists():
        # Installed code directories are read-only; only the directories need relaxing.
        from android_managed import remove_runtime
        remove_runtime(path)


def uninstall(state, package):
    """Remove an app with its code, data and runtime copies, like `pm uninstall` without -k."""
    from android_sessions import release_app_id
    with idle_app(state, package) as app:
        # One rename takes it out of the library atomically, whatever the deletion then meets.
        trash = state / (".removing-" + uuid.uuid4().hex)
        app.rename(trash)
        try:
            remove_tree(trash)
        except OSError as error:
            raise ValueError(f"{package} was uninstalled but {trash} could not be fully deleted: {error}") from error
    release_app_id(state, package)


def clear_data(state, package):
    """Reset an installed app's data to a first-run state, like `pm clear`; the install stays."""
    from android_sessions import app_id
    with idle_app(state, package) as app:
        uid = app_id(state, package)
        for root, managed in ((app / "rootfs", False), (app / "managed/rootfs", True)):
            if not root.is_dir() or root.is_symlink():
                continue
            # pm clear also deletes the app's Keystore keys (FrameworkKeystore keeps one file per uid).
            keystore = root / "data/misc/keystore"
            if keystore.is_dir() and not keystore.is_symlink():
                for suffix in ("", ".bak", ".lock"):
                    (keystore / f"bifrost-{uid}.json{suffix}").unlink(missing_ok=True)
            for relative in APP_DATA:
                target = root / relative / package
                # Data in the guest's reach can be a symlink, but its parents are runtime-owned.
                if not target.parent.resolve().is_relative_to(root.resolve()):
                    raise ValueError("Refusing redirected app data: " + relative)
                remove_tree(target)
            if managed:
                from android_managed import ensure_app_data
                ensure_app_data(root, package)
            else:
                for relative in (f"data/user/0/{package}/files", f"data/user/0/{package}/cache",
                                 f"storage/emulated/0/Android/data/{package}/files"):
                    (root / relative).mkdir(parents=True, exist_ok=True)


def apk_package_name(path, aapt2):
    """The package name an APK declares, so `compat` also takes the file that `install` takes."""
    if not Path(aapt2).is_file():
        raise ValueError("AAPT2 is missing; run python3 Tools/Android/fetch-aapt2.py or give a package name")
    return preflight.badging_fields(preflight.run([str(aapt2), "dump", "badging", str(path)]))["package"]["name"]


def compatibility_report(args):
    import android_compat
    data = android_compat.load()
    if args.package and args.package.lower().endswith(".apk") and Path(args.package).is_file():
        args.package = apk_package_name(args.package, args.aapt2)
    if args.package:
        if not PACKAGE.fullmatch(args.package):
            raise ValueError("Invalid package name")
        value = android_compat.entry(data, args.package)
        print(json.dumps(value) if args.json else "\n".join(android_compat.describe(data, args.package)))
        return 0
    # Without a package: every app the library lists, then recorded apps that are not installed.
    installed = [app["package"] for app in installed_apps(args.state_root) if not app.get("system")]
    packages = installed + [x for x in data["apps"] if x not in installed]
    if args.json:
        print(json.dumps({x: dict(android_compat.entry(data, x), installed=x in installed) for x in packages}))
        return 0
    for package in packages:
        print(android_compat.summary(package, android_compat.entry(data, package)) +
              ("" if package in installed else "  [not installed]"))
    return 0


def configure(args):
    app = args.state_root / args.package
    if not PACKAGE.fullmatch(args.package) or not (app / "package.json").is_file():
        raise ValueError("Package is not installed: " + args.package)
    settings = android_settings.load(app)
    if args.key and not android_settings.known(args.key):
        raise ValueError("Unknown setting: " + args.key)
    if args.reset:
        if args.value is not None:
            raise ValueError("--reset takes at most a setting name")
        settings = {key: value for key, value in settings.items() if args.key and key != args.key}
        android_settings.save(app, settings)
    elif args.value is not None:
        if not args.key:
            raise ValueError("A value needs a setting name")
        settings[args.key] = android_settings.parse(args.key, args.value)
        android_settings.save(app, settings)
    elif args.key and not args.json:
        value = settings.get(args.key)
        print("default" if value is None else str(value).lower() if isinstance(value, bool) else value)
        return 0
    if (args.reset or args.value is not None) and args.key in (None, "menu_entry"):
        update_desktop(args.state_root, args.package)
    if args.json:
        print(json.dumps(settings))
    elif args.reset or args.value is not None:
        print("Saved; applies the next time the app starts")
    else:
        for key in (*android_settings.KEYS, *sorted(k for k in settings if android_settings.property_name(k))):
            value = settings.get(key)
            print(f"{key} = " + ("default" if value is None else str(value).lower() if isinstance(value, bool) else value))
    return 0


def confirm(question, assume_yes):
    if assume_yes:
        return
    if not sys.stdin.isatty():
        raise ValueError("This cannot be undone; pass --yes to confirm")
    if input(question + " [y/N] ").strip().lower() not in ("y", "yes"):
        raise ValueError("Cancelled")


def start_background(package, state, options=()):
    from android_sessions import control
    if not PACKAGE.fullmatch(package) or not (state / package / "package.json").is_file():
        raise ValueError("Package is not installed: " + package)
    app = state / package
    saved = control(app)
    if saved.get("state") in ("preparing", "running", "stopping"):
        raise ValueError("App is already active: " + package)
    session_id = uuid.uuid4().hex
    directory = app / "managed/sessions" / session_id
    directory.mkdir(parents=True)
    options, saved_environment = android_settings.apply_to_launch(app, options, os.environ)
    with (directory / "launcher.log").open("wb") as log:
        child = subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
                                  "--state-root", str(state.resolve()), "run", package,
                                  "--session-id", session_id, *options], stdout=log,
                                 stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True,
                                 env=dict(os.environ, **saved_environment))
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        saved = control(app)
        if saved.get("session_id") == session_id: return saved
        if child.poll() is not None:
            raise ValueError((directory / "launcher.log").read_text(errors="replace").strip())
        time.sleep(0.05)
    # It may still be starting; leave the owned worker running and report its log.
    return {"package": package, "session_id": session_id, "state": "starting",
            "log": str(directory / "launcher.log")}


def add_launch_options(parser):
    from android_managed import add_trace_arguments
    add_trace_arguments(parser)
    parser.add_argument("--emu", type=Path, default=android_paths.emulator())
    parser.add_argument("--no-jit", action="store_true")
    parser.add_argument("--binder-broker", type=Path, help="join an Asgard shared Binder realm")
    parser.add_argument("--android-window-plugin", type=Path)
    parser.add_argument("--timeout", type=int, default=0, help="session seconds; zero runs until window close")
    for mode in ("", "-title", "-window", "-overlay"):
        parser.add_argument("--window-stats" + mode, action=argparse.BooleanOptionalAction,
                            default=None, help="configure emulator presentation statistics")
    parser.add_argument("--window-stats-mode", choices=("minimal", "normal", "overlay", "1", "2", "3"), default=None,
                        help="stats layout: title only, title and in-window overlay, or overlay only (turns stats on)")
    parser.add_argument("--frames", type=int, default=0, help="NativeActivity redraw cycles; zero runs until window close")


def main():
    parser = argparse.ArgumentParser(prog="asgard", description=__doc__)
    parser.add_argument("--state-root", type=Path, default=android_paths.state_root())
    commands = parser.add_subparsers(dest="command", required=True)
    installer = commands.add_parser("install", help="install an .apk, .apkm, .apks or .xapk")
    installer.add_argument("input", type=Path)
    installer.add_argument("-d", "--allow-downgrade", action="store_true",
                           help="allow an older signed version, preserving app data and settings")
    installer.add_argument(
        "--runtime-root", type=Path, default=ROOT / "Tools/android-runtime/rootfs"
    )
    installer.add_argument(
        "--adapters", type=Path, default=ROOT / "build/android-adapters"
    )
    installer.add_argument(
        "--aapt2", type=Path, default=ROOT / "Tools/android-tools/aapt2"
    )
    installer.add_argument(
        "--apksigner", type=Path, default=ROOT / "Tools/android-tools/apksigner.jar"
    )
    installer.add_argument("--media-root", type=Path,
                           help="verified real Media NDK bundle from Tools/Android/android_media.py")
    runner = commands.add_parser("run", help="run one app in the foreground (an APK file is installed first)")
    add_launch_options(runner)
    runner.add_argument("package")
    runner.add_argument("--session-id", help=argparse.SUPPRESS)
    starter = commands.add_parser("start", help="launch one or more apps in independent background sessions")
    starter.add_argument("packages", nargs="+")
    add_launch_options(starter)
    listing = commands.add_parser("list", aliases=["apps"], help="show installed apps and live sessions")
    listing.add_argument("--json", action="store_true")
    for name, text in (("status", "show whether an app is running"), ("stop", "ask a running app to stop"),
                       ("logs", "print the end of an app's session log"), ("resume", "bring a backgrounded app back")):
        command = commands.add_parser(name, help=text)
        command.add_argument("package")
        if name == "status": command.add_argument("--json", action="store_true")
        if name == "logs":
            command.add_argument("--lines", type=int, default=80)
            command.add_argument("--logd", action="store_true")
    compatibility = commands.add_parser("compat", help="show the documented compatibility status of tested apps")
    compatibility.add_argument("package", nargs="?", help="package name, or an .apk file to read it from")
    compatibility.add_argument("--aapt2", type=Path, default=ROOT / "Tools/android-tools/aapt2")
    compatibility.add_argument("--json", action="store_true")
    configuration = commands.add_parser("config", help="show, set or reset an app's saved settings (applied at each start)")
    configuration.add_argument("package")
    configuration.add_argument("key", nargs="?", type=android_settings.setting_key,
                               help="window_stats, window_stats_mode, back_key, enter_key, jit_disk_cache, jit_disk_cache_mb, network, window_mode, menu_entry or property.NAME")
    configuration.add_argument("value", nargs="?")
    configuration.add_argument("--reset", action="store_true", help="restore the default of KEY, or of every setting")
    configuration.add_argument("--json", action="store_true")
    for name, text in (("uninstall", "remove an app with its code and all of its data"),
                       ("clear-data", "reset an app's saves and settings; the app stays installed")):
        command = commands.add_parser(name, help=text)
        command.add_argument("package")
        command.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    launcher = commands.add_parser("launcher", help="open the desktop app library")
    launcher.add_argument("--fullscreen", action="store_true",
                          help="open full screen (the default in Steam's Game Mode); a gamepad press switches the library to its "
                               "large-tile layout")
    commands.add_parser("refresh-icons", help="read the adaptive icons of apps installed before they were supported")
    desktop = commands.add_parser("desktop", help="make menu entries for the installed apps, or remove them")
    desktop.add_argument("--remove", action="store_true", help="remove every entry this state root made")
    desktop.add_argument("--default", choices=("on", "off"),
                         help="whether apps without a menu_entry setting get an entry (saved); then write the entries")
    google = commands.add_parser("google", help="sign in to Google (microG) and manage the device's Google accounts")
    actions = google.add_subparsers(dest="action", required=True)
    for name, text in (("sign-in", "add a Google account in microG's sign-in window"),
                       ("settings", "open microG's settings (accounts, device registration, attestation)")):
        add_launch_options(actions.add_parser(name, help=text))
    accounts = actions.add_parser("accounts", help="list the signed-in Google accounts")
    accounts.add_argument("--json", action="store_true")
    actions.add_parser("remove", help="sign an account out of every app").add_argument("account")
    games = actions.add_parser("games-profile", help="allow or forbid microG creating a Play Games profile "
                               "for an account that has none (Play Games sign-in answers 404 without one)")
    games.add_argument("choice", nargs="?", choices=("status", "on", "off"), default="status")
    games.add_argument("--yes", action="store_true", help="do not ask for confirmation when turning it on")
    diagnostic = commands.add_parser("probe", help="dlopen native libraries without JNI/Activity startup")
    diagnostic.add_argument("package")
    diagnostic.add_argument("--library", action="append", help="packaged basename; repeatable, default all")
    diagnostic.add_argument("--preload", action="append", help="packaged basename to load first; repeatable in order")
    diagnostic.add_argument("--emu", type=Path, default=android_paths.emulator())
    diagnostic.add_argument("--no-jit", action="store_true")
    diagnostic.add_argument("--timeout", type=int, default=45, help="seconds per library, 1..300")
    args = parser.parse_args()
    try:
        # Names typed by hand: part of a package name or title will do, except where a wrong guess destroys data.
        fuzzy = args.command not in ("uninstall", "clear-data")
        if args.command in ("run", "status", "stop", "logs", "resume", "config", "probe", "uninstall", "clear-data"):
            args.package = resolve_package(args.state_root, args.package, fuzzy)
        elif args.command == "start":
            args.packages = [resolve_package(args.state_root, name, fuzzy) for name in args.packages]
        if args.command == "launcher":
            from android_launcher import show
            return show(args.state_root.resolve(), args.fullscreen)
        if args.command == "refresh-icons":
            refresh_icons(args.state_root, ROOT / "Tools/android-tools/aapt2")
            # A menu entry written before names the bitmap this may have replaced.
            import android_desktop
            android_desktop.sync(args.state_root, installed_apps(args.state_root))
            update_desktop(args.state_root)
            return 0
        if args.command == "desktop":
            import android_desktop
            if args.default:
                android_desktop.set_default(args.state_root, args.default == "on")
            if args.remove:
                removed = android_desktop.remove_all(args.state_root)
                print(f"Removed {len(removed)} menu entries from {android_desktop.applications_dir()}")
            else:
                written, removed = android_desktop.sync(args.state_root, installed_apps(args.state_root))
                print(f"Wrote {len(written)} menu entries in {android_desktop.applications_dir()}"
                      + (f", removed {len(removed)} for apps that are off or gone" if removed else ""))
                if not written and not android_desktop.default_on(args.state_root):
                    print("No app is set to show in the menu. Turn one on with `asgard config PACKAGE menu_entry on` "
                          "(or in its settings in the library), or all of them with `asgard desktop --default on`.")
            return 0
        if args.command in ("list", "apps"):
            apps = installed_apps(args.state_root)
            if args.json: print(json.dumps(apps))
            else:
                for app in apps:
                    session = app["session"]
                    print(f"{app['package']}  {session['state']}  pid={session.get('pid') or '-'}"
                          f"  version={app.get('version', '?')}")
                if not apps: print("No apps installed. Use install <APK/APKM> or open launcher.")
            return 0
        if args.command in ("status", "stop", "logs", "resume"):
            from android_sessions import control
            if not PACKAGE.fullmatch(args.package) or not (args.state_root / args.package / "package.json").is_file():
                raise ValueError("Package is not installed: " + args.package)
            value = control(args.state_root / args.package, args.command if args.command in ("stop", "resume") else "status")
            if args.command == "resume" and (value.get("error") or value.get("state") != "running"):
                raise ValueError(value.get("error", "App is not running"))
            if args.command == "logs":
                if not 1 <= args.lines <= 10000: raise ValueError("--lines must be in 1..10000")
                path = value.get("logd" if args.logd else "log")
                if not path or not Path(path).is_file(): raise ValueError("No session log available")
                from collections import deque
                with Path(path).open(errors="replace") as log:
                    print("".join(deque(log, maxlen=args.lines)), end="")
            elif args.command == "status" and args.json: print(json.dumps(value))
            else:
                print(f"{args.package}: {'stop requested' if args.command == 'stop' and value['state'] in ('running', 'preparing', 'stopping') else value['state']}"
                      f" (pid={value.get('pid') or '-'}, exit={value.get('exit_code')})")
            return 0
        if args.command == "config":
            return configure(args)
        if args.command == "compat":
            return compatibility_report(args)
        if args.command in ("uninstall", "clear-data"):
            removing = args.command == "uninstall"
            confirm(f"{'Uninstall' if removing else 'Clear all data of'} {args.package}?", args.yes)
            (uninstall if removing else clear_data)(args.state_root, args.package)
            if removing: update_desktop(args.state_root, args.package)
            print(f"{'Uninstalled' if removing else 'Cleared the data of'} {args.package}")
            return 0
        if args.command == "install":
            value = install(
                args.input,
                args.state_root,
                args.runtime_root,
                args.adapters,
                args.aapt2,
                args.apksigner,
                args.media_root,
                allow_downgrade=args.allow_downgrade,
            )
            print(
                f"Installed {value['package']} version {value['version_code']}: {value['apk_count']} signed APKs, {value['arm64_library_count']} ARM64 libraries"
            )
            update_desktop(args.state_root, value["package"])
            return 0
        if args.command == "probe":
            return probe(args.package, args.state_root, args.emu.resolve(), args.library,
                         args.no_jit, args.timeout, args.preload)
        if args.command == "google":
            import android_google
            if args.action == "accounts":
                found = android_google.accounts(args.state_root)
                if args.json: print(json.dumps(found))
                else:
                    for entry in found: print(entry["name"])
                    if not found: print("No Google account is signed in. Use: google sign-in")
                return 0
            if args.action == "games-profile":
                if args.choice == "status":
                    print("Play Games profile creation by microG: " +
                          ("allowed" if android_google.games_profile_allowed(args.state_root) else "not allowed"))
                    return 0
                if args.choice == "on":
                    print(android_google.GAMES_PROFILE_NOTICE)
                    if not args.yes and input("Allow it? Type yes: ").strip().lower() != "yes":
                        print("Not changed")
                        return 1
                android_google.set_games_profile(args.state_root, args.choice == "on")
                print("Play Games profile creation by microG: " + ("allowed" if args.choice == "on" else "not allowed")
                      + "; apps pick it up at their next launch")
                return 0
            if args.action == "remove":
                android_google.remove(args.state_root, args.account)
                print(f"Removed {args.account}; apps drop it at their next launch")
                return 0
            # The microG session is an ordinary background session of the system app.
            android_google.prepare(args.state_root, android_google.LOGIN_ACTIVITY if args.action == "sign-in"
                                   else android_google.SETTINGS_ACTIVITY)
            args.command, args.packages = "start", [android_google.GMS]
        if not 0 <= args.frames <= 36000:
            raise ValueError("--frames must be in 0..36000")
        if args.timeout < 0:
            raise ValueError("--timeout must be nonnegative")
        emu_args = []
        for mode in ("", "_title", "_window", "_overlay"):
            value = getattr(args, "window_stats" + mode)
            if value is not None:
                emu_args.append(("--window-stats" if value else "--no-window-stats") +
                                mode.replace("_", "-"))
        if args.window_stats_mode is not None:
            emu_args.append("--window-stats-mode=" + args.window_stats_mode)
        if args.command == "start":
            options = ["--emu", str(args.emu.resolve()), "--timeout", str(args.timeout),
                       "--frames", str(args.frames), *emu_args]
            if args.binder_broker: options += ["--binder-broker", str(args.binder_broker.resolve())]
            if args.no_jit: options.append("--no-jit")
            if args.android_window_plugin: options += ["--android-window-plugin", str(args.android_window_plugin.resolve())]
            if args.trace_apk: options += ["--trace-apk", args.trace_apk, "--trace-after", str(args.trace_after),
                                          "--trace-duration", str(args.trace_duration)]
            elif args.trace_after or args.trace_duration != 15: raise ValueError("Trace window requires --trace-apk")
            failed = False
            for package in dict.fromkeys(args.packages):
                try:
                    value = start_background(package, args.state_root, options)
                    print(f"{package}: {value['state']} (session {value['session_id']})")
                    failed |= value["state"] in ("failed", "interrupted")
                except (OSError, ValueError) as error:
                    print(str(error), file=sys.stderr)
                    failed = True
            return 1 if failed else 0
        if PACKAGE.fullmatch(args.package):
            emu_args, saved_environment = android_settings.apply_to_launch(
                args.state_root / args.package, emu_args, os.environ)
            os.environ.update(saved_environment)
        if args.binder_broker:
            emu_args += ["--android-binder-broker", str(args.binder_broker.resolve())]
        return launch(
            args.package,
            args.state_root,
            args.emu.resolve(),
            args.no_jit,
            args.android_window_plugin,
            args.frames,
            emu_args,
            args.timeout,
            args.trace_apk,
            args.trace_after,
            args.trace_duration,
            args.session_id,
        )
    except preflight.UnsupportedAbi as error:
        print(f"Asgard: {error}", file=sys.stderr)
        return preflight.EXIT_UNSUPPORTED_ABI
    except (
        OSError,
        ValueError,
        KeyError,
        zipfile.BadZipFile,
        subprocess.SubprocessError,
    ) as error:
        print(f"Asgard: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
