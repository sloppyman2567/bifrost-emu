"""Per-app settings saved beside the app and applied to every launch, from the CLI or the library."""
import json

SETTINGS = "launcher-settings.json"
# Names accepted by the emulator's [android] back_key / BIFROST_ANDROID_BACK_KEY.
BACK_KEYS = (("escape", "Esc"), ("backspace", "Backspace"), ("delete", "Delete"),
             ("f1", "F1"), ("f2", "F2"), ("f12", "F12"), ("none", "No back key"))
# auto follows the host's default route; offline blocks all non-loopback IP traffic (BIFROST_ANDROID_OFFLINE=1) and
# reports no network to Android.
NETWORKS = ("auto", "offline")
CHOICES = {"back_key": tuple(name for name, _ in BACK_KEYS), "network": NETWORKS}
KEYS = ("window_stats", "back_key", "network")
BOOLEANS = {"on": True, "true": True, "yes": True, "1": True, "off": False, "false": False, "no": False, "0": False}


def valid(key, value):
    if key == "window_stats":
        return type(value) is bool
    return key in CHOICES and isinstance(value, str) and value in CHOICES[key]


def load(app):
    """Saved settings; unknown keys and malformed values fall back to defaults."""
    try:
        saved = json.loads((app / SETTINGS).read_text())
    except (OSError, ValueError):
        return {}
    return {key: saved[key] for key in KEYS if isinstance(saved, dict) and key in saved and valid(key, saved[key])}


def save(app, settings):
    from android_sessions import publish
    publish(app / SETTINGS, settings)


def parse(key, text):
    """The value a user typed for `key`; ValueError names the accepted ones."""
    if key not in KEYS:
        raise ValueError(f"Unknown setting {key!r}; settings: {', '.join(KEYS)}")
    if key == "window_stats":
        if text.lower() not in BOOLEANS:
            raise ValueError("window_stats must be on or off")
        return BOOLEANS[text.lower()]
    if text not in CHOICES[key]:
        raise ValueError(f"{key} must be one of: {', '.join(CHOICES[key])}")
    return text


def launch_changes(settings):
    """Emulator flags and environment that a launch adds for these settings; defaults add nothing."""
    flags = ["--window-stats" if settings["window_stats"] else "--no-window-stats"] if "window_stats" in settings else []
    env = {}
    if "back_key" in settings:
        env["BIFROST_ANDROID_BACK_KEY"] = settings["back_key"]
    if settings.get("network") == "offline":
        env["BIFROST_ANDROID_OFFLINE"] = "1"
    return flags, env


def apply_to_launch(app, options, environment):
    """Options and extra environment from saved choices; the command line and environment win."""
    flags, env = launch_changes(load(app))
    if any(option.startswith(("--window-stats", "--no-window-stats")) for option in options):
        flags = []
    return list(options) + flags, {key: value for key, value in env.items() if key not in environment}
