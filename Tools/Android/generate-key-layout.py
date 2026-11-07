#!/usr/bin/env python3
"""Generate the KeyCharacterMap tables of the pinned Android 15 Virtual.kcm (the virtual keyboard)
and Generic.kcm (a gamepad, which has no map of its own: an Xbox 360 pad's fallback keys).

Follows libinput's KeyCharacterMap parser (android-15.0.0_r1): a behavior is
added to the front of its key's list, so the table keeps that order, and a key
without a `number` takes the last digit or phone symbol among its characters.
No downloaded sources enter the checkout. --check verifies the committed table.
"""
import argparse
import hashlib
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "Guest/Android/Managed/Framework/FrameworkKeyLayout.java"
TAG = "android-15.0.0_r1"
# (input path, SHA-256, Java name prefix)
INPUTS = (
    ("data/keyboards/Virtual.kcm", "6356265ec24b7eb2bf835ea269247cf46de646bd0d40a5755cf75d5fa7c581f1", ""),
    ("data/keyboards/Generic.kcm", "4710a130708720a6eed7f1b56e895b2f0e42579de66e728bdb4e8a7580aea354", "GAMEPAD_"),
)
# android/input.h AMETA_* for the modifier names of KeyCharacterMap.cpp.
MODIFIERS = {
    "shift": 0x1, "lshift": 0x40, "rshift": 0x80, "alt": 0x2, "lalt": 0x10, "ralt": 0x20,
    "ctrl": 0x1000, "lctrl": 0x2000, "rctrl": 0x4000, "meta": 0x10000, "lmeta": 0x20000,
    "rmeta": 0x40000, "sym": 0x4, "fn": 0x8, "capslock": 0x100000, "numlock": 0x200000,
    "scrolllock": 0x400000,
}
TYPES = {"NUMERIC": 1, "PREDICTIVE": 2, "ALPHA": 3, "FULL": 4, "SPECIAL_FUNCTION": 5, "OVERLAY": 6}
ESCAPES = {"n": 10, "t": 9, "\\": 92, "'": 39, '"': 34}
PHONE_SYMBOLS = set("()#*-+,.':;/")


def literal(text, at):
    """One 'x' character literal starting at text[at]; returns (UTF-16 unit, next index)."""
    if text[at] != "'":
        raise ValueError(text)
    at += 1
    if text[at] == "\\":
        if text[at + 1] == "u":
            value, at = int(text[at + 2:at + 6], 16), at + 6
        else:
            value, at = ESCAPES[text[at + 1]], at + 2
    elif 32 <= ord(text[at]) <= 126 and text[at] != "'":
        value, at = ord(text[at]), at + 1
    else:
        raise ValueError(text)
    if text[at] != "'":
        raise ValueError(text)
    return value, at + 1


def behavior(text):
    """The part after ':': a character, `none`, `fallback KEY` or `replace KEY`."""
    at, character, have_character, fallback, replacement = 0, 0, False, "-", "-"
    while True:
        while at < len(text) and text[at] in " \t":
            at += 1
        if at >= len(text) or text[at] == "#":
            return character, fallback, replacement
        if text[at] == "'":
            value, at = literal(text, at)
            if not value or have_character or replacement != "-":
                raise ValueError(text)
            character, have_character = value, True
            continue
        words = text[at:].split(None, 2)
        if words[0] == "none" and not have_character and replacement == "-":
            have_character, at = True, at + 4
        elif words[0] in ("fallback", "replace") and len(words) > 1 and fallback == "-" == replacement:
            if words[0] == "fallback":
                fallback = words[1]
            elif have_character:
                raise ValueError(text)
            else:
                replacement = words[1]
            at = text.index(words[1], at + len(words[0])) + len(words[1])
        else:
            raise ValueError(text)


def modifier(token):
    if token == "base":
        return 0
    state = 0
    for name in token.split("+"):
        if name not in MODIFIERS or state & MODIFIERS[name]:
            raise ValueError(token)
        state |= MODIFIERS[name]
    return state


def parse(source, name):
    keyboard_type, keys, current = 0, [], None
    for number, raw in enumerate(source.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        try:
            if current is None:
                words = line.split()
                if words[0] == "type" and not keyboard_type:
                    keyboard_type = TYPES[words[1]]
                elif words[0] == "key" and words[2] == "{" and len(words) == 3:
                    current = {"name": words[1], "label": 0, "number": 0, "behaviors": []}
                else:
                    raise ValueError(line)
            elif line.startswith("}"):
                if not current["number"]:  # KeyCharacterMap::Parser::finishKey
                    digit = symbol = 0
                    for _, character, _, _ in current["behaviors"]:
                        if ord("0") <= character <= ord("9"):
                            digit = character
                        elif character and chr(character) in PHONE_SYMBOLS:
                            symbol = character
                    current["number"] = digit or symbol
                keys.append(current)
                current = None
            else:
                head, _, tail = line.partition(":")
                character, fallback, replacement = behavior(tail)
                for name in (p.strip() for p in head.split(",")):
                    if name in ("label", "number"):
                        if current[name]:
                            raise ValueError("duplicate " + name)
                        current[name] = character
                    else:
                        meta = modifier(name)
                        if any(b[0] == meta for b in current["behaviors"]):
                            raise ValueError("duplicate behavior")
                        current["behaviors"].insert(0, (meta, character, fallback, replacement))
        except (ValueError, KeyError, IndexError) as error:
            raise ValueError(f"{name}:{number}: {error}") from error
    if current is not None or not keyboard_type or len({k["name"] for k in keys}) != len(keys):
        raise ValueError(f"{name}: unterminated key, missing type or duplicate key")
    return keyboard_type, keys


def table(source, path, prefix):
    keyboard_type, keys = parse(source, Path(path).name)
    rows = []
    for key in keys:
        row = f'{key["name"]} {key["label"]} {key["number"]}'
        for meta, character, fallback, replacement in key["behaviors"]:
            row += f" | {meta} {character} {fallback} {replacement}"
        rows.append(f'        "{row}",')
    kind = next(name for name, value in TYPES.items() if value == keyboard_type)
    return [
        f"    // {Path(path).name}",
        f"    static final int {prefix}TYPE = {keyboard_type};  // KeyCharacterMap.{kind}",
        f"    static final String[] {prefix}KEYS = {{",
        *rows,
        "    };",
    ]


def generate(sources):
    lines = [
        "// Generated by Tools/Android/generate-key-layout.py; do not edit.",
        f"// AOSP {TAG}; Apache-2.0. Inputs (SHA-256):",
        *(f"// {sha256} {path}" for path, sha256, _ in INPUTS),
        "final class FrameworkKeyLayout {",
        "    // `KEY label number`, then ` | meta character fallback replacement` per behavior,",
        "    // in the order libinput keeps them. Characters are UTF-16 units, `-` is no key.",
    ]
    for (path, _, prefix), source in zip(INPUTS, sources):
        lines += table(source, path, prefix)
    return "\n".join([*lines, "}", ""])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify the committed table")
    args = parser.parse_args()
    sources = []
    for path, sha256, _ in INPUTS:
        url = f"https://raw.githubusercontent.com/aosp-mirror/platform_frameworks_base/{TAG}/{path}"
        with urllib.request.urlopen(url, timeout=60) as response:
            source = response.read()
        if hashlib.sha256(source).hexdigest() != sha256:
            raise SystemExit(f"{Path(path).name} does not match the pinned input")
        sources.append(source.decode())
    text = generate(sources)
    if args.check:
        if OUTPUT.read_text() != text:
            raise SystemExit(f"{OUTPUT.relative_to(ROOT)} is stale; run {Path(__file__).name}")
        print(f"{OUTPUT.relative_to(ROOT)} is up to date")
    else:
        OUTPUT.write_text(text)
        print(f"Wrote {OUTPUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
