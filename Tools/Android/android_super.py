#!/usr/bin/env python3
"""Unpack partitions from an Android dynamic-partition ("super") image.

A portable replacement for AOSP's `lpunpack`, which no Ubuntu release packages. The format is
liblp's (system/core/fs_mgr/liblp/include/liblp/metadata_format.h): a geometry block at offset 4096,
metadata slots after the two geometry copies, partitions made of linear or zero extents.
Only the first metadata slot is read, which is what `lpunpack` does without `-s`.
"""

import hashlib
import struct
import sys
from pathlib import Path

RESERVED = 4096                 # LP_PARTITION_RESERVED_BYTES
GEOMETRY_SIZE = 4096            # LP_METADATA_GEOMETRY_SIZE
GEOMETRY_MAGIC = 0x616C4467     # "gDla"
HEADER_MAGIC = 0x414C5030       # "0PLA"
SECTOR = 512
EXTENT_LINEAR, EXTENT_ZERO = 0, 1
CHUNK = 1 << 20
MAX_METADATA = 1 << 20


def _geometry(image):
    image.seek(RESERVED)
    block = image.read(GEOMETRY_SIZE)
    magic, size = struct.unpack_from("<II", block)
    if magic != GEOMETRY_MAGIC or not 52 <= size <= GEOMETRY_SIZE:
        raise ValueError("Not an Android super image (bad geometry)")
    if hashlib.sha256(block[:8] + bytes(32) + block[40:size]).digest() != block[8:40]:
        raise ValueError("Super image geometry checksum mismatch")
    max_size, slots, _block_size = struct.unpack_from("<III", block, 40)
    if not 0 < max_size <= MAX_METADATA or slots < 1:
        raise ValueError("Invalid super image geometry")
    return max_size


def read_partitions(image):
    """{name: [(kind, sector_count, first_sector)]} for the first metadata slot."""
    max_size = _geometry(image)
    image.seek(RESERVED + 2 * GEOMETRY_SIZE)
    metadata = image.read(max_size)
    magic, _major, _minor, header_size = struct.unpack_from("<IHHI", metadata)
    if magic != HEADER_MAGIC or not 128 <= header_size <= len(metadata):
        raise ValueError("Super image metadata header is invalid")
    header_checksum = metadata[12:44]
    tables_size = struct.unpack_from("<I", metadata, 44)[0]
    tables_checksum = metadata[48:80]
    header = metadata[:12] + bytes(32) + metadata[44:header_size]
    if hashlib.sha256(header).digest() != header_checksum:
        raise ValueError("Super image metadata header checksum mismatch")
    tables = metadata[header_size:header_size + tables_size]
    if len(tables) != tables_size or hashlib.sha256(tables).digest() != tables_checksum:
        raise ValueError("Super image metadata tables checksum mismatch")
    (p_off, p_count, p_size, e_off, e_count, e_size) = struct.unpack_from("<6I", metadata, 80)
    if p_size < 52 or e_size < 24:
        raise ValueError("Unsupported super image table entry size")
    extents = []
    for index in range(e_count):
        sectors, kind, first, source = struct.unpack_from("<QIQI", tables, e_off + index * e_size)
        if kind == EXTENT_LINEAR and source != 0:
            raise ValueError("Super image spans several block devices")
        extents.append((kind, sectors, first))
    partitions = {}
    for index in range(p_count):
        entry = tables[p_off + index * p_size:p_off + (index + 1) * p_size]
        name = entry[:36].split(b"\0", 1)[0].decode("ascii")
        _attributes, first_extent, extent_count, _group = struct.unpack_from("<IIII", entry, 36)
        partitions[name] = extents[first_extent:first_extent + extent_count]
    return partitions


def unpack(super_image, destination, names=()):
    """Write each wanted partition to destination/<name>.img (all of them when `names` is empty)."""
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    written = []
    with open(super_image, "rb") as image:
        partitions = read_partitions(image)
        wanted = list(names) or sorted(partitions)
        missing = [name for name in wanted if name not in partitions]
        if missing:
            raise ValueError("Partition not in super image: " + ", ".join(missing))
        for name in wanted:
            target = destination / (name + ".img")
            with open(target, "wb") as output:
                for kind, sectors, first in partitions[name]:
                    remaining = sectors * SECTOR
                    if kind == EXTENT_ZERO:
                        while remaining:
                            step = min(CHUNK, remaining)
                            output.write(bytes(step))
                            remaining -= step
                        continue
                    image.seek(first * SECTOR)
                    while remaining:
                        block = image.read(min(CHUNK, remaining))
                        if not block:
                            raise ValueError("Super image is truncated")
                        output.write(block)
                        remaining -= len(block)
            written.append(target)
    return written


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit("usage: android_super.py SUPER.img OUTDIR [PARTITION...]")
    for path in unpack(sys.argv[1], sys.argv[2], sys.argv[3:]):
        print(path)
