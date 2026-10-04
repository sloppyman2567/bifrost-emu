#!/usr/bin/env python3
"""Network-free checks for downloader status, integrity and preservation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

FETCH = Path(__file__).resolve().parents[1] / 'tools/fetch-musl-toolchain.sh'


class MuslFetchTests(unittest.TestCase):
    def test_bootstrap_failures_and_existing_install(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tools = root / 'tools'
            tools.mkdir()
            script = tools / FETCH.name
            shutil.copyfile(FETCH, script)
            mockbin = root / 'bin'
            mockbin.mkdir()
            curl = mockbin / 'curl'
            curl.write_text('#!/bin/bash\nexit 28\n')
            curl.chmod(0o755)
            env = dict(os.environ, PATH=f'{mockbin}:{os.environ["PATH"]}')
            result = subprocess.run(['bash', str(script)], env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('exit 28', result.stderr)
            self.assertIn('Bootlin', result.stdout)
            self.assertFalse((tools / 'aarch64-linux-musl-cross').exists())
            self.assertFalse(list(tools.glob('.musl-fetch.*')))

            curl.write_text('#!/bin/bash\nwhile (($#)); do\n'
                            'if [[ "$1" == -o ]]; then printf corrupt > "$2"; exit 0; fi\nshift\ndone\n')
            result = subprocess.run(['bash', str(script), '--bootlin'], env=env,
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('FAILED', result.stdout)
            self.assertFalse((tools / 'aarch64-linux-musl-cross').exists())

            compiler = tools / 'aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc'
            compiler.parent.mkdir(parents=True)
            compiler.write_text('#!/bin/bash\necho existing\n')
            compiler.chmod(0o755)
            result = subprocess.run(['bash', str(script), '--bootlin'], env=env,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('already present', result.stdout)
            self.assertEqual(compiler.read_text(), '#!/bin/bash\necho existing\n')


if __name__ == '__main__':
    unittest.main()
