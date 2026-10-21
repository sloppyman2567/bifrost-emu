#!/usr/bin/env python3
"""Write ROOTFS/etc/ld.so.cache for the AArch64 glibc ld.so in the rootfs.

glibc's ld.so only reads /etc/ld.so.cache (never ld.so.conf), and ldconfig -r
needs chroot, so the cache is generated here: new format "glibc-ld.so.cache"
1.1, one entry per AArch64 shared object that has a DT_SONAME, found in the
directories listed in ROOTFS/etc/ld.so.conf.

Usage: make-ld-cache.py ROOTFS
"""
import os
import struct
import sys

FLAGS = 0x0A03  # FLAG_AARCH64_LIB64 | FLAG_ELF_LIBC6
EM_AARCH64 = 183
PT_LOAD, PT_DYNAMIC = 1, 2
DT_NULL, DT_STRTAB, DT_SONAME = 0, 5, 14


def soname(path):
    """DT_SONAME of an AArch64 ELF64 shared object, or None."""
    try:
        with open(path, "rb") as f:
            head = f.read(64)
            if len(head) < 64 or head[:4] != b"\x7fELF" or head[4] != 2 or head[5] != 1:
                return None
            e_type, e_machine = struct.unpack_from("<HH", head, 16)
            if e_machine != EM_AARCH64 or e_type != 3:
                return None
            e_phoff, = struct.unpack_from("<Q", head, 32)
            e_phentsize, e_phnum = struct.unpack_from("<HH", head, 54)
            f.seek(e_phoff)
            phdrs = [struct.unpack("<IIQQQQQQ", f.read(e_phentsize)[:56]) for _ in range(e_phnum)]
            loads = [p for p in phdrs if p[0] == PT_LOAD]
            dyn = next((p for p in phdrs if p[0] == PT_DYNAMIC), None)
            if dyn is None:
                return None
            f.seek(dyn[2])
            data = f.read(dyn[5])
            strtab = name_off = None
            for off in range(0, len(data) - 15, 16):
                tag, val = struct.unpack_from("<QQ", data, off)
                if tag == DT_NULL:
                    break
                if tag == DT_STRTAB:
                    strtab = val
                elif tag == DT_SONAME:
                    name_off = val
            if strtab is None or name_off is None:
                return None
            for p in loads:  # vaddr -> file offset
                if p[3] <= strtab < p[3] + p[5]:
                    f.seek(p[2] + strtab - p[3] + name_off)
                    return f.read(256).split(b"\0", 1)[0].decode()
    except (OSError, struct.error):
        return None
    return None


def libcmp(a, b):
    """glibc _dl_cache_libcmp: digit runs compare numerically."""
    a, b = a.encode(), b.encode() + b"\0"
    i = j = 0
    while i < len(a):
        c1, c2 = a[i], b[j]
        if 48 <= c1 <= 57:
            if not 48 <= c2 <= 57:
                return 1
            v1 = v2 = 0
            while i < len(a) and 48 <= a[i] <= 57:
                v1 = v1 * 10 + a[i] - 48
                i += 1
            while 48 <= b[j] <= 57:
                v2 = v2 * 10 + b[j] - 48
                j += 1
            if v1 != v2:
                return v1 - v2
        elif 48 <= c2 <= 57:
            return -1
        elif c1 != c2:
            return c1 - c2
        else:
            i += 1
            j += 1
    return 0 - b[j]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    conf = os.path.join(root, "etc", "ld.so.conf")
    dirs = [d.strip() for d in open(conf) if d.strip() and not d.startswith("#")] if os.path.exists(conf) else []
    dirs += ["/usr/lib/aarch64-linux-gnu"]
    entries = {}  # soname -> guest path; first directory wins, as in ldconfig
    for d in dirs:
        host = os.path.join(root, d.lstrip("/"))
        if not os.path.isdir(host):
            continue
        for name in sorted(os.listdir(host)):
            path = os.path.join(host, name)
            if ".so" not in name or not os.path.isfile(path):
                continue
            so = soname(path)
            if so and so not in entries:
                entries[so] = d.rstrip("/") + "/" + name
    import functools
    # ldconfig sorts descending by _dl_cache_libcmp; ld.so binary-searches it.
    keys = sorted(entries, key=functools.cmp_to_key(libcmp), reverse=True)
    strings = b""
    offsets = {}
    def intern(s):
        nonlocal strings
        if s not in offsets:
            offsets[s] = len(strings)
            strings += s.encode() + b"\0"
        return offsets[s]
    rows = [(intern(k), intern(entries[k])) for k in keys]
    header_size = 48
    table = header_size + 24 * len(rows)
    out = b"glibc-ld.so.cache" + b"1.1"
    out += struct.pack("<II", len(rows), len(strings))
    out += struct.pack("<B3xI12x", 0, 0)
    for key, value in rows:
        out += struct.pack("<iIIIQ", FLAGS, table + key, table + value, 0, 0)
    out += strings
    assert len(out) == table + len(strings)
    dest = os.path.join(root, "etc", "ld.so.cache")
    with open(dest + ".tmp", "wb") as f:
        f.write(out)
    os.replace(dest + ".tmp", dest)
    print(f"ld.so.cache: {len(rows)} libraries")


if __name__ == "__main__":
    main()
