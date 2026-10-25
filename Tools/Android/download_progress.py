"""Progress for the pinned downloads of the Android setup: what is downloading, how much, how fast.

On a terminal one line updates in place:

    Android 15 system image  [=========>          ]  47%  345.1/734.0 MiB  18.4 MiB/s  ETA 0:21

When output is piped (a log, CI) it prints a line every 10 percent instead, and nothing for a download that
finishes in under two seconds. A finished download that took a second or more, or is at least 1 MiB, always
leaves a one-line summary. Everything goes to stderr.
"""
import sys
import time

BAR = 20


def format_size(count):
    for unit, factor in (("GiB", 1 << 30), ("MiB", 1 << 20), ("KiB", 1 << 10)):
        if count >= factor:
            return f"{count / factor:.1f} {unit}"
    return f"{count} B"


def format_time(seconds):
    minutes, seconds = divmod(int(seconds + 0.5), 60)
    hours, minutes = divmod(minutes, 60)
    return f"{hours}:{minutes:02d}:{seconds:02d}" if hours else f"{minutes}:{seconds:02d}"


class Progress:
    """Counts bytes of one download. `total` is the expected size, or None when it is not known."""

    def __init__(self, label, total=None, stream=None, clock=time.monotonic):
        self.label, self.total = label, total or None
        self.stream = stream if stream is not None else sys.stderr
        self.tty = bool(getattr(self.stream, "isatty", lambda: False)())
        self.clock = clock
        self.start = self.drawn = clock()
        self.done = 0
        self.next_percent = 10
        self.inline = False

    def speed(self, now):
        elapsed = now - self.start
        return self.done / elapsed if elapsed > 0 else 0.0

    def line(self, now):
        speed = self.speed(now)
        rate = f"{format_size(int(speed))}/s"
        if not self.total:
            return f"{self.label}  {format_size(self.done)}  {rate}"
        fraction = min(1.0, self.done / self.total)
        eta = f"  ETA {format_time((self.total - self.done) / speed)}" if speed > 0 and now - self.start >= 1 else ""
        text = f"{format_size(self.done)}/{format_size(self.total)}  {rate}{eta}"
        if not self.tty:
            return f"{self.label}: {int(fraction * 100)}%  {text}"
        filled = int(fraction * BAR)
        bar = "=" * max(0, filled - 1) + (">" if 0 < filled < BAR else "=" if filled else "") + " " * (BAR - filled)
        return f"{self.label}  [{bar}] {int(fraction * 100):3d}%  {text}"

    def advance(self, count):
        self.done += count
        now = self.clock()
        if self.tty:
            if now - self.drawn >= 0.2:
                self.drawn = now
                self.stream.write("\r\x1b[K" + self.line(now))
                self.stream.flush()
                self.inline = True
        elif now - self.start >= 2:
            if self.total:
                if self.done * 100 >= self.total * self.next_percent:
                    while self.done * 100 >= self.total * self.next_percent:
                        self.next_percent += 10
                    self.stream.write(self.line(now) + "\n")
                    self.stream.flush()
            elif now - self.drawn >= 5:
                self.drawn = now
                self.stream.write(self.line(now) + "\n")
                self.stream.flush()

    def finish(self):
        now = self.clock()
        elapsed = now - self.start
        if self.inline:
            self.stream.write("\r\x1b[K")
        if self.inline or elapsed >= 1 or self.done >= 1 << 20:
            rate = f" ({format_size(int(self.done / elapsed))}/s)" if elapsed >= 1 else ""
            self.stream.write(f"{self.label}: {format_size(self.done)} in {format_time(elapsed)}{rate}\n")
        self.stream.flush()


def content_length(response):
    try:
        value = int(response.headers.get("Content-Length", ""))
    except (AttributeError, TypeError, ValueError):
        return None
    return value if value > 0 else None


def read(response, limit, label, **options):
    """Up to limit + 1 bytes of `response` (so a caller can tell it was too long), with progress."""
    total = content_length(response)
    progress = Progress(label, min(total, limit + 1) if total else None, **options)
    chunks, count = [], 0
    while count <= limit:
        block = response.read(min(1 << 20, limit + 1 - count))
        if not block:
            break
        chunks.append(block)
        count += len(block)
        progress.advance(len(block))
    progress.finish()
    return b"".join(chunks)
