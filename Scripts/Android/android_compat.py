"""Documented compatibility status of tested apps (compatibility.json); an app without an entry is untested.

An entry may carry `settings`: the app's built-in defaults (android_settings.KEYS), which a saved setting overrides."""
import json
from pathlib import Path
import re

DATA = Path(__file__).with_name("compatibility.json")
UNTESTED = "untested"


def load(path=DATA):
    data = json.loads(path.read_text())
    if data.get("schema_version") != 1 or not isinstance(data.get("apps"), dict):
        raise ValueError("Unsupported compatibility data")
    return data


def entry(data, package):
    """The recorded entry, or an untested placeholder."""
    return data["apps"].get(package) or {"status": UNTESTED, "evidence": "No test is recorded for this app."}


def summary(package, value):
    version = value.get("tested_version")
    detail = ", ".join(x for x in (version and f"v{version}", value.get("checked")) if x)
    return f"{package}  {value['status']}" + (f"  ({detail})" if detail else "")


def describe(data, package):
    value = entry(data, package)
    lines = [summary(package, value)]
    if value["status"] in data["statuses"]:
        lines.append("  " + data["statuses"][value["status"]])
    lines.append("  Evidence: " + value["evidence"])
    lines += ["  - " + note for note in value.get("notes", [])]
    if value.get("page"):
        lines.append("  Details: " + value["page"])
    return lines


def validate(data, root):
    """Problems in the data file, for the test that guards it."""
    problems = []
    for package, value in data["apps"].items():
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+", package):
            problems.append("invalid package " + package)
        if value.get("status") not in data["statuses"]:
            problems.append(f"{package}: unknown status {value.get('status')!r}")
        if not value.get("evidence") or not value.get("name"):
            problems.append(f"{package}: name and evidence are required")
        if not re.fullmatch(r"\d{4}-\d{2}(-\d{2})?", str(value.get("checked", ""))):
            problems.append(f"{package}: checked must be YYYY-MM or YYYY-MM-DD")
        settings = value.get("settings", {})
        if not isinstance(settings, dict):
            problems.append(f"{package}: settings must be an object")
        else:
            import android_settings
            problems += [f"{package}: invalid default setting {key}={settings[key]!r}" for key in settings
                         if key not in android_settings.KEYS or not android_settings.valid(key, settings[key])]
        if value.get("page") and not (root / value["page"]).is_file():
            problems.append(f"{package}: missing page {value['page']}")
    return problems
