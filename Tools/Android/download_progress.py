"""The pinned downloads of the Android setup: resumable, with progress (what is downloading, how much, how fast).

`fetch` keeps a partial download in `<name>.part` beside its destination, so Ctrl-C, a lost connection or a
crashed run never throws away what arrived: the next run asks the server for the rest (HTTP Range), or starts
again when the server cannot resume. The finished file is checked before it replaces the destination.

On a terminal one line updates in place:

    Android 15 system image  [=========>          ]  47%  345.1/734.0 MiB  18.4 MiB/s  ETA 0:21

When output is piped (a log, CI) it prints a line every 10 percent instead, and nothing for a download that
finishes in under two seconds. A finished download that took a second or more, or is at least 1 MiB, always
leaves a one-line summary. Everything goes to stderr.
"""
import hashlib
import http.client
from pathlib import Path
import re
import sys
import time
import urllib.error
import urllib.request

BAR = 20
RETRIES = 3  # attempts in a row that bring no new bytes before a failure is reported
RETRY_CODES = (429, 500, 502, 503, 504)


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

    def __init__(self, label, total=None, stream=None, clock=time.monotonic, resumed=0):
        self.label, self.total = label, total or None
        self.stream = stream if stream is not None else sys.stderr
        self.tty = bool(getattr(self.stream, "isatty", lambda: False)())
        self.clock = clock
        self.start = self.drawn = clock()
        self.resumed = self.done = resumed  # bytes already on disk count towards the total, not the speed
        self.next_percent = resumed * 100 // self.total // 10 * 10 + 10 if self.total and resumed else 10
        self.inline = False

    def speed(self, now):
        elapsed = now - self.start
        return (self.done - self.resumed) / elapsed if elapsed > 0 else 0.0

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

    def clear(self):
        """Wipe the in-place line, before something else is printed."""
        if self.inline:
            self.stream.write("\r\x1b[K")
            self.stream.flush()

    def finish(self):
        now = self.clock()
        elapsed = now - self.start
        self.clear()
        if self.inline or elapsed >= 1 or self.done >= 1 << 20:
            rate = f" ({format_size(int((self.done - self.resumed) / elapsed))}/s)" if elapsed >= 1 else ""
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


def sha256_check(expected, what):
    """A `fetch` check: the file's SHA-256 must be `expected`, else ValueError naming `what`."""
    def check(path):
        digest = hashlib.sha256()
        with open(path, "rb") as stream:
            for block in iter(lambda: stream.read(1 << 20), b""):
                digest.update(block)
        if digest.hexdigest() != expected:
            raise ValueError(f"{what} SHA-256 mismatch")
    return check


def _size(path):
    try:
        return path.stat().st_size
    except FileNotFoundError:
        return 0


def _content_range(value):
    """(first byte, total or None) of a `Content-Range: bytes a-b/total` or `bytes */total` header; None if absent."""
    match = re.fullmatch(r"\s*bytes\s+(?:(\d+)-\d+|\*)/(\d+|\*)\s*", value or "")
    if not match:
        return None
    return (int(match.group(1)) if match.group(1) else None), (int(match.group(2)) if match.group(2) != "*" else None)


class _Restart(Exception):
    """The partial file is unusable and has been removed; start again from the first byte."""


def _transfer(url, part, label, limit, opener, timeout, stream, clock):
    """Append what the server sends to `part` (from its current size on); returns once the body is complete.

    The result is how many bytes of an earlier run were kept (0 when the file was started again)."""
    offset = _size(part)
    headers = {"Range": f"bytes={offset}-"} if offset else {}
    try:
        response = opener(urllib.request.Request(url, headers=headers), timeout=timeout)
    except urllib.error.HTTPError as error:
        total = _content_range(error.headers.get("Content-Range")) if error.code == 416 and offset else None
        error.close()
        if total is None:
            raise
        if total[1] == offset:
            return offset  # an earlier run already received every byte
        part.unlink()  # longer or shorter than the file now on the server
        raise _Restart() from None
    with response:
        status, length = getattr(response, "status", 200), content_length(response)
        position = _content_range(response.headers.get("Content-Range")) if status == 206 else None
        if offset and not (position and position[0] == offset):
            if status == 206:
                part.unlink()  # some other range than the one asked for
                raise _Restart()
            offset = 0  # the server cannot resume and sent the whole file again
        if position:
            total = position[1] or (position[0] + length if length else None)
        else:
            total = length
        if total and total > limit:
            part.unlink(missing_ok=True)
            raise ValueError(f"{label} exceeds the download bound")
        progress = Progress(label, total, stream=stream, clock=clock, resumed=offset)
        if offset:
            stream.write(f"{label}: resuming at {format_size(offset)}\n")
        with open(part, "ab" if offset else "wb") as output:
            try:
                while True:
                    block = response.read(1 << 20)
                    if not block:
                        break
                    output.write(block)
                    progress.advance(len(block))
                    if progress.done > limit:
                        raise ValueError(f"{label} exceeds the download bound")
            except ValueError:
                output.close()
                part.unlink(missing_ok=True)
                raise
            except BaseException:
                progress.clear()
                raise
        if total and progress.done < total:
            progress.clear()
            raise ConnectionError(f"{label}: the connection closed after {format_size(progress.done)} "
                                  f"of {format_size(total)}")
        progress.finish()
        return offset


def fetch(url, destination, label, limit, check=None, opener=None, timeout=60, stream=None, clock=time.monotonic,
          pause=1.0):
    """Download `url` to `destination`, resuming from `<destination>.part` when an earlier run left one.

    The partial file survives Ctrl-C (which exits with status 130 after saying so), errors and a dropped
    connection (retried here, resuming, up to RETRIES times in a row without new bytes). Only a download that
    is complete and passes `check(path)` (raise to reject) replaces `destination`; a rejected file is removed,
    and when it was built on resumed bytes the download is tried once more from the start. `limit` bounds the
    file size. Returns `destination`."""
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    part = destination.with_name(destination.name + ".part")
    opener = opener or urllib.request.urlopen
    stream = stream if stream is not None else sys.stderr
    reused, restarted, failures = False, False, RETRIES
    while True:
        before = _size(part)
        try:
            reused = _transfer(url, part, label, limit, opener, timeout, stream, clock) > 0 or reused
        except KeyboardInterrupt:
            stream.write(f"\n{label}: interrupted with {format_size(_size(part))} kept in {part}; "
                         "run the same command again to resume\n")
            stream.flush()
            raise SystemExit(130) from None
        except _Restart:
            reused = False
            continue
        except (OSError, http.client.HTTPException) as error:
            if isinstance(error, urllib.error.HTTPError) and error.code not in RETRY_CODES:
                raise
            failures = RETRIES if _size(part) > before else failures - 1
            if failures <= 0:
                stream.write(f"{label}: failed with {format_size(_size(part))} kept in {part}; "
                             "run the same command again to resume\n")
                raise
            stream.write(f"{label}: {error}; resuming\n")
            time.sleep(pause)
            continue
        if check:
            try:
                check(part)
            except Exception:
                part.unlink(missing_ok=True)
                if not reused or restarted:
                    raise
                restarted, reused = True, False
                stream.write(f"{label}: the resumed download failed its check; downloading it again\n")
                continue
        part.replace(destination)
        return destination
