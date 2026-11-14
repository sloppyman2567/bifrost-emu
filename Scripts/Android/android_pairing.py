"""Whether an installed Asgard and the bifrost-emu that runs it were installed together.

Scripts/Setup/install-asgard.py records, in the install's marker, the SHA-256 of the emulator the install belongs with:
the build `make install` copies (the Makefile exports it as BIFROST_BUILT_EMULATOR), or the package's /usr/bin/bifrost-emu.
`asgard` compares that with the emulator it was started with. Updating only one of the two leaves them different, and
what fails then (a flag the emulator does not know, a launcher the emulator does not expect) says nothing about the cause.

This only warns: a different build is not always an incompatible one, and a developer rebuilds the emulator constantly.
An install that recorded no emulator (it was made without one built, or by a version before this check), a checkout and
an emulator that cannot be read are not judged. BIFROST_NO_PAIRING_WARNING=1 silences the warning.
"""
import hashlib
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
MARKER = ".asgard-install"
UPDATE = ("Update both from the same checkout: make && sudo make install && sudo make install-asgard "
          "(for the package: apt upgrade, then asgard-setup).")


def digest(path):
    """The SHA-256 (hex) of the file at `path`, or None when it cannot be read."""
    try:
        with open(path, "rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest()
    except OSError:
        return None


def recorded(root=ROOT):
    """The digest the install at `root` recorded for its emulator, or None (a checkout, or an install without one)."""
    try:
        marker = json.loads((Path(root) / MARKER).read_text())
        value = marker["emulator"]["sha256"]
        return value if isinstance(value, str) and value else None
    except (OSError, ValueError, KeyError, TypeError):
        return None


def mismatch(emulator, root=ROOT):
    """The warning to print when this install was made for a different emulator than `emulator`, else None."""
    expected = recorded(root)
    if expected is None:
        return None
    actual = digest(emulator)
    if actual is None or actual == expected:
        return None
    return (f"warning: this Asgard was installed for a different bifrost-emu build than {emulator} "
            f"(installed for {expected[:12]}, running {actual[:12]}). {UPDATE} "
            "BIFROST_NO_PAIRING_WARNING=1 silences this.")


def warn(emulator, root=ROOT, out=None):
    """Print the warning, once, to `out` (stderr). Returns whether it was printed."""
    if os.environ.get("BIFROST_NO_PAIRING_WARNING"):
        return False
    message = mismatch(emulator, root)
    if message is None:
        return False
    print(message, file=out or sys.stderr)
    return True
