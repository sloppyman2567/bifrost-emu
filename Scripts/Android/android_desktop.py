"""Menu entries for installed Android apps: each one starts from the desktop's own launcher, with its icon.

An entry is `asgard-PACKAGE.desktop` in $XDG_DATA_HOME/applications (default ~/.local/share/applications). Its Exec line starts
the app through the Asgard that wrote it. The app's window gets the same name as its application id (SDL_APP_ID), so a taskbar
that matches windows to entries by application id shows the app's icon on its button.

Entries are made only for the default state root: a scratch state root, such as a test's, must not fill the user's menu.
`ASGARD_DESKTOP_ENTRIES=0` turns them off; `asgard desktop` writes them for every installed app on request and
`asgard desktop --remove` removes all of them.
"""
import os
from pathlib import Path
import re
import sys

import android_paths

PREFIX = "asgard-"
PACKAGE = re.compile(r"[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z0-9_]+)+")
LAUNCHABLE = ("managed_activity", "native_activity")
ICON_SIZE = 256


def desktop_id(package):
    return PREFIX + package


def applications_dir():
    xdg = os.environ.get("XDG_DATA_HOME", "")
    return (Path(xdg) if xdg.startswith("/") else Path.home() / ".local/share") / "applications"


def entry_path(package):
    return applications_dir() / (desktop_id(package) + ".desktop")


def enabled(state):
    """Entries follow the user's own state root; the opt-out is checked here, `asgard desktop` ignores it."""
    return os.environ.get("ASGARD_DESKTOP_ENTRIES") != "0" and Path(state).resolve() == Path(android_paths.state_root()).resolve()


def window_environment(package):
    """Environment that names the app's window after its entry (SDL3 reads SDL_APP_ID, for Wayland and X11)."""
    return {} if os.environ.get("SDL_APP_ID") else {"SDL_APP_ID": desktop_id(package)}


def escape_value(text):
    """A localestring value: no control characters, backslash escaped."""
    return "".join(" " if ord(c) < 32 or ord(c) == 127 else c for c in text).replace("\\", "\\\\")


def exec_argument(argument):
    """One argument of an Exec line, quoted as the Desktop Entry specification asks: reserved characters force double
    quotes, and the ones that stay special inside them are escaped (then the backslashes of that escaping, again)."""
    if argument and not re.search(r"[\s\"'\\><~|&;$*?#()`%]", argument):
        return argument
    quoted = re.sub(r'(["`$\\])', r"\\\1", argument).replace("%", "%%")
    return '"' + quoted.replace("\\", "\\\\") + '"'


def exec_line(package):
    asgard = Path(__file__).resolve().with_name("asgard.py")
    return " ".join(exec_argument(part) for part in (sys.executable, str(asgard), "start", package))


def compose_adaptive(layers, side):
    """Android's composition of an adaptive icon: both layers are 108 dp, of which the inner 72 dp show. A Qt image, null if a
    layer will not load. The library draws its cards with this, and the entries save it."""
    from PyQt6.QtCore import QRectF, Qt
    from PyQt6.QtGui import QColor, QImage, QPainter
    image = QImage(side, side, QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(Qt.GlobalColor.transparent)
    painter = QPainter(image)
    painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
    target = QRectF(0, 0, side, side)
    for name in ("background", "foreground"):
        value = layers[name]
        if value.startswith("#"):
            painter.fillRect(target, QColor(value))
            continue
        layer = QImage(value)
        if layer.isNull():
            painter.end()
            return QImage()
        painter.drawImage(target, layer, QRectF(layer.width() / 6, layer.height() / 6, layer.width() * 2 / 3, layer.height() * 2 / 3))
    painter.end()
    return image


def icon_file(app, state):
    """The path for the entry's Icon key: the app's own PNG, or a PNG made beside it (its composed adaptive icon, or its
    JPEG or WebP bitmap, which not every desktop can load as an icon), or None."""
    source = app.get("icon")
    if source and source.lower().endswith(".png"):
        return source
    layers = None if source else app.get("icon_layers")
    if not source and not layers:
        return None
    target = Path(state) / app["package"] / "desktop-icon.png"
    parts = [source] if source else [value for value in layers.values() if not value.startswith("#")]
    try:
        newest = max(Path(part).stat().st_mtime for part in parts)
    except (OSError, ValueError):
        newest = 0
    if target.is_file() and target.stat().st_mtime >= newest:
        return str(target)
    try:
        from PyQt6.QtGui import QImage
        image = QImage(source) if source else compose_adaptive(layers, ICON_SIZE)
    except ImportError:  # no PyQt6: the entry is made without an icon, and the library has none either
        return None
    if image.isNull():
        return None
    temporary = target.with_suffix(".png.tmp")
    if not image.save(str(temporary), "PNG"):
        temporary.unlink(missing_ok=True)
        return None
    temporary.replace(target)
    return str(target)


def entry_text(app, state, icon):
    lines = ["[Desktop Entry]", "Type=Application", "Version=1.5", "Name=" + escape_value(app["label"]),
             "Comment=Android app running on Bifrost", "Exec=" + exec_line(app["package"]), "Terminal=false",
             "StartupWMClass=" + desktop_id(app["package"])]
    if icon:
        lines.append("Icon=" + escape_value(icon))
    lines += ["X-Asgard-Package=" + app["package"], "X-Asgard-State=" + escape_value(str(Path(state).resolve()))]
    return "\n".join(lines) + "\n"


def launchable(app):
    return (not app.get("error") and not app.get("system") and app.get("launcher") in LAUNCHABLE
            and bool(PACKAGE.fullmatch(app["package"])))


def write_entry(app, state):
    path = entry_path(app["package"])
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(entry_text(app, state, icon_file(app, state)))
    temporary.replace(path)
    return path


def made_here(path, state):
    """Whether an entry file was written for this state root; one for another root, or by hand, is not ours to remove."""
    try:
        text = path.read_text()
    except (OSError, UnicodeDecodeError):
        return False
    return ("X-Asgard-State=" + escape_value(str(Path(state).resolve()))) in text.splitlines()


def sync(state, apps, only=None):
    """Make the entries match the installed apps (or just `only`): write those that can be launched and remove the ones
    this state root made for apps that are gone. Returns (written, removed) package lists."""
    present = {app["package"]: app for app in apps}
    written, removed = [], []
    for package, app in present.items():
        if only not in (None, package) or not launchable(app):
            continue
        write_entry(app, state)
        written.append(package)
    directory = applications_dir()
    for path in sorted(directory.glob(PREFIX + "*.desktop")) if directory.is_dir() else ():
        package = path.name[len(PREFIX):-len(".desktop")]
        if only not in (None, package) or (package in present and launchable(present[package])):
            continue
        if made_here(path, state):
            path.unlink(missing_ok=True)
            removed.append(package)
    return written, removed


def remove_all(state):
    directory = applications_dir()
    removed = []
    for path in sorted(directory.glob(PREFIX + "*.desktop")) if directory.is_dir() else ():
        if made_here(path, state):
            path.unlink(missing_ok=True)
            removed.append(path.name[len(PREFIX):-len(".desktop")])
    return removed
