#!/usr/bin/env python3
"""Focused malformed-package and ELF-alignment checks; no app execution."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("preflight", Path(__file__).with_name("android_preflight.py"))
preflight = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preflight)


def elf(align, address=0):
    data = bytearray(128)
    data[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<Q",data,32,64)
    struct.pack_into("<HH",data,54,56,1)
    struct.pack_into("<IIQQQQQQ",data,64,1,5,0,address,0,len(data),4096,align)
    return data


class PreflightTests(unittest.TestCase):
    def test_alignment_is_not_runtime_page_size(self):
        with tempfile.TemporaryDirectory() as work:
            path = Path(work)/"library.so"
            for align in (4096,16384,65536):
                path.write_bytes(elf(align))
                result = preflight.elf_alignment(path)
                self.assertEqual(result["elf_load_alignments"],[align])
                self.assertEqual(result["elf_16k_aligned"],align >= 16384)
            for data in (elf(12288),elf(16384,4096),elf(4096)[:80]):
                path.write_bytes(data)
                with self.assertRaises(ValueError): preflight.elf_alignment(path)

    def test_nested_paths_are_never_extracted(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            path = root/"bundle.apkm"
            with zipfile.ZipFile(path,"w") as archive:
                archive.writestr("../../escape.apk",b"fixture")
            with preflight.apk_paths(path,root) as paths:
                self.assertEqual(paths[0][1],root/"package-0.apk")
                self.assertEqual(paths[0][1].read_bytes(),b"fixture")

    def test_launcher_alias_resolves_activity_metadata(self):
        manifest = {"children":[{"tag":"application","children":[
            {"tag":"activity-alias","attributes":{"name":".Launch","targetActivity":"android.app.NativeActivity"}},
            {"tag":"activity","attributes":{"name":"android.app.NativeActivity"},"children":[
                {"tag":"meta-data","attributes":{"name":"android.app.lib_name","value":"engine"}}
            ]}
        ]}]}
        self.assertEqual(preflight.launcher_details("org.test",".Launch",manifest),
            {"class":"android.app.NativeActivity","kind":"native_activity",
             "native_library":"engine","native_entrypoint":"ANativeActivity_onCreate"})


if __name__ == "__main__": unittest.main()
