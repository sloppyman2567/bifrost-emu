"""Where Asgard keeps state and finds the emulator: a source checkout or an installed tree.

An installed tree (`Scripts/Setup/install-asgard.py`) holds the same layout as the
checkout under `<prefix>/share/bifrost/asgard`, marked by `.asgard-install`, and is
read-only for ordinary users, so per-user state lives under the XDG data directory."""
import os
import re
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


JDK_ROOTS = ("/usr/lib/jvm", "/usr/lib64/jvm", "/usr/java")


def javac():
    """JAVAC, javac on PATH, JAVA_HOME's, else the newest JDK in a distribution's JVM directory; None without a JDK.

    A distribution can install a JDK with only its default runtime on PATH (Arch's archlinux-java, Debian's
    alternatives after a JRE-only default)."""
    configured = os.environ.get("JAVAC")
    if configured:
        return configured
    found = shutil.which("javac")
    if found:
        return found
    home = os.environ.get("JAVA_HOME")
    if home and os.access(os.path.join(home, "bin/javac"), os.X_OK):
        return os.path.join(home, "bin/javac")
    def version(path):
        return [int(part) for part in re.findall(r"\d+", path.parent.parent.name)]
    candidates = [path for root in JDK_ROOTS for path in Path(root).glob("*/bin/javac") if os.access(path, os.X_OK)]
    return str(max(candidates, key=version)) if candidates else None


def emulator():
    """The bifrost-emu next to an installed tree (<prefix>/bin), else the checkout's."""
    if installed():
        beside = ROOT.parents[2] / "bin/bifrost-emu"
        found = beside if beside.is_file() else shutil.which("bifrost-emu")
        if found:
            return Path(found)
    return ROOT / "bifrost-emu"
