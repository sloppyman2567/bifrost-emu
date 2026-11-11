"""SHA-256 of a file, remembered while the file is unchanged.

Every launch checks the installed package, the shared framework image and the app's runtime copy against the
digests recorded when they were built: about 1.8 GiB, over 2 s of hashing each time. A digest is reused when the
file's device, inode, size, modification time and change time are all what they were when it was hashed. Any
write, truncation, permission or timestamp change moves the change time, which no unprivileged process can set
back, and replacing a file gives it a new inode, so every ordinary modification is hashed again. What this cannot
see is a change that bypasses the kernel's write path (on-disk corruption, raw device writes): those are found by
the next full hash, after `forget()` or a cache deletion.

The table is a cache: deleting it costs one full hash per file. Any database problem falls back to hashing.
"""
import atexit
import hashlib
import os
from pathlib import Path
import sqlite3
import time

# A file changed this recently may change again within the same timestamp tick, so its stats would not show it.
SETTLE_NS = 2_000_000_000
PRUNE_AFTER_DAYS = 30


def cache_path():
    base = os.environ.get("XDG_CACHE_HOME") or str(Path.home() / ".cache")
    return Path(base) / "bifrost" / "file-digests.sqlite"


class _Cache:
    def __init__(self):
        self.connection = None
        self.pending = 0
        self.failed = False

    def open(self):
        if self.connection is not None or self.failed or os.environ.get("BIFROST_FILE_DIGEST_CACHE") == "0":
            return self.connection
        try:
            path = cache_path()
            path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            connection = sqlite3.connect(path, timeout=5, isolation_level=None)
            connection.execute("PRAGMA journal_mode=WAL")
            connection.execute("PRAGMA synchronous=OFF")
            connection.execute("CREATE TABLE IF NOT EXISTS digests (device INTEGER, inode INTEGER, size INTEGER, "
                               "mtime INTEGER, ctime INTEGER, sha256 TEXT, used INTEGER, "
                               "PRIMARY KEY (device, inode))")
            today = int(time.time() // 86400)
            connection.execute("DELETE FROM digests WHERE used < ?", (today - PRUNE_AFTER_DAYS,))
            connection.execute("BEGIN")
            self.connection, self.today = connection, today
            atexit.register(self.close)
        except (sqlite3.Error, OSError):
            self.failed = True
        return self.connection

    def lookup(self, info):
        connection = self.open()
        if connection is None: return None
        try:
            row = connection.execute("SELECT size, mtime, ctime, sha256, used FROM digests WHERE device=? AND inode=?",
                                     (info.st_dev, info.st_ino)).fetchone()
            if row is None or tuple(row[:3]) != (info.st_size, info.st_mtime_ns, info.st_ctime_ns): return None
            if row[4] != self.today:
                connection.execute("UPDATE digests SET used=? WHERE device=? AND inode=?",
                                   (self.today, info.st_dev, info.st_ino))
                self.wrote()
            return row[3]
        except sqlite3.Error:
            self.failed, self.connection = True, None
            return None

    def store(self, info, value):
        connection = self.open()
        if connection is None: return
        try:
            connection.execute("INSERT OR REPLACE INTO digests VALUES (?, ?, ?, ?, ?, ?, ?)",
                               (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns, value,
                                self.today))
            self.wrote()
        except sqlite3.Error:
            self.failed, self.connection = True, None

    def wrote(self):
        self.pending += 1
        if self.pending >= 512: self.commit()

    def commit(self):
        if self.connection is None or not self.pending: return
        try:
            self.connection.execute("COMMIT")
            self.connection.execute("BEGIN")
            self.pending = 0
        except sqlite3.Error:
            self.failed, self.connection = True, None

    def close(self):
        self.commit()
        if self.connection is not None:
            try: self.connection.close()
            except sqlite3.Error: pass
            self.connection = None


_cache = _Cache()


def digest(path):
    """The file's SHA-256 (hex), from the cache when its stats are unchanged since it was last hashed."""
    with open(path, "rb") as stream:
        before = os.fstat(stream.fileno())
        cached = _cache.lookup(before)
        if cached is not None:
            return cached
        value = hashlib.file_digest(stream, "sha256").hexdigest()
        after = os.fstat(stream.fileno())
    stats = lambda info: (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)
    settled = time.time_ns() - max(after.st_mtime_ns, after.st_ctime_ns) > SETTLE_NS
    if stats(before) == stats(after) and settled:
        _cache.store(after, value)
    return value


def flush():
    """Make what this process hashed visible to others now (it is also written at exit)."""
    _cache.commit()
