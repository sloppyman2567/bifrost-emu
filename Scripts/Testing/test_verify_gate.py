#!/usr/bin/env python3
"""Check that the differential gate cannot turn failures into green CI."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

GATE = Path(__file__).resolve().with_name('verify_jit.sh')


class VerificationGateTests(unittest.TestCase):
    def test_failures_are_fatal(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'jit_probe.c'
            fixture = root / 'jit_probe.elf'
            source.write_text('/* gate probe */')
            fixture.touch()
            emulator = root / 'emu'
            emulator.write_text('#!/bin/bash\necho "$PROBE_OUTPUT"\nexit "${PROBE_RC:-0}"\n')
            emulator.chmod(0o755)
            cases = [('', 0, 0), ('', 139, 1), ('', 124, 1)]
            cases += [(message, 0, 1) for message in (
                '[VERIFY] block: PC DIVERGENCE',
                '[VERIFY] block: DIVERGENCE',
                '[VERIFY-MEM] *** REAL MEMORY DIVERGENCE',
                '[MEMFULL] *** REAL FULL-MEMORY DIVERGENCE',
                '[VERIFY] suspended: concurrent guest execution',
            )]
            for output, code, expected in cases:
                with self.subTest(output=output, code=code):
                    env = dict(os.environ, PROBE_OUTPUT=output, PROBE_RC=str(code))
                    result = subprocess.run(['bash', str(GATE), str(emulator), str(root)],
                                            env=env, capture_output=True, text=True)
                    self.assertEqual(result.returncode, expected, result.stdout)
            # Missing, stale, and empty fixture inventories must fail too.
            fixture.unlink()
            for state in ('missing', 'stale', 'empty'):
                if state == 'stale':
                    fixture.touch()
                    os.utime(source, ns=(fixture.stat().st_mtime_ns + 1_000_000_000,) * 2)
                elif state == 'empty':
                    source.unlink()
                result = subprocess.run(['bash', str(GATE), str(emulator), str(root)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 1, (state, result.stdout))


if __name__ == '__main__':
    unittest.main()
