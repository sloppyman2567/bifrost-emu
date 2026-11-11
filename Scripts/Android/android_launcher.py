"""Desktop app library using the same CLI and process supervisor as terminal launches."""
import html
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time

import android_gamepad
from android_package import PACKAGE, ROOT
from android_settings import (BACK_KEYS, ENTER_KEYS, JIT_DISK_CACHE_SIZES, JIT_DISK_CACHES, NETWORKS, STATS_MODES, WINDOW_MODES,
                              CONTROLLERS,
                              load as load_settings, save as save_settings)
from android_sessions import session_log

ACTIVE = ("preparing", "starting", "running", "stopping")
LAUNCHABLE = ("managed_activity", "native_activity")


def launcher_socket(state):
    identity = hashlib.sha256(os.fsencode(Path(state).resolve())).hexdigest()[:24]
    return Path(tempfile.gettempdir()) / f"asgard-launcher-{os.getuid()}-{identity}.sock"


def activate_existing(state):
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(1)
            client.connect(str(launcher_socket(state)))
            _, uid, _ = struct.unpack('3i', client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
            if uid != os.getuid(): raise PermissionError('Foreign Asgard launcher')
            client.sendall(b'activate\n')
            return client.makefile('rb').readline(16) == b'ok\n'
    except (FileNotFoundError, ConnectionRefusedError):
        return False


def activate_launcher(state):
    if activate_existing(state): return
    process = subprocess.Popen([sys.executable, str(ROOT / 'Scripts/Android/asgard.py'),
                                '--state-root', str(Path(state).resolve()), 'launcher'],
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, start_new_session=True)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            if activate_existing(state): return
        except socket.timeout:
            pass
        if process.poll() is not None:
            raise RuntimeError('Asgard launcher could not open')
        time.sleep(.05)
    raise RuntimeError('Asgard launcher did not become ready')
STATES = {"preparing": "Starting", "starting": "Starting", "running": "Running", "stopping": "Stopping",
          "stopped": "Stopped", "exited": "Exited normally", "closed": "Closed normally",
          "failed": "Failed", "timed_out": "Timed out", "interrupted": "Ended unexpectedly",
          "invalid": "Install is damaged"}
# First match wins: a Chartboost line about missing Play services counts once.
SIGNATURES = (
    # The host window did not open (WindowBackend.cpp's fail() and Surface.cpp's open_host_window).
    ("window", "Host window could not be opened", re.compile(
        r"\[android\] (?:SDL3 (?:video initialization|window creation|window properties) failed|window backend \S+ open failed)")),
    ("crash", "Emulator caught a guest crash", re.compile(r"^\[crash\] ")),
    ("signal", "Native crash (Fatal signal)", re.compile(r"Fatal signal \d+")),
    ("java", "Java crash (FATAL EXCEPTION)", re.compile(r"FATAL EXCEPTION")),
    ("anr", "App not responding (ANR)", re.compile(r"\bANR in ")),
    ("play", "Google Play services unavailable", re.compile(r"GooglePlayServicesNotAvailableException")),
    # Play's app protection (pairip) asks the Play Store app to confirm the install, then quits when it cannot.
    ("license", "Google Play licence check failed", re.compile(r"\sE LicenseClient\s*: Error while checking license")),
    ("ads", "Ad SDK errors", re.compile(
        r"\sE (AppLovinSdk|LevelPlaySDK|IronSource\w*|UnityAds|\[ChartboostMonetization\]|Vungle\w*"
        r"|Ads|AdMob\w*|Mintegral\w*|InMobi\w*|Pangle\w*|Fyber\w*|Liftoff\w*)\s*:")),
)
LOGCAT = re.compile(r"^\d\d-\d\d \d\d:\d\d:\d\d\.\d+\s+\d+\s+\d+\s+[VDIWEF]\s+")


def card_state(app, pending=None):
    """Primary action, availability and badge for one library card."""
    state = app["session"].get("state", "stopped")
    if pending:
        return {"action": pending, "enabled": False, "badge": None,
                "text": "Starting…" if pending == "play" else "Stopping…"}
    if state in ("preparing", "starting"):
        return {"action": "stop", "enabled": True, "badge": "Starting", "text": "Stop"}
    if state == "running":
        if app["session"].get("background"):
            return {"action": "resume", "enabled": True, "badge": "Running", "text": "Return to app"}
        return {"action": "stop", "enabled": True, "badge": "Running", "text": "Stop"}
    if state == "stopping":
        return {"action": "stop", "enabled": False, "badge": "Running", "text": "Stopping…"}
    if app.get("error") or state == "invalid":
        return {"action": "play", "enabled": False, "badge": None, "text": "Play",
                "reason": "This install is damaged. Install the APK again."}
    if app.get("launcher") not in LAUNCHABLE:
        return {"action": "play", "enabled": False, "badge": None, "text": "Play",
                "reason": "This APK has no launchable Activity."}
    return {"action": "play", "enabled": True, "badge": None, "text": "Play"}


def launch_request(app, trace=False):
    """CLI arguments for one Play click; the saved per-app settings are applied by the command."""
    args = ["start", app["package"]]
    # Call tracing follows ART methods, so NativeActivity apps launch without it.
    if trace and app.get("launcher") == "managed_activity": args += ["--trace-apk", "calls"]
    return args


INSTALLABLE = (".apk", ".apkm", ".apks", ".xapk")
SORTS = (("name", "Name"), ("played", "Recently played"))
FAILED = ("failed", "timed_out", "interrupted")
MIN_CARD, MAX_CARD, GAP = 208, 280, 18
GUTTER = 10  # between the cards and the scrollbar
# The gamepad layout: tiles and type for arm's length, no controls that need a mouse or a keyboard.
BIG_ICON, BIG_MIN_CARD, BIG_MAX_CARD, BIG_FONT = 150, 300, 380, 1.3


def installable(paths):
    """The existing package files among chosen or dropped paths."""
    return [str(path) for path in paths if Path(path).suffix.lower() in INSTALLABLE and Path(path).is_file()]


def sort_apps(apps, mode):
    name = lambda app: (app["label"].casefold(), app["package"])
    if mode == "played":
        return sorted(apps, key=lambda app: (-(app["session"].get("started_at") or 0), *name(app)))
    return sorted(apps, key=name)


def grid_metrics(width, minimum=MIN_CARD, maximum=MAX_CARD):
    """Columns and card width that fill `width`: cards stretch between `minimum` and `maximum`."""
    columns = max(1, (width + GAP) // (minimum + GAP))
    return columns, min(maximum, (width - (columns - 1) * GAP) // columns)


def poll_wait(minimized, active, busy):
    """Seconds between library refreshes; None while the window is minimized."""
    if minimized: return None
    return 1.5 if active or busy else 10.0


def activity_line(app, now=None):
    """Second line of a card: the latest session in a few words, and whether it needs attention."""
    now = time.time() if now is None else now
    session = app["session"]; state = session.get("state", "stopped"); started = session.get("started_at")
    if state == "running" and started: return "Running for " + span(now - started), ""
    if state in ACTIVE: return STATES.get(state, state), ""
    if state in FAILED + ("exited",) and (ending := session_ending(session)): return ending[0], "problem"
    if started: return "Played " + clock(started, now), ""
    return "Not played yet", ""


TILE_COLORS = ("#d4574e", "#d9822b", "#b8962e", "#3f9e57", "#2f9d95", "#3a85c8", "#6b63d1", "#a95bc4", "#cc5a8d", "#6d7b8a")


def tile_color(label):
    """A stable color per app, for the tile of an app without an icon."""
    return TILE_COLORS[int(hashlib.sha256(label.encode()).hexdigest()[:4], 16) % len(TILE_COLORS)]


def opaque_bounds(image):
    """(x, y, width, height) of an icon's visible pixels, or None without an alpha channel or pixels."""
    from PyQt6.QtGui import QImage
    if not image.hasAlphaChannel(): return None
    alpha = image.convertToFormat(QImage.Format.Format_Alpha8)
    width, height, stride = alpha.width(), alpha.height(), alpha.bytesPerLine()
    data = bytes(alpha.constBits().asarray(stride * height))
    table = bytes(0 if value <= 16 else 255 for value in range(256))
    top = bottom = None; left, right = width, 0
    for y in range(height):
        row = data[y * stride:y * stride + width].translate(table)
        trimmed = row.lstrip(b"\0")
        if not trimmed: continue
        top = y if top is None else top; bottom = y
        left = min(left, width - len(trimmed)); right = max(right, len(row.rstrip(b"\0")))
    return None if top is None else (left, top, right - left, bottom - top + 1)


def short_error(output, fallback="The command failed"):
    lines = [x.strip() for x in output.splitlines() if x.strip()]
    if not lines: return fallback
    text = re.sub(r"^(?:Android package|Asgard): ", "", lines[-1])
    return text if len(text) <= 300 else text[:299] + "…"


# Desktops that keep the clock format apart from the locale: (schema, key, value meaning 12-hour).
DESKTOP_CLOCKS = {"gnome": ("org.gnome.desktop.interface", "clock-format", "'12h'"),
                  "unity": ("org.gnome.desktop.interface", "clock-format", "'12h'"),
                  "budgie": ("org.gnome.desktop.interface", "clock-format", "'12h'"),
                  "cinnamon": ("org.cinnamon.desktop.interface", "clock-use-24h", "false")}


def desktop_clock():
    """True or False from the desktop's own clock setting, None where it has none or will not say."""
    names = os.environ.get("XDG_CURRENT_DESKTOP", "").lower().split(":")
    for name in names:
        name = name.removeprefix("x-")
        if name in DESKTOP_CLOCKS:
            schema, key, twelve = DESKTOP_CLOCKS[name]
            try:
                result = subprocess.run(["gsettings", "get", schema, key], capture_output=True, text=True, timeout=2)
            except (OSError, subprocess.SubprocessError):
                return None
            return result.stdout.strip() == twelve if result.returncode == 0 and result.stdout.strip() else None
    return None


def locale_clock():
    """(12-hour?, AM text, PM text) for the user's time locale. Qt's own locale data comes first: nl_langinfo needs the
    locale generated on glibc, and musl reports 24-hour for every locale."""
    try:
        from PyQt6.QtCore import QLocale
        system = QLocale.system()
        # Quoted text is literal: "H:mm 'Uhr'" is not a day period.
        pattern = re.sub(r"'[^']*'", "", system.timeFormat(QLocale.FormatType.ShortFormat))
        return "a" in pattern.lower(), system.amText(), system.pmText()
    except ImportError:
        pass
    import locale
    saved = locale.setlocale(locale.LC_TIME)
    try:
        locale.setlocale(locale.LC_TIME, "")
        return (bool(re.search(r"%[Irl]", locale.nl_langinfo(locale.T_FMT))),
                locale.nl_langinfo(locale.AM_STR), locale.nl_langinfo(locale.PM_STR))
    except (locale.Error, AttributeError):
        return False, "", ""
    finally:
        locale.setlocale(locale.LC_TIME, saved)


CLOCK_STYLE = None


def clock_style():
    global CLOCK_STYLE
    if CLOCK_STYLE is None:
        twelve, am, pm = locale_clock()
        chosen = desktop_clock()
        CLOCK_STYLE = (twelve if chosen is None else chosen, am or "AM", pm or "PM")
    return CLOCK_STYLE


def time_of_day(moment):
    """The time as the user reads it: 4:26 PM or 16:26."""
    twelve, am, pm = clock_style()
    if not twelve:
        return time.strftime("%H:%M", moment)
    return f"{moment.tm_hour % 12 or 12}:{moment.tm_min:02d} {am if moment.tm_hour < 12 else pm}"


def clock(stamp, now):
    moment, today = time.localtime(stamp), time.localtime(now)
    day = (time.strftime("Today", moment) if moment[:3] == today[:3] else
           "Yesterday" if time.localtime(now - 86400)[:3] == moment[:3] else
           time.strftime("%b %d" if moment.tm_year == today.tm_year else "%b %d, %Y", moment))
    return day + " " + time_of_day(moment)


def span(seconds):
    seconds = max(0, int(seconds))
    if seconds < 60: return f"{seconds} s"
    if seconds < 3600: return f"{seconds // 60} min"
    return f"{seconds // 3600} h {seconds % 3600 // 60} min"


def describe_session(session, now=None):
    """Plain-language outcome, start time and duration of the latest session."""
    now = time.time() if now is None else now
    state = session.get("state", "stopped")
    outcome = STATES.get(state, state.replace("_", " ").capitalize())
    ending = session_ending(session) if state in FAILED + ("exited",) else None
    advice = ending[1] if ending else None
    if ending: outcome = ending[0]
    elif state == "failed": outcome = "The app closed itself"
    elif state == "timed_out": advice = "A time limit set for this session ended it. That isn't a problem with the app."
    started, ended = session.get("started_at"), session.get("ended_at")
    when = clock(started, now) if started else None
    duration = span((ended or now) - started) if started and (ended or state in ACTIVE) else None
    return {"outcome": outcome, "when": when, "duration": duration, "error": session.get("error"), "advice": advice}


def drawer_stamp(session, log_info, developer=False, now=None):
    # Elapsed time must advance even when preparation hasn't produced new output.
    clock = int((time.time() if now is None else now) // 5) if session.get("state") in ACTIVE else None
    return (session.get("session_id"), session.get("state"), session.get("phase"), session_log(session),
            log_info and log_info.st_size, clock, developer)


def log_findings(lines):
    """Count known problem signatures; keep the first example of each."""
    found = {}
    for line in lines:
        for key, title, pattern in SIGNATURES:
            match = pattern.search(line)
            if not match: continue
            entry = found.setdefault(key, {"key": key, "title": title, "count": 0, "example": None, "sources": []})
            entry["count"] += 1
            if entry["example"] is None:
                text = LOGCAT.sub("", line.strip())
                entry["example"] = text if len(text) <= 160 else text[:159] + "…"
            if key == "ads" and match[1] not in entry["sources"]: entry["sources"].append(match[1])
            break
    return [found[key] for key, _, _ in SIGNATURES if key in found]


def read_findings(path, limit=16 * 1024 * 1024):
    with Path(path).open("rb") as stream:
        size = stream.seek(0, 2)
        stream.seek(max(0, size - limit))
        if size > limit: stream.readline()
        return log_findings(stream.read().decode(errors="replace").splitlines())


# Which findings can explain a run ending, in order, with what the card says and what to try. Ad and Play services
# errors are not here: games usually keep running through them.
CAUSES = (
    ("window", "Couldn't open a window", None),     # its advice depends on the log line: window_advice
    ("crash", "Crashed in native code", "The app's native code hit a fault. Try again; if it keeps happening, the log shows where."),
    ("signal", "Crashed in native code", "The app's native code hit a fault. Try again; if it keeps happening, the log shows where."),
    ("java", "Crashed in Java code", "The app hit an unhandled error. Try again; if it keeps happening, the log shows where."),
    ("anr", "Stopped responding", "The app stopped responding and was ended. Try again; slow first launches can do this."),
    ("license", "Play licence check failed",
     "This app asks the Google Play Store to confirm it was installed from Play. Bifrost has no Play Store, so the check "
     "fails and the app quits. Bifrost does not fake the answer, so this app cannot run here."),
)
def window_advice(example):
    """What to try when the host window did not open, from the first log line that said so."""
    example = example or ""
    if "(-38)" in example:       # -ENOSYS: no SDL3 in this build, or its library could not be loaded
        return ("Bifrost found no window library (SDL3). Install your distribution's SDL3 package (libsdl3), or in a source "
                "checkout run: python3 Tools/Fetch/fetch-sdl3.py")
    if re.search(r"video device|display|wayland|x11|xcb|DISPLAY", example, re.I):
        return ("There was no desktop to open a window on. Start the library from a graphical session, not over SSH or on a "
                "text console.")
    return "The desktop would not give Bifrost a window. The log has the reason."


# Android apps often quit this way (Process.killProcess on themselves), and so does the crash handler, so causes come first.
SELF_KILL = re.compile(r"\s(\d+)\s+\d+\s+I Process\s*: Sending signal\. PID: \1 SIG: 9")


def explain_ending(session, findings=(), self_killed=False):
    """Why a run ended, for a person: (card line, what to try), or None when nothing went wrong."""
    state, code = session.get("state"), session.get("exit_code")
    if state == "timed_out": return None
    found = {item.get("key") for item in findings}
    if state == "exited":  # exit code 0: quitting is normal, so only a known deliberate quit gets a line
        return next(((line, advice) for key, line, advice in CAUSES if key == "license" and key in found), None)
    if state == "interrupted":
        return "Ended unexpectedly", "Asgard lost track of this session, for example after a restart or if it was killed. Try again."
    for key, line, advice in CAUSES:
        if key not in found: continue
        if advice is None: advice = window_advice(next((item.get("example") for item in findings if item.get("key") == key), None))
        return line, advice
    if session.get("error"): return "Couldn't start", "The app did not get as far as opening. Try again; the log shows why."
    if self_killed: return None
    if code == 137: return "Ended from outside", "Something outside the app ended it, such as low memory or another program. Try again."
    return "Last run failed", "The log shows no known cause. Try again, or open the log for details."


def quit_reason(log_path):
    """Why an app that exited with status 0 quit by itself, as one sentence, or None when it was an ordinary exit."""
    try: ending = explain_ending({"state": "exited", "exit_code": 0}, read_findings(log_path))
    except OSError: return None
    return f"{ending[0]}. {ending[1]}" if ending else None


ENDINGS = {}  # (session, state, log size) -> explain_ending's result: a log is scanned once per ended run.


def session_ending(session):
    log = session_log(session)
    try: size = os.stat(log).st_size if log else None
    except OSError: size = None
    key = (session.get("session_id"), session.get("state"), size)
    if key not in ENDINGS:
        findings, killed = [], False
        if size is not None:
            try:
                findings = read_findings(log)
                with open(log, "rb") as stream:
                    stream.seek(max(0, size - 4096))
                    killed = bool(SELF_KILL.search(stream.read().decode(errors="replace")))
            except OSError: pass
        ENDINGS[key] = explain_ending(session, findings, killed)
    return ENDINGS[key]


_PAD_ROW = []


def pad_row_class():
    """The large row of the gamepad pages and sheets (made once; Qt is imported only when it is first needed)."""
    if _PAD_ROW: return _PAD_ROW[0]
    from PyQt6.QtCore import Qt
    from PyQt6.QtWidgets import QFrame, QHBoxLayout, QLabel

    class PadRow(QFrame):
        """One row: its name and value. Left and Right change the value, A (Return or Space) toggles or steps on, Up and
        Down move to the neighbouring row."""
        def __init__(self, title, value, step, enabled=None, tone="", changed=None):
            super().__init__()
            self.setObjectName("padrow"); self.setFocusPolicy(Qt.FocusPolicy.StrongFocus); self.setProperty("tone", tone)
            self.value, self.step, self.enabled, self.changed = value, step, enabled, changed
            row = QHBoxLayout(self); row.setContentsMargins(22, 14, 22, 14)
            self.name = QLabel(title); self.shown = QLabel(); self.shown.setObjectName("padvalue")
            self.shown.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
            row.addWidget(self.name, 1); row.addWidget(self.shown)
            self.refresh()

        def refresh(self):
            if self.enabled: self.setEnabled(self.enabled())
            text = self.value() if self.value else ""
            self.shown.setText(text); self.shown.setProperty("on", text == "On")
            self.shown.style().unpolish(self.shown); self.shown.style().polish(self.shown)

        def keyPressEvent(self, event):
            key = event.key()
            if key == Qt.Key.Key_Left and self.step: self.step(-1)
            elif key == Qt.Key.Key_Right and self.step: self.step(1)
            elif key in (Qt.Key.Key_Return, Qt.Key.Key_Enter, Qt.Key.Key_Space) and self.step: self.step(1)
            elif key == Qt.Key.Key_Down: self.focusNextChild(); return
            elif key == Qt.Key.Key_Up: self.focusPreviousChild(); return
            else: super().keyPressEvent(event); return
            if self.changed: self.changed()

    _PAD_ROW.append(PadRow)
    return PadRow


def settings_dialog(window, app, settings):
    """Edit one persistent app profile; changes are staged until Save."""
    from PyQt6.QtCore import Qt, QTimer, QUrl
    from PyQt6.QtGui import QDesktopServices, QIcon, QPalette
    from PyQt6.QtWidgets import (QAbstractItemView, QCheckBox, QComboBox, QDialog, QDialogButtonBox, QFrame,
                                 QFormLayout, QHeaderView, QHBoxLayout, QLabel, QPushButton, QScrollArea,
                                 QTableWidget, QTableWidgetItem, QTabWidget, QVBoxLayout, QWidget)
    import android_settings
    from android_package import API

    # The widgets show the app's built-in defaults under what is saved; only a change from what they showed is written, so a
    # default that later changes still reaches an app whose settings were never touched.
    saved = settings
    defaults = android_settings.app_defaults(app["package"])
    settings = {**defaults, **saved}
    PadRow = pad_row_class()

    class Settings(QDialog):
        def __init__(self):
            # In the gamepad layout the page is drawn inside the library window (a child widget over a dimmed backdrop), not
            # as a window of its own: the pad steers the main window's widgets, and a separate window takes focus differently.
            if getattr(window, "big", False): super().__init__(window.centralWidget(), Qt.WindowType.SubWindow)   # QDialog adds Qt.Dialog to plain flags
            else: super().__init__(window)
            self.backdrop = None
            self.setWindowTitle(app["label"] + " settings")
            self.resize(620, 500)
            self.settings, self.removals = dict(saved), []
            layout = QVBoxLayout(self); layout.setContentsMargins(16, 14, 16, 14); layout.setSpacing(12)
            dim = lambda label: (label.setForegroundRole(QPalette.ColorRole.PlaceholderText), label)[1]
            intro = dim(QLabel("Saved for this app and shared with the command line. Changes apply the next time it starts."))
            intro.setWordWrap(True); layout.addWidget(intro)
            tabs = QTabWidget(); layout.addWidget(tabs, 1)
            general = QWidget(); page = QVBoxLayout(general); page.setContentsMargins(14, 14, 14, 14); page.setSpacing(12)
            form = QFormLayout(); page.addLayout(form); form.setVerticalSpacing(10); form.setHorizontalSpacing(14)
            form.setLabelAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
            form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.FieldsStayAtSizeHint)
            self.stats = QCheckBox("Show FPS, load and temperatures while the app runs")
            self.stats.setChecked(settings.get("window_stats", False)); form.addRow("Window stats", self.stats)
            self.stats_mode = QComboBox()
            for name, text in STATS_MODES: self.stats_mode.addItem(text, name)
            self.stats_mode.setCurrentIndex(max(0, self.stats_mode.findData(settings.get("window_stats_mode", "normal"))))
            self.stats_mode.setEnabled(self.stats.isChecked()); self.stats.toggled.connect(self.stats_mode.setEnabled)
            form.addRow("Stats layout", self.stats_mode)
            self.back = QComboBox()
            for name, text in BACK_KEYS: self.back.addItem(text, name)
            self.back.setCurrentIndex(max(0, self.back.findData(settings.get("back_key", "escape"))))
            form.addRow("Back key", self.back)
            self.enter = QComboBox()
            for name, text in ENTER_KEYS: self.enter.addItem(text, name)
            self.enter.setCurrentIndex(max(0, self.enter.findData(settings.get("enter_key", "enter"))))
            form.addRow("Enter key", self.enter)
            self.disk_cache = QComboBox()
            for name, text in JIT_DISK_CACHES: self.disk_cache.addItem(text, name)
            self.disk_cache.setCurrentIndex(max(0, self.disk_cache.findData(settings.get("jit_disk_cache", "default"))))
            self.disk_cache.setToolTip("Keep translated code between launches so later starts compile less. "
                                       "Default follows [jit] disk_cache in bifrost.toml, which is off unless you set it")
            form.addRow("JIT disk cache", self.disk_cache)
            self.disk_cache_mb = QComboBox()
            for name, text in JIT_DISK_CACHE_SIZES: self.disk_cache_mb.addItem(text, name)
            self.disk_cache_mb.setCurrentIndex(max(0, self.disk_cache_mb.findData(settings.get("jit_disk_cache_mb", "default"))))
            self.disk_cache_mb.setToolTip("Most disk space this app's cache may use. "
                                          "Default follows [jit] disk_cache_mb in bifrost.toml, normally 512 MB")
            form.addRow("JIT disk cache size", self.disk_cache_mb)
            self.network = QComboBox()
            for name, text in zip(NETWORKS, ("Follow this computer", "Offline")): self.network.addItem(text, name)
            self.network.setCurrentIndex(max(0, self.network.findData(settings.get("network", "auto"))))
            form.addRow("Network", self.network)
            self.window_mode = QComboBox()
            for name, text in WINDOW_MODES: self.window_mode.addItem(text, name)
            self.window_mode.setCurrentIndex(max(0, self.window_mode.findData(settings.get("window_mode", "resizable"))))
            self.window_mode.setToolTip("Resizable: the window's size becomes the app's, for an app that adapts to it. Keep the "
                                        "app's shape: the app gets a fixed panel of the shape it opened at, scaled to the window, "
                                        "for a game that draws wrongly in a window of another shape")
            form.addRow("Window", self.window_mode)
            self.controller = QComboBox()
            for name, text in CONTROLLERS: self.controller.addItem(text, name)
            self.controller.setCurrentIndex(max(0, self.controller.findData(settings.get("controller", "auto"))))
            self.controller.setToolTip("For an app that only takes touch. Auto: the pad stays a pad, and Start and Select held "
                                       "together turn a touch cursor on or off (left stick moves, A touches, X is a second "
                                       "finger). Gamepad only: never a cursor, and the game gets Start and Select. Touch cursor "
                                       "always on: the cursor is there from the start")
            form.addRow("Controller", self.controller)
            import android_desktop
            self.menu_default = android_desktop.default_on(window.state)
            self.menu = QCheckBox("Show a launcher for this app in the desktop's application menu")
            self.menu.setChecked(settings.get("menu_entry", self.menu_default)); form.addRow("Application menu", self.menu)
            self.choices = {"window_stats_mode": self.stats_mode, "back_key": self.back, "enter_key": self.enter,
                            "jit_disk_cache": self.disk_cache, "jit_disk_cache_mb": self.disk_cache_mb, "network": self.network,
                            "window_mode": self.window_mode, "controller": self.controller}
            form.addRow("Runtime", dim(QLabel(f"Android API {API} · bundled runtime")))
            directory = window.state / app["package"]
            folder = QPushButton("Open app folder")
            folder.setToolTip("The app environment: its files, data and logs")
            folder.clicked.connect(lambda: QDesktopServices.openUrl(QUrl.fromLocalFile(str(directory.resolve()))))
            form.addRow("Files", folder)
            page.addStretch(1)
            removal = QHBoxLayout(); removal.setSpacing(8)
            running = app["session"].get("state") in ACTIVE
            for text, command, tip, name in (
                    ("Clear data…", "clear-data", "Delete this app's saves and settings; the app stays installed", ""),
                    ("Uninstall…", "uninstall", "Remove this app with all of its data", "danger")):
                button = QPushButton(text); button.setObjectName(name)
                button.setToolTip("Stop the app first" if running else tip); button.setEnabled(not running)
                button.clicked.connect(lambda _, command=command: self.reject() if window.remove_app(app, command) else None)
                removal.addWidget(button); self.removals.append(button)
            removal.addStretch(); page.addLayout(removal)
            tabs.addTab(general, "General")

            compatibility = QWidget(); compat = QVBoxLayout(compatibility); compat.setContentsMargins(14, 14, 14, 14); compat.setSpacing(10)
            policy = QFormLayout(); self.emulator = QComboBox()
            for text, value in (("Runtime default", None), ("Off", "0"), ("On", "1")):
                self.emulator.addItem(text, value)
            value = settings.get("property.ro.kernel.qemu")
            if value is not None and value not in ("0", "1"):
                self.emulator.addItem("Custom: " + (value or "(empty)"), value)
            self.emulator.setCurrentIndex(max(0, self.emulator.findData(value)))
            policy.addRow("Android emulator flag", self.emulator); compat.addLayout(policy)
            self.emulator.setToolTip("Android property: ro.kernel.qemu")
            note = dim(QLabel("Choose whether this app sees Android's emulator flag. Runtime default uses the bundled image's value."))
            note.setWordWrap(True); compat.addWidget(note)
            advanced = QLabel("Advanced device properties")
            compat.addWidget(advanced)
            self.properties = QTableWidget(0, 2)
            self.properties.setHorizontalHeaderLabels(("Property", "Value"))
            self.properties.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
            self.properties.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
            self.properties.setAccessibleName("Device property overrides")
            for name, value in sorted(android_settings.properties(settings).items()):
                if name != "ro.kernel.qemu": self.add_property(name, value)
            compat.addWidget(self.properties)
            actions = QHBoxLayout()
            add = QPushButton("Add property"); add.clicked.connect(lambda: self.add_property())
            remove = QPushButton("Remove selected"); remove.clicked.connect(self.remove_properties)
            actions.addWidget(add); actions.addWidget(remove); actions.addStretch(); compat.addLayout(actions)
            hint = dim(QLabel("Remove an override to use the runtime default. Values may be empty. "
                              "Properties are preserved when the runtime is rebuilt; changing them may require rebuilding it."))
            hint.setWordWrap(True); compat.addWidget(hint)
            try:
                import android_compat
                data = android_compat.load(); value = android_compat.entry(data, app["package"])
                text = value["status"].capitalize() + (f" ({value['checked']})" if value.get("checked") else "") + ". " + value["evidence"]
            except (OSError, ValueError, KeyError):
                text = "Compatibility report unavailable"
            report = QLabel(text); report.setWordWrap(True)
            report.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse); compat.addWidget(report)
            tabs.addTab(compatibility, "Compatibility")
            managed = app.get("launcher") == "managed_activity"
            if not managed:
                self.emulator.setEnabled(False); self.properties.setEnabled(False)
                add.setEnabled(False); remove.setEnabled(False)
                note.setText("Device property overrides apply to managed Android apps. This app uses the native Activity runtime.")

            self.error = QLabel(); self.error.setWordWrap(True); self.error.hide(); layout.addWidget(self.error)
            buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Save | QDialogButtonBox.StandardButton.Cancel)
            reset = buttons.addButton("Restore defaults", QDialogButtonBox.ButtonRole.ResetRole)
            reset.clicked.connect(self.reset_profile)
            # Desktop themes put clip-art on standard buttons, chosen when the box is built, before any stylesheet.
            for button in buttons.buttons(): button.setIcon(QIcon())
            buttons.accepted.connect(self.accept); buttons.rejected.connect(self.reject); layout.addWidget(buttons)
            self.initial = (self.stats.isChecked(), self.stats_mode.currentData(), self.back.currentData(),
                            self.enter.currentData(), self.disk_cache.currentData(), self.disk_cache_mb.currentData(),
                            self.network.currentData(), self.window_mode.currentData(), self.controller.currentData(),
                            self.menu.isChecked())
            # The gamepad page's rows draw their own focus; the desktop widgets get a ring.
            if getattr(window, "big", False): self.pad_page(intro, tabs, buttons, managed)
            else: android_gamepad.focus_ring(self)

        def pad_page(self, intro, tabs, buttons, managed):
            """The gamepad layout of this dialog: one large row per setting over the same widgets, changed with the d-pad.
            Nothing here needs a keyboard or a mouse; the desktop widgets stay, hidden, as the model that saves."""
            for widget in (intro, tabs, buttons): widget.hide()
            # A panel inside the library window, which stays in view behind it, not a screen of its own.
            self.setObjectName("padsettings"); self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
            self.resize(min(1000, round(window.width() * 0.82)), round(window.height() * 0.9))
            self.finished.connect(self.release)
            heading = QLabel(app["label"]); font = heading.font(); font.setPointSizeF(font.pointSizeF() * 1.5); font.setBold(True)
            heading.setFont(font); layout = self.layout(); layout.insertWidget(0, heading)
            self.pad_rows = []
            changed = self.refresh_rows

            def choice(title, box):
                step = lambda d: box.setCurrentIndex((box.currentIndex() + d) % box.count())
                return PadRow(title, lambda: "‹   " + box.currentText() + "   ›", step, box.isEnabled, changed=changed)

            def toggle(title, box):
                return PadRow(title, lambda: "On" if box.isChecked() else "Off", lambda d: box.toggle(), changed=changed)

            def action(title, run, enabled=None, tone=""):
                return PadRow(title, None, lambda d: run() if d > 0 else None, enabled, tone, changed)

            rows = [toggle("Show FPS, load and temperatures", self.stats), choice("Stats layout", self.stats_mode),
                    choice("Back key", self.back), choice("Enter key", self.enter),
                    choice("JIT disk cache", self.disk_cache), choice("JIT disk cache size", self.disk_cache_mb),
                    choice("Network", self.network), choice("Window", self.window_mode),
                    choice("Controller", self.controller),
                    toggle("Show in the desktop's application menu", self.menu)]
            if managed: rows.append(choice("Android emulator flag", self.emulator))
            clear, uninstall = self.removals
            rows += [action("Clear data…", clear.click, clear.isEnabled),
                     action("Uninstall…", uninstall.click, uninstall.isEnabled, "danger"),
                     action("Restore defaults", self.pad_menu), action("Save", self.accept), action("Cancel", self.reject)]
            body = QWidget(); column = QVBoxLayout(body); column.setContentsMargins(0, 0, 14, 0); column.setSpacing(8)
            for row in rows: column.addWidget(row); self.pad_rows.append(row)
            column.addStretch()
            scroll = QScrollArea(); scroll.setWidgetResizable(True); scroll.setFrameShape(QFrame.Shape.NoFrame)
            scroll.setWidget(body); layout.insertWidget(1, scroll, 1)
            reader = getattr(window, "pad_reader", None)
            label = reader.label if reader else (lambda button, default: default)
            bar = QFrame(); bar.setObjectName("hints"); line = QHBoxLayout(bar); line.setContentsMargins(4, 10, 4, 0); line.setSpacing(8)
            for name, words in ((label(android_gamepad.SOUTH, "A"), "Change"), ("Left / Right", "Adjust"),
                                (label(android_gamepad.NORTH, "Y"), "Restore defaults"), (label(android_gamepad.EAST, "B"), "Cancel"),
                                ("Start", "Save")):
                key, text = QLabel(name), QLabel(words); key.setObjectName("padkey"); text.setForegroundRole(QPalette.ColorRole.PlaceholderText)
                line.addWidget(key); line.addWidget(text); line.addSpacing(16)
            line.addStretch(); layout.addWidget(bar)
            self.refresh_rows(); QTimer.singleShot(0, rows[0].setFocus)

        def refresh_rows(self):
            for row in self.pad_rows: row.refresh()

        def showEvent(self, event):
            super().showEvent(event)
            if not self.isWindow():          # the gamepad page: put it over a backdrop, in the middle of the library window
                if self.backdrop is None:
                    self.backdrop = QFrame(self.parentWidget()); self.backdrop.setObjectName("sheet"); self.backdrop.show()
                    window.sheet_widget = self; window.pages.setEnabled(False)
                self.reposition(); window.update_hints()

        def reposition(self):
            body = self.parentWidget()
            if self.backdrop is not None: self.backdrop.setGeometry(body.rect()); self.backdrop.raise_()
            self.raise_()
            self.move(max(0, (body.width() - self.width()) // 2), max(0, (body.height() - self.height()) // 2))

        def release(self, *_):
            """Take the backdrop away. Safe to repeat: QDialog emits `finished` on every accept() or reject()."""
            if self.backdrop is None: return
            self.backdrop.deleteLater(); self.backdrop = None
            if window.sheet_widget is self: window.sheet_widget = None
            window.pages.setEnabled(True); window.update_hints()
            QTimer.singleShot(0, window.focus_card)

        def pad_options(self):
            """Start: save."""
            self.accept()

        def pad_menu(self):
            """Y: restore the defaults (staged until Save, like the button)."""
            self.reset_profile(); self.refresh_rows()

        def add_property(self, name="", value=""):
            row = self.properties.rowCount(); self.properties.insertRow(row)
            self.properties.setItem(row, 0, QTableWidgetItem(name))
            self.properties.setItem(row, 1, QTableWidgetItem(value))
            self.properties.setCurrentCell(row, 0)

        def remove_properties(self):
            for row in sorted({i.row() for i in self.properties.selectedIndexes()}, reverse=True):
                self.properties.removeRow(row)

        def reset_profile(self):
            self.settings = {}
            self.stats.setChecked(False); self.stats_mode.setCurrentIndex(self.stats_mode.findData("normal"))
            self.back.setCurrentIndex(0); self.enter.setCurrentIndex(0); self.disk_cache.setCurrentIndex(0)
            self.disk_cache_mb.setCurrentIndex(0); self.network.setCurrentIndex(0); self.window_mode.setCurrentIndex(0)
            self.controller.setCurrentIndex(0); self.menu.setChecked(self.menu_default)
            for key, value in defaults.items():   # the app's own defaults, not the global ones
                if key in self.choices: self.choices[key].setCurrentIndex(max(0, self.choices[key].findData(value)))
                elif key == "window_stats": self.stats.setChecked(value)
                elif key == "menu_entry": self.menu.setChecked(value)
            self.initial = (self.stats.isChecked(), self.stats_mode.currentData(), self.back.currentData(),
                            self.enter.currentData(), self.disk_cache.currentData(), self.disk_cache_mb.currentData(),
                            self.network.currentData(), self.window_mode.currentData(), self.controller.currentData(),
                            self.menu.isChecked())
            self.emulator.setCurrentIndex(0); self.properties.setRowCount(0); self.error.hide()

        def result_settings(self):
            result = {k: v for k, v in self.settings.items() if not android_settings.property_name(k)}
            for key, value, initial in zip(("window_stats", "window_stats_mode", "back_key", "enter_key", "jit_disk_cache", "jit_disk_cache_mb",
                     "network", "window_mode", "controller", "menu_entry"),
                    (self.stats.isChecked(), self.stats_mode.currentData(), self.back.currentData(),
                     self.enter.currentData(), self.disk_cache.currentData(), self.disk_cache_mb.currentData(),
                     self.network.currentData(), self.window_mode.currentData(), self.controller.currentData(),
                     self.menu.isChecked()), self.initial):
                if value != initial or key in result: result[key] = value
            value = self.emulator.currentData()
            if value is not None: result["property.ro.kernel.qemu"] = value
            for row in range(self.properties.rowCount()):
                name = self.properties.item(row, 0).text()
                value = self.properties.item(row, 1).text()
                if not name and not value: continue
                key = android_settings.PROPERTY_PREFIX + name
                if not android_settings.property_name(key): raise ValueError(f"Invalid property name in row {row + 1}")
                if name == "ro.kernel.qemu": raise ValueError("Use the Android emulator flag control above for ro.kernel.qemu")
                if key in result: raise ValueError("Duplicate property: " + name)
                result[key] = android_settings.parse(key, value)
            return result

        def accept(self):
            try: self.result_settings()
            except ValueError as error:
                self.error.setText(str(error)); self.error.show(); return
            super().accept()

    return Settings()


def build(state, application, settings=None, pads=None):
    """The library window without its single-instance socket or event loop (show() adds both).

    `pads` is the gamepad backend (see android_gamepad): None opens SDL3's, False leaves gamepads out."""
    try:
        from PyQt6.QtCore import QEvent, QEventLoop, QPoint, QPointF, QProcess, QProcessEnvironment, QRectF, QSettings, QSize, QTimer, Qt, QUrl
        from PyQt6.QtGui import (QActionGroup, QColor, QDesktopServices, QFont, QIcon, QImage, QPainter, QPainterPath,
                                 QPalette, QPixmap)
        from PyQt6.QtWidgets import (QCheckBox, QDialog, QFileDialog, QFrame, QGridLayout, QHBoxLayout, QLabel,
                                     QLineEdit, QListWidget, QMainWindow, QMenu, QMessageBox, QProgressBar, QPushButton,
                                     QScrollArea, QSizePolicy, QStackedWidget, QToolButton, QVBoxLayout, QWidget)
    except ImportError as error:
        raise ValueError("The desktop launcher requires PyQt6; the CLI works without it") from error
    application.setApplicationName("Asgard")
    icon_size, spacing = 88, GAP
    base_font = QFont(application.font())

    def theme():
        """Colors for the current palette; the stylesheet and every glyph derive from them."""
        palette = application.palette()
        window, role = palette.color(QPalette.ColorRole.Window), QPalette.ColorRole
        dark = window.lightness() < 128
        accent = palette.color(role.Highlight)
        return {"dark": dark, "text": palette.color(role.WindowText), "muted": palette.color(role.PlaceholderText),
                "accent": accent, "accent_text": accent.lighter(130) if dark else accent.darker(140),
                "on_accent": palette.color(role.HighlightedText),
                "danger": QColor("#ff6b78" if dark else "#c0392b"), "good": QColor("#3ddc84" if dark else "#1e8e4e")}

    def tint(color, alpha):
        return f"rgba({color.red()}, {color.green()}, {color.blue()}, {alpha})"

    def make_style(colors, big=False):
        accent, danger, good = colors["accent"], colors["danger"], colors["good"]
        return f"""
        QDialogButtonBox {{ dialogbuttonbox-buttons-have-icons: 0; }}
        QDialog QPushButton {{ background: transparent; border: 1px solid {tint(colors["text"], .28)}; border-radius: 9px;
            min-height: 32px; padding: 0 14px; }}
        QDialog QPushButton:hover {{ background: palette(button); }}
        QDialog QPushButton:disabled {{ color: {colors["muted"].name()}; }}
        QDialog QPushButton:default {{ background: palette(highlight); color: palette(highlighted-text); border: none;
            font-weight: 600; }}
        QDialog QPushButton:default:hover {{ background: {accent.lighter(112).name()}; }}
        QDialog QPushButton#danger {{ color: {danger.name()}; border-color: {tint(danger, .45)}; }}
        QDialog QPushButton#danger:hover {{ background: {danger.darker(130).name()}; color: white; }}
        QDialog QPushButton#danger:disabled {{ color: {colors["muted"].name()}; border-color: {tint(colors["text"], .15)}; }}
        QTabWidget::pane {{ border: none; border-top: 1px solid {tint(colors["text"], .18)}; top: -1px; }}
        QTabWidget > QWidget > QWidget {{ background: transparent; }}
        QTabBar::tab {{ background: transparent; color: {colors["muted"].name()}; padding: 7px 14px;
            border-bottom: 2px solid transparent; }}
        QTabBar::tab:hover {{ color: {colors["text"].name()}; }}
        QTabBar::tab:selected {{ color: {colors["text"].name()}; border-bottom-color: palette(highlight); }}
        QScrollBar:vertical {{ background: transparent; width: 10px; margin: 0; }}
        QScrollBar::handle:vertical {{ background: palette(mid); border-radius: 4px; min-height: 36px; margin: 0 1px; }}
        QScrollBar::handle:vertical:hover {{ background: {colors["muted"].name()}; }}
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{ height: 0; }}
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {{ background: transparent; }}
        QFrame#card {{ background: palette(base); border: 2px solid transparent; border-radius: 14px; }}
        QFrame#card:hover {{ border-color: palette(mid); }}
        QFrame#card:focus, QFrame#card[selected="true"] {{ border-color: palette(highlight); }}
        QLabel#badge {{ background: palette(mid); color: white; border-radius: 9px; padding: 2px 10px; font-weight: 600; }}
        QLabel#badge[state="running"] {{ background: #1f9d55; }}
        QLabel#badge[state="starting"] {{ background: #d9730d; }}
        QLabel#line[tone="problem"] {{ color: {danger.name()}; }}
        QPushButton#play {{ background: {tint(accent, .18)}; color: {colors["accent_text"].name()};
            border: none; border-radius: 9px; min-height: 34px; padding: 0 14px; font-weight: 600; }}
        QPushButton#play:hover {{ background: palette(highlight); color: palette(highlighted-text); }}
        QPushButton#play[role="resume"] {{ background: {tint(good, .18)}; color: {good.name()}; }}
        QPushButton#play[role="resume"]:hover {{ background: {good.darker(125).name()}; color: white; }}
        QPushButton#play[role="stop"] {{ background: {tint(danger, .16)}; color: {danger.name()}; }}
        QPushButton#play[role="stop"]:hover {{ background: {danger.darker(130).name()}; color: white; }}
        QPushButton#play:disabled {{ background: palette(button); color: {colors["muted"].name()}; }}
        QPushButton#primary {{ background: palette(highlight); color: palette(highlighted-text); border: none;
            border-radius: 9px; min-height: 34px; padding: 0 16px; font-weight: 600; }}
        QPushButton#primary:hover {{ background: {accent.lighter(112).name()}; }}
        QPushButton#primary:disabled {{ background: palette(button); color: {colors["muted"].name()}; }}
        QPushButton#ghost, QPushButton#sort {{ background: transparent; border: 1px solid palette(mid); border-radius: 9px;
            min-height: 32px; padding: 0 12px; }}
        QPushButton#primary, QPushButton#ghost, QPushButton#sort, QLineEdit#search {{ max-height: 34px; }}
        QPushButton#ghost:hover, QPushButton#sort:hover {{ background: palette(button); }}
        QPushButton#sort {{ padding-right: 30px; }}
        QPushButton#sort::menu-indicator {{ subcontrol-position: right center; right: 11px; }}
        QToolButton#tool {{ border: none; border-radius: 8px; padding: 6px; }}
        QToolButton#tool:hover {{ background: palette(button); }}
        QLineEdit#search {{ background: palette(base); border: 1px solid palette(mid); border-radius: 9px; padding: 0 10px;
            min-height: 32px; }}
        QLineEdit#search:focus {{ border-color: palette(highlight); }}
        QFrame#notice, QFrame#progress, QFrame#drawer, QFrame#developer {{
            background: palette(alternate-base); border: 1px solid palette(mid); border-radius: 10px; }}
        QFrame#drawer QScrollArea, QFrame#drawer QScrollArea > QWidget > QWidget {{ background: transparent; border: none; }}
        QFrame#notice QLabel, QFrame#progress QLabel, QFrame#drawer QLabel {{ background: transparent; }}
        QLabel#drop {{ background: palette(window); border: 2px dashed palette(highlight); border-radius: 16px;
            font-weight: 600; }}
        QFrame#sheet {{ background: rgba(0, 0, 0, 150); }}
        QFrame#sheetpanel {{ background: palette(window); border: 2px solid palette(mid); border-radius: 18px; }}
        QFrame#sheetpanel QLabel {{ background: transparent; }}
        QFrame#padrow {{ background: palette(base); border: 3px solid transparent; border-radius: 14px; }}
        QFrame#padrow:focus {{ background: {tint(accent, .18)}; border-color: palette(highlight); }}
        QFrame#padrow QLabel {{ background: transparent; }}
        QFrame#padrow:disabled QLabel {{ color: {colors["muted"].name()}; }}
        QFrame#padrow[tone="danger"] QLabel#padvalue, QFrame#padrow[tone="danger"] QLabel {{ color: {danger.name()}; }}
        QLabel#padvalue {{ color: {colors["accent_text"].name()}; font-weight: 600; }}
        QLabel#padvalue[on="true"] {{ color: {good.name()}; }}
        QDialog#padsettings {{ background: palette(window); border: 2px solid palette(mid); border-radius: 18px; }}
        QLabel#padkey {{ border: 2px solid {colors["muted"].name()}; border-radius: 14px; min-height: 24px; max-height: 24px;
            min-width: 24px; padding: 0 7px; font-weight: 700; qproperty-alignment: AlignCenter; }}
        QFrame#hints {{ border: none; border-top: 1px solid {tint(colors["text"], .18)}; }}
        """ + (f"""
        QFrame#card {{ border-width: 4px; border-radius: 20px; }}
        QFrame#card:focus, QFrame#card[selected="true"] {{ background: {tint(accent, .16)}; }}
        QPushButton#play {{ min-height: 46px; border-radius: 12px; }}
        QDialog QPushButton {{ min-height: 44px; }}
        """ if big else "")

    ICON = 18
    glyph_cache = {}

    def draw_glyph(painter, name):
        """One glyph on an 18 x 18 canvas, stroked and filled with the painter's pen."""
        pen = painter.pen(); color = pen.color()
        line = lambda *xy: painter.drawLine(QPointF(xy[0], xy[1]), QPointF(xy[2], xy[3]))
        ring = lambda x, y, r: painter.drawEllipse(QPointF(x, y), r, r)
        painter.setBrush(Qt.BrushStyle.NoBrush)
        if name == "play":
            path = QPainterPath(); path.moveTo(6, 4); path.lineTo(14.2, 9); path.lineTo(6, 14); path.closeSubpath()
            painter.setBrush(color); painter.drawPath(path)
        elif name == "stop":
            painter.setBrush(color); painter.drawRoundedRect(QRectF(4.6, 4.6, 8.8, 8.8), 2, 2)
        elif name == "tune":
            line(3, 6, 6, 6); line(10.6, 6, 15, 6); ring(8.3, 6, 2.3); line(3, 12, 8, 12); line(12.6, 12, 15, 12); ring(10.3, 12, 2.3)
        elif name == "history":
            ring(9, 9, 6.4); line(9, 9, 9, 5.4); line(9, 9, 11.6, 10.6)
        elif name == "search":
            ring(8, 8, 4.7); line(11.5, 11.5, 15, 15)
        elif name == "plus":
            line(9, 3.6, 9, 14.4); line(3.6, 9, 14.4, 9)
        elif name == "person":
            ring(9, 6.2, 2.9); path = QPainterPath(); path.moveTo(3.6, 15.2); path.cubicTo(3.6, 10.6, 14.4, 10.6, 14.4, 15.2)
            painter.drawPath(path)
        elif name == "close":
            line(4.6, 4.6, 13.4, 13.4); line(13.4, 4.6, 4.6, 13.4)
        elif name == "info":
            ring(9, 9, 6.8); line(9, 8.2, 9, 12.6); painter.setBrush(color); ring(9, 5.7, .55)
        elif name == "alert":
            ring(9, 9, 6.8); line(9, 5.2, 9, 9.8); painter.setBrush(color); ring(9, 12.5, .55)

    def glyph(name, kind="text"):
        """A monochrome vector icon; `kind` picks its colors, and its hover (Active) variant."""
        colors = theme()
        key = (name, kind, colors["text"].rgb(), colors["accent"].rgb(), colors["dark"])
        if key in glyph_cache: return glyph_cache[key]
        normal, active = {"text": (colors["text"], colors["text"]),
                          "muted": (colors["muted"], colors["text"]),
                          "accent": (colors["accent_text"], colors["on_accent"]),
                          "danger": (colors["danger"], QColor("white")),
                          "good": (colors["good"], QColor("white")),
                          "onaccent": (colors["on_accent"], colors["on_accent"])}[kind]
        ratio = max(2.0, application.devicePixelRatio())
        icon = QIcon()
        for mode, color in ((QIcon.Mode.Normal, normal), (QIcon.Mode.Active, active),
                            (QIcon.Mode.Disabled, QColor(colors["muted"]))):
            pixmap = QPixmap(round(ICON * ratio), round(ICON * ratio)); pixmap.setDevicePixelRatio(ratio)
            pixmap.fill(Qt.GlobalColor.transparent)
            painter = QPainter(pixmap); painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            pen = painter.pen(); pen.setColor(QColor(color)); pen.setWidthF(1.6)
            pen.setCapStyle(Qt.PenCapStyle.RoundCap); pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin); painter.setPen(pen)
            draw_glyph(painter, name); painter.end()
            icon.addPixmap(pixmap, mode)
        glyph_cache[key] = icon
        return icon

    application.setWindowIcon(glyph("play", "accent"))

    def muted(label):
        label.setForegroundRole(QPalette.ColorRole.PlaceholderText)
        return label

    def scaled(label, factor, bold=False):
        font = label.font(); font.setPointSizeF(font.pointSizeF() * factor); font.setBold(bold); label.setFont(font)
        return label

    def restyle(widget):
        widget.style().unpolish(widget); widget.style().polish(widget)

    class ClickableLabel(QLabel):
        on_click = None
        def mouseReleaseEvent(self, event):
            if self.on_click and event.button() == Qt.MouseButton.LeftButton: self.on_click()
            else: super().mouseReleaseEvent(event)

    def adaptive_image(layers, side, inset=0):
        """Android's composition of an adaptive icon (shared with the menu entries). Null if a layer will not load, so the
        card falls back to its placeholder."""
        import android_desktop
        return android_desktop.compose_adaptive(layers, side, inset)

    def artwork(app, size, ratio, palette):
        """Rounded launcher tile at device pixels: the icon filling it (a padded icon is cropped to its
        pixels), or a tinted tile with the game's initial; a hairline keeps pale icons defined."""
        pixmap = QPixmap(round(size * ratio), round(size * ratio))
        pixmap.setDevicePixelRatio(ratio); pixmap.fill(Qt.GlobalColor.transparent)
        painter = QPainter(pixmap)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing); painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        path = QPainterPath(); path.addRoundedRect(QRectF(0, 0, size, size), size * .22, size * .22)
        layers = app.get("icon_layers")
        image = adaptive_image(layers, round(size * ratio), app.get("icon_inset", 0)) if layers else QImage(app["icon"]) if app.get("icon") else QImage()
        if not image.isNull():
            box = None if layers else opaque_bounds(image)
            if box and box[2] * box[3] < .97 * image.width() * image.height():
                image = image.copy(*box)
            side = min(image.width(), image.height())
            painter.setClipPath(path); painter.fillPath(path, palette.color(QPalette.ColorRole.Base))
            painter.drawImage(QRectF(0, 0, size, size), image,
                              QRectF((image.width() - side) / 2, (image.height() - side) / 2, side, side))
            painter.setClipping(False)
        else:
            painter.fillPath(path, QColor(tile_color(app["label"])))
            font = painter.font(); font.setPixelSize(round(size * .46)); font.setBold(True); painter.setFont(font)
            painter.setPen(QColor("white"))
            painter.drawText(QRectF(0, 0, size, size), Qt.AlignmentFlag.AlignCenter,
                             next((c.upper() for c in app["label"] if c.isalnum()), "?"))
        ring = QColor(palette.color(QPalette.ColorRole.WindowText)); ring.setAlpha(34)
        painter.setPen(ring); painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawRoundedRect(QRectF(.5, .5, size - 1, size - 1), size * .22, size * .22)
        painter.end()
        return pixmap

    PadRow = pad_row_class()

    class Sheet(QFrame):
        """A menu or a question drawn inside the library window: a dimmed backdrop over it and a panel of rows. The gamepad
        layout uses these in place of popup menus and message boxes, which are windows of their own and take keys and focus
        differently from the cards the pad is known to steer."""
        def __init__(self, window, title, entries, message=""):
            body = window.centralWidget()
            super().__init__(body)
            self.window_, self.previous, self.loop, self.closed = window, application.focusWidget(), None, False
            self.setObjectName("sheet"); self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
            outer = QVBoxLayout(self); outer.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.panel = QFrame(); self.panel.setObjectName("sheetpanel")
            self.panel.setFixedWidth(min(560, max(320, body.width() - 80)))
            column = QVBoxLayout(self.panel); column.setContentsMargins(22, 20, 22, 20); column.setSpacing(8)
            column.addWidget(scaled(QLabel(title), 1.3, True))
            if message:
                note = muted(QLabel(message)); note.setWordWrap(True); column.addWidget(note); column.addSpacing(4)
            self.rows = []
            for text, run, enabled, tone in entries:
                row = PadRow(text, None, (lambda d, run=run: self.choose(run) if d > 0 else None), (lambda enabled=enabled: enabled), tone)
                column.addWidget(row); self.rows.append(row)
            outer.addWidget(self.panel)
            self.setGeometry(body.rect()); self.show(); self.raise_()
            window.pages.setEnabled(False)       # the cards underneath cannot take the focus or a click
            first = next((row for row in self.rows if row.isEnabled()), None)
            if first: first.setFocus()

        def choose(self, run):
            self.close_sheet()
            if run: run() if self.loop is not None else QTimer.singleShot(0, run)

        def close_sheet(self):
            if self.closed: return
            self.closed = True
            window = self.window_
            if window.sheet is self: window.sheet = None
            window.pages.setEnabled(True)
            self.hide(); self.deleteLater()
            if self.previous is not None:
                try: self.previous.setFocus()
                except RuntimeError: pass
            window.update_hints()
            if self.loop is not None: self.loop.quit()

        def keyPressEvent(self, event):
            if event.key() == Qt.Key.Key_Escape: self.close_sheet()
            else: super().keyPressEvent(event)

        def mousePressEvent(self, event):
            if not self.panel.geometry().contains(event.position().toPoint()): self.close_sheet()

    def confirm(parent, title, text, action, danger=True):
        """Ask before something, with the action on the button and no theme clip-art. Destructive unless `danger` is False."""
        host = parent if hasattr(parent, "show_sheet") else getattr(parent, "window_", None)
        if host is not None and host.big:      # in the gamepad layout the question is a sheet, not a window of its own
            answer = []
            host.show_sheet(title, [("Cancel", None, True, ""), (action, lambda: answer.append(True), True, "danger" if danger else "")],
                            text, wait=True)
            return bool(answer)
        box = QMessageBox(parent); box.setWindowTitle(title); box.setText(text); box.setIcon(QMessageBox.Icon.NoIcon)
        accept = box.addButton(action, QMessageBox.ButtonRole.DestructiveRole if danger else QMessageBox.ButtonRole.AcceptRole)
        cancel = box.addButton(QMessageBox.StandardButton.Cancel); box.setDefaultButton(cancel); box.setEscapeButton(cancel)
        if danger: accept.setObjectName("danger"); restyle(accept)
        for button in box.buttons(): button.setIcon(QIcon())
        box.exec()
        return box.clickedButton() is accept

    class Card(QFrame):
        def __init__(self, window, app):
            super().__init__()
            self.window, self.app, self.art, self.card_width = window, app, None, MIN_CARD
            self.setObjectName("card"); self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
            self.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
            layout = QVBoxLayout(self); layout.setContentsMargins(16, 16, 16, 14); layout.setSpacing(3)
            top = QGridLayout(); top.setContentsMargins(0, 0, 0, 0)
            self.icon = QLabel(); self.icon.setFixedSize(icon_size, icon_size)
            self.badge = QLabel(); self.badge.setObjectName("badge")
            top.addWidget(self.icon, 0, 0, Qt.AlignmentFlag.AlignHCenter)
            top.addWidget(self.badge, 0, 0, Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignRight)
            layout.addLayout(top); layout.addSpacing(8)
            centered = Qt.AlignmentFlag.AlignHCenter
            self.title = scaled(QLabel(), 1.1, True); self.title.setAlignment(centered)
            self.meta = muted(QLabel()); self.meta.setAlignment(centered)
            self.window_of_card = window
            self.line = muted(ClickableLabel()); self.line.setObjectName("line"); self.line.setAlignment(centered)
            self.developer = muted(QLabel()); self.developer.setWordWrap(True); self.developer.setAlignment(centered)
            self.developer.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
            for widget in (self.title, self.meta, self.line, self.developer): layout.addWidget(widget)
            layout.addSpacing(10)
            actions = QHBoxLayout(); actions.setSpacing(2)
            self.action = QPushButton(); self.action.setObjectName("play")
            self.action.clicked.connect(lambda: window.act(self.app))
            self.gear = QToolButton(); self.gear.setObjectName("tool")
            self.gear.setIcon(glyph("tune", "muted")); self.gear.setIconSize(QSize(ICON, ICON))
            self.gear.setToolTip("App settings"); self.gear.clicked.connect(lambda: window.configure(self.app))
            self.history = QToolButton(); self.history.setObjectName("tool")
            self.history.setIcon(glyph("history", "muted")); self.history.setIconSize(QSize(ICON, ICON))
            self.history.setToolTip("Last session")
            self.history.clicked.connect(lambda: window.open_drawer(self.app["package"]))
            actions.addWidget(self.action, 1); actions.addSpacing(4); actions.addWidget(self.gear); actions.addWidget(self.history)
            layout.addLayout(actions)
            self.update_app(app)

        def set_width(self, width):
            self.card_width = width; self.setFixedWidth(width); self.relabel()

        def relabel(self):
            label = self.app["label"]
            words = self.title.fontMetrics().elidedText(label, Qt.TextElideMode.ElideRight, self.card_width - 40)
            self.title.setText(words); self.title.setToolTip(label if words != label else "")
            if getattr(self, "meta_text", None):
                shown = self.meta.fontMetrics().elidedText(self.meta_text, Qt.TextElideMode.ElideRight, self.card_width - 40)
                self.meta.setText(shown); self.meta.setToolTip(self.meta_text if shown != self.meta_text else "")

        def update_app(self, app, pending=None):
            self.app = app
            rule = card_state(app, pending)
            ratio = self.devicePixelRatioF()
            art = (app.get("icon"), str(app.get("icon_layers")), app["label"], ratio, self.palette().cacheKey())
            if art != self.art:
                self.art = art; self.icon.setPixmap(artwork(app, icon_size, ratio, self.palette()))
            version = app.get("version_name") or app.get("version")
            moment = time.localtime(app["installed_at"]) if app.get("installed_at") else None
            installed = moment and "Installed " + time.strftime("%b %d" + ("" if moment.tm_year == time.localtime().tm_year
                                                                           else ", %Y"), moment)
            # Before relabel, which draws it: an update installed over a listed app showed the old version and date.
            self.meta_text = " · ".join(str(x) for x in (version, installed) if x) or " "
            self.relabel()
            text, tone = activity_line(app)
            self.line.setText(text); self.line.setProperty("tone", tone)
            # The red line says what went wrong; a click opens the drawer with what to do about it.
            self.line.on_click = (lambda: self.window_of_card.open_drawer(self.app["package"])) if tone else None
            self.line.setToolTip("See what happened" if tone else "")
            if tone: self.line.setCursor(Qt.CursorShape.PointingHandCursor)
            else: self.line.unsetCursor()
            self.line.setForegroundRole(QPalette.ColorRole.WindowText if tone else QPalette.ColorRole.PlaceholderText)
            restyle(self.line)
            self.badge.setText(rule["badge"] or ""); self.badge.setVisible(bool(rule["badge"]))
            self.badge.setProperty("state", (rule["badge"] or "").lower()); restyle(self.badge)
            self.action.setText(rule["text"]); self.action.setEnabled(rule["enabled"])
            self.action.setProperty("role", rule["action"]); restyle(self.action)
            self.action.setIcon(glyph("stop" if rule["action"] == "stop" else "play",
                                      {"stop": "danger", "resume": "good"}.get(rule["action"], "accent")))
            self.action.setIconSize(QSize(ICON, ICON))
            self.gear.setIcon(glyph("tune", "muted")); self.history.setIcon(glyph("history", "muted"))
            self.action.setToolTip(rule.get("reason", ""))
            big = self.window.big
            for button in (self.gear, self.history): button.setVisible(not big)
            for button in (self.action, self.gear, self.history):
                button.setFocusPolicy(Qt.FocusPolicy.NoFocus if big else Qt.FocusPolicy.StrongFocus)
            session = app["session"]
            self.history.setEnabled(bool(session.get("session_id") or app.get("error")))
            self.developer.setText(f"{app['package']}\nUID {session.get('uid', '—')} · PID {session.get('pid') or '—'}")
            self.developer.setVisible(self.window.developer_mode())

        def set_selected(self, value):
            self.setProperty("selected", value); restyle(self)

        def mousePressEvent(self, event):
            self.window.select(self.app["package"]); super().mousePressEvent(event)

        def mouseDoubleClickEvent(self, event):
            self.window.activate_card(self.app)

        def focusInEvent(self, event):
            self.window.select(self.app["package"]); super().focusInEvent(event)

        def keyPressEvent(self, event):
            key, package = event.key(), self.app["package"]
            moves = {Qt.Key.Key_Left: (-1, 0), Qt.Key.Key_Right: (1, 0), Qt.Key.Key_Up: (0, -1), Qt.Key.Key_Down: (0, 1)}
            if key in (Qt.Key.Key_Return, Qt.Key.Key_Enter, Qt.Key.Key_Space): self.window.activate_card(self.app)
            elif key in moves: self.window.move_focus(package, *moves[key])
            elif key == Qt.Key.Key_Menu: self.window.card_menu(self.app, self.mapToGlobal(self.rect().center()), keyboard=True)
            else: super().keyPressEvent(event)

        def contextMenuEvent(self, event):
            self.window.select(self.app["package"]); self.window.card_menu(self.app, event.globalPos())

    class GoogleAccounts(QDialog):
        """Accounts signed in on this device. Sign-in is microG's own window with Google's page."""
        def __init__(self, window):
            super().__init__(window)
            self.window_, self.signing_in = window, False
            self.setWindowTitle("Google account")
            layout = QVBoxLayout(self); layout.setContentsMargins(16, 14, 16, 14); layout.setSpacing(12)
            heading = QHBoxLayout()
            badge = QLabel("Experimental"); badge.setObjectName("badge"); heading.addWidget(badge); heading.addStretch()
            layout.addLayout(heading)
            self.intro = muted(QLabel("Sign in once and every app on this device can use the account. Sign-in opens "
                                      "microG's window showing Google's own page; your password goes to Google, "
                                      "never to Asgard. This is experimental: some apps and Google services may "
                                      "not work with it."))
            self.intro.setWordWrap(True); layout.addWidget(self.intro)
            self.accounts = QListWidget(); self.accounts.setMinimumHeight(90); layout.addWidget(self.accounts)
            self.hint = muted(QLabel()); self.hint.setWordWrap(True); layout.addWidget(self.hint)
            row = QHBoxLayout()
            self.sign_in = QPushButton("Sign in with Google…")
            self.sign_in.clicked.connect(lambda: self.open_window("sign-in"))
            self.settings = QPushButton("microG settings…")
            self.settings.setToolTip("Device registration, attestation and other Google services settings")
            self.settings.clicked.connect(lambda: self.open_window("settings"))
            self.remove = QPushButton("Remove")
            self.remove.clicked.connect(self.remove_selected)
            close = QPushButton("Close"); close.clicked.connect(self.accept)
            for widget in (self.sign_in, self.settings, self.remove): row.addWidget(widget)
            row.addStretch(); row.addWidget(close); layout.addLayout(row)
            self.accounts.itemSelectionChanged.connect(self.update_buttons)
            self.timer = QTimer(self); self.timer.timeout.connect(self.refresh); self.timer.start(1500)
            self.refresh()

        def showEvent(self, event):
            super().showEvent(event)
            self.fit()

        def fit(self):
            """Room for every button's whole label, and for the wrapped lines: a layout does not grow a dialog for those."""
            margins = self.layout().contentsMargins()
            buttons = self.findChildren(QPushButton)
            for button in buttons: button.setMinimumWidth(button.sizeHint().width())
            width = max(560, sum(b.sizeHint().width() for b in buttons) + 3 * self.layout().spacing() + 40 + margins.left() + margins.right())
            self.setFixedWidth(width)
            inner = width - margins.left() - margins.right()
            for label in (self.intro, self.hint): label.setFixedHeight(label.heightForWidth(inner))
            self.accounts.setVisible(bool(getattr(self, "found", [])))
            self.layout().activate(); self.adjustSize()

        def update_buttons(self):
            self.remove.setEnabled(bool(self.accounts.selectedItems()) and not self.signing_in)
            self.sign_in.setEnabled(not self.signing_in); self.settings.setEnabled(not self.signing_in)

        def refresh(self):
            def listed(code, output):
                try:
                    if code: raise ValueError(short_error(output))
                    found = [x["name"] for x in json.loads(output)]
                except (ValueError, KeyError, TypeError) as error:
                    self.hint.setText("Couldn't read the accounts: " + str(error)[:200]); return
                selected = {x.text() for x in self.accounts.selectedItems()}
                self.accounts.clear()
                for name in found:
                    self.accounts.addItem(name)
                    if name in selected: self.accounts.item(self.accounts.count() - 1).setSelected(True)
                self.update_hint(found)
            self.window_.command(["google", "accounts", "--json"], listed)
            def status(code, output):
                try: state = json.loads(output).get("state") if not code else None
                except ValueError: state = None
                active = state in ACTIVE
                if active != self.signing_in: self.signing_in = active; self.update_buttons(); self.update_hint(None)
            self.window_.command(["status", "com.google.android.gms", "--json"], status)

        def update_hint(self, found):
            if found is not None: self.found = found
            if self.signing_in and self.window_.google_requester:
                self.hint.setText(f"{self.window_.google_requester} asked for Google sign-in. Finish in the open Google "
                                  "window; the account appears here when it closes.")
            elif self.signing_in:
                self.hint.setText("A microG window is open. Finish there; the account appears here when it closes.")
            elif not getattr(self, "found", []):
                self.hint.setText("No Google account is signed in. Apps that need Google Play services will ask for one.")
            else:
                self.hint.setText("Apps pick up account changes the next time they start.")
            self.update_buttons()
            self.fit()

        def open_window(self, action):
            def started(code, output):
                if code: self.hint.setText("Couldn't open the window: " + short_error(output))
                self.refresh()
            self.signing_in = True; self.update_buttons()
            self.window_.command(["google", action], started)

        def remove_selected(self):
            items = self.accounts.selectedItems()
            if not items: return
            name = items[0].text()
            if not confirm(self, "Remove account", f"Sign {name} out of every app on this device?", "Remove"): return
            def removed(code, output):
                if code: self.hint.setText("Couldn't remove the account: " + short_error(output))
                self.refresh()
            self.window_.command(["google", "remove", name], removed)

    class Library(QMainWindow):
        def __init__(self):
            super().__init__()
            self.state = state
            self.jobs, self.pending, self.cards = set(), {}, {}
            self.refreshing, self.apps, self.columns = False, [], 0
            self.icons_scanned = None
            self.selected, self.drawer_package, self.drawer_stamp, self.drawer_scanned = None, None, None, 0
            self.order, self.announce = [], None
            self.settings = settings or QSettings("Asgard", "Library")
            # layout: auto follows the input (a pad press switches to the gamepad layout), or big, or desktop.
            self.big, self.pad_layout, self.pad_reader, self.pad_name = False, str(self.settings.value("layout", "auto")), None, ""
            self.card_limits, self.fullscreen_wanted, self.was_active = (MIN_CARD, MAX_CARD), False, set()
            self.sheet, self.sheet_widget = None, None   # the open Sheet; the settings page while it is drawn in this window
            self.metrics, self.polled, self.install_queue, self.install_batch = None, 0.0, [], None
            self.setup_status, self.setting_up, self.notice_action_run = None, False, None  # what asgard setup --check last said
            self.setWindowTitle("Asgard"); self.setMinimumSize(720, 480); self.setAcceptDrops(True)
            self.setWindowIcon(application.windowIcon()); self.theming = False
            body = QWidget(); outer = QVBoxLayout(body)
            outer.setContentsMargins(24, 20, 24, 16); outer.setSpacing(12)
            heading = QHBoxLayout(); heading.setSpacing(10)
            titles = QVBoxLayout(); titles.setSpacing(0)
            titles.addWidget(scaled(QLabel("App library"), 1.8, True))
            self.summary = muted(QLabel()); titles.addWidget(self.summary)
            heading.addLayout(titles); heading.addStretch()
            self.search = QLineEdit(); self.search.setObjectName("search"); self.search.setPlaceholderText("Search apps")
            self.search.setClearButtonEnabled(True)
            self.search_action = self.search.addAction(glyph("search", "muted"), QLineEdit.ActionPosition.LeadingPosition)
            self.search.textChanged.connect(lambda: self.render(True))
            self.sort_menu, self.sort_group = QMenu(self), QActionGroup(self)
            for key, text in SORTS:
                action = self.sort_menu.addAction(text); action.setCheckable(True); action.setData(key)
                self.sort_group.addAction(action)
                action.triggered.connect(lambda _, key=key: self.set_sort(key))
            self.sort_button = QPushButton(); self.sort_button.setObjectName("sort")
            self.sort_button.setMenu(self.sort_menu); self.sort_button.setToolTip("Sort apps")
            self.sort_mode = "name"
            self.set_sort(str(self.settings.value("sort", "name")), save=False)
            self.install_button = QPushButton(glyph("plus", "onaccent"), "Install APK…")
            self.install_button.setObjectName("primary"); self.install_button.setIconSize(QSize(ICON, ICON))
            self.install_button.clicked.connect(self.install)
            self.google_requester = None
            self.google_button = QPushButton(glyph("person", "text"), "Google account")
            self.google_button.setObjectName("ghost"); self.google_button.setToolTip("Google account (experimental)…")
            self.google_button.setIconSize(QSize(ICON, ICON)); self.google_button.clicked.connect(self.google_account)
            self.search.setMinimumWidth(140); self.search.setMaximumWidth(380)
            heading.addWidget(self.search); heading.addWidget(self.sort_button)
            heading.addWidget(self.google_button); heading.addWidget(self.install_button)
            outer.addLayout(heading)
            self.progress = QFrame(); self.progress.setObjectName("progress")
            row = QHBoxLayout(self.progress); row.setContentsMargins(12, 8, 12, 8)
            self.progress_text = QLabel(); bar = QProgressBar(); bar.setRange(0, 0); bar.setMaximumWidth(220)
            row.addWidget(self.progress_text, 1); row.addWidget(bar)
            self.progress.hide(); outer.addWidget(self.progress)
            self.notice = QFrame(); self.notice.setObjectName("notice")
            row = QHBoxLayout(self.notice); row.setContentsMargins(12, 8, 8, 8)
            self.notice_icon = QLabel(); self.notice_text = QLabel(); self.notice_text.setWordWrap(True)
            self.notice_text.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
            self.notice_details = QPushButton("Last session"); self.notice_details.setObjectName("ghost")
            self.notice_details.clicked.connect(
                lambda: self.open_drawer(self.notice_details.property("package")))
            self.notice_action = QPushButton(); self.notice_action.setObjectName("ghost"); self.notice_action.hide()
            self.notice_action.clicked.connect(lambda: self.notice_action_run and self.notice_action_run())
            close = QToolButton(); close.setAutoRaise(True)
            close.setObjectName("tool"); close.setIcon(glyph("close", "muted")); close.clicked.connect(self.notice.hide)
            row.addWidget(self.notice_icon); row.addWidget(self.notice_text, 1); row.addWidget(self.notice_details)
            row.addWidget(self.notice_action); row.addWidget(close)
            self.notice.hide(); outer.addWidget(self.notice)
            self.notice_timer = QTimer(self); self.notice_timer.setSingleShot(True); self.notice_timer.timeout.connect(self.notice.hide)
            middle = QHBoxLayout(); middle.setSpacing(12)
            self.pages = QStackedWidget()
            self.scroll = QScrollArea(); self.scroll.setWidgetResizable(True); self.scroll.setFrameShape(QFrame.Shape.NoFrame)
            grid_body = QWidget(); self.grid = QGridLayout(grid_body)
            self.grid.setContentsMargins(0, 0, GUTTER, 0); self.grid.setSpacing(spacing)
            self.grid.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignLeft)
            self.scroll.setWidget(grid_body); self.pages.addWidget(self.scroll)
            self.scroll.viewport().installEventFilter(self)
            empty = QWidget(); column = QVBoxLayout(empty); column.addStretch()
            self.empty_title = scaled(QLabel(), 1.5, True); self.empty_hint = muted(QLabel()); self.empty_hint.setWordWrap(True)
            self.empty_hint.setFixedWidth(420)
            self.empty_install = QPushButton(glyph("plus", "onaccent"), "Install APK…")
            self.empty_install.setObjectName("primary"); self.empty_install.setIconSize(QSize(ICON, ICON))
            self.empty_install.clicked.connect(self.install)
            for widget in (self.empty_title, self.empty_hint, self.empty_install):
                if isinstance(widget, QLabel): widget.setAlignment(Qt.AlignmentFlag.AlignCenter)
                column.addWidget(widget, 0, Qt.AlignmentFlag.AlignHCenter)
            column.setSpacing(10); column.addStretch(); self.pages.addWidget(empty)
            middle.addWidget(self.pages, 1); middle.addWidget(self.build_drawer())
            outer.addLayout(middle, 1)
            self.developer_box = self.build_developer(); outer.addWidget(self.developer_box)
            outer.addWidget(self.build_hints())
            self.setCentralWidget(body)
            # Nothing is selected on launch, and clicking empty space leaves the search box.
            body.setFocusPolicy(Qt.FocusPolicy.ClickFocus); body.setFocus()
            self.drop_hint = QLabel("<div style='font-size: 22pt; font-weight: 600'>Drop to install</div>"
                                    "<div style='margin-top: 6px'>APK, APKM, APKS or XAPK</div>", body)
            self.drop_hint.setObjectName("drop"); self.drop_hint.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.drop_hint.hide()
            geometry = self.settings.value("geometry")
            if not (geometry and self.restoreGeometry(geometry)): self.resize(1080, 720)
            self.timer = QTimer(self); self.timer.timeout.connect(self.tick); self.timer.start(1500)
            if self.pad_layout == "big" or (self.pad_layout == "auto" and android_gamepad.in_game_mode()): self.set_big(True)
            self.render(True); self.refresh()

        def build_hints(self):
            """The bar of button hints at the bottom of the gamepad layout; its keys are drawn by the stylesheet."""
            self.hints = QFrame(); self.hints.setObjectName("hints")
            row = QHBoxLayout(self.hints); row.setContentsMargins(4, 10, 4, 0); row.setSpacing(8)
            self.hint_parts = []
            for _ in range(6):
                key, text = QLabel(), muted(QLabel()); key.setObjectName("padkey")
                self.hint_parts.append((key, text)); row.addWidget(key); row.addWidget(text); row.addSpacing(16)
            row.addStretch(); self.hints.hide()
            return self.hints

        def update_hints(self):
            if not self.big: return
            reader = self.pad_reader
            label = reader.label if reader else (lambda button, default: default)
            app = self.app(self.selected)
            rule = card_state(app, self.pending.get(app["package"])) if app else None
            if self.sheet_active():
                for (key, text), (name, words, visible) in zip(self.hint_parts, (
                        (label(android_gamepad.SOUTH, "A"), "Choose", True), ("", "", False), ("", "", False), ("", "", False),
                        (label(android_gamepad.EAST, "B"), "Back", True), ("", "", False))):
                    key.setText(name); text.setText(words); key.setVisible(visible); text.setVisible(visible)
                return
            closing = self.drawer.isVisible() or self.notice.isVisible()
            history = bool(app and (app["session"].get("session_id") or app.get("error")))   # something to look at
            shown = ((label(android_gamepad.SOUTH, "A"), rule["text"] if rule else "Select", True),
                     (label(android_gamepad.WEST, "X"), "Sort", True), (label(android_gamepad.NORTH, "Y"), "Menu", True),
                     ("Select", "Last session", history and not self.drawer.isVisible()),
                     (label(android_gamepad.EAST, "B"), "Close", closing), ("Start", "Library menu", True))
            for (key, text), (name, words, visible) in zip(self.hint_parts, shown):
                key.setText(name); text.setText(words); key.setVisible(visible); text.setVisible(visible)

        def set_big(self, on, remember=False):
            """The gamepad layout (large tiles, hints, no mouse-only controls) or the desktop one."""
            nonlocal icon_size
            if remember: self.settings.setValue("layout", "big" if on else "desktop"); self.pad_layout = "big" if on else "desktop"
            if on == self.big: return
            self.big = on
            icon_size = BIG_ICON if on else 88
            font = QFont(base_font)
            if on: font.setPointSizeF(font.pointSizeF() * BIG_FONT)
            application.setFont(font)
            if on: self.developer_toggle.setChecked(False)
            for widget in (self.search, self.sort_button, self.google_button, self.install_button, self.developer_box):
                widget.setVisible(not on)
            self.hints.setVisible(on)
            self.card_limits = (BIG_MIN_CARD, BIG_MAX_CARD) if on else (MIN_CARD, MAX_CARD)
            for card in self.cards.values():                         # their fonts and icons are set when they are built
                self.grid.removeWidget(card); card.hide(); card.deleteLater()
            # No card, no place in the grid: restyling resizes the viewport at once, and reflow() walks the order.
            self.cards.clear(); self.order = []; self.apply_theme(); self.update_hints()
            QTimer.singleShot(0, self.focus_card)

        def focus_card(self):
            """Focus the selected card, else the first one; the empty library's button when there are none."""
            card = self.cards.get(self.selected) or (self.cards.get(self.order[0]) if self.order else None)
            if card is not None and card.isVisible(): card.setFocus(); self.scroll.ensureWidgetVisible(card)
            elif self.pages.currentIndex() == 1 and self.empty_install.isVisible(): self.empty_install.setFocus()
            return card is not None

        def pad_wake(self):
            """A pad press while nothing here shows it is listening: take the gamepad layout when the layout follows
            the input, and put the focus on a card. True when that was all the press was for."""
            if not self.big and self.pad_layout == "auto":
                self.set_big(True); return True
            focused = application.focusWidget()
            if focused is not None and focused.objectName() == "card": return False
            self.focus_card(); return True

        def pad_action(self, action):
            """What only the library does with a pad action; True when it took the action. Menus and dialogs, which
            have their own keys, get all of them."""
            if application.activePopupWidget() or application.activeModalWidget(): return False
            if self.sheet is not None and action == "back":
                self.sheet.close_sheet(); return True
            if self.sheet_active(): return False      # its rows (or the settings page, with its own Start and Y) take the rest
            if self.pad_wake(): return True
            if action == "sort":
                keys = [key for key, _ in SORTS]
                self.set_sort(keys[(keys.index(self.sort_mode) + 1) % len(keys)])
                self.show_notice("Sorted by " + dict(SORTS)[self.sort_mode].lower(), timeout=1500)
            elif action == "options": self.launcher_menu()
            elif action == "details":
                # The pad's Select: what the card's red line or history button opens with a mouse.
                app = self.app(self.selected)
                if app and (app["session"].get("session_id") or app.get("error")): self.open_drawer(app["package"])
                else: self.show_notice("Nothing has been recorded for this app yet.", timeout=2500)
            elif action == "back":
                if self.drawer.isVisible(): self.close_drawer()
                elif self.notice.isVisible(): self.notice.hide()
                self.update_hints()
            elif action in ("page_up", "page_down") and self.selected in self.order:
                card = self.cards[self.selected]; rows = max(1, self.scroll.viewport().height() // (card.height() + GAP))
                index = self.order.index(self.selected) + (1 if action == "page_down" else -1) * rows * self.columns
                target = self.cards[self.order[max(0, min(len(self.order) - 1, index))]]
                target.setFocus(); self.scroll.ensureWidgetVisible(target)
            else: return False
            return True

        def sheet_active(self):
            return self.sheet is not None or self.sheet_widget is not None

        def show_sheet(self, title, entries, message="", wait=False):
            """A menu or question inside this window: entries are (text, run, enabled, tone). With `wait` it returns when the
            sheet closes, running the chosen entry first, like a message box."""
            if self.sheet is not None: self.sheet.close_sheet()
            sheet = self.sheet = Sheet(self, title, entries, message)
            self.update_hints()
            if wait:
                # Registered first (self.sheet), so the pad and the hints already know a sheet is open while this waits.
                sheet.loop = QEventLoop(); sheet.loop.exec()
            return sheet

        def layout_overlays(self):
            body = self.centralWidget()
            if self.sheet is not None: self.sheet.setGeometry(body.rect())
            if self.sheet_widget is not None: self.sheet_widget.reposition()

        def launcher_menu(self):
            """Start: what the toolbar does with a mouse, as a menu a pad can walk."""
            setup = bool(self.setup_status and self.setup_status["can_set_up"] and not self.setting_up)
            self.show_sheet("Library", [
                *([("Set up Asgard…", self.setup_asgard, True, "")] if setup else []),
                ("Install APK…", self.install, True, ""), ("Google account…", self.google_account, True, ""),
                ("Sort by " + ("recently played" if self.sort_mode == "name" else "name"), lambda: self.pad_action("sort"), True, ""),
                ("Desktop layout" if self.big else "Gamepad layout", lambda: self.set_big(not self.big, remember=True), True, ""),
                ("Leave full screen" if self.isFullScreen() else "Full screen",
                 lambda: self.set_fullscreen(not self.isFullScreen()), True, ""),
                ("Quit the library", self.close, True, "danger")])

        def set_fullscreen(self, on):
            self.fullscreen_wanted = on
            if on: self.showFullScreen()
            else: self.showNormal()

        def present(self):
            """Show the window in front, full screen if that is how it was asked for."""
            if self.fullscreen_wanted: self.showFullScreen()
            else: self.showNormal()
            self.raise_(); self.activateWindow()

        def build_drawer(self):
            self.drawer = QFrame(); self.drawer.setObjectName("drawer"); self.drawer.setFixedWidth(340)
            layout = QVBoxLayout(self.drawer); layout.setContentsMargins(16, 12, 10, 14); layout.setSpacing(8)
            top = QHBoxLayout(); top.addWidget(muted(QLabel("LAST SESSION"))); top.addStretch()
            close = QToolButton(); close.setAutoRaise(True)
            close.setObjectName("tool"); close.setIcon(glyph("close", "muted")); close.clicked.connect(self.close_drawer)
            top.addWidget(close); layout.addLayout(top)
            self.drawer_title = scaled(QLabel(), 1.1, True); self.drawer_title.setWordWrap(True)
            self.drawer_body = QLabel(); self.drawer_body.setWordWrap(True); self.drawer_body.setTextFormat(Qt.TextFormat.RichText)
            self.drawer_body.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignLeft)
            self.drawer_body.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
            body = QScrollArea(); body.setWidgetResizable(True); body.setFrameShape(QFrame.Shape.NoFrame); body.setWidget(self.drawer_body)
            layout.addWidget(self.drawer_title); layout.addWidget(body, 1)
            self.open_log = QPushButton("Open full log"); self.open_log.setObjectName("ghost")
            self.open_log.clicked.connect(lambda: self.open_path(session_log(self.drawer_app()["session"])))
            self.open_folder = QPushButton("Open session folder"); self.open_folder.setObjectName("ghost")
            self.open_folder.clicked.connect(lambda: self.open_path(str(Path(log).parent)
                if (log := session_log(self.drawer_app()["session"])) else None))
            layout.addWidget(self.open_log); layout.addWidget(self.open_folder)
            self.drawer.hide()
            return self.drawer

        def build_developer(self):
            box = QWidget(); layout = QVBoxLayout(box); layout.setContentsMargins(0, 0, 0, 0); layout.setSpacing(6)
            self.developer_toggle = QToolButton(); self.developer_toggle.setText("Developer")
            self.developer_toggle.setCheckable(True); self.developer_toggle.setAutoRaise(True)
            self.developer_toggle.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextBesideIcon)
            self.developer_toggle.setArrowType(Qt.ArrowType.RightArrow)
            self.developer_toggle.toggled.connect(self.toggle_developer)
            layout.addWidget(self.developer_toggle)
            self.developer = QFrame(); self.developer.setObjectName("developer")
            panel = QVBoxLayout(self.developer); panel.setContentsMargins(12, 10, 12, 10)
            self.trace = QCheckBox("Trace Java framework calls (apps with a Java Activity only)")
            self.trace.setToolTip("NativeActivity apps launch without tracing")
            row = QHBoxLayout(); self.path = muted(QLabel()); self.path.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
            choose = QPushButton("Choose library…"); choose.clicked.connect(self.choose)
            row.addWidget(QLabel("Library folder:")); row.addWidget(self.path, 1); row.addWidget(choose)
            panel.addWidget(self.trace)
            panel.addLayout(row)
            self.path.setText(str(self.state)); self.developer.hide(); layout.addWidget(self.developer)
            return box

        def developer_mode(self):
            return self.developer_toggle.isChecked()

        def toggle_developer(self, shown):
            self.developer_toggle.setArrowType(Qt.ArrowType.DownArrow if shown else Qt.ArrowType.RightArrow)
            self.developer.setVisible(shown)
            if not shown: self.trace.setChecked(False)
            self.path.setText(str(self.state))
            for card in self.cards.values(): card.developer.setVisible(shown)
            self.render(True); self.update_drawer(force=True)

        def command(self, args, done=None, env=None, progress=None):
            """Run an asgard command. `progress(line)` gets each line of its output as it arrives; `done(code, output)` ends it."""
            process = QProcess(self)
            process.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
            received, pending = [], [b""]
            def read():
                data = bytes(process.readAllStandardOutput())
                received.append(data)
                if progress:
                    pending[0] += data
                    *lines, pending[0] = pending[0].split(b"\n")
                    for line in lines:
                        if line.strip(): progress(line.decode(errors="replace").strip())
            process.readyReadStandardOutput.connect(read)
            if env:
                environment = QProcessEnvironment.systemEnvironment()
                for key, value in env.items(): environment.insert(key, value)
                process.setProcessEnvironment(environment)
            self.jobs.add(process)
            def finished(code, status):
                read()
                output = b"".join(received).decode(errors="replace")
                self.jobs.discard(process); process.deleteLater()
                if done: done(code, output)
            process.finished.connect(finished)
            def failed(error):
                if error == QProcess.ProcessError.FailedToStart:
                    self.jobs.discard(process); process.deleteLater()
                    self.show_notice("Couldn't run Asgard: " + process.errorString(), error=True)
                    if done: done(1, process.errorString())
            process.errorOccurred.connect(failed)
            process.start(sys.executable, [str(ROOT / "Scripts/Android/asgard.py"), "--state-root", str(self.state), *args])

        def refresh(self):
            if self.refreshing: return
            self.refreshing, self.polled = True, time.monotonic()
            requested_state = self.state
            def loaded(code, output):
                self.refreshing = False
                if requested_state != self.state: self.refresh(); return
                try:
                    if code: raise ValueError(short_error(output))
                    # System apps (microG's sign-in window) are not games.
                    listed = json.loads(output)
                    self.apps = sorted((a for a in listed if not a.get("system")),
                                       key=lambda a: (a["label"].casefold(), a["package"]))
                except (ValueError, KeyError) as error:
                    self.show_notice("Couldn't read the app library: " + str(error)[:300], error=True); return
                self.google_request(listed)
                active = {a["package"] for a in self.apps if a["session"].get("state") in ACTIVE}
                if self.big and self.was_active - active and not self.isActiveWindow(): self.present()
                self.was_active = active
                self.render(); self.update_drawer()
                # Apps installed before adaptive icons were read get theirs once, without holding up the window.
                if self.icons_scanned != self.state and any(a.get("icon_unscanned") for a in self.apps):
                    self.icons_scanned = self.state
                    self.command(["refresh-icons"], lambda code, output: self.refresh())
                app = self.app(self.announce)
                if app:
                    self.announce = None; self.select(app["package"])
                    self.show_notice(f"{app['label']} is installed.", timeout=8000)
            self.command(["list", "--json"], loaded)

        def render(self, relayout=False):
            query = self.search.text().casefold()
            visible = [a for a in sort_apps(self.apps, self.sort_mode)
                       if query in (a["label"] + " " + a["package"]).casefold()]
            for package in set(self.cards) - {a["package"] for a in self.apps}:
                self.cards.pop(package).deleteLater()
            for app in self.apps:
                card = self.cards.get(app["package"])
                if card is None:
                    card = self.cards[app["package"]] = Card(self, app); relayout = True
                    card.set_selected(app["package"] == self.selected)
                else:
                    card.update_app(app, self.pending.get(app["package"]))
            order = [a["package"] for a in visible]
            if relayout or order != self.order: self.reflow(order)
            self.update_hints()
            self.update_summary()
            if not self.apps:
                self.empty_title.setText("No apps yet")
                self.empty_hint.setText("Install an APK, APKM, APKS or XAPK file, or drop one anywhere in this window.")
                self.empty_hint.setFixedHeight(self.empty_hint.heightForWidth(420))
                self.empty_install.show(); self.pages.setCurrentIndex(1)
            elif not visible:
                self.empty_title.setText("No matches")
                self.empty_hint.setText(f"No installed app matches “{self.search.text()}”.")
                self.empty_hint.setFixedHeight(self.empty_hint.heightForWidth(420))
                self.empty_install.hide(); self.pages.setCurrentIndex(1)
            else:
                self.pages.setCurrentIndex(0)

        def update_summary(self):
            running = sum(a["session"].get("state") in ACTIVE for a in self.apps)
            self.summary.setText(f"{len(self.apps)} app{'s' * (len(self.apps) != 1)}" + (f" · {running} running" if running else "")
                                 + (f" · {self.pad_name} connected" if self.pad_name else ""))

        def pad_status(self, name):
            """The controller the library sees, shown beside the app count so that it is plain whether one is in use."""
            self.pad_name = name; self.update_summary()

        def reflow(self, order=None):
            if order is not None: self.order = order
            columns, width = grid_metrics(self.scroll.viewport().width() - GUTTER, *self.card_limits)
            self.columns, self.metrics = columns, (columns, width)
            focused = application.focusWidget()      # taking the cards out of the grid drops the focus
            keep = focused.app["package"] if focused is not None and focused.objectName() == "card" else None
            for card in self.cards.values():
                self.grid.removeWidget(card); card.hide()
            previous = None
            self.order = [package for package in self.order if package in self.cards]
            for index, package in enumerate(self.order):
                card = self.cards[package]; card.set_width(width)
                self.grid.addWidget(card, index // columns, index % columns); card.show()
                if previous: QWidget.setTabOrder(previous, card)
                previous = card
            if keep in self.order: self.cards[keep].setFocus()

        def eventFilter(self, watched, event):
            # The viewport gets its size after the window's own resize event, so only
            # its own Resize carries the width the grid must fill.
            if watched is self.scroll.viewport() and event.type() == QEvent.Type.Resize:
                if grid_metrics(self.scroll.viewport().width() - GUTTER, *self.card_limits) != self.metrics: self.reflow()
            return super().eventFilter(watched, event)

        def resizeEvent(self, event):
            super().resizeEvent(event)
            self.layout_overlays()
            self.google_button.setText("Google account" if self.width() >= 900 else "")

        def changeEvent(self, event):
            super().changeEvent(event)
            if event.type() in (QEvent.Type.ActivationChange, QEvent.Type.WindowStateChange) \
                    and self.isActiveWindow() and not self.isMinimized():
                self.refresh()
            elif event.type() in (QEvent.Type.PaletteChange, QEvent.Type.ApplicationPaletteChange) and not self.theming:
                self.theming = True; self.apply_theme(); self.theming = False

        def closeEvent(self, event):
            self.settings.setValue("geometry", self.saveGeometry())
            self.stop_jobs()
            super().closeEvent(event)

        def stop_jobs(self, grace=2.0):
            """End the commands this window started, before Qt destroys them. A QProcess destroyed while it runs emits
            `finished` from its own destructor, and the slot then reads a QProcess that is already being deleted: the
            RuntimeError is unhandled inside a slot, so PyQt aborts the process (SIGABRT) on every close that finds a
            poll in flight. The signals are cut first, so no slot runs against a half-destroyed window; the commands get
            `grace` seconds to finish (a launch hands its session over) and are killed after that."""
            jobs = list(self.jobs)
            self.jobs.clear()
            for process in jobs:
                for signal in (process.finished, process.errorOccurred):
                    try: signal.disconnect()
                    except (TypeError, RuntimeError): pass
            deadline = time.monotonic() + grace
            for process in jobs:
                if process.state() == QProcess.ProcessState.NotRunning: continue
                if not process.waitForFinished(max(1, int((deadline - time.monotonic()) * 1000))):
                    process.kill(); process.waitForFinished(2000)

        def tick(self):
            """Poll the library, less often when nothing is happening and not at all when minimized."""
            busy = bool(self.pending) or any(a["session"].get("state") in ACTIVE for a in self.apps)
            wait = poll_wait(self.isMinimized() or not self.isVisible(), self.isActiveWindow(), busy)
            if wait is not None and time.monotonic() - self.polled >= wait: self.refresh()

        def set_sort(self, mode, save=True):
            labels = dict(SORTS)
            self.sort_mode = mode if mode in labels else "name"
            self.sort_button.setText("Sort: " + labels[self.sort_mode])
            for action in self.sort_group.actions(): action.setChecked(action.data() == self.sort_mode)
            if save: self.settings.setValue("sort", self.sort_mode); self.render(True)

        def apply_theme(self):
            """Stylesheet and glyphs follow the palette, including a theme change while running."""
            self.setStyleSheet(make_style(theme(), self.big))
            self.search_action.setIcon(glyph("search", "muted")); self.google_button.setIcon(glyph("person", "text"))
            self.install_button.setIcon(glyph("plus", "onaccent")); self.empty_install.setIcon(glyph("plus", "onaccent"))
            for card in self.cards.values(): card.art = None; card.update_app(card.app, self.pending.get(card.app["package"]))
            self.render(True)

        def activate_card(self, app):
            if card_state(app, self.pending.get(app["package"]))["enabled"]: self.act(app)

        def move_focus(self, package, dx, dy):
            if package not in self.order: return
            index = self.order.index(package) + dx + dy * self.columns
            if 0 <= index < len(self.order):
                card = self.cards[self.order[index]]; card.setFocus(); self.scroll.ensureWidgetVisible(card)

        def build_menu(self, app):
            menu = QMenu(self); rule = card_state(app, self.pending.get(app["package"]))
            primary = menu.addAction(rule["text"]); primary.setEnabled(rule["enabled"])
            primary.triggered.connect(lambda: self.act(app))
            menu.addSeparator()
            menu.addAction("App settings…").triggered.connect(lambda: self.configure(app))
            session = menu.addAction("Last session"); session.triggered.connect(lambda: self.open_drawer(app["package"]))
            session.setEnabled(bool(app["session"].get("session_id") or app.get("error")))
            menu.addSeparator()
            running = app["session"].get("state") in ACTIVE
            for text, command in (("Clear data…", "clear-data"), ("Uninstall…", "uninstall")):
                action = menu.addAction(text); action.setEnabled(not running)
                action.setToolTip("Stop the app first" if running else "")
                action.triggered.connect(lambda _, command=command: self.remove_app(app, command))
            return menu

        def menu_entries(self, app):
            """The card menu as sheet entries: (text, run, enabled, tone)."""
            rule = card_state(app, self.pending.get(app["package"])); running = app["session"].get("state") in ACTIVE
            return [(rule["text"], lambda: self.act(app), rule["enabled"], ""),
                    ("App settings…", lambda: self.configure(app), True, ""),
                    ("Last session", lambda: self.open_drawer(app["package"]),
                     bool(app["session"].get("session_id") or app.get("error")), ""),
                    ("Clear data…", lambda: self.remove_app(app, "clear-data"), not running, ""),
                    ("Uninstall…", lambda: self.remove_app(app, "uninstall"), not running, "danger")]

        def card_menu(self, app, position, keyboard=False):
            # The gamepad layout, and a key (the keyboard's Menu key, the pad's Y), get a sheet inside this window; a popup
            # menu is a window of its own, and a mouse's context menu stays one.
            if self.big or keyboard: self.show_sheet(app["label"], self.menu_entries(app))
            else: self.build_menu(app).popup(position)

        def dropped(self, event):
            return [url.toLocalFile() for url in event.mimeData().urls() if url.isLocalFile()]

        def show_drop_hint(self):
            body = self.centralWidget(); origin = self.pages.mapTo(body, QPoint(0, 0))
            self.drop_hint.setGeometry(origin.x(), origin.y(), body.width() - origin.x() - 24, self.pages.height())
            self.drop_hint.raise_(); self.drop_hint.show()

        def dragEnterEvent(self, event):
            if installable(self.dropped(event)):
                event.acceptProposedAction(); self.show_drop_hint()

        def dragLeaveEvent(self, event):
            self.drop_hint.hide()

        def dropEvent(self, event):
            self.drop_hint.hide()
            paths = installable(self.dropped(event))
            if paths: event.acceptProposedAction(); self.install_files(paths)

        def app(self, package):
            return next((a for a in self.apps if a["package"] == package), None)

        def select(self, package):
            self.selected = package
            for name, card in self.cards.items(): card.set_selected(name == package)
            if self.drawer.isVisible() and package != self.drawer_package: self.open_drawer(package)
            self.update_hints()

        def act(self, app):
            package = app["package"]
            if card_state(app, self.pending.get(package))["action"] == "stop": return self.stop_app(app)
            if card_state(app, self.pending.get(package))["action"] == "resume":
                def resumed(code, output):
                    if code: self.show_notice(short_error(output), error=True, package=package)
                    self.refresh()
                self.command(["resume", package], resumed)
                return
            args = launch_request(app, self.trace.isChecked())
            self.pending[package] = "play"; self.render()
            def started(code, output):
                self.pending.pop(package, None)
                if code:
                    self.show_notice(f"{app['label']} didn't start: {short_error(output)}", error=True, package=package)
                self.refresh()
            self.command(args, started)

        def google_account(self):
            GoogleAccounts(self).exec()

        def google_request(self, listed):
            """Say which app an open Google window belongs to, while its window is up."""
            window = next((a for a in listed if a.get("system") and a["package"] == "com.google.android.gms"
                           and a["session"].get("state") in ACTIVE), None)
            requester = window.get("requester") if window else None
            self.google_requester = requester["label"] if isinstance(requester, dict) and requester.get("label") else None
            text = self.google_requester and f"{self.google_requester} is asking you to sign in with Google. Finish in the Google window."
            if text and self.notice_text.text() != text:
                self.show_notice(text)
            elif not text and self.notice_text.text().endswith("Finish in the Google window."):
                self.notice.hide()

        def stop_app(self, app):
            package = app["package"]
            self.pending[package] = "stop"; self.render()
            def stopped(code, output):
                self.pending.pop(package, None)
                if code: self.show_notice(f"Couldn't stop {app['label']}: {short_error(output)}", error=True, package=package)
                self.refresh()
            self.command(["stop", package], stopped)

        def configure(self, app):
            if not PACKAGE.fullmatch(app["package"]): return
            directory = self.state / app["package"]
            opened = load_settings(directory)
            dialog = settings_dialog(self, app, opened)
            if dialog.exec() == QDialog.DialogCode.Accepted:
                try:
                    # Apply only this dialog's edits: `asgard config` may have saved others while it was open.
                    edited = dialog.result_settings(); current = load_settings(directory)
                    for key in opened.keys() | edited.keys():
                        if key not in edited: current.pop(key, None)
                        elif edited[key] != opened.get(key): current[key] = edited[key]
                    save_settings(directory, current)
                    if current.get("menu_entry") != opened.get("menu_entry"):
                        import android_package
                        android_package.update_desktop(self.state, app["package"])
                    if app["session"].get("state") in ACTIVE:
                        self.show_notice(f"Settings saved. They apply the next time {app['label']} starts.", timeout=6000)
                except OSError as error:
                    self.show_notice("Couldn't save settings: " + str(error), error=True)

        def remove_app(self, app, command):
            """Ask, then clear or uninstall; True once the command was started."""
            removing = command == "uninstall"
            question = (f"Uninstall {app['label']}? Its saves and settings are deleted too." if removing else
                        f"Delete all saves and settings of {app['label']}? The app stays installed.")
            if not confirm(self, "Uninstall" if removing else "Clear data", question + "\n\nThis cannot be undone.",
                           "Uninstall" if removing else "Clear data"): return False
            package = app["package"]
            def removed(code, output):
                if code:
                    self.show_notice(f"Couldn't {'uninstall' if removing else 'clear data of'} {app['label']}: "
                                     f"{short_error(output)}", error=True)
                else:
                    self.show_notice(f"{app['label']} {'was uninstalled' if removing else 'now has no saved data'}.", timeout=6000)
                    if removing and self.drawer_package == package: self.close_drawer()
                self.refresh()
            self.command([command, "--yes", package], removed)
            return True

        def show_notice(self, text, error=False, package=None, timeout=0, action=None):
            """`action` is (button text, what it runs): a way to fix what the notice says."""
            self.notice_icon.setPixmap(glyph("alert", "danger").pixmap(ICON, ICON) if error
                                       else glyph("info", "accent").pixmap(ICON, ICON))
            self.notice_text.setText(text)
            self.notice_action_run = action[1] if action else None
            self.notice_action.setVisible(bool(action))
            if action: self.notice_action.setText(action[0])
            self.notice_details.setProperty("package", package); self.notice_details.setVisible(bool(package))
            self.notice.show()
            if timeout: self.notice_timer.start(timeout)
            else: self.notice_timer.stop()

        def drawer_app(self):
            return self.app(self.drawer_package) or {"session": {}}

        def open_drawer(self, package):
            if not package: return
            self.drawer_package, self.drawer_stamp = package, None
            if self.selected != package: self.select(package)
            self.drawer.show(); self.update_drawer(force=True); self.update_hints()

        def close_drawer(self):
            self.drawer.hide(); self.drawer_package = None; self.update_hints()

        def update_drawer(self, force=False):
            if not self.drawer.isVisible(): return
            app = self.app(self.drawer_package)
            if app is None: self.close_drawer(); return
            session = app["session"]; log = session_log(session)
            try: info = Path(log).stat() if log else None
            except OSError: info = None
            stamp = drawer_stamp(session, info, self.developer_mode())
            # A growing log of a live game is rescanned at most every five seconds.
            if not force and (stamp == self.drawer_stamp or (session.get("state") in ACTIVE and self.drawer_stamp
                    and stamp[:4] == self.drawer_stamp[:4] and time.monotonic() - self.drawer_scanned < 5)):
                return
            self.drawer_stamp, self.drawer_scanned = stamp, time.monotonic()
            self.drawer_title.setText(app["label"])
            described = describe_session(session)
            parts = [f"<p><b>{html.escape(described['outcome'])}</b>"]
            if described["when"]:
                parts.append("<br>Started " + html.escape(described["when"]) +
                             (" · " + html.escape(described["duration"]) if described["duration"] else ""))
            parts.append("</p>")
            if described["advice"]: parts.append(f"<p>{html.escape(described['advice'])}</p>")
            if session.get("phase"):
                prefix = "Current step" if session.get("state") in ACTIVE else "Last step"
                parts.append(f"<p>{prefix}: {html.escape(session['phase'])}</p>")
            elif session.get("state") == "preparing":
                parts.append("<p>Preparing Android runtime or optimizing app code. The app window has not opened yet.</p>")
            if described["error"] or app.get("error"):
                parts.append(f"<p>{html.escape(str(described['error'] or app['error']))}</p>")
            if info:
                try: findings = read_findings(log)
                except OSError: findings = []
                if findings:
                    parts.append("<p><b>What the log shows</b></p><ul style='margin-left: -24px'>")
                    for item in findings:
                        sources = (" — " + ", ".join(item["sources"])) if item["sources"] else ""
                        parts.append(f"<li>{html.escape(item['title'])}{html.escape(sources)}: {item['count']:,} "
                                     f"line{'s' * (item['count'] != 1)}<br><small>{html.escape(item['example'])}</small></li>")
                    parts.append("</ul>")
                    if all(item["title"] in ("Ad SDK errors", "Google Play services unavailable") for item in findings):
                        parts.append("<p><small>Ad and Play services errors are expected here: Google Play "
                                     "services are not available, and apps usually keep running.</small></p>")
                else:
                    parts.append("<p>No known problem signatures in the log.</p>")
            elif session.get("session_id"):
                parts.append("<p>This session has no log.</p>")
            else:
                parts.append("<p>This app hasn't been played yet.</p>")
            if self.developer_mode() and session.get("session_id"):
                parts.append(f"<p><small>Session {html.escape(session['session_id'])} · PID {session.get('pid') or '—'}"
                             f" · exit {session.get('exit_code') if session.get('exit_code') is not None else '—'}</small></p>")
            self.drawer_body.setText("".join(parts))
            self.open_log.setEnabled(bool(info)); self.open_folder.setVisible(self.developer_mode() and bool(log))

        def open_path(self, path):
            if path: QDesktopServices.openUrl(QUrl.fromLocalFile(path))

        def choose(self):
            path = QFileDialog.getExistingDirectory(self, "Choose app library folder", str(self.state))
            if path:
                self.state = Path(path); self.path.setText(path); self.apps = []
                self.close_drawer(); self.render(True); self.refresh()

        def check_setup(self):
            """Ask `asgard setup --check` what this tree still lacks, and offer to fix it. Unchanged when nothing is missing."""
            def checked(code, output):
                try: status = json.loads(output)
                except ValueError: return          # an older Asgard without the command: say nothing
                self.setup_status = status if status.get("missing") else None
                if self.setup_status: self.offer_setup()
                elif self.notice_action_run == self.setup_asgard: self.notice.hide()
            self.command(["setup", "--check", "--json"], checked)

        def offer_setup(self):
            """The notice that says what is missing: with a button where this tree can set itself up."""
            status = self.setup_status
            titles = [item["title"] for item in status["missing"]]
            shown = ", ".join(titles[:3]) + (f" and {len(titles) - 3} more" if len(titles) > 3 else "")
            if status["can_set_up"]:
                self.show_notice(f"Asgard isn't set up yet: it still needs {shown}. Apps can't be installed or started until then.",
                                 error=True, action=("Set up…", self.setup_asgard))
            else:
                self.show_notice(f"This copy of Asgard is incomplete ({shown} missing). Install it again from the source "
                                 "checkout with: make install-asgard", error=True)

        def setup_asgard(self):
            """Download and build what is missing (`asgard setup`), after saying what that involves."""
            if self.setting_up or not self.setup_status or not self.setup_status["can_set_up"]: return
            steps = "\n".join(f"• {item['title']}: {item['why']}" for item in self.setup_status["missing"])
            if not confirm(self, "Set up Asgard",
                           "Asgard will download and build the following. This needs an internet connection and can take a "
                           "while: a full setup is about a gigabyte. It keeps what it finishes, so a failure or closing "
                           f"this window doesn't lose it, and running it again resumes.\n\n{steps}", "Set up", danger=False):
                return
            self.setting_up, self.setup_step = True, ""; self.notice.hide()
            self.progress_text.setText("Setting up Asgard…"); self.progress.show()
            def progress(line):
                text = line[3:] if line.startswith("== ") else None
                if text: self.setup_step = text
                self.progress_text.setText("Setting up Asgard: " + (self.setup_step or "starting")
                                           + ("" if text else " (" + line[:80] + ")"))
            def finished(code, output):
                self.setting_up = False; self.progress.hide()
                if code:
                    self.show_notice("Setup stopped: " + short_error(output), error=True,
                                     action=("Try again", self.check_setup))
                else:
                    self.show_notice("Asgard is set up. You can install apps now.", timeout=10000)
                    self.setup_status = None
                self.refresh()
            self.command(["setup"], finished, progress=progress)

        def install(self):
            paths, _ = QFileDialog.getOpenFileNames(self, "Install Android apps", str(Path.home()),
                "Android packages (*.apk *.apkm *.apks *.xapk);;All files (*)")
            self.install_files(paths)

        def install_files(self, paths):
            """Queue packages; they install one at a time, and anything already queued keeps its place."""
            paths = installable(paths)
            if not paths: return
            if self.setup_status and self.install_batch is None:
                self.offer_setup(); return
            fresh = self.install_batch is None
            if fresh:
                self.install_batch = {"total": 0, "done": 0, "installed": 0, "failed": [], "announce": None}
                self.notice.hide()
            self.install_queue.extend(paths); self.install_batch["total"] += len(paths)
            if fresh: self.install_next()

        def install_next(self):
            batch = self.install_batch
            if not self.install_queue: return self.install_finished()
            path = self.install_queue.pop(0); name = Path(path).name; started = time.monotonic()
            def tick():
                position = f" ({batch['done'] + 1} of {batch['total']})" if batch["total"] > 1 else ""
                self.progress_text.setText(f"Installing {name}{position}… verifying signatures and unpacking "
                                           f"({span(time.monotonic() - started)})")
            ticker = QTimer(self); ticker.timeout.connect(tick); ticker.start(1000); tick(); self.progress.show()
            def installed(code, output):
                ticker.stop(); ticker.deleteLater()
                batch["done"] += 1
                if code: batch["failed"].append((name, short_error(output)))
                else:
                    batch["installed"] += 1
                    match = re.search(r"^Installed (\S+) ", output, re.M)
                    if match: batch["announce"] = match[1]
                self.install_next()
            self.command(["install", path], installed)

        def install_finished(self):
            batch, self.install_batch = self.install_batch, None
            self.progress.hide()
            if any("not provisioned" in error for _, error in batch["failed"]): self.check_setup()
            if batch["failed"]:
                name, error = batch["failed"][0]
                more = f" ({len(batch['failed']) - 1} more failed)" if len(batch["failed"]) > 1 else ""
                done = f"{batch['installed']} of {batch['total']} installed. " if batch["total"] > 1 else ""
                self.show_notice(f"{done}Couldn't install {name}: {error}{more}", error=True)
            elif batch["total"] > 1:
                self.show_notice(f"{batch['installed']} apps installed.", timeout=8000)
            # A lone install is announced by the first listing that contains the package.
            if batch["installed"] == 1: self.announce = batch["announce"]
            self.refresh()

    window = Library()
    backend = android_gamepad.open_backend() if pads is None else (pads or None)
    if backend: android_gamepad.connect(window, application, backend)
    elif pads is None: print("Asgard: gamepad control is off: " + android_gamepad.STATUS["reason"], file=sys.stderr)
    window.apply_theme(); window.update_hints()
    return window


def show(state, fullscreen=False):
    import fcntl
    state = Path(state).resolve()
    try:
        from PyQt6.QtCore import QTimer
        from PyQt6.QtNetwork import QLocalServer
        from PyQt6.QtWidgets import QApplication
    except ImportError as error:
        raise ValueError("The desktop launcher requires PyQt6; the CLI works without it") from error
    application = QApplication.instance() or QApplication([sys.argv[0]])
    # Ctrl-C ends the event loop. Left alone, KeyboardInterrupt is raised inside whichever timer slot runs, and PyQt aborts the
    # process (SIGABRT) on an exception in a slot. The timers keep Python running, so the handler runs promptly; it asks the
    # event loop to quit, so a Ctrl-C that arrives before the loop has started is not lost or acted on half way through startup.
    import signal
    signal.signal(signal.SIGINT, lambda *_: QTimer.singleShot(0, application.quit))
    directory = state / '.asgard'
    directory.mkdir(parents=True, exist_ok=True)
    server = QLocalServer(application)
    server.setSocketOptions(QLocalServer.SocketOption.UserAccessOption)
    with (directory / 'launcher.lock').open('a+b') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if activate_existing(state): return 0
        path = launcher_socket(state)
        if path.exists() or path.is_symlink():
            info = path.lstat()
            if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
                raise ValueError('Refusing to replace foreign launcher socket')
            path.unlink()
        if not server.listen(str(path)): raise RuntimeError(server.errorString())
    window = build(state, application)
    # Full screen when asked for, and in Steam's Game Mode, where a pad is all there is.
    window.fullscreen_wanted = fullscreen or android_gamepad.in_game_mode()
    window.present()
    window.check_setup()
    def activate():
        while server.hasPendingConnections():
            client = server.nextPendingConnection()
            def receive(client=client):
                if not client.canReadLine(): return
                if bytes(client.readLine(64)) != b'activate\n':
                    client.abort(); return
                if window.state != state:
                    window.state = state; window.path.setText(str(state)); window.refresh()
                window.present()
                client.write(b'ok\n'); client.flush(); client.disconnectFromServer()
            client.readyRead.connect(receive)
            client.disconnected.connect(client.deleteLater)
            timer = QTimer(client); timer.setSingleShot(True)
            timer.timeout.connect(client.abort); timer.start(1000)
            receive()
    server.newConnection.connect(activate)
    # Any way out of the event loop (the window closing, Ctrl-C asking it to quit) ends the window's commands while their
    # objects are still alive; Python's teardown destroys them otherwise, mid-signal.
    application.aboutToQuit.connect(window.stop_jobs)
    return application.exec()
