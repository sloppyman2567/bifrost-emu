#!/usr/bin/env python3
"""download_progress.py: the progress lines of the Android setup's downloads (terminal and piped), and the helpers."""
import io
from pathlib import Path
import sys
import unittest

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


if __name__ == "__main__":
    unittest.main()
