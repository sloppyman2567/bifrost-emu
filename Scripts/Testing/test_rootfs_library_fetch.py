#!/usr/bin/env python3
"""fetch-realworld-libs.sh, fetch-curl-deps.sh and install-rootfs.sh --full against a broken or changed mirror.

Offline: a fake `curl` earlier on PATH serves a small mirror from a temporary directory, or fails
the way the real one does. It logs its arguments, so the download limits can be checked too."""
import gzip
import io
import os
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
FAKE_CURL = r'''#!/bin/bash
printf '%s\n' "$*" >> "$FAKE_CURL_LOG"
while [ $# -gt 0 ]; do
    case "$1" in
        -o) output="$2"; shift ;;
        http*) url="$1" ;;
    esac
    shift
done
case "$FAKE_CURL_MODE:$url" in
    timeout-index:*Packages.gz) echo "curl: (28) Operation timed out after 90001 milliseconds" >&2; exit 28 ;;
    missing-deb:*.deb) echo "curl: (22) The requested URL returned error: 404" >&2; exit 22 ;;
esac
source="$FAKE_MIRROR/${url#http://deb.debian.org/debian/}"
[ -f "$source" ] || { echo "curl: (22) The requested URL returned error: 404" >&2; exit 22; }
cp "$source" "$output"
'''


def make_deb(path, library):
    """A .deb holding usr/lib/<library>, in the layout the fetchers unpack."""
    work = path.parent / (path.name + ".build")
    work.mkdir()
    with tarfile.open(work / "data.tar.xz", "w:xz") as data:
        info = tarfile.TarInfo(f"./usr/lib/{library}")
        content = b"library " + library.encode()
        info.size = len(content)
        data.addfile(info, io.BytesIO(content))
    (work / "debian-binary").write_text("2.0\n")
    subprocess.run(["ar", "rc", str(path.resolve()), "debian-binary", "data.tar.xz"], cwd=work, check=True)
    shutil.rmtree(work)


class RootfsLibraryFetchTests(unittest.TestCase):
    def setUp(self):
        self.work = Path(tempfile.mkdtemp(prefix="bifrost-library-fetch."))
        self.addCleanup(shutil.rmtree, self.work, ignore_errors=True)
        self.bin = self.work / "bin"
        self.bin.mkdir()
        (self.bin / "curl").write_text(FAKE_CURL)
        (self.bin / "curl").chmod(0o755)
        self.log = self.work / "curl.log"
        self.mirror = self.work / "mirror"
        self.rootfs = self.work / "rootfs"
        (self.rootfs / "lib").mkdir(parents=True)
        # A libblkid1 version unlike the one the script once hard-coded.
        pool = self.mirror / "pool/main/u/util-linux"
        pool.mkdir(parents=True)
        self.deb = "libblkid1_9.99-1+deb12u9_arm64.deb"
        make_deb(pool / self.deb, "libblkid.so.1.1.0")
        self.write_index(f"Package: libblkid1\nVersion: 9.99\nFilename: pool/main/u/util-linux/{self.deb}\n\n"
                         "Package: libcurl4\nFilename: pool/main/c/curl/libcurl4_9.99_arm64.deb\n\n".encode())
        self.environment = {**os.environ, "PATH": f"{self.bin}:{os.environ['PATH']}", "FAKE_CURL_LOG": str(self.log),
                            "FAKE_MIRROR": str(self.mirror)}

    def write_index(self, content, packed=True):
        index = self.mirror / "dists/bookworm/main/binary-arm64/Packages.gz"
        index.parent.mkdir(parents=True, exist_ok=True)
        index.write_bytes(gzip.compress(content) if packed else content)

    def run_script(self, script, *arguments, mode="", cwd=REPOSITORY):
        return subprocess.run([str(cwd / "Scripts/Setup" / script), *arguments], capture_output=True, text=True, cwd=cwd,
                              env={**self.environment, "FAKE_CURL_MODE": mode})

    def test_pool_paths_come_from_the_index(self):
        result = self.run_script("fetch-realworld-libs.sh", str(self.rootfs))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"{self.deb} ... OK", result.stdout)
        self.assertEqual((self.rootfs / "lib/libblkid.so.1.1.0").read_text(), "library libblkid.so.1.1.0")
        self.assertTrue((self.rootfs / "lib/libblkid.so.1").is_symlink())
        self.assertIn("libselinux1: NOT FOUND in index", result.stdout)     # reported and skipped
        self.assertIn("1 packages installed, 7 failed", result.stdout)

    def test_a_relative_rootfs_works(self):
        result = self.run_script("fetch-realworld-libs.sh", os.path.relpath(self.rootfs, REPOSITORY))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.rootfs / "lib/libblkid.so.1.1.0").is_file())

    def test_the_index_gets_a_longer_limit_than_a_deb(self):
        self.run_script("fetch-realworld-libs.sh", str(self.rootfs))
        lines = self.log.read_text().splitlines()
        index = next(line for line in lines if "Packages.gz" in line)
        deb = next(line for line in lines if self.deb in line)
        self.assertRegex(index, r"--max-time (\d{3,})")
        self.assertGreater(int(index.split("--max-time ")[1].split()[0]), 90)
        self.assertIn("--speed-limit", index)                               # a stall still ends it
        self.assertIn("--max-time 90 ", deb)

    def test_index_failure_exits_nonzero_with_curls_own_error(self):
        for script in ("fetch-curl-deps.sh", "fetch-realworld-libs.sh"):
            result = self.run_script(script, str(self.rootfs), mode="timeout-index")
            self.assertEqual(result.returncode, 1, script)
            self.assertIn("curl: (28) Operation timed out", result.stderr)
            self.assertIn("could not download Packages.gz", result.stderr)

    def test_a_corrupt_index_is_an_error_not_an_empty_success(self):
        self.write_index(b"this is not gzip", packed=False)
        for script in ("fetch-curl-deps.sh", "fetch-realworld-libs.sh"):
            result = self.run_script(script, str(self.rootfs))
            self.assertEqual(result.returncode, 1, script)
            self.assertIn("corrupt or lists no packages", result.stderr)

    def test_a_failed_deb_shows_curls_error_and_the_run_continues(self):
        result = self.run_script("fetch-curl-deps.sh", str(self.rootfs), mode="missing-deb")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("libcurl4_9.99_arm64.deb ... FAILED: curl: (22)", result.stdout)
        self.assertIn("0 packages installed, 27 failed", result.stdout)

    def test_a_missing_rootfs_is_reported_before_any_download(self):
        result = self.run_script("fetch-curl-deps.sh", str(self.work / "absent"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("create the rootfs first", result.stderr)
        self.assertFalse(self.log.exists())

    def fake_checkout(self):
        """Just enough for install-rootfs.sh: a stub rootfs build and ld cache, the real fetchers."""
        checkout = self.work / "checkout"
        setup = checkout / "Scripts/Setup"
        setup.mkdir(parents=True)
        for name in ("install-rootfs.sh", "fetch-curl-deps.sh", "fetch-realworld-libs.sh", "debian-arm64-index.sh"):
            shutil.copy2(REPOSITORY / "Scripts/Setup" / name, setup / name)
        (setup / "setup-rootfs.sh").write_text('#!/bin/bash\nmkdir -p "$1/lib"\ntouch "$1/lib/ld-linux-aarch64.so.1"\n')
        (setup / "setup-rootfs.sh").chmod(0o755)
        (setup / "make-ld-cache.py").write_text('import sys\nopen(sys.argv[1] + "/etc-ld-cache", "w").close()\n')
        (checkout / "Tools/aarch64-linux-gnu-cross").mkdir(parents=True)
        (checkout / "Tools/aarch64-linux-musl-cross").mkdir(parents=True)
        return checkout

    def test_full_rootfs_survives_a_mirror_failure(self):
        prefix = self.work / "prefix"
        result = self.run_script("install-rootfs.sh", "--prefix", str(prefix), "--full", "--no-fetch",
                                 mode="timeout-index", cwd=self.fake_checkout())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("curl: (28) Operation timed out", result.stderr)
        self.assertIn("warning: fetch-realworld-libs.sh failed", result.stderr)
        self.assertIn("warning: fetch-curl-deps.sh failed", result.stderr)
        self.assertTrue((prefix / "share/bifrost/rootfs/lib/ld-linux-aarch64.so.1").exists())
        self.assertTrue((prefix / "share/bifrost/rootfs/etc-ld-cache").exists())  # the cache step still ran


if __name__ == "__main__":
    unittest.main()
