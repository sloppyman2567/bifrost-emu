"""Desktop game library using the same CLI and process supervisor as terminal launches."""
import html
import json
from pathlib import Path
import re
import sys
import time

from android_package import PACKAGE, ROOT

ACTIVE = ("preparing", "starting", "running", "stopping")
LAUNCHABLE = ("managed_activity", "native_activity")
STATES = {"preparing": "Starting", "starting": "Starting", "running": "Running", "stopping": "Stopping",
          "stopped": "Stopped", "exited": "Exited normally", "closed": "Closed normally",
          "failed": "Failed", "timed_out": "Timed out", "interrupted": "Ended unexpectedly",
          "invalid": "Install is damaged"}
# Names accepted by the emulator's [android] back_key / BIFROST_ANDROID_BACK_KEY.
BACK_KEYS = (("escape", "Esc"), ("backspace", "Backspace"), ("delete", "Delete"),
             ("f1", "F1"), ("f2", "F2"), ("f12", "F12"), ("none", "No back key"))
SETTINGS = "launcher-settings.json"
# First match wins: a Chartboost line about missing Play services counts once.
SIGNATURES = (
    ("crash", "Emulator caught a guest crash", re.compile(r"^\[crash\] ")),
    ("signal", "Native crash (Fatal signal)", re.compile(r"Fatal signal \d+")),
    ("java", "Java crash (FATAL EXCEPTION)", re.compile(r"FATAL EXCEPTION")),
    ("anr", "App not responding (ANR)", re.compile(r"\bANR in ")),
    ("play", "Google Play services unavailable", re.compile(r"GooglePlayServicesNotAvailableException")),
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


def load_settings(app):
    """Per-game launcher settings; unknown or malformed values fall back to defaults."""
    try:
        saved = json.loads((app / SETTINGS).read_text())
    except (OSError, ValueError):
        return {}
    settings = {}
    if isinstance(saved, dict):
        if type(saved.get("window_stats")) is bool: settings["window_stats"] = saved["window_stats"]
        if isinstance(saved.get("back_key"), str) and saved["back_key"] in dict(BACK_KEYS):
            settings["back_key"] = saved["back_key"]
    return settings


def save_settings(app, settings):
    from android_sessions import publish
    publish(app / SETTINGS, settings)


def launch_request(app, settings, interpreter=False, trace=False):
    """CLI arguments and environment for one Play click; defaults add nothing."""
    args = ["start", app["package"]]
    if "window_stats" in settings:
        args.append("--window-stats" if settings["window_stats"] else "--no-window-stats")
    if interpreter: args.append("--no-jit")
    # Call tracing follows ART methods, so NativeActivity apps launch without it.
    if trace and app.get("launcher") == "managed_activity": args += ["--trace-apk", "calls"]
    env = {"BIFROST_ANDROID_BACK_KEY": settings["back_key"]} if "back_key" in settings else {}
    return args, env


def short_error(output, fallback="The command failed"):
    lines = [x.strip() for x in output.splitlines() if x.strip()]
    if not lines: return fallback
    text = re.sub(r"^(?:Android package|Asgard): ", "", lines[-1])
    return text if len(text) <= 300 else text[:299] + "…"


def clock(stamp, now):
    moment, today = time.localtime(stamp), time.localtime(now)
    day = (time.strftime("Today", moment) if moment[:3] == today[:3] else
           "Yesterday" if time.localtime(now - 86400)[:3] == moment[:3] else
           time.strftime("%b %d, %Y", moment))
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
    code = session.get("exit_code")
    if state == "failed" and code not in (None, 0, 1): outcome += f" (exit status {code})"
    started, ended = session.get("started_at"), session.get("ended_at")
    when = clock(started, now) if started else None
    duration = span((ended or now) - started) if started and (ended or state in ACTIVE) else None
    return {"outcome": outcome, "when": when, "duration": duration, "error": session.get("error")}


def log_findings(lines):
    """Count known problem signatures; keep the first example of each."""
    found = {}
    for line in lines:
        for key, title, pattern in SIGNATURES:
            match = pattern.search(line)
            if not match: continue
            entry = found.setdefault(key, {"title": title, "count": 0, "example": None, "sources": []})
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


def show(state):
    try:
        from PyQt6.QtCore import QProcess, QProcessEnvironment, QRectF, QTimer, Qt, QUrl
        from PyQt6.QtGui import QDesktopServices, QIcon, QPainter, QPainterPath, QPalette, QPixmap
        from PyQt6.QtWidgets import (QApplication, QCheckBox, QComboBox, QDialog, QDialogButtonBox,
                                     QFileDialog, QFormLayout, QFrame, QGridLayout, QHBoxLayout, QLabel,
                                     QLineEdit, QMainWindow, QProgressBar, QPushButton, QScrollArea,
                                     QSizePolicy, QStackedWidget, QStyle, QToolButton, QVBoxLayout, QWidget)
    except ImportError as error:
        raise ValueError("The desktop launcher requires PyQt6; the CLI works without it") from error

    application = QApplication.instance() or QApplication([sys.argv[0]])
    application.setApplicationName("Asgard")
    card_width, icon_size, spacing = 240, 72, 16
    style = """
        QFrame#card { background: palette(base); border: 1px solid palette(mid); border-radius: 10px; }
        QFrame#card[selected="true"] { border: 2px solid palette(highlight); }
        QLabel#badge { background: palette(highlight); color: palette(highlighted-text);
                       border-radius: 9px; padding: 2px 9px; font-weight: 600; }
        QFrame#notice, QFrame#progress, QFrame#drawer, QFrame#developer {
            background: palette(alternate-base); border: 1px solid palette(mid); border-radius: 8px; }
    """

    def themed(name, fallback):
        return QIcon.fromTheme(name, application.style().standardIcon(fallback))

    def muted(label):
        label.setForegroundRole(QPalette.ColorRole.PlaceholderText)
        return label

    def scaled(label, factor, bold=False):
        font = label.font(); font.setPointSizeF(font.pointSizeF() * factor); font.setBold(bold); label.setFont(font)
        return label

    def artwork(app, size, ratio, palette):
        """Rounded launcher icon at device pixels, or a lettered placeholder."""
        pixmap = QPixmap(round(size * ratio), round(size * ratio))
        pixmap.setDevicePixelRatio(ratio); pixmap.fill(Qt.GlobalColor.transparent)
        painter = QPainter(pixmap)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing); painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        path = QPainterPath(); path.addRoundedRect(QRectF(0, 0, size, size), size * .22, size * .22)
        image = QPixmap(app["icon"]) if app.get("icon") else QPixmap()
        if not image.isNull():
            painter.setClipPath(path); painter.drawPixmap(QRectF(0, 0, size, size), image, QRectF(image.rect()))
        else:
            painter.fillPath(path, palette.color(QPalette.ColorRole.Highlight))
            font = painter.font(); font.setPixelSize(round(size * .46)); font.setBold(True); painter.setFont(font)
            painter.setPen(palette.color(QPalette.ColorRole.HighlightedText))
            painter.drawText(QRectF(0, 0, size, size), Qt.AlignmentFlag.AlignCenter,
                             next((c.upper() for c in app["label"] if c.isalnum()), "?"))
        painter.end()
        return pixmap

    class Card(QFrame):
        def __init__(self, window, app):
            super().__init__()
            self.window, self.app, self.art = window, app, None
            self.setObjectName("card"); self.setFixedWidth(card_width)
            self.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
            layout = QVBoxLayout(self); layout.setContentsMargins(14, 14, 14, 12); layout.setSpacing(6)
            top = QHBoxLayout(); self.icon = QLabel(); self.icon.setFixedSize(icon_size, icon_size)
            self.badge = QLabel(); self.badge.setObjectName("badge")
            top.addWidget(self.icon); top.addStretch(); top.addWidget(self.badge, 0, Qt.AlignmentFlag.AlignTop)
            layout.addLayout(top); layout.addSpacing(4)
            self.title = scaled(QLabel(), 1.1, True); self.title.setWordWrap(True)
            self.title.setFixedHeight(self.title.fontMetrics().lineSpacing() * 2 + 2)
            self.title.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignLeft)
            self.meta = muted(QLabel())
            self.developer = muted(QLabel()); self.developer.setWordWrap(True)
            self.developer.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
            for widget in (self.title, self.meta, self.developer): layout.addWidget(widget)
            actions = QHBoxLayout(); actions.setSpacing(6)
            self.action = QPushButton(); self.action.setMinimumHeight(34)
            self.action.clicked.connect(lambda: window.act(self.app))
            self.gear = QToolButton(); self.gear.setIcon(themed("configure", QStyle.StandardPixmap.SP_FileDialogDetailedView))
            self.gear.setToolTip("Game settings"); self.gear.setAutoRaise(True)
            self.gear.clicked.connect(lambda: window.configure(self.app))
            self.history = QToolButton(); self.history.setIcon(themed("view-history", QStyle.StandardPixmap.SP_FileDialogInfoView))
            self.history.setToolTip("Last session"); self.history.setAutoRaise(True)
            self.history.clicked.connect(lambda: window.open_drawer(self.app["package"]))
            actions.addWidget(self.action, 1); actions.addWidget(self.gear); actions.addWidget(self.history)
            layout.addLayout(actions)
            self.update_app(app)

        def update_app(self, app, pending=None):
            self.app = app
            rule = card_state(app, pending)
            ratio = self.devicePixelRatioF()
            art = (app.get("icon"), app["label"], ratio, self.palette().cacheKey())
            if art != self.art:
                self.art = art; self.icon.setPixmap(artwork(app, icon_size, ratio, self.palette()))
            metrics = self.title.fontMetrics()
            words = metrics.elidedText(app["label"], Qt.TextElideMode.ElideRight, (card_width - 32) * 2 - metrics.averageCharWidth() * 4)
            self.title.setText(words); self.title.setToolTip(app["label"] if words != app["label"] else "")
            installed = time.strftime("%b %d, %Y", time.localtime(app["installed_at"])) if app.get("installed_at") else None
            self.meta.setText(" · ".join(str(x) for x in (app.get("version_name") or app.get("version"), installed and "Installed " + installed) if x))
            self.badge.setText(rule["badge"] or ""); self.badge.setVisible(bool(rule["badge"]))
            self.action.setText(rule["text"]); self.action.setEnabled(rule["enabled"])
            self.action.setIcon(themed("media-playback-stop" if rule["action"] == "stop" else "media-playback-start",
                                       QStyle.StandardPixmap.SP_MediaStop if rule["action"] == "stop" else QStyle.StandardPixmap.SP_MediaPlay))
            self.action.setToolTip(rule.get("reason", ""))
            session = app["session"]
            self.history.setEnabled(bool(session.get("session_id") or app.get("error")))
            self.developer.setText(f"{app['package']}\nUID {session.get('uid', '—')} · PID {session.get('pid') or '—'}")
            self.developer.setVisible(self.window.developer_mode())

        def set_selected(self, value):
            self.setProperty("selected", value); self.style().unpolish(self); self.style().polish(self)

        def mousePressEvent(self, event):
            self.window.select(self.app["package"]); super().mousePressEvent(event)

    class Settings(QDialog):
        def __init__(self, window, app, settings):
            super().__init__(window)
            self.setWindowTitle(app["label"] + " settings")
            layout = QVBoxLayout(self)
            form = QFormLayout(); form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.ExpandingFieldsGrow)
            self.stats = QCheckBox("Show FPS and graphics stats in the window title")
            self.stats.setChecked(settings.get("window_stats", True))
            form.addRow("Window stats", self.stats)
            self.back = QComboBox()
            for name, text in BACK_KEYS: self.back.addItem(text, name)
            self.back.setCurrentIndex(max(0, self.back.findData(settings.get("back_key", "escape"))))
            self.back.setToolTip("Host key delivered to the game as Android's Back button")
            form.addRow("Back key", self.back)
            profile = QComboBox(); profile.addItem("Default display"); profile.setEnabled(False)
            form.addRow("Device profile", profile)
            note = muted(QLabel("Matching your phone's screen and identity is planned; games use the default display for now."))
            note.setWordWrap(True); form.addRow("", note)
            layout.addLayout(form)
            buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Save | QDialogButtonBox.StandardButton.Cancel)
            buttons.accepted.connect(self.accept); buttons.rejected.connect(self.reject)
            layout.addWidget(buttons)
            self.initial = (self.stats.isChecked(), self.back.currentData())
            self.settings = dict(settings)

        def result_settings(self):
            # Store only choices the user made, so defaults keep following the emulator's.
            if self.stats.isChecked() != self.initial[0] or "window_stats" in self.settings:
                self.settings["window_stats"] = self.stats.isChecked()
            if self.back.currentData() != self.initial[1] or "back_key" in self.settings:
                self.settings["back_key"] = self.back.currentData()
            return self.settings

    class Library(QMainWindow):
        def __init__(self):
            super().__init__()
            self.state = state
            self.jobs, self.pending, self.cards = set(), {}, {}
            self.refreshing, self.apps, self.columns = False, [], 0
            self.selected, self.drawer_package, self.drawer_stamp, self.drawer_scanned = None, None, None, 0
            self.order, self.announce = [], None
            self.setWindowTitle("Asgard"); self.setMinimumSize(720, 480)
            body = QWidget(); outer = QVBoxLayout(body)
            outer.setContentsMargins(24, 20, 24, 16); outer.setSpacing(12)
            heading = QHBoxLayout(); heading.setSpacing(10)
            titles = QVBoxLayout(); titles.setSpacing(0)
            titles.addWidget(scaled(QLabel("Game library"), 1.8, True))
            self.summary = muted(QLabel()); titles.addWidget(self.summary)
            heading.addLayout(titles); heading.addStretch()
            self.search = QLineEdit(); self.search.setPlaceholderText("Search games")
            self.search.setClearButtonEnabled(True); self.search.setMinimumWidth(220)
            self.search.textChanged.connect(lambda: self.render(True))
            self.install_button = QPushButton(themed("list-add", QStyle.StandardPixmap.SP_DialogOpenButton), "Install APK…")
            self.install_button.setMinimumHeight(34); self.install_button.setDefault(True)
            self.install_button.clicked.connect(self.install)
            heading.addWidget(self.search); heading.addWidget(self.install_button)
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
            self.notice_details = QPushButton("Last session"); self.notice_details.clicked.connect(
                lambda: self.open_drawer(self.notice_details.property("package")))
            close = QToolButton(); close.setAutoRaise(True)
            close.setIcon(themed("window-close", QStyle.StandardPixmap.SP_TitleBarCloseButton)); close.clicked.connect(self.notice.hide)
            row.addWidget(self.notice_icon); row.addWidget(self.notice_text, 1); row.addWidget(self.notice_details); row.addWidget(close)
            self.notice.hide(); outer.addWidget(self.notice)
            self.notice_timer = QTimer(self); self.notice_timer.setSingleShot(True); self.notice_timer.timeout.connect(self.notice.hide)
            middle = QHBoxLayout(); middle.setSpacing(12)
            self.pages = QStackedWidget()
            self.scroll = QScrollArea(); self.scroll.setWidgetResizable(True); self.scroll.setFrameShape(QFrame.Shape.NoFrame)
            grid_body = QWidget(); self.grid = QGridLayout(grid_body)
            self.grid.setContentsMargins(0, 0, 0, 0); self.grid.setSpacing(spacing)
            self.grid.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignLeft)
            self.scroll.setWidget(grid_body); self.pages.addWidget(self.scroll)
            empty = QWidget(); column = QVBoxLayout(empty); column.addStretch()
            self.empty_title = scaled(QLabel(), 1.4, True); self.empty_hint = muted(QLabel()); self.empty_hint.setWordWrap(True)
            self.empty_install = QPushButton(themed("list-add", QStyle.StandardPixmap.SP_DialogOpenButton), "Install APK…")
            self.empty_install.clicked.connect(self.install)
            for widget in (self.empty_title, self.empty_hint, self.empty_install):
                if isinstance(widget, QLabel): widget.setAlignment(Qt.AlignmentFlag.AlignCenter)
                column.addWidget(widget, 0, Qt.AlignmentFlag.AlignHCenter)
            column.addStretch(); self.pages.addWidget(empty)
            middle.addWidget(self.pages, 1); middle.addWidget(self.build_drawer())
            outer.addLayout(middle, 1)
            outer.addWidget(self.build_developer())
            self.setCentralWidget(body); self.resize(1080, 720)
            self.timer = QTimer(self); self.timer.timeout.connect(self.refresh); self.timer.start(1500)
            self.render(True); self.refresh()

        def build_drawer(self):
            self.drawer = QFrame(); self.drawer.setObjectName("drawer"); self.drawer.setFixedWidth(340)
            layout = QVBoxLayout(self.drawer); layout.setContentsMargins(16, 12, 10, 14); layout.setSpacing(8)
            top = QHBoxLayout(); top.addWidget(scaled(QLabel("Last session"), 1.15, True)); top.addStretch()
            close = QToolButton(); close.setAutoRaise(True)
            close.setIcon(themed("window-close", QStyle.StandardPixmap.SP_TitleBarCloseButton)); close.clicked.connect(self.close_drawer)
            top.addWidget(close); layout.addLayout(top)
            self.drawer_title = QLabel(); self.drawer_title.setWordWrap(True)
            self.drawer_body = QLabel(); self.drawer_body.setWordWrap(True); self.drawer_body.setTextFormat(Qt.TextFormat.RichText)
            self.drawer_body.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignLeft)
            self.drawer_body.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
            body = QScrollArea(); body.setWidgetResizable(True); body.setFrameShape(QFrame.Shape.NoFrame); body.setWidget(self.drawer_body)
            layout.addWidget(self.drawer_title); layout.addWidget(body, 1)
            self.open_log = QPushButton(themed("document-open", QStyle.StandardPixmap.SP_FileIcon), "Open full log")
            self.open_log.clicked.connect(lambda: self.open_path(self.drawer_app()["session"].get("log")))
            self.open_folder = QPushButton(themed("folder-open", QStyle.StandardPixmap.SP_DirOpenIcon), "Open session folder")
            self.open_folder.clicked.connect(lambda: self.open_path(str(Path(self.drawer_app()["session"]["log"]).parent)))
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
            self.refreshing = True
            requested_state = self.state
            def loaded(code, output):
                self.refreshing = False
                if requested_state != self.state: self.refresh(); return
                try:
                    if code: raise ValueError(short_error(output))
                    self.apps = sorted(json.loads(output), key=lambda a: (a["label"].casefold(), a["package"]))
                except (ValueError, KeyError) as error:
                    self.show_notice("Couldn't read the game library: " + str(error)[:300], error=True); return
                self.render(); self.update_drawer()
                app = self.app(self.announce)
                if app:
                    self.announce = None; self.select(app["package"])
                    self.show_notice(f"{app['label']} is installed.", timeout=8000)
            self.command(["list", "--json"], loaded)

        def render(self, relayout=False):
            query = self.search.text().casefold()
            visible = [a for a in self.apps if query in (a["label"] + " " + a["package"]).casefold()]
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
            self.summary.setText(f"{len(self.apps)} game{'s' * (len(self.apps) != 1)}" + (f" · {running} running" if running else ""))
            if not self.apps:
                self.empty_title.setText("No games yet")
                self.empty_hint.setText("Install an APK, APKM, APKS or XAPK file to add it to your library.")
                self.empty_install.show(); self.pages.setCurrentIndex(1)
            elif not visible:
                self.empty_title.setText("No matches")
                self.empty_hint.setText(f"No installed game matches “{self.search.text()}”.")
                self.empty_install.hide(); self.pages.setCurrentIndex(1)
            else:
                self.pages.setCurrentIndex(0)

        def reflow(self, order):
            self.order = order
            width = self.scroll.viewport().width()
            self.columns = max(1, (width + spacing) // (card_width + spacing))
            for card in self.cards.values():
                self.grid.removeWidget(card); card.hide()
            for index, package in enumerate(order):
                card = self.cards[package]
                self.grid.addWidget(card, index // self.columns, index % self.columns); card.show()

        def resizeEvent(self, event):
            super().resizeEvent(event)
            width = self.scroll.viewport().width()
            if max(1, (width + spacing) // (card_width + spacing)) != self.columns: self.reflow(self.order)

        def app(self, package):
            return next((a for a in self.apps if a["package"] == package), None)

        def select(self, package):
            self.selected = package
            for name, card in self.cards.items(): card.set_selected(name == package)
            if self.drawer.isVisible() and package != self.drawer_package: self.open_drawer(package)

        def act(self, app):
            package = app["package"]
            if card_state(app, self.pending.get(package))["action"] == "stop": return self.stop_app(app)
            settings = load_settings(self.state / package) if PACKAGE.fullmatch(package) else {}
            args, env = launch_request(app, settings, self.interpreter.isChecked(), self.trace.isChecked())
            self.pending[package] = "play"; self.render()
            def started(code, output):
                self.pending.pop(package, None)
                if code:
                    self.show_notice(f"{app['label']} didn't start: {short_error(output)}", error=True, package=package)
                self.refresh()
            self.command(args, started, env)

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
            dialog = Settings(self, app, load_settings(directory))
            if dialog.exec() == QDialog.DialogCode.Accepted:
                try:
                    save_settings(directory, dialog.result_settings())
                    if app["session"].get("state") in ACTIVE:
                        self.show_notice(f"Settings saved. They apply the next time {app['label']} starts.", timeout=6000)
                except OSError as error:
                    self.show_notice("Couldn't save settings: " + str(error), error=True)

        def show_notice(self, text, error=False, package=None, timeout=0):
            icon = QStyle.StandardPixmap.SP_MessageBoxWarning if error else QStyle.StandardPixmap.SP_MessageBoxInformation
            self.notice_icon.setPixmap(application.style().standardIcon(icon).pixmap(20, 20))
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
            session = app["session"]; log = session.get("log")
            try: info = Path(log).stat() if log else None
            except OSError: info = None
            stamp = (session.get("session_id"), session.get("state"), info and info.st_size, self.developer_mode())
            # A growing log of a live game is rescanned at most every five seconds.
            if not force and (stamp == self.drawer_stamp or (session.get("state") in ACTIVE and self.drawer_stamp
                    and stamp[:2] == self.drawer_stamp[:2] and time.monotonic() - self.drawer_scanned < 5)):
                return
            self.drawer_stamp, self.drawer_scanned = stamp, time.monotonic()
            self.drawer_title.setText(app["label"])
            described = describe_session(session)
            parts = [f"<p><b>{html.escape(described['outcome'])}</b>"]
            if described["when"]:
                parts.append("<br>Started " + html.escape(described["when"]) +
                             (" · " + html.escape(described["duration"]) if described["duration"] else ""))
            parts.append("</p>")
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
                                     "services are not available, and games usually keep running.</small></p>")
                else:
                    parts.append("<p>No known problem signatures in the log.</p>")
            elif session.get("session_id"):
                parts.append("<p>This session has no log.</p>")
            else:
                parts.append("<p>This game hasn't been played yet.</p>")
            if self.developer_mode() and session.get("session_id"):
                parts.append(f"<p><small>Session {html.escape(session['session_id'])} · PID {session.get('pid') or '—'}"
                             f" · exit {session.get('exit_code') if session.get('exit_code') is not None else '—'}</small></p>")
            self.drawer_body.setText("".join(parts))
            self.open_log.setEnabled(bool(info)); self.open_folder.setVisible(self.developer_mode() and bool(log))

        def open_path(self, path):
            if path: QDesktopServices.openUrl(QUrl.fromLocalFile(path))

        def choose(self):
            path = QFileDialog.getExistingDirectory(self, "Choose game library folder", str(self.state))
            if path:
                self.state = Path(path); self.path.setText(path); self.apps = []
                self.close_drawer(); self.render(True); self.refresh()

        def install(self):
            path, _ = QFileDialog.getOpenFileName(self, "Install Android game", str(Path.home()),
                "Android packages (*.apk *.apkm *.apks *.xapk);;All files (*)")
            if not path: return
            name = Path(path).name; started = time.monotonic()
            self.install_button.setEnabled(False); self.empty_install.setEnabled(False); self.notice.hide()
            def tick():
                self.progress_text.setText(f"Installing {name}… verifying signatures and unpacking "
                                           f"({span(time.monotonic() - started)})")
            ticker = QTimer(self); ticker.timeout.connect(tick); ticker.start(1000); tick(); self.progress.show()
            def installed(code, output):
                ticker.stop(); ticker.deleteLater(); self.progress.hide()
                self.install_button.setEnabled(True); self.empty_install.setEnabled(True)
                if code:
                    self.show_notice(f"Couldn't install {name}: {short_error(output)}", error=True); return
                match = re.search(r"^Installed (\S+) ", output, re.M)
                # Announced by the first listing that contains the package.
                self.announce = match[1] if match else None
                self.refresh()
            self.command(["install", path], installed)

    window = Library(); window.setStyleSheet(style); window.show()
    return application.exec()
