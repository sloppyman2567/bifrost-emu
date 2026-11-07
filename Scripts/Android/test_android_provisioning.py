#!/usr/bin/env python3
"""asgard setup: what an Asgard tree is missing, and running the missing steps (no downloads, no builds)."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import re
import sys
import tempfile
import types
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import android_provisioning as provisioning  # noqa: E402

ROOT = provisioning.ROOT
MICROG = (("microg-a.apk", 5), ("microg-b.apk", 3))


def tree(directory, *, sources=True, done=()):
    """A tree with a fake microG tool; `done` lists step keys whose results already exist."""
    root = Path(directory)
    (root / "Tools/Android").mkdir(parents=True)
    packages = {f"pkg{i}": ("dir", name, size, "", 0) for i, (name, size) in enumerate(MICROG)}
    (root / "Tools/Android/android_microg.py").write_text(f"PACKAGES = {packages!r}\n")
    if sources:
        (root / "Makefile").write_text("")
    for step in provisioning.STEPS:
        if step.key in done:
            make(root, step)
    return root


def make(root, step):
    if step.key == "microg":
        (root / "build/microg").mkdir(parents=True, exist_ok=True)
        for name, size in MICROG:
            (root / "build/microg" / name).write_bytes(b"x" * size)
    for output in step.outputs:
        (root / output).parent.mkdir(parents=True, exist_ok=True)
        (root / output).write_text("")


class Recorder:
    """A stand-in for subprocess.run: records each command and makes what the step's commands produce."""

    def __init__(self, root, fail=None, tools_fail=False):
        self.root, self.fail, self.tools_fail, self.calls = root, fail, tools_fail, []

    def __call__(self, command, cwd=None, env=None):
        self.calls.append(list(command))
        if command[0] == "bash":
            return types.SimpleNamespace(returncode=1 if self.tools_fail else 0)
        for step in provisioning.STEPS:
            if list(command) in [list(c) for c in step.commands]:
                if self.fail == step.key:
                    return types.SimpleNamespace(returncode=3)
                if command == list(step.commands[-1]):
                    make(self.root, step)
                return types.SimpleNamespace(returncode=0)
        raise AssertionError(f"unexpected command {command}")


class MissingTests(unittest.TestCase):
    def test_every_step_is_missing_from_an_empty_tree_and_each_result_removes_its_step(self):
        with tempfile.TemporaryDirectory() as directory:
            root = tree(directory)
            self.assertEqual([step.key for step in provisioning.missing(root)], [s.key for s in provisioning.STEPS])
            for index, step in enumerate(provisioning.STEPS):
                make(root, step)
                self.assertEqual(len(provisioning.missing(root)), len(provisioning.STEPS) - index - 1, step.key)
            self.assertEqual(provisioning.status(root)["missing"], [])

    def test_a_step_needs_all_of_its_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = tree(directory)
            tools = next(step for step in provisioning.STEPS if step.key == "tools")
            (root / "Tools/android-tools").mkdir(parents=True)
            (root / tools.outputs[0]).write_text("")           # aapt2 without apksigner
            self.assertIn("tools", [step.key for step in provisioning.missing(root)])

    def test_microg_must_have_the_pinned_size(self):
        with tempfile.TemporaryDirectory() as directory:
            root = tree(directory)
            (root / "build/microg").mkdir(parents=True)
            for name, size in MICROG:
                (root / "build/microg" / name).write_bytes(b"x" * (size - 1))      # a download cut short
            self.assertIn("microg", [step.key for step in provisioning.missing(root)])
            make(root, next(step for step in provisioning.STEPS if step.key == "microg"))
            self.assertNotIn("microg", [step.key for step in provisioning.missing(root)])

    def test_status_says_whether_this_tree_can_set_itself_up(self):
        with tempfile.TemporaryDirectory() as directory:
            root = tree(directory)
            status = provisioning.status(root)
            self.assertTrue(status["can_set_up"] and not status["installed"])
            self.assertEqual(status["missing"][0]["id"], "runtime")
            self.assertIn("fetch-android-runtime.py", status["missing"][0]["command"])
            self.assertTrue(all(item["title"] and item["why"] for item in status["missing"]))
        with tempfile.TemporaryDirectory() as directory:
            root = tree(directory, sources=False)
            self.assertFalse(provisioning.status(root)["can_set_up"])          # no Makefile: nothing to build from
        with tempfile.TemporaryDirectory() as directory:
            root = tree(directory)
            (root / provisioning.MARKER).write_text("{}")
            status = provisioning.status(root)
            self.assertTrue(status["installed"] and not status["can_set_up"])  # an installed copy is reinstalled


class RunTests(unittest.TestCase):
    def run_setup(self, root, **options):
        lines = []
        recorder = Recorder(root, **options)
        code = provisioning.run_setup(root, out=lines.append, runner=recorder)
        return code, lines, recorder.calls

    def test_the_missing_steps_run_in_order_after_the_host_tools_are_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            root = tree(directory, done=("runtime", "tools", "microg"))
            code, lines, calls = self.run_setup(root)
            self.assertEqual(code, 0, lines)
            self.assertEqual(calls[0][0], "bash")
            self.assertEqual(calls[0][2:], list(provisioning.HOST_TOOLS))
            script_names = [Path(call[1]).name for call in calls[1:]]
            self.assertEqual(script_names, ["build-android-adapters.py", "android_managed.py",
                                            "android_framework.py", "android_art.py"])     # finished steps are skipped
            self.assertEqual(lines[-1], "Asgard is set up.")
            self.assertEqual(provisioning.missing(root), [])

    def test_a_tree_that_is_complete_runs_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = tree(directory, done=[step.key for step in provisioning.STEPS])
            code, lines, calls = self.run_setup(root)
            self.assertEqual((code, calls, lines), (0, [], ["Asgard is set up."]))

    def test_a_failing_step_stops_the_setup_and_says_how_to_continue(self):
        with tempfile.TemporaryDirectory() as directory:
            root = tree(directory)
            code, lines, calls = self.run_setup(root, fail="adapters")
            self.assertEqual(code, 1)
            self.assertIn("runtime", [step.key for step in provisioning.STEPS][:1])
            self.assertNotIn("android_framework.py", [Path(call[-1]).name for call in calls])   # nothing after it ran
            self.assertRegex(lines[-1], r"Couldn't set up Runtime adapters: build-android-adapters\.py stopped with status 3")
            self.assertIn("resumes", lines[-1])
            self.assertEqual([step.key for step in provisioning.missing(root)][0], "adapters")

    def test_missing_host_tools_stop_before_any_step(self):
        with tempfile.TemporaryDirectory() as directory:
            root = tree(directory)
            code, lines, calls = self.run_setup(root, tools_fail=True)
            self.assertEqual(code, 1)
            self.assertEqual([call[0] for call in calls], ["bash"])
            self.assertIn("tools the setup needs are missing", lines[-1])

    def test_a_step_that_makes_nothing_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            root = tree(directory, done=[step.key for step in provisioning.STEPS if step.key != "launcher"])
            lines = []
            code = provisioning.run_setup(root, out=lines.append,
                                          runner=lambda command, cwd=None, env=None: types.SimpleNamespace(returncode=0))
            self.assertEqual(code, 1)
            self.assertIn("App launcher did not appear", lines[-1])

    def test_an_installed_copy_is_not_set_up_from_here(self):
        with tempfile.TemporaryDirectory() as directory:
            root = tree(directory)
            (root / provisioning.MARKER).write_text("{}")
            code, lines, calls = self.run_setup(root)
            self.assertEqual((code, calls), (1, []))
            self.assertIn("make install-asgard", lines[-1])


class CommandTests(unittest.TestCase):
    def invoke(self, argv, root):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = provisioning.main(argv, root=root)
        return code, out.getvalue()

    def test_check_lists_what_is_missing_and_how_to_fix_it(self):
        with tempfile.TemporaryDirectory() as directory:
            root = tree(directory)
            code, text = self.invoke(["--check"], root)
            self.assertEqual(code, 1)
            self.assertIn("Android runtime:", text)
            self.assertIn("asgard.py setup", text)
        with tempfile.TemporaryDirectory() as directory:
            root = tree(directory, done=[step.key for step in provisioning.STEPS])
            self.assertEqual(self.invoke(["--check"], root), (0, "Asgard is set up.\n"))

    def test_check_json_is_for_the_library_and_always_succeeds(self):
        with tempfile.TemporaryDirectory() as directory:
            root = tree(directory)
            code, text = self.invoke(["--check", "--json"], root)
            report = json.loads(text)
            self.assertEqual(code, 0)
            self.assertEqual([item["id"] for item in report["missing"]], [s.key for s in provisioning.STEPS])
            self.assertTrue(report["can_set_up"])

    def test_json_alone_is_refused(self):
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            provisioning.main(["--json"], root=ROOT)


class DriftTests(unittest.TestCase):
    """The steps repeat what the Makefile and the installer know; a change in one must show up in the others."""

    def test_the_steps_make_everything_the_installer_requires(self):
        spec = importlib.util.spec_from_file_location("install_asgard", ROOT / "Scripts/Setup/install-asgard.py")
        installer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(installer)
        produced = {output for step in provisioning.STEPS for output in step.outputs}
        for path in (*installer.REQUIRED, *installer.MANIFESTS):
            self.assertIn(path, produced, f"{path} is required by install-asgard.py but no setup step makes it")

    def test_the_host_tools_and_commands_match_the_makefile(self):
        makefile = (ROOT / "Makefile").read_text()
        tools = re.search(r"^ASGARD_TOOLS := (.*)$", makefile, re.M).group(1).split()
        self.assertEqual(list(provisioning.HOST_TOOLS), tools)
        recipe = re.search(r"^android-provision:\n((?:\t.*\n)+)", makefile, re.M).group(1)
        for step in provisioning.STEPS:
            for command in step.commands:
                script = command[1]
                self.assertIn(script, recipe, f"{script} is not part of make android-provision")
                self.assertTrue((ROOT / script).is_file(), script)


if __name__ == "__main__":
    unittest.main()
