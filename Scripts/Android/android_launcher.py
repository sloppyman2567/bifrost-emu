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

from android_package import PACKAGE, ROOT
from android_settings import BACK_KEYS, ENTER_KEYS, JIT_DISK_CACHE_SIZES, JIT_DISK_CACHES, NETWORKS, STATS_MODES, load as load_settings, save as save_settings
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


def launch_request(app, interpreter=False, trace=False):
    """CLI arguments for one Play click; the saved per-app settings are applied by the command."""
    args = ["start", app["package"]]
    if interpreter: args.append("--no-jit")
    # Call tracing follows ART methods, so NativeActivity apps launch without it.
    if trace and app.get("launcher") == "managed_activity": args += ["--trace-apk", "calls"]
    return args


INSTALLABLE = (".apk", ".apkm", ".apks", ".xapk")
SORTS = (("name", "Name"), ("played", "Recently played"))
FAILED = ("failed", "timed_out", "interrupted")
MIN_CARD, MAX_CARD, GAP = 208, 280, 18
GUTTER = 10  # between the cards and the scrollbar


def installable(paths):
    """The existing package files among chosen or dropped paths."""
    return [str(path) for path in paths if Path(path).suffix.lower() in INSTALLABLE and Path(path).is_file()]


def sort_apps(apps, mode):
    name = lambda app: (app["label"].casefold(), app["package"])
    if mode == "played":
        return sorted(apps, key=lambda app: (-(app["session"].get("started_at") or 0), *name(app)))
    return sorted(apps, key=name)


def grid_metrics(width):
    """Columns and card width that fill `width`: cards stretch between MIN_CARD and MAX_CARD."""
    columns = max(1, (width + GAP) // (MIN_CARD + GAP))
    return columns, min(MAX_CARD, (width - (columns - 1) * GAP) // columns)


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


def clock(stamp, now):
    moment, today = time.localtime(stamp), time.localtime(now)
    day = (time.strftime("Today", moment) if moment[:3] == today[:3] else
           "Yesterday" if time.localtime(now - 86400)[:3] == moment[:3] else
           time.strftime("%b %d" if moment.tm_year == today.tm_year else "%b %d, %Y", moment))
    return day + time.strftime(" %H:%M", moment)


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
    ("crash", "Crashed in native code", "The app's native code hit a fault. Try again; if it keeps happening, the log shows where."),
    ("signal", "Crashed in native code", "The app's native code hit a fault. Try again; if it keeps happening, the log shows where."),
    ("java", "Crashed in Java code", "The app hit an unhandled error. Try again; if it keeps happening, the log shows where."),
    ("anr", "Stopped responding", "The app stopped responding and was ended. Try again; slow first launches can do this."),
    ("license", "Closed itself: Google Play licence check",
     "This app asks the Google Play Store to confirm it was installed from Play. Bifrost has no Play Store, so the check "
     "fails and the app quits. Bifrost does not fake the answer, so this app cannot run here."),
)
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
        if key in found: return line, advice
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


def settings_dialog(window, app, settings):
    """Edit one persistent app profile; changes are staged until Save."""
    from PyQt6.QtCore import Qt, QUrl
    from PyQt6.QtGui import QDesktopServices, QIcon, QPalette
    from PyQt6.QtWidgets import (QAbstractItemView, QCheckBox, QComboBox, QDialog, QDialogButtonBox,
                                 QFormLayout, QHeaderView, QHBoxLayout, QLabel, QPushButton,
                                 QTableWidget, QTableWidgetItem, QTabWidget, QVBoxLayout, QWidget)
    import android_settings
    from android_package import API

    class Settings(QDialog):
        def __init__(self):
            super().__init__(window)
            self.setWindowTitle(app["label"] + " settings")
            self.resize(620, 500)
            self.settings = dict(settings)
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
            import android_desktop
            self.menu_default = android_desktop.default_on(window.state)
            self.menu = QCheckBox("Show a launcher for this app in the desktop's application menu")
            self.menu.setChecked(settings.get("menu_entry", self.menu_default)); form.addRow("Application menu", self.menu)
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
                removal.addWidget(button)
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
                            self.network.currentData(), self.menu.isChecked())

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
            self.disk_cache_mb.setCurrentIndex(0); self.network.setCurrentIndex(0); self.menu.setChecked(self.menu_default)
            self.initial = (False, "normal", "escape", "enter", "default", "default", "auto", self.menu_default)
            self.emulator.setCurrentIndex(0); self.properties.setRowCount(0); self.error.hide()

        def result_settings(self):
            result = {k: v for k, v in self.settings.items() if not android_settings.property_name(k)}
            for key, value, initial in zip(("window_stats", "window_stats_mode", "back_key", "enter_key", "jit_disk_cache", "jit_disk_cache_mb",
                     "network", "menu_entry"),
                    (self.stats.isChecked(), self.stats_mode.currentData(), self.back.currentData(),
                     self.enter.currentData(), self.disk_cache.currentData(), self.disk_cache_mb.currentData(),
                     self.network.currentData(), self.menu.isChecked()), self.initial):
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


def build(state, application, settings=None):
    """The library window without its single-instance socket or event loop (show() adds both)."""
    try:
        from PyQt6.QtCore import QEvent, QPoint, QPointF, QProcess, QProcessEnvironment, QRectF, QSettings, QSize, QTimer, Qt, QUrl
        from PyQt6.QtGui import (QActionGroup, QColor, QDesktopServices, QIcon, QImage, QPainter, QPainterPath,
                                 QPalette, QPixmap)
        from PyQt6.QtWidgets import (QCheckBox, QDialog, QFileDialog, QFrame, QGridLayout, QHBoxLayout, QLabel,
                                     QLineEdit, QListWidget, QMainWindow, QMenu, QMessageBox, QProgressBar, QPushButton,
                                     QScrollArea, QSizePolicy, QStackedWidget, QToolButton, QVBoxLayout, QWidget)
    except ImportError as error:
        raise ValueError("The desktop launcher requires PyQt6; the CLI works without it") from error
    application.setApplicationName("Asgard")
    icon_size, spacing = 88, GAP

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

    def make_style(colors):
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
        """

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

    def adaptive_image(layers, side):
        """Android's composition of an adaptive icon (shared with the menu entries). Null if a layer will not load, so the
        card falls back to its placeholder."""
        import android_desktop
        return android_desktop.compose_adaptive(layers, side)

    def artwork(app, size, ratio, palette):
        """Rounded launcher tile at device pixels: the icon filling it (a padded icon is cropped to its
        pixels), or a tinted tile with the game's initial; a hairline keeps pale icons defined."""
        pixmap = QPixmap(round(size * ratio), round(size * ratio))
        pixmap.setDevicePixelRatio(ratio); pixmap.fill(Qt.GlobalColor.transparent)
        painter = QPainter(pixmap)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing); painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        path = QPainterPath(); path.addRoundedRect(QRectF(0, 0, size, size), size * .22, size * .22)
        layers = app.get("icon_layers")
        image = adaptive_image(layers, round(size * ratio)) if layers else QImage(app["icon"]) if app.get("icon") else QImage()
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

    def confirm(parent, title, text, action):
        """Ask before something destructive, with the action on the button and no theme clip-art."""
        box = QMessageBox(parent); box.setWindowTitle(title); box.setText(text); box.setIcon(QMessageBox.Icon.NoIcon)
        accept = box.addButton(action, QMessageBox.ButtonRole.DestructiveRole)
        cancel = box.addButton(QMessageBox.StandardButton.Cancel); box.setDefaultButton(cancel); box.setEscapeButton(cancel)
        accept.setObjectName("danger"); restyle(accept)
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
            self.meta_text = None
            art = (app.get("icon"), str(app.get("icon_layers")), app["label"], ratio, self.palette().cacheKey())
            if art != self.art:
                self.art = art; self.icon.setPixmap(artwork(app, icon_size, ratio, self.palette()))
            self.relabel()
            version = app.get("version_name") or app.get("version")
            moment = time.localtime(app["installed_at"]) if app.get("installed_at") else None
            installed = moment and "Installed " + time.strftime("%b %d" + ("" if moment.tm_year == time.localtime().tm_year
                                                                           else ", %Y"), moment)
            self.meta_text = " · ".join(str(x) for x in (version, installed) if x) or " "
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
            elif key == Qt.Key.Key_Menu: self.window.card_menu(self.app, self.mapToGlobal(self.rect().center()))
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
            self.metrics, self.polled, self.install_queue, self.install_batch = None, 0.0, [], None
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
            close = QToolButton(); close.setAutoRaise(True)
            close.setObjectName("tool"); close.setIcon(glyph("close", "muted")); close.clicked.connect(self.notice.hide)
            row.addWidget(self.notice_icon); row.addWidget(self.notice_text, 1); row.addWidget(self.notice_details); row.addWidget(close)
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
            outer.addWidget(self.build_developer())
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
            self.render(True); self.refresh()

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
            self.interpreter = QCheckBox("Run in the interpreter (very slow; for debugging emulator bugs)")
            self.trace = QCheckBox("Trace Java framework calls (apps with a Java Activity only)")
            self.trace.setToolTip("NativeActivity apps launch without tracing")
            row = QHBoxLayout(); self.path = muted(QLabel()); self.path.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
            choose = QPushButton("Choose library…"); choose.clicked.connect(self.choose)
            row.addWidget(QLabel("Library folder:")); row.addWidget(self.path, 1); row.addWidget(choose)
            for widget in (self.interpreter, self.trace): panel.addWidget(widget)
            panel.addLayout(row)
            self.path.setText(str(self.state)); self.developer.hide(); layout.addWidget(self.developer)
            return box

        def developer_mode(self):
            return self.developer_toggle.isChecked()

        def toggle_developer(self, shown):
            self.developer_toggle.setArrowType(Qt.ArrowType.DownArrow if shown else Qt.ArrowType.RightArrow)
            self.developer.setVisible(shown)
            if not shown: self.interpreter.setChecked(False); self.trace.setChecked(False)
            self.path.setText(str(self.state))
            for card in self.cards.values(): card.developer.setVisible(shown)
            self.render(True); self.update_drawer(force=True)

        def command(self, args, done=None, env=None):
            process = QProcess(self)
            process.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
            if env:
                environment = QProcessEnvironment.systemEnvironment()
                for key, value in env.items(): environment.insert(key, value)
                process.setProcessEnvironment(environment)
            self.jobs.add(process)
            def finished(code, status):
                output = bytes(process.readAllStandardOutput()).decode(errors="replace")
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
            running = sum(a["session"].get("state") in ACTIVE for a in self.apps)
            self.summary.setText(f"{len(self.apps)} app{'s' * (len(self.apps) != 1)}" + (f" · {running} running" if running else ""))
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

        def reflow(self, order=None):
            if order is not None: self.order = order
            columns, width = grid_metrics(self.scroll.viewport().width() - GUTTER)
            self.columns, self.metrics = columns, (columns, width)
            for card in self.cards.values():
                self.grid.removeWidget(card); card.hide()
            previous = None
            for index, package in enumerate(self.order):
                card = self.cards[package]; card.set_width(width)
                self.grid.addWidget(card, index // columns, index % columns); card.show()
                if previous: QWidget.setTabOrder(previous, card)
                previous = card

        def eventFilter(self, watched, event):
            # The viewport gets its size after the window's own resize event, so only
            # its own Resize carries the width the grid must fill.
            if watched is self.scroll.viewport() and event.type() == QEvent.Type.Resize:
                if grid_metrics(self.scroll.viewport().width() - GUTTER) != self.metrics: self.reflow()
            return super().eventFilter(watched, event)

        def resizeEvent(self, event):
            super().resizeEvent(event)
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
            super().closeEvent(event)

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
            self.setStyleSheet(make_style(theme()))
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

        def card_menu(self, app, position):
            self.build_menu(app).popup(position)

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

        def act(self, app):
            package = app["package"]
            if card_state(app, self.pending.get(package))["action"] == "stop": return self.stop_app(app)
            if card_state(app, self.pending.get(package))["action"] == "resume":
                def resumed(code, output):
                    if code: self.show_notice(short_error(output), error=True, package=package)
                    self.refresh()
                self.command(["resume", package], resumed)
                return
            args = launch_request(app, self.interpreter.isChecked(), self.trace.isChecked())
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

        def show_notice(self, text, error=False, package=None, timeout=0):
            self.notice_icon.setPixmap(glyph("alert", "danger").pixmap(ICON, ICON) if error
                                       else glyph("info", "accent").pixmap(ICON, ICON))
            self.notice_text.setText(text)
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
            self.drawer.show(); self.update_drawer(force=True)

        def close_drawer(self):
            self.drawer.hide(); self.drawer_package = None

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

        def install(self):
            paths, _ = QFileDialog.getOpenFileNames(self, "Install Android apps", str(Path.home()),
                "Android packages (*.apk *.apkm *.apks *.xapk);;All files (*)")
            self.install_files(paths)

        def install_files(self, paths):
            """Queue packages; they install one at a time, and anything already queued keeps its place."""
            paths = installable(paths)
            if not paths: return
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

    window = Library(); window.apply_theme()
    return window


def show(state):
    import fcntl
    state = Path(state).resolve()
    try:
        from PyQt6.QtCore import QTimer
        from PyQt6.QtNetwork import QLocalServer
        from PyQt6.QtWidgets import QApplication
    except ImportError as error:
        raise ValueError("The desktop launcher requires PyQt6; the CLI works without it") from error
    application = QApplication.instance() or QApplication([sys.argv[0]])
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
    window.show()
    def activate():
        while server.hasPendingConnections():
            client = server.nextPendingConnection()
            def receive(client=client):
                if not client.canReadLine(): return
                if bytes(client.readLine(64)) != b'activate\n':
                    client.abort(); return
                if window.state != state:
                    window.state = state; window.path.setText(str(state)); window.refresh()
                window.showNormal(); window.raise_(); window.activateWindow()
                client.write(b'ok\n'); client.flush(); client.disconnectFromServer()
            client.readyRead.connect(receive)
            client.disconnected.connect(client.deleteLater)
            timer = QTimer(client); timer.setSingleShot(True)
            timer.timeout.connect(client.abort); timer.start(1000)
            receive()
    server.newConnection.connect(activate)
    return application.exec()
