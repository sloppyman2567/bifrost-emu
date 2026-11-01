#!/usr/bin/env python3
"""Check plugin startup failures and side-effect-free help/config requests."""
import argparse
import os
import pathlib
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emu", required=True)
    parser.add_argument("--plugin", required=True)
    parser.add_argument("--guest", default="Tests/Unit/hello.elf")
    args = parser.parse_args()
    # Avoid environment-dependent loader state and accidental soname search.
    plugin = str(pathlib.Path(args.plugin).resolve())
    guest = str(pathlib.Path(args.guest).resolve())
    guest_path = pathlib.Path(guest)
    source = guest_path.with_suffix(".c")
    if not guest_path.is_file() or (source.is_file() and source.stat().st_mtime > guest_path.stat().st_mtime):
        raise RuntimeError(f"missing or stale guest fixture: cross-build {guest}")
    with tempfile.TemporaryDirectory(prefix="bifrost-plugin-cli-") as directory:
        log = pathlib.Path(directory) / "events"
        config = pathlib.Path(directory) / "empty.conf"
        config.write_text("")
        env = dict(os.environ, BIFROST_TEST_PLUGIN_LOG=str(log))
        env.pop("BIFROST_ROOT", None)
        cases = [
            (["--android-window-plugin"], 2, "", False),
            (["--android-window-plugin", "", guest], 2, "", False),
            (["--android-window-plugin", plugin, "--help"], 0, "", False),
            (["--android-window-plugin", plugin, "--print-config"], 0, "", False),
            (["--android-window-plugin", plugin, "--print-config", "--android", "unused.so"], 0, "", False),
            (["--android-window-plugin", plugin, guest, "--print-config"], 0, "", True),
            (["--android-window-plugin", plugin, "--android-window-plugin", plugin, guest], 2, "", False),
            (["--android-window-plugin", str(log) + ".absent.so", guest], 2, "", False),
            (["--android-window-plugin", plugin, guest], 2, "bad-version", True),
            (["--android-window-plugin", plugin, guest], 2, "resize-fail", True),
            (["--android-window-plugin", str(log) + ".absent.so", "--android", "unused.so"], 2, "", False),
            (["--android-window-plugin", plugin, guest], 0, "", True),
        ]
        for options, expected, mode, loaded in cases:
            log.unlink(missing_ok=True)
            result = subprocess.run(
                [args.emu, "--config", str(config), *options],
                env=dict(env, BIFROST_TEST_PLUGIN_MODE=mode),
                capture_output=True, text=True, timeout=10,
            )
            if result.returncode != expected or log.exists() != loaded:
                raise RuntimeError(f"plugin startup check failed: {options}\n"
                                   f"exit={result.returncode}, expected={expected}\n"
                                   f"{result.stdout}{result.stderr}")
    print(f"Plugin CLI: {len(cases)} startup/help/error cases passed")


if __name__ == "__main__":
    main()
