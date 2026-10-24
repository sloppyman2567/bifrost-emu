"""Where Asgard keeps state and finds the emulator: a source checkout or an installed tree.

An installed tree (`Scripts/Setup/install-asgard.py`) holds the same layout as the
checkout under `<prefix>/share/bifrost/asgard`, marked by `.asgard-install`, and is
read-only for ordinary users, so per-user state lives under the XDG data directory."""
import os
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MARKER = ".asgard-install"


def installed():
    return (ROOT / MARKER).is_file()


def data_home():
    xdg = os.environ.get("XDG_DATA_HOME", "")
    return (Path(xdg) if xdg.startswith("/") else Path.home() / ".local/share") / "bifrost"


def state_root():
    """BIFROST_ANDROID_STATE, else ~/.local/share/bifrost/android (installed) or ROOT/android-state."""
    configured = os.environ.get("BIFROST_ANDROID_STATE")
    if configured:
        return Path(configured)
    return data_home() / "android" if installed() else ROOT / "android-state"


def emulator():
    """The bifrost-emu next to an installed tree (<prefix>/bin), else the checkout's."""
    if installed():
        beside = ROOT.parents[2] / "bin/bifrost-emu"
        found = beside if beside.is_file() else shutil.which("bifrost-emu")
        if found:
            return Path(found)
    return ROOT / "bifrost-emu"
