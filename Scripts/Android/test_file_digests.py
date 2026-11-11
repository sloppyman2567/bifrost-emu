#!/usr/bin/env python3
"""file_digests.py: launch-time integrity hashes are reused only while a file's stats are unchanged (offline)."""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools/Android"))
import file_digests


class FileDigestTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.base = Path(self.directory.name)
        patcher = mock.patch.dict(os.environ, {"XDG_CACHE_HOME": str(self.base / "cache")})
        patcher.start()
        self.addCleanup(patcher.stop)
        self.fresh_cache()
        self.reads = 0
        real = hashlib.file_digest
        def counted(*args, **kwargs):
            self.reads += 1
            return real(*args, **kwargs)
        patcher = mock.patch.object(file_digests.hashlib, "file_digest", counted)
        patcher.start()
        self.addCleanup(patcher.stop)

    def fresh_cache(self):
        """A new process's view: what the previous one committed, nothing in memory."""
        file_digests._cache.close()
        file_digests._cache = file_digests._Cache()

    def write(self, name, data):
        path = self.base / name
        path.write_bytes(data)
        return path

    def test_reused_until_any_change(self):
        path = self.write("lib.so", b"A" * 4096)
        with mock.patch.object(file_digests, "SETTLE_NS", 0):
            self.assertEqual(file_digests.digest(path), hashlib.sha256(b"A" * 4096).hexdigest())
            file_digests.flush()
            self.fresh_cache()
            self.assertEqual(file_digests.digest(path), hashlib.sha256(b"A" * 4096).hexdigest())
            self.assertEqual(self.reads, 1)
            # Same size, modification time put back: the change time still moves, so it is read again.
            before = path.stat()
            with path.open("r+b") as stream: stream.write(b"B")
            os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
            self.assertEqual(path.stat().st_size, before.st_size)
            self.assertEqual(file_digests.digest(path), hashlib.sha256(b"B" + b"A" * 4095).hexdigest())
            self.assertEqual(self.reads, 2)
            # A replacement is another inode.
            replacement = self.write("lib.so.new", b"A" * 4096)
            replacement.replace(path)
            self.assertEqual(file_digests.digest(path), hashlib.sha256(b"A" * 4096).hexdigest())
            self.assertEqual(self.reads, 3)

    def test_recent_file_is_not_remembered(self):
        # A file changed within the last two seconds could change again inside the same timestamp tick.
        path = self.write("fresh.odex", b"x" * 100)
        for _ in range(2): self.assertEqual(file_digests.digest(path), hashlib.sha256(b"x" * 100).hexdigest())
        self.assertEqual(self.reads, 2)

    def test_unusable_cache_still_hashes(self):
        (self.base / "cache").write_text("not a directory")
        self.fresh_cache()
        path = self.write("base.apk", b"apk")
        with mock.patch.object(file_digests, "SETTLE_NS", 0):
            for _ in range(2): self.assertEqual(file_digests.digest(path), hashlib.sha256(b"apk").hexdigest())
        self.assertEqual(self.reads, 2)


if __name__ == "__main__":
    unittest.main()
