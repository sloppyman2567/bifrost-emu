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
    def test_launcher_icon_density_and_adaptive_bitmap_fallback(self):
        fields = preflight.badging_fields("package: name='org.bifrost.fixture' versionCode='1'\n"
                                          "application: label='Fixture' icon='res/mipmap-anydpi-v26/icon.xml'\n"
                                          "application-icon-160:'res/mipmap-mdpi/icon.png'\n"
                                          "application-icon-480:'res/mipmap-xxhdpi/icon.png'\n")
        self.assertEqual(preflight.launcher_icon(fields, Path('unused.apk'), 'unused-aapt2'),
                         'res/mipmap-xxhdpi/icon.png')
        resources = '''resource 0x7f010000 mipmap/icon
          (anydpi-v26) (file) res/mipmap-anydpi-v26/icon.xml
          (mdpi) (file) res/mipmap-mdpi/icon.png
          (xxhdpi) (file) res/mipmap-xxhdpi/icon.png
          (xxxhdpi-night) (file) res/mipmap-xxxhdpi-night/icon.png
        resource 0x7f010001 mipmap/other
          (xxxhdpi) (file) res/mipmap-xxxhdpi/other.png
        '''
        self.assertEqual(preflight.resource_bitmap(resources, 'res/mipmap-anydpi-v26/icon.xml'),
                         'res/mipmap-xxhdpi/icon.png')
        self.assertIsNone(preflight.resource_bitmap(resources, 'res/missing.xml'))

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
