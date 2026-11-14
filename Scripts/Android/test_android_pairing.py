#!/usr/bin/env python3
"""android_pairing: an installed Asgard warns when the emulator running it is not the one it was installed for."""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import android_pairing as pairing  # noqa: E402


def sha(data):
    return hashlib.sha256(data).hexdigest()


class PairingTests(unittest.TestCase):
    def setUp(self):
        self.work = Path(tempfile.mkdtemp(prefix="bifrost-pairing."))
        self.addCleanup(__import__("shutil").rmtree, self.work, ignore_errors=True)
        self.root = self.work / "asgard"
        self.root.mkdir()
        self.emulator = self.work / "bifrost-emu"
        self.emulator.write_bytes(b"emulator build A")

    def install(self, **marker):
        (self.root / pairing.MARKER).write_text(json.dumps({"format": 1, "complete": True, **marker}))

    def test_a_matching_emulator_is_not_a_warning(self):
        self.install(emulator={"sha256": sha(b"emulator build A")})
        self.assertIsNone(pairing.mismatch(self.emulator, self.root))

    def test_another_build_is_named_with_both_digests_and_how_to_update(self):
        self.install(emulator={"sha256": sha(b"emulator build B")})
        message = pairing.mismatch(self.emulator, self.root)
        self.assertIn(sha(b"emulator build B")[:12], message)
        self.assertIn(sha(b"emulator build A")[:12], message)
        self.assertIn("sudo make install-asgard", message)
        self.assertIn("BIFROST_NO_PAIRING_WARNING", message)

    def test_what_cannot_be_judged_is_not_a_warning(self):
        self.assertIsNone(pairing.mismatch(self.emulator, self.root))                       # a checkout: no marker
        self.install()
        self.assertIsNone(pairing.mismatch(self.emulator, self.root))                       # installed without an emulator
        self.install(emulator=None)
        self.assertIsNone(pairing.mismatch(self.emulator, self.root))
        self.install(emulator={"sha256": 5})                                                # malformed
        self.assertIsNone(pairing.mismatch(self.emulator, self.root))
        (self.root / pairing.MARKER).write_text("not json")
        self.assertIsNone(pairing.mismatch(self.emulator, self.root))
        self.install(emulator={"sha256": sha(b"emulator build B")})
        self.assertIsNone(pairing.mismatch(self.work / "absent", self.root))                # an emulator that cannot be read

    def test_the_warning_goes_to_the_given_stream_once_and_can_be_silenced(self):
        self.install(emulator={"sha256": sha(b"emulator build B")})
        out = io.StringIO()
        self.assertTrue(pairing.warn(self.emulator, self.root, out))
        self.assertEqual(out.getvalue().count("warning:"), 1)
        with mock.patch.dict(os.environ, {"BIFROST_NO_PAIRING_WARNING": "1"}):
            quiet = io.StringIO()
            self.assertFalse(pairing.warn(self.emulator, self.root, quiet))
            self.assertEqual(quiet.getvalue(), "")
        self.install(emulator={"sha256": sha(b"emulator build A")})
        self.assertFalse(pairing.warn(self.emulator, self.root, io.StringIO()))

    def test_the_warning_defaults_to_stderr(self):
        self.install(emulator={"sha256": sha(b"emulator build B")})
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            pairing.warn(self.emulator, self.root)
        self.assertIn("different bifrost-emu build", err.getvalue())


if __name__ == "__main__":
    unittest.main()
