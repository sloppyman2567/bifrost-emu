"""Desktop app library using the same CLI and process supervisor as terminal launches."""
import json
from pathlib import Path
import sys

from android_package import ROOT


def show(state):
    try:
        from PyQt6.QtCore import QProcess, QTimer, Qt, QUrl
        from PyQt6.QtGui import QDesktopServices, QFontDatabase
        from PyQt6.QtWidgets import (QApplication, QCheckBox, QFileDialog, QHBoxLayout, QLabel,
                                     QLineEdit, QMainWindow, QMessageBox, QPlainTextEdit, QPushButton,
                                     QSplitter, QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget,
                                     QAbstractItemView)
    except ImportError as error:
        raise ValueError("The desktop launcher requires PyQt6; the CLI works without it") from error

    class Library(QMainWindow):
        def __init__(self):
            super().__init__()
            self.state = state
            self.jobs = set()
            self.refreshing = False
            self.apps = []
            self.log_stamp = None
            self.setWindowTitle("Asgard · Android Apps")
            self.resize(1060, 720)
            body = QWidget()
            layout = QVBoxLayout(body)
            layout.setContentsMargins(24, 22, 24, 18)
            layout.setSpacing(14)
            heading = QHBoxLayout()
            title = QLabel("Apps")
            font = title.font(); font.setPointSize(24); font.setBold(True); title.setFont(font)
            heading.addWidget(title); heading.addStretch()
            install = QPushButton("Install APK…"); install.clicked.connect(self.install)
            choose = QPushButton("Choose library…"); choose.clicked.connect(self.choose)
            heading.addWidget(choose); heading.addWidget(install)
            layout.addLayout(heading)
            self.path = QLabel(str(self.state)); self.path.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
            layout.addWidget(self.path)
            self.search = QLineEdit(); self.search.setPlaceholderText("Search apps or package names")
            self.search.textChanged.connect(self.render); layout.addWidget(self.search)
            split = QSplitter(Qt.Orientation.Vertical)
            self.table = QTableWidget(0, 4)
            self.table.setHorizontalHeaderLabels(["App", "Version", "Status", "Process"])
            self.table.horizontalHeader().setStretchLastSection(True)
            self.table.setColumnWidth(0, 450); self.table.setColumnWidth(2, 150)
            self.table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
            self.table.setSelectionMode(QAbstractItemView.SelectionMode.ExtendedSelection)
            self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
            self.table.verticalHeader().hide()
            self.table.itemSelectionChanged.connect(self.selection_changed)
            self.table.cellDoubleClicked.connect(lambda *_: self.launch())
            split.addWidget(self.table)
            details = QWidget(); panel = QVBoxLayout(details); panel.setContentsMargins(0, 12, 0, 0)
            actions = QHBoxLayout()
            self.start = QPushButton("Launch"); self.start.clicked.connect(self.launch)
            self.stop = QPushButton("Stop"); self.stop.clicked.connect(self.stop_apps)
            self.folder = QPushButton("Open session folder"); self.folder.clicked.connect(self.open_folder)
            self.interpreter = QCheckBox("Interpreter")
            self.trace = QCheckBox("Trace calls")
            self.trace.setToolTip("Records Java framework calls; only for apps with a managed (Java) Activity")
            for widget in (self.start, self.stop, self.folder, self.interpreter, self.trace): actions.addWidget(widget)
            actions.addStretch(); panel.addLayout(actions)
            self.info = QLabel("Select an app to see its session and logs.")
            self.info.setWordWrap(True); self.info.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
            panel.addWidget(self.info)
            self.log = QPlainTextEdit(); self.log.setReadOnly(True)
            self.log.setPlaceholderText("Session output will appear here.")
            self.log.setFont(QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont))
            self.log.setMaximumBlockCount(1000); panel.addWidget(self.log)
            split.addWidget(details); split.setSizes([320, 280]); layout.addWidget(split)
            self.setCentralWidget(body)
            self.timer = QTimer(self); self.timer.timeout.connect(self.refresh); self.timer.start(1500)
            self.refresh(); self.selection_changed()

        def command(self, args, done=None):
            process = QProcess(self)
            process.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
            self.jobs.add(process)
            def finished(code, status):
                output = bytes(process.readAllStandardOutput()).decode(errors="replace")
                self.jobs.discard(process); process.deleteLater()
                if done: done(code, output)
                elif code: QMessageBox.warning(self, "Asgard", output[-6000:] or f"Command exited with {code}")
                else: self.statusBar().showMessage(output.strip(), 8000)
            process.finished.connect(finished)
            process.errorOccurred.connect(lambda error: self.statusBar().showMessage(process.errorString(), 8000))
            process.start(sys.executable, [str(ROOT / "Scripts/Android/asgard.py"), "--state-root", str(self.state), *args])

        def refresh(self):
            if self.refreshing: return
            self.refreshing = True
            requested_state = self.state
            def loaded(code, output):
                self.refreshing = False
                if requested_state != self.state: self.refresh(); return
                try:
                    if code: raise ValueError(output)
                    self.apps = json.loads(output)
                    self.render()
                    self.statusBar().showMessage(f"{len(self.apps)} installed · " +
                        f"{sum(a['session']['state'] in ('preparing', 'running', 'stopping') for a in self.apps)} active")
                except (ValueError, KeyError) as error: self.statusBar().showMessage(str(error)[:500])
            self.command(["list", "--json"], loaded)

        def selected(self):
            return [self.table.item(row.row(), 0).data(Qt.ItemDataRole.UserRole)
                    for row in self.table.selectionModel().selectedRows()]

        def render(self):
            selected = {a["package"] for a in self.selected()}
            query = self.search.text().casefold()
            visible = [a for a in self.apps if query in (a["label"] + " " + a["package"]).casefold()]
            self.table.blockSignals(True); self.table.setRowCount(len(visible))
            for row, app in enumerate(visible):
                session = app["session"]
                for col, text in enumerate((app["label"], str(app.get("version", "—")), session["state"], str(session.get("pid") or "—"))):
                    item = QTableWidgetItem(text); item.setToolTip(app["package"])
                    if col == 0: item.setData(Qt.ItemDataRole.UserRole, app)
                    self.table.setItem(row, col, item)
                if app["package"] in selected:
                    from PyQt6.QtCore import QItemSelectionModel
                    self.table.selectionModel().select(self.table.model().index(row, 0),
                        QItemSelectionModel.SelectionFlag.Select | QItemSelectionModel.SelectionFlag.Rows)
            self.table.blockSignals(False)
            self.selection_changed()

        def selection_changed(self):
            selected = self.selected()
            active = lambda a: a["session"]["state"] in ("preparing", "running", "stopping")
            self.start.setEnabled(bool(selected) and any(not active(a) and a.get("launcher") in ("managed_activity", "native_activity") for a in selected))
            self.stop.setEnabled(any(active(a) for a in selected))
            self.folder.setEnabled(len(selected) == 1 and bool(selected[0]["session"].get("log")))
            # Call tracing follows ART methods, so NativeActivity apps cannot use it.
            traceable = bool(selected) and all(a.get("launcher") == "managed_activity" for a in selected)
            self.trace.setEnabled(traceable)
            if not traceable: self.trace.setChecked(False)
            if len(selected) != 1:
                self.info.setText(f"{len(selected)} apps selected" if selected else "Select an app to see its session and logs.")
                self.log.clear(); self.log_stamp = None; return
            app = selected[0]; session = app["session"]
            self.info.setText(f"{app['package']} · UID {session.get('uid', '—')} · {session['state']}"
                + (f" · exit {session['exit_code']}" if session.get("exit_code") is not None else "")
                + ("\n" + str(session.get("error") or app.get("error")) if session.get("error") or app.get("error") else ""))
            path = session.get("log")
            try:
                if not path: raise FileNotFoundError
                log = Path(path); stamp = (path, log.stat().st_mtime_ns, log.stat().st_size)
                if stamp == self.log_stamp: return
                self.log_stamp = stamp
                with log.open("rb") as stream:
                    stream.seek(max(0, stamp[2] - 128 * 1024)); output = stream.read().decode(errors="replace")
                bar = self.log.verticalScrollBar(); tail = bar.value() >= bar.maximum() - 4; position = bar.value()
                self.log.setPlainText(output)
                bar.setValue(bar.maximum() if tail else position)
            except OSError:
                self.log.clear(); self.log_stamp = None

        def launch(self):
            packages = [a["package"] for a in self.selected() if a["session"]["state"] not in ("preparing", "running", "stopping")
                        and a.get("launcher") in ("managed_activity", "native_activity")]
            if not packages: return
            args = ["start", *packages]
            if self.interpreter.isChecked(): args.append("--no-jit")
            if self.trace.isChecked(): args += ["--trace-apk", "calls"]
            self.command(args)

        def stop_apps(self):
            for app in self.selected():
                if app["session"]["state"] in ("preparing", "running", "stopping"):
                    self.command(["stop", app["package"]])

        def open_folder(self):
            selected = self.selected()
            if len(selected) == 1 and selected[0]["session"].get("log"):
                QDesktopServices.openUrl(QUrl.fromLocalFile(str(Path(selected[0]["session"]["log"]).parent)))

        def choose(self):
            path = QFileDialog.getExistingDirectory(self, "Choose app library", str(self.state))
            if path:
                self.state = Path(path); self.path.setText(path); self.apps = []; self.render(); self.refresh()

        def install(self):
            path, _ = QFileDialog.getOpenFileName(self, "Install Android app", str(Path.home()),
                "Android packages (*.apk *.apkm *.apks *.xapk);;All files (*)")
            if path:
                self.statusBar().showMessage("Installing " + Path(path).name + "…")
                self.command(["install", path], lambda code, output:
                    (QMessageBox.warning(self, "Install failed", output[-6000:]) if code else self.refresh()))

    application = QApplication.instance() or QApplication([sys.argv[0]])
    application.setApplicationName("Asgard")
    window = Library(); window.show()
    return application.exec()
