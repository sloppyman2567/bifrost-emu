"""Google accounts for the managed Android runtime.

Sign-in is microG's own UI (GmsCore's LoginActivity: Google's embedded setup page in a
WebView), run as a session of the provisioned com.google.android.gms system app. Accounts are
stored by the guest account service in each root; this module keeps one copy of them, with the
device registration microG made for them, under <state>/.google and imports it into every app
root at launch, so one sign-in serves every app. A root that signs in or out during a session
publishes its accounts when the session ends.

An app that starts one of microG's Activities (Google Sign-In, the account chooser, a consent
screen) reaches the same session through ActivityRelay: the guest stages the Intent in its
cache, the relay runs that Activity as the system app's session in a window of its own, and the
result (plus the shared accounts) goes back to the app.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import tempfile
import threading
import time
from xml.etree import ElementTree

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools/Android"))
import android_microg

GMS = "com.google.android.gms"
ACCOUNT_TYPE = "com.google"
LOGIN_ACTIVITY = "org.microg.gms.auth.login.LoginActivity"
SETTINGS_ACTIVITY = "org.microg.gms.ui.SettingsActivity"
# The request an app staged for one of microG's Activities (FrameworkRemoteActivities) and what
# the session answers; the relay merges both into the app's result.json.
ACTIVITY = "data/system/users/0/bifrost-activity"
REQUEST_FILES = ("intent.bin", "caller.bin", "request.json")
RESULT_FILES = ("nested.json", "result.bin", "response.bin")
ACTIVE = ("preparing", "running", "stopping", "starting")
CLASS = re.compile(r"[A-Za-z_$][A-Za-z0-9_$.]*\Z")
SHARED = ".google"
ACCOUNTS = "data/system/users/0/bifrost-accounts.json"
# What ties an account to the device microG registered it from (its check-in identity, the
# services configuration and the account caches). Settings such as the attestation toggle
# (com.google.android.gms_preferences.xml) stay per root.
DATA = "data/user/0/" + GMS
IDENTITY = (ACCOUNTS, DATA + "/shared_prefs/checkin.xml", DATA + "/databases/gservices.db",
            DATA + "/shared_prefs/google_account_cache.xml", DATA + "/shared_prefs/common.selected_account_prefs.xml",
            DATA + "/databases/pluscontacts.db")
MARKER = "data/system/users/0/bifrost-google-import.json"
# Whether microG may create a Play Games profile for a signed-in account that has none
# (games/v1/players/me answers 404 and Games sign-in reports SIGN_IN_REQUIRED). microG's own
# setting is off; this is the user's choice for every app root, made with `google games-profile`.
GAMES_PROFILE = "games-profile.json"
GAMES_PROFILE_KEY = "game_allow_create_player"
PREFERENCES = DATA + "/shared_prefs/com.google.android.gms_preferences.xml"
GAMES_PROFILE_NOTICE = """Allowing this lets microG create a Google Play Games profile for a Google account
that has none, the first time a game signs in to Play Games with it. microG asks Google for a
gamer tag, sets the profile hidden and not discoverable, and the account then has a Play Games
profile at Google until you delete it in Play Games settings. Nothing is created unless an
account without a profile signs in to a game. It applies to every account and app on this device."""


def app_directory(state):
    return Path(state) / GMS


def prepare(state, activity=LOGIN_ACTIVITY, request=None, requester=None):
    """Register the microG system app as a launchable session and choose its Activity.

    `request` is the host directory of an app's staged request for that Activity and `requester`
    the app (package and label) that asked, which Asgard shows while the window is open."""
    directory = app_directory(state)
    directory.mkdir(parents=True, exist_ok=True)
    from android_sessions import publish
    publish(directory / "package.json", {
        "schema_version": 1, "package": GMS, "label": "Google account", "system": True,
        "version_code": 252432034, "version_name": android_microg.VERSION, "icon": None,
        "installed_at": int(time.time()), "guest_code": "/system/priv-app/GmsCore",
        "launcher": {"kind": "managed_activity", "class": activity},
        "files": {}, "packages": [], "apk_count": 1, "arm64_library_count": 0,
        **({"request": str(request), "requester": requester} if request else {})})
    return directory


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest() if Path(path).is_file() else None


def stamp(root):
    """Content hash of the files that carry accounts, as a mapping."""
    return {name: digest(root / name) for name in IDENTITY}


def read_accounts(path):
    """Accounts in a store file as dictionaries without their secrets."""
    try:
        document = json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return []
    if not isinstance(document, dict) or document.get("version") != 1: return []
    return [{"name": entry["name"], "type": entry["type"], "services": entry.get("data", {}).get("services", "")}
            for entry in document.get("accounts", []) if isinstance(entry, dict)
            and isinstance(entry.get("name"), str) and isinstance(entry.get("type"), str)]


def accounts(state):
    """Google accounts of the shared copy."""
    return [entry for entry in read_accounts(Path(state) / SHARED / ACCOUNTS) if entry["type"] == ACCOUNT_TYPE]


MAX_GUEST_FILE = 16 * 1024 * 1024
PACKAGE_NAME = re.compile(r"[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z0-9_]+)+\Z")


def open_regular(path):
    """Open a guest-writable file for reading, never through a symlink and never a special file.

    Guest processes own these paths, so a symlink to a host file must not make the host read it."""
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK)
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode) or os.fstat(descriptor).st_size > MAX_GUEST_FILE:
            raise OSError(f"Not a regular guest file: {path}")
        return os.fdopen(descriptor, "rb")
    except BaseException:
        os.close(descriptor)
        raise


def is_regular(path):
    try:
        return stat.S_ISREG(os.lstat(path).st_mode)
    except OSError:
        return False


def read_regular(path):
    with open_regular(path) as stream:
        return stream.read(MAX_GUEST_FILE + 1)


def _copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as stream:
        pending = Path(stream.name)
        try:
            with open_regular(source) as original:
                shutil.copyfileobj(original, stream)
            stream.flush()
            os.fsync(stream.fileno())
            pending.chmod(0o600)
            pending.replace(target)
        finally:
            pending.unlink(missing_ok=True)


def import_shared(state, root):
    """Bring the shared accounts into an app root before its session starts."""
    shared = Path(state) / SHARED
    if not (shared / ACCOUNTS).is_file(): return False
    current = stamp(shared)
    marker = root / MARKER
    try:
        if json.loads(marker.read_text()) == current: return False
    except (OSError, ValueError):
        pass
    for name in IDENTITY:
        source, target = shared / name, root / name
        if source.is_file(): _copy(source, target)
        else: target.unlink(missing_ok=True)
    _copy_text(marker, current)
    return True


def _copy_text(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_name(path.name + ".tmp")
    pending.write_text(json.dumps(value, sort_keys=True) + "\n")
    pending.replace(path)


def games_profile_allowed(state):
    """Whether the user allowed microG to create Play Games profiles (never chosen: no)."""
    try:
        document = json.loads(read_regular(Path(state) / SHARED / GAMES_PROFILE))
    except (OSError, ValueError):
        return False
    return isinstance(document, dict) and document.get("allow_create_player") is True


def set_games_profile(state, allowed):
    _copy_text(Path(state) / SHARED / GAMES_PROFILE, {"version": 1, "allow_create_player": bool(allowed)})


def apply_games_profile(state, root):
    """Write the user's Play Games profile choice into an app root's microG preferences.

    Runs before the root's session starts, so microG is not holding the file. A root is left
    alone until the choice was made, so microG's own settings window still works there."""
    if not is_regular(Path(state) / SHARED / GAMES_PROFILE): return False
    value = "true" if games_profile_allowed(state) else "false"
    path = root / PREFERENCES
    try:
        text = read_regular(path).decode("utf-8")
        # SharedPreferences files have no DOCTYPE; refuse one rather than expand entities.
        if "<!" in text.replace("<!--", ""): return False
        document = ElementTree.fromstring(text)
    except FileNotFoundError:
        document = ElementTree.Element("map")
    except (OSError, ValueError, ElementTree.ParseError):
        return False
    if document.tag != "map": return False
    entry = next((e for e in document if e.tag == "boolean" and e.get("name") == GAMES_PROFILE_KEY), None)
    if entry is None:
        entry = ElementTree.SubElement(document, "boolean", name=GAMES_PROFILE_KEY)
    elif entry.get("value") == value: return False
    entry.set("value", value)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
        pending = Path(stream.name)
        try:
            stream.write(ElementTree.tostring(document, encoding="utf-8", xml_declaration=True))
            stream.flush()
            os.fsync(stream.fileno())
            pending.chmod(0o600)
            pending.replace(path)
        finally:
            pending.unlink(missing_ok=True)
    return True


def publish_shared(state, root):
    """Publish a root's accounts if its session changed them."""
    current = stamp(root)
    marker = root / MARKER
    try:
        if json.loads(marker.read_text()) == current: return False
    except (OSError, ValueError):
        pass
    if not (root / ACCOUNTS).is_file(): return False
    shared = Path(state) / SHARED
    for name in IDENTITY:
        if (root / name).is_file(): _copy(root / name, shared / name)
        else: (shared / name).unlink(missing_ok=True)
    _copy_text(marker, stamp(shared))
    return True


def remove(state, name, account_type=ACCOUNT_TYPE):
    """Remove an account from the shared copy; app roots drop it at their next launch."""
    path = Path(state) / SHARED / ACCOUNTS
    document = json.loads(path.read_text())
    kept = [entry for entry in document["accounts"] if (entry["name"], entry["type"]) != (name, account_type)]
    if len(kept) == len(document["accounts"]): raise ValueError("No such account: " + name)
    document["accounts"] = kept
    document["revision"] = document.get("revision", 0) + 1
    _copy_text(path, document)
    return True


def stage_request(root, request):
    """Put an app's request in the system app's root, or clear a stale one (request is None)."""
    staged = root / ACTIVITY
    shutil.rmtree(staged, ignore_errors=True)
    if request is None: return
    staged.mkdir(parents=True)
    for name in REQUEST_FILES: _copy(Path(request) / name, staged / name)


def finish_request(root, request):
    """Hand the session's answer back to the request directory; `done` is written last."""
    staged, request = root / ACTIVITY, Path(request)
    for name in RESULT_FILES:
        if is_regular(staged / name): _copy(staged / name, request / name)
    shutil.rmtree(staged, ignore_errors=True)
    # The request directory is the app's: replace, never write through, what it may have put at `done`.
    (request / "done").unlink(missing_ok=True)
    os.close(os.open(request / "done", os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600))


class ActivityCompletion:
    """Publish a remote Activity's answer before its service VM exits, once only."""

    def __init__(self, state, root, request):
        self.state, self.root, self.request = Path(state), Path(root), Path(request)
        self.lock, self.finished = threading.Lock(), False

    def __call__(self):
        with self.lock:
            if not self.finished:
                publish_shared(self.state, self.root)
                finish_request(self.root, self.request)
                self.finished = True
        return dict(finished=True)


def session_options(emu, emu_args, no_jit, window_plugin):
    """The `android_package.py run` options that start a session like the one being supervised."""
    options, arguments = ["--emu", str(emu)], list(emu_args)
    for index, argument in enumerate(arguments):
        if argument == "--android-binder-broker": options += ["--binder-broker", arguments[index + 1]]
        elif argument.startswith(("--window-stats", "--no-window-stats")): options.append(argument)
    if no_jit: options.append("--no-jit")
    if window_plugin: options += ["--android-window-plugin", str(window_plugin)]
    return options


def alive(pid):
    try:
        return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0] != "Z"
    except (OSError, IndexError):
        return False


class ActivityRelay:
    """Runs the microG Activities that an app's guest asks for, one window each."""

    def __init__(self, state, root, options):
        self.state, self.root, self.options = Path(state), Path(root).resolve(), list(options)
        self.stopped, self.threads, self.lock = threading.Event(), [], threading.Lock()
        self.last = None  # supervisor PID of the last session this relay started

    def request(self, guest_path):
        """The staged request directory under the app's root, or an error."""
        path = (self.root / str(guest_path).lstrip("/")).resolve()
        cache = path.parent
        if (cache.name != "google-activity" or not re.fullmatch(r"[0-9a-f]{32}", path.name)
                or cache.parent.parent.parent != self.root / "data/user/0"
                or not all(is_regular(path / name) for name in REQUEST_FILES)):
            raise ValueError("Invalid activity request")
        return path

    def start(self, guest_path):
        directory = self.request(guest_path)
        thread = threading.Thread(target=self.run, args=(directory,), name="AsgardGoogleActivity", daemon=True)
        with self.lock:
            if self.stopped.is_set(): raise RuntimeError("Activity relay is closed")
            self.threads = [item for item in self.threads if item.is_alive()] + [thread]
        thread.start()
        return dict(accepted=True)

    def run(self, directory):
        result = {"resultCode": 0, "response": "none"}
        try:
            request = json.loads(read_regular(directory / "request.json"))
            package, _, name = request["component"].partition("/")
            if package != GMS or not CLASS.match(name): raise ValueError("Not a Play services Activity")
            self.session(directory, name)
            nested = directory / "nested.json"
            if is_regular(nested): result.update(json.loads(read_regular(nested)))
            else: result["error"] = "The Google window ended without a result"
            # The window's session published its accounts: bring them into this app's root.
            result["changed"] = import_shared(self.state, self.root)
        except Exception as error:  # The guest waits for result.json: always answer.
            result["error"] = str(error) or type(error).__name__
        from android_sessions import publish
        publish(directory / "result.json", result)

    def requester(self, directory):
        """The package and label of the app whose request this is."""
        package = json.loads(read_regular(directory / "request.json"))["caller"].partition("/")[0]
        if not PACKAGE_NAME.match(package): raise ValueError("Invalid requesting package")
        try:
            label = json.loads(read_regular(self.state / package / "package.json")).get("label")
        except (OSError, ValueError):
            label = None
        return {"package": package, "label": label if isinstance(label, str) and label else package}

    def session(self, directory, activity):
        import android_package
        from android_sessions import control
        app = app_directory(self.state)
        # The result is returned as soon as the window closes; that session is still shutting down
        # and holds the app lock.
        deadline = time.monotonic() + 30
        while self.last is not None and alive(self.last) and time.monotonic() < deadline: time.sleep(.1)
        if control(app).get("state") in ACTIVE: raise RuntimeError("A Google window is already open")
        prepare(self.state, activity, directory, self.requester(directory))
        started = android_package.start_background(GMS, self.state, self.options)
        pid, stopping, ended = started.get("supervisor_pid"), False, None
        self.last = pid
        while True:
            if is_regular(directory / "done"): return
            if self.stopped.is_set() and not stopping:
                control(app, "stop")
                stopping = True
            live = control(app)
            if (live.get("session_id") == started.get("session_id") and live.get("state") in ACTIVE) \
                    or (pid and alive(pid)):
                ended = None
            elif ended is None:
                ended = time.monotonic()
            elif time.monotonic() - ended > 2:
                raise RuntimeError("The Google window ended without finishing (see the Asgard session log)")
            time.sleep(.2)

    def close(self):
        self.stopped.set()
        with self.lock: threads = list(self.threads)
        for thread in threads: thread.join(timeout=15)
