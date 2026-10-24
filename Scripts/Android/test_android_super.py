#!/usr/bin/env python3
"""android_super.py unpacks dynamic partitions like AOSP's lpunpack (which no Ubuntu release packages).

Builds a tiny super image by hand: two partitions, linear and zero extents, valid checksums. Checks the
extracted bytes, the selection by name, and that a corrupt image or unknown partition is rejected.
Pass --compare SUPER.img to also compare every partition byte for byte with the real `lpunpack`.
"""
import argparse
import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools/Android"))
import android_super as lp

SECTOR = 512


def build(path, partitions, max_size=4096):
    """partitions: {name: [("linear", bytes) | ("zero", sectors)]}"""
    data_start = 1024 * 1024  # first sector of data, 1 MiB in
    blob, extents, parts, cursor = bytearray(), [], [], data_start // SECTOR
    for name, pieces in partitions.items():
        first_extent = len(extents)
        for kind, value in pieces:
            if kind == "linear":
                padded = value + bytes(-len(value) % SECTOR)
                extents.append(struct.pack("<QIQI", len(padded) // SECTOR, lp.EXTENT_LINEAR, cursor, 0))
                cursor += len(padded) // SECTOR
                blob += padded
            else:
                extents.append(struct.pack("<QIQI", value, lp.EXTENT_ZERO, 0, 0))
        parts.append(struct.pack("<36sIIII", name.encode(), 0, first_extent, len(pieces), 0))
    tables = b"".join(parts) + b"".join(extents)
    header_size = 128
    fields = struct.pack("<6I", 0, len(parts), 52, len(parts) * 52, len(extents), 24)
    unsigned = struct.pack("<IHHI", lp.HEADER_MAGIC, 10, 0, header_size) + bytes(32)
    unsigned += struct.pack("<I", len(tables)) + hashlib.sha256(tables).digest() + fields
    unsigned += bytes(header_size - len(unsigned))
    header = unsigned[:12] + hashlib.sha256(unsigned).digest() + unsigned[44:]
    metadata = (header + tables).ljust(max_size, b"\0")
    geometry = struct.pack("<II", lp.GEOMETRY_MAGIC, 52) + bytes(32) + struct.pack("<III", max_size, 1, 4096)
    geometry += bytes(52 - len(geometry))
    geometry = geometry[:8] + hashlib.sha256(geometry[:8] + bytes(32) + geometry[40:52]).digest() + geometry[40:]
    image = bytearray(data_start)
    image[lp.RESERVED:lp.RESERVED + len(geometry)] = geometry
    image[lp.RESERVED + lp.GEOMETRY_SIZE:lp.RESERVED + lp.GEOMETRY_SIZE + len(geometry)] = geometry
    image[lp.RESERVED + 2 * lp.GEOMETRY_SIZE:lp.RESERVED + 2 * lp.GEOMETRY_SIZE + len(metadata)] = metadata
    path.write_bytes(bytes(image) + bytes(blob))


def expect_error(action, text):
    try:
        action()
    except ValueError as error:
        assert text in str(error), f"expected {text!r}, got {error}"
        return
    raise AssertionError(f"expected an error containing {text!r}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compare", help="a real super.img to compare with the real lpunpack")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        first = bytes(range(256)) * 5 + b"tail"          # not sector-aligned: padded with zeros
        second = b"B" * 1024
        build(work / "super.img", {"system": [("linear", first), ("zero", 2), ("linear", second)],
                                   "vendor": [("linear", b"V" * 700)]})
        written = lp.unpack(work / "super.img", work / "out")
        assert [p.name for p in written] == ["system.img", "vendor.img"], written
        system = (work / "out/system.img").read_bytes()
        padded_first = first + bytes(-len(first) % SECTOR)
        assert system == padded_first + bytes(2 * SECTOR) + second, "system extents (linear, zero, linear)"
        assert (work / "out/vendor.img").read_bytes() == b"V" * 700 + bytes(1024 - 700), "vendor"
        lp.unpack(work / "super.img", work / "one", ["vendor"])
        assert sorted(p.name for p in (work / "one").iterdir()) == ["vendor.img"], "selection by name"
        expect_error(lambda: lp.unpack(work / "super.img", work / "x", ["nope"]), "Partition not in super image")
        damaged = bytearray((work / "super.img").read_bytes())
        damaged[lp.RESERVED + 2 * lp.GEOMETRY_SIZE + 60] ^= 0xFF  # inside the header checksum field
        (work / "damaged.img").write_bytes(bytes(damaged))
        expect_error(lambda: lp.unpack(work / "damaged.img", work / "y"), "checksum mismatch")
        (work / "junk.img").write_bytes(bytes(20000))
        expect_error(lambda: lp.unpack(work / "junk.img", work / "z"), "bad geometry")
        (work / "short.img").write_bytes((work / "super.img").read_bytes()[:-1000])
        expect_error(lambda: lp.unpack(work / "short.img", work / "w"), "truncated")
        print("synthetic super image: unpack, selection and rejection checks passed")
        if args.compare:
            real = shutil.which("lpunpack")
            if not real:
                print("lpunpack not installed: comparison skipped")
            else:
                subprocess.run([real, args.compare, str(work / "real")], check=True, capture_output=True)
                lp.unpack(args.compare, work / "mine")
                for path in sorted((work / "real").glob("*.img")):
                    same = hashlib.file_digest(open(path, "rb"), "sha256").digest() == \
                        hashlib.file_digest(open(work / "mine" / path.name, "rb"), "sha256").digest()
                    assert same, f"{path.name} differs from lpunpack"
                print("identical to lpunpack:", ", ".join(p.name for p in sorted((work / "real").glob("*.img"))))
    print("test_android_super: ALL PASS")


if __name__ == "__main__":
    main()
