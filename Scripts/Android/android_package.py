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

import android_preflight as preflight

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
               "managed/libGLESv2.so", "managed/libGLESv3.so", "managed/libaaudio.so",
               "managed/libOpenSLES.so", "managed/mapper.bifrost.so"}
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


def install(source, state, runtime, adapters, aapt2, apksigner, media_root=None):
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
            icon = copy_icon(next(p for r, p in packages if not r["split"]), base.get("icon"), staging)
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


def copy_icon(apk, member, staging):
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
    output = staging / ("icon" + kind)
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
        uid = app_id(state, package)
        with Session(app, package, uid, session_id) as session:
            return launch_session(package, app, metadata, emu, no_jit, window_plugin, frames,
                                  emu_args, timeout, trace_apk, trace_after, trace_duration, session)


def launch_session(package, app, metadata, emu, no_jit, window_plugin, frames,
                   emu_args, timeout, trace_apk, trace_after, trace_duration, session):
    launcher = metadata["launcher"]
    if launcher["kind"] == "managed_activity":
        if frames:
            raise ValueError("--frames is a NativeActivity redraw bound; use --timeout for managed apps")
        code, root = verify_installed(package, app, metadata)
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
    with log_path.open("wb") as log:
        return session.run(command, stdout=log, timeout=timeout)


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
            result.append({"package": app.name, "label": metadata.get("label") or base.get("label") or app.name,
                           "version": metadata["version_code"],
                           "version_name": metadata.get("version_name") or base.get("version_name"),
                           "icon": str(app / icon) if isinstance(icon, str) and re.fullmatch(r"icon\.(?:png|jpg|webp)", icon)
                                   and (app / icon).is_file() else None,
                           "installed_at": metadata.get("installed_at"),
                           "launcher": metadata["launcher"]["kind"], "session": session})
        except (OSError, ValueError, KeyError) as error:
            result.append({"package": app.name, "label": app.name, "error": str(error),
                           "session": {"state": "invalid"}})
    return result


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
    with (directory / "launcher.log").open("wb") as log:
        child = subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
                                  "--state-root", str(state.resolve()), "run", package,
                                  "--session-id", session_id, *options], stdout=log,
                                 stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
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
    parser.add_argument("--emu", type=Path, default=ROOT / "bifrost-emu")
    parser.add_argument("--no-jit", action="store_true")
    parser.add_argument("--binder-broker", type=Path, help="join an Asgard shared Binder realm")
    parser.add_argument("--android-window-plugin", type=Path)
    parser.add_argument("--timeout", type=int, default=0, help="session seconds; zero runs until window close")
    for mode in ("", "-title", "-window", "-overlay"):
        parser.add_argument("--window-stats" + mode, action=argparse.BooleanOptionalAction,
                            default=None, help="configure emulator presentation statistics")
    parser.add_argument("--frames", type=int, default=0, help="NativeActivity redraw cycles; zero runs until window close")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-root", type=Path, default=ROOT / "android-state")
    commands = parser.add_subparsers(dest="command", required=True)
    installer = commands.add_parser("install")
    installer.add_argument("input", type=Path)
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
    runner = commands.add_parser("run")
    add_launch_options(runner)
    runner.add_argument("package")
    runner.add_argument("--session-id", help=argparse.SUPPRESS)
    starter = commands.add_parser("start", help="launch one or more apps in independent background sessions")
    starter.add_argument("packages", nargs="+")
    add_launch_options(starter)
    listing = commands.add_parser("list", aliases=["apps"], help="show installed apps and live sessions")
    listing.add_argument("--json", action="store_true")
    for name in ("status", "stop", "logs"):
        command = commands.add_parser(name)
        command.add_argument("package")
        if name == "status": command.add_argument("--json", action="store_true")
        if name == "logs":
            command.add_argument("--lines", type=int, default=80)
            command.add_argument("--logd", action="store_true")
    commands.add_parser("launcher", help="open the desktop app library")
    diagnostic = commands.add_parser("probe", help="dlopen native libraries without JNI/Activity startup")
    diagnostic.add_argument("package")
    diagnostic.add_argument("--library", action="append", help="packaged basename; repeatable, default all")
    diagnostic.add_argument("--preload", action="append", help="packaged basename to load first; repeatable in order")
    diagnostic.add_argument("--emu", type=Path, default=ROOT / "bifrost-emu")
    diagnostic.add_argument("--no-jit", action="store_true")
    diagnostic.add_argument("--timeout", type=int, default=45, help="seconds per library, 1..300")
    args = parser.parse_args()
    try:
        if args.command == "launcher":
            from android_launcher import show
            return show(args.state_root.resolve())
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
        if args.command in ("status", "stop", "logs"):
            from android_sessions import control
            if not PACKAGE.fullmatch(args.package) or not (args.state_root / args.package / "package.json").is_file():
                raise ValueError("Package is not installed: " + args.package)
            value = control(args.state_root / args.package, "stop" if args.command == "stop" else "status")
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
        if args.command == "install":
            value = install(
                args.input,
                args.state_root,
                args.runtime_root,
                args.adapters,
                args.aapt2,
                args.apksigner,
                args.media_root,
            )
            print(
                f"Installed {value['package']} version {value['version_code']}: {value['apk_count']} signed APKs, {value['arm64_library_count']} ARM64 libraries"
            )
            return 0
        if args.command == "probe":
            return probe(args.package, args.state_root, args.emu.resolve(), args.library,
                         args.no_jit, args.timeout, args.preload)
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
