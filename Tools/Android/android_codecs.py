"""Install the pinned image's real Codec2 services and a narrow store adapter.

This prepares immutable runtime inputs. A realm separately selects enabled
components from host decode probes and owns all service processes.
"""
from pathlib import Path
import hashlib
import subprocess
import xml.etree.ElementTree as ET

from android_media import ROOT, LIBRARY, dump, needed
from android_guest_dependencies import prepare_headers
import android_art

SOURCES = ("codec2_store.c", "codec2_store.S", "media_resource.c", "media_log.c")
COMPONENTS = {"avc": "c2.goldfish.h264.decoder", "hevc": "c2.goldfish.hevc.decoder",
              "vp8": "c2.goldfish.vp8.decoder", "vp9": "c2.goldfish.vp9.decoder"}


def fingerprint():
    digest = hashlib.sha256(Path(__file__).read_bytes())
    for name in SOURCES:
        digest.update((ROOT / "Guest/Android/Launchers" / name).read_bytes())
    digest.update((ROOT / "Tests/Unit/android_bionic_start.S").read_bytes())
    return digest.hexdigest()


def install(root, system, work):
    """Called after platform dependency closure, before private APEX insertion."""
    vendor = work / "partitions/vendor.img"
    dump(system, "/system/lib64/libresourcemanagerservice.so",
         root / "system/lib64/libresourcemanagerservice.so")
    # Resolve the resource manager in the app/system scope before adding the
    # private codec APEX. Its libstagefright_foundation ABI is different.
    graph, _ = android_art.native_dependencies(root, system, limit=1024)
    android_art.unpack_apex(system, "media.swcodec", "capex", work, root)
    libraries = ["libgoldfish_codec2_store.so", "libcodec2_goldfish_common.so"]
    libraries += ["libcodec2_goldfish_" + codec + "dec.so" for codec in COMPONENTS]
    for name in libraries:
        dump(vendor, "/lib64/" + name, root / "vendor/lib64" / name)
    # Vendor codecs are loaded ONLY in the codec daemon's private namespace;
    # they legitimately consume private libvpx and Codec2 libraries there.
    known = {path.name for path in root.rglob("*.so") if path.is_file()}
    queue = list((root / "apex/com.android.media.swcodec").rglob("*.so"))
    queue += list((root / "vendor/lib64").glob("*.so"))
    queue += [root / "apex/com.android.media.swcodec/bin/mediaswcodec"]
    while queue:
        path = queue.pop()
        dependencies = needed(path)
        graph[str(path.relative_to(root))] = dependencies
        for name in dependencies:
            if not LIBRARY.fullmatch(name):
                raise ValueError("Invalid codec dependency")
            if name not in known:
                target = root / "system/lib64" / name
                dump(system, "/system/lib64/" + name, target)
                known.add(name)
                queue.append(target)
    source = ROOT / "Guest/Android/Launchers"
    bionic = root / "apex/com.android.runtime/lib64/bionic"
    headers = prepare_headers(work / "codec-headers")
    common = ["clang", "--target=aarch64-linux-android35", "-fuse-ld=lld", "-nostdlib",
              "-O2", "-Wl,-z,max-page-size=16384", *headers]
    subprocess.run(common + ["-shared", "-fPIC", "-Wl,-z,defs", str(source / "media_log.c"),
                   str(root / "system/lib64/liblog.so"), str(bionic / "libc.so"),
                   "-o", str(root / "system/lib64/libbifrost-media-log.so")], check=True)
    subprocess.run(common + ["-shared", "-fPIC", "-Wl,-z,defs",
                   str(source / "codec2_store.c"), str(source / "codec2_store.S"),
                   str(root / "vendor/lib64/libgoldfish_codec2_store.so"),
                   str(bionic / "libc.so"), str(bionic / "libdl.so"),
                   "-o", str(root / "system/lib64/libbifrost-codec2-store.so")], check=True)
    subprocess.run(common + ["-fPIE", "-pie", "-Wl,--dynamic-linker=/system/bin/linker64",
                   "-Wl,--allow-shlib-undefined", str(ROOT / "Tests/Unit/android_bionic_start.S"),
                   str(source / "media_resource.c"),
                   str(root / "system/lib64/libresourcemanagerservice.so"),
                   str(root / "system/lib64/libbinder_ndk.so"), str(bionic / "libc.so"),
                   "-o", str(root / "system/bin/bifrost-media-resource")], check=True)
    # Keep the image's codec limits, but omit reference-emulator measurements,
    # OMX entries (no OMX service), and unvalidated adaptive-playback promises.
    original = work / "goldfish-media-codecs.xml"
    dump(vendor, "/etc/media_codecs_google_video.xml", original)
    codecs = ET.Element("MediaCodecs")
    decoders = ET.SubElement(codecs, "Decoders")
    for codec in ET.parse(original).iter("MediaCodec"):
        if codec.get("name") not in COMPONENTS.values():
            continue
        for child in list(codec):
            if child.tag != "Limit" or child.get("name") not in ("size", "alignment", "bitrate"):
                codec.remove(child)
        # Native C2 size interfaces accept even dimensions from 2 through 4096.
        for limit in codec.findall("Limit"):
            if limit.get("name") == "size":
                limit.set("min", "2x2")
        # Auto routing includes CPU fallback. Do not classify this mixed host
        # component as guaranteed hardware acceleration in MediaCodecInfo.
        ET.SubElement(codec, "Attribute", name="software-codec")
        decoders.append(codec)
    if len(decoders) != 4:
        raise ValueError("Unexpected pinned host codec catalog")
    config = root / "vendor/etc/media_codecs.xml"
    config.parent.mkdir(parents=True, exist_ok=True)
    ET.indent(codecs)
    config.write_bytes(ET.tostring(codecs, encoding="utf-8", xml_declaration=True) + b"\n")
    return {"adapter_fingerprint": fingerprint(), "native_dependencies": graph,
            "components": COMPONENTS, "stock_store": "software"}


def linker_section(paths):
    return ("\n[codec2]\nnamespace.default.isolated = true\n"
            "namespace.default.visible = true\nnamespace.default.search.paths = "
            + ":".join(paths) + "\nnamespace.default.permitted.paths = /apex:/system:/vendor\n")
