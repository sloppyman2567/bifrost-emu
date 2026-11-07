#!/usr/bin/env python3
"""download_progress.py: the progress lines of the Android setup's downloads (terminal and piped), the helpers, and
fetch, which keeps and resumes an interrupted download (against a local HTTP server that can honour or ignore Range)."""
import hashlib
import http.server
import io
from pathlib import Path
import re
import shutil
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools/Android"))
import android_media
import download_progress as progress


class Stream(io.StringIO):
    def __init__(self, tty):
        super().__init__()
        self.tty = tty

    def isatty(self):
        return self.tty


class Clock:
    def __init__(self):
        self.now = 100.0

    def __call__(self):
        return self.now


class Response(io.BytesIO):
    def __init__(self, data, length=True):
        super().__init__(data)
        self.headers = {"Content-Length": str(len(data))} if length else {}


MIB = 1 << 20


class ProgressTests(unittest.TestCase):
    def test_sizes_and_times_read_naturally(self):
        self.assertEqual(progress.format_size(512), "512 B")
        self.assertEqual(progress.format_size(3 * 1024), "3.0 KiB")
        self.assertEqual(progress.format_size(734 * MIB), "734.0 MiB")
        self.assertEqual(progress.format_size(3 << 30), "3.0 GiB")
        self.assertEqual((progress.format_time(5), progress.format_time(65), progress.format_time(3725)),
                         ("0:05", "1:05", "1:02:05"))

    def test_piped_output_prints_every_ten_percent_after_two_seconds_and_a_summary(self):
        # 80 MiB at 8 MiB/s in exact quarter-second steps: nothing for two seconds, then a line per ten percent.
        stream, clock = Stream(False), Clock()
        bar = progress.Progress("image", 80 * MIB, stream, clock)
        for _ in range(40):
            clock.now += 0.25
            bar.advance(2 * MIB)
        bar.finish()
        lines = stream.getvalue().splitlines()
        self.assertNotIn("\r", stream.getvalue())
        self.assertEqual([line.split("%")[0].removeprefix("image: ") for line in lines[:-1]],
                         [str(percent) for percent in range(20, 101, 10)])
        self.assertEqual(lines[0], "image: 20%  16.0 MiB/80.0 MiB  8.0 MiB/s  ETA 0:08")
        self.assertEqual(lines[-1], "image: 80.0 MiB in 0:10 (8.0 MiB/s)")

    def test_a_terminal_updates_one_line_in_place_and_ends_with_a_summary(self):
        stream, clock = Stream(True), Clock()
        bar = progress.Progress("image", 100 * MIB, stream, clock)
        for _ in range(25):
            clock.now += 0.25
            bar.advance(2 * MIB)
        text = stream.getvalue()
        self.assertIn("\r\x1b[K", text)
        self.assertNotIn("\n", text)
        last = text.split("\r\x1b[K")[-1]
        self.assertRegex(last, r"^image  \[=+>? *\] +50%  50\.0 MiB/100\.0 MiB  8\.0 MiB/s  ETA 0:06$")
        bar.finish()
        self.assertTrue(stream.getvalue().endswith("\r\x1b[Kimage: 50.0 MiB in 0:06 (8.0 MiB/s)\n"))

    def test_a_fast_small_download_stays_quiet(self):
        for tty in (False, True):
            stream, clock = Stream(tty), Clock()
            bar = progress.Progress("header", 64 * 1024, stream, clock)
            clock.now += 0.05
            bar.advance(64 * 1024)
            bar.finish()
            self.assertEqual(stream.getvalue(), "")

    def test_unknown_size_shows_bytes_and_speed(self):
        stream, clock = Stream(False), Clock()
        bar = progress.Progress("stream", None, stream, clock)
        for _ in range(40):
            clock.now += 0.25
            bar.advance(2 * MIB)
        self.assertRegex(stream.getvalue().splitlines()[0], r"^stream  \d+\.\d MiB  8\.0 MiB/s$")
        self.assertNotIn("%", stream.getvalue())

    def test_read_returns_up_to_limit_plus_one_and_reports(self):
        stream = Stream(False)
        data = bytes(range(256)) * 4096  # 1 MiB
        self.assertEqual(progress.read(Response(data), len(data), "tool", stream=stream), data)
        self.assertEqual(len(progress.read(Response(data), len(data) - 10, "tool", stream=stream)), len(data) - 9)
        self.assertEqual(progress.read(Response(data, length=False), len(data), "tool", stream=stream), data)
        self.assertEqual(progress.content_length(Response(b"")), None)
        self.assertIn("tool: 1.0 MiB in", stream.getvalue())

    def test_copy_bytes_still_copies_exactly_and_rejects_a_short_download(self):
        data = b"android" * 300000
        sink = io.BytesIO()
        android_media.copy_bytes(io.BytesIO(data), sink, len(data))  # a local copy shows nothing
        self.assertEqual(sink.getvalue(), data)
        sink = io.BytesIO()
        android_media.copy_bytes(io.BytesIO(data), sink, len(data), "download")
        self.assertEqual(sink.getvalue(), data)
        with self.assertRaisesRegex(ValueError, "Truncated Android image"):
            android_media.copy_bytes(io.BytesIO(data[:100]), io.BytesIO(), len(data), "download")


class Server:
    """Serves `data` at /file. `ranges` False ignores Range; `cut` ends the first response after that many bytes."""

    def __init__(self, data, ranges=True, cut=None):
        self.data, self.requests = data, []
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *arguments):
                pass

            def do_GET(self):
                outer.requests.append(self.headers.get("Range"))
                match = re.fullmatch(r"bytes=(\d+)-", self.headers.get("Range") or "")
                start = int(match.group(1)) if match and ranges else 0
                if start >= len(data):
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{len(data)}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                self.send_response(206 if match and ranges else 200)
                if match and ranges:
                    self.send_header("Content-Range", f"bytes {start}-{len(data) - 1}/{len(data)}")
                body = data[start:]
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                try:
                    if cut is not None and len(outer.requests) == 1:
                        self.wfile.write(body[:cut])
                        self.close_connection = True
                    else:
                        self.wfile.write(body)
                except ConnectionError:
                    pass  # the client gave up (Ctrl-C, size limit)

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.httpd.server_port}/file"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def interrupted_after(blocks):
    """An opener whose download raises KeyboardInterrupt (Ctrl-C) once `blocks` reads have returned."""
    def opener(request, timeout):
        response = urllib.request.urlopen(request, timeout=timeout)
        read, count = response.read, [0]

        def limited(amount):
            count[0] += 1
            if count[0] > blocks:
                raise KeyboardInterrupt
            return read(amount)
        response.read = limited
        return response
    return opener


DATA = bytes(range(256)) * 4096 * 3 + b"tail"  # 3 MiB and a bit: four reads of at most 1 MiB


class FetchTests(unittest.TestCase):
    def setUp(self):
        self.work = Path(tempfile.mkdtemp(prefix="bifrost-fetch."))
        self.addCleanup(shutil.rmtree, self.work, ignore_errors=True)
        self.target = self.work / "image.zip"
        self.part = self.work / "image.zip.part"
        self.check = progress.sha256_check(hashlib.sha256(DATA).hexdigest(), "image")

    def serve(self, **options):
        server = Server(options.pop("data", DATA), **options)
        self.addCleanup(server.close)
        return server

    def fetch(self, server, **options):
        stream = options.setdefault("stream", Stream(False))
        options.setdefault("check", self.check)
        options.setdefault("pause", 0)
        progress.fetch(server.url, self.target, "image", options.pop("limit", len(DATA)), **options)
        return stream.getvalue()

    def test_a_complete_download_replaces_the_destination_and_leaves_no_part(self):
        self.fetch(self.serve())
        self.assertEqual(self.target.read_bytes(), DATA)
        self.assertFalse(self.part.exists())

    def test_ctrl_c_keeps_the_partial_file_and_the_next_run_resumes_it(self):
        server = self.serve()
        stream = Stream(False)
        with self.assertRaises(SystemExit) as raised:
            self.fetch(server, opener=interrupted_after(2), stream=stream)
        self.assertEqual(raised.exception.code, 130)
        self.assertEqual(self.part.read_bytes(), DATA[:2 * MIB])
        self.assertFalse(self.target.exists())
        self.assertIn("kept in", stream.getvalue())
        output = self.fetch(server)
        self.assertEqual(server.requests, [None, f"bytes={2 * MIB}-"])  # only the missing end was requested
        self.assertEqual(self.target.read_bytes(), DATA)
        self.assertFalse(self.part.exists())
        self.assertIn("resuming at 2.0 MiB", output)

    def test_a_server_that_ignores_range_restarts_the_download(self):
        server = self.serve(ranges=False)
        self.part.write_bytes(DATA[:MIB])
        self.fetch(server)
        self.assertEqual(self.target.read_bytes(), DATA)

    def test_a_dropped_connection_resumes_by_itself(self):
        server = self.serve(cut=MIB + 5)
        output = self.fetch(server)
        self.assertEqual(self.target.read_bytes(), DATA)
        self.assertEqual(server.requests, [None, f"bytes={MIB + 5}-"])
        self.assertIn("resuming", output)

    def test_a_part_that_already_holds_every_byte_is_finished_without_a_transfer(self):
        server = self.serve()
        self.part.write_bytes(DATA)
        self.fetch(server)
        self.assertEqual(self.target.read_bytes(), DATA)
        self.assertEqual(len(server.requests), 1)  # the 416 answer only

    def test_a_corrupt_part_is_dropped_and_downloaded_again_once(self):
        server = self.serve()
        self.part.write_bytes(b"x" * MIB)
        output = self.fetch(server)
        self.assertEqual(self.target.read_bytes(), DATA)
        self.assertEqual(server.requests, [f"bytes={MIB}-", None])
        self.assertIn("downloading it again", output)

    def test_a_download_that_fails_its_check_is_removed_and_never_installed(self):
        server = self.serve(data=DATA[:-1] + b"X")
        with self.assertRaisesRegex(ValueError, "image SHA-256 mismatch"):
            self.fetch(server)
        self.assertEqual(list(self.work.iterdir()), [])

    def test_the_size_limit_stops_the_download_and_keeps_nothing(self):
        with self.assertRaisesRegex(ValueError, "exceeds the download bound"):
            self.fetch(self.serve(), limit=MIB)
        self.assertEqual(list(self.work.iterdir()), [])

    def test_a_missing_file_is_an_error_not_a_retry(self):
        server = self.serve()
        server.url += "-missing"
        server.httpd.RequestHandlerClass.do_GET = lambda handler: handler.send_error(404)
        with self.assertRaises(urllib.error.HTTPError):
            self.fetch(server)
        self.assertEqual(list(self.work.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
