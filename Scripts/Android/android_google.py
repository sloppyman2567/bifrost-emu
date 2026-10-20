"""Google accounts for the managed Android runtime.

Sign-in is microG's own UI (GmsCore's LoginActivity: Google's embedded setup page in a
WebView), run as a session of the provisioned com.google.android.gms system app. Accounts are
stored by the guest account service in each root; this module keeps one copy of them, with the
device registration microG made for them, under <state>/.google and imports it into every app
root at launch, so one sign-in serves every app. A root that signs in or out during a session
publishes its accounts when the session ends.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools/Android"))
import android_microg

GMS = "com.google.android.gms"
ACCOUNT_TYPE = "com.google"
LOGIN_ACTIVITY = "org.microg.gms.auth.login.LoginActivity"
SETTINGS_ACTIVITY = "org.microg.gms.ui.SettingsActivity"
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


def app_directory(state):
    return Path(state) / GMS


def prepare(state, activity=LOGIN_ACTIVITY):
    """Register the microG system app as a launchable session and choose its Activity."""
    directory = app_directory(state)
    directory.mkdir(parents=True, exist_ok=True)
    from android_sessions import publish
    publish(directory / "package.json", {
        "schema_version": 1, "package": GMS, "label": "Google account", "system": True,
        "version_code": 252432034, "version_name": android_microg.VERSION, "icon": None,
        "installed_at": int(time.time()), "guest_code": "/system/priv-app/GmsCore",
        "launcher": {"kind": "managed_activity", "class": activity},
        "files": {}, "packages": [], "apk_count": 1, "arm64_library_count": 0})
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


def _copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as stream:
        pending = Path(stream.name)
        try:
            with Path(source).open("rb") as original:
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
