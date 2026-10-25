#!/usr/bin/env python3
"""A broker that an upgrade left running: detected by its program, restarted when idle, refused when busy."""
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import asgard

ROOT = Path(__file__).resolve().parents[2]
EMU = Path(os.environ.get("BIFROST_EMU", ROOT / "bifrost-emu"))


def sleeper(*arguments):
    """A process whose command line carries `arguments`, for the /proc scan."""
    return subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)", *map(str, arguments)])


class StaleBrokerTests(unittest.TestCase):
    def setUp(self):
        self.directory = Path(tempfile.mkdtemp(prefix="asgard-stale-"))
        self.children = []
        self.addCleanup(shutil.rmtree, self.directory, True)
        self.addCleanup(self.stop_children)

    def stop_children(self):
        for child in self.children:
            child.kill()
            child.wait()

    def spawn(self, *arguments):
        child = sleeper(*arguments)
        self.children.append(child)
        return child

    def test_owner_of_a_socket_is_found_from_its_peer_credentials(self):
        path = self.directory / "control.sock"
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
            server.bind(str(path))
            server.listen(1)
            self.assertEqual(asgard.broker_pid(path), os.getpid())
        self.assertIsNone(asgard.broker_pid(self.directory / "missing.sock"))

    def test_same_program_follows_the_bytes_not_the_name(self):
        first, other = self.directory / "first", self.directory / "other"
        shutil.copy(sys.executable, first)
        shutil.copy(sys.executable, other)
        with other.open("ab") as stream:
            stream.write(b"\0")  # a different build
        child = subprocess.Popen([str(first), "-c", "import time; time.sleep(60)"])
        self.children.append(child)
        time.sleep(0.3)
        self.assertTrue(asgard.same_program(child.pid, first))
        self.assertFalse(asgard.same_program(child.pid, other))
        # An upgrade replaces the file; the running process keeps reading its old bytes through /proc.
        replacement = self.directory / "replacement"
        shutil.copy(sys.executable, replacement)
        os.replace(replacement, first)
        self.assertTrue(asgard.same_program(child.pid, first), "same bytes in a new file are the same program")
        os.replace(other, first)
        self.assertFalse(asgard.same_program(child.pid, first))
        self.assertTrue(asgard.same_program(os.getpid() + 10 ** 7, first), "a process that cannot be read is left alone")

    def test_clients_are_apps_not_the_broker_or_its_services(self):
        path = self.directory / "realm.sock"
        app = self.spawn("--android-binder-broker", path, "--android-runtime")
        self.spawn("--asgard-broker", path)
        self.spawn("--asgard-service-parent", 1, "--android-binder-broker", path)
        service = self.spawn("--asgard-service-parent", 1, "--android-binder-broker", path, "--service-child")
        self.spawn("--android-binder-broker", self.directory / "another.sock")
        time.sleep(0.3)
        self.assertEqual(sorted(asgard.broker_clients(path)), sorted([app.pid, service.pid]))

    @unittest.skipUnless(EMU.is_file(), "needs a bifrost-emu build (BIFROST_EMU)")
    def test_idle_stale_broker_restarts_and_a_busy_one_is_refused(self):
        old, state = self.directory / "bifrost-emu-old", self.directory / "state"
        shutil.copy(EMU, old)
        with old.open("ab") as stream:
            stream.write(b"\0")  # trailing bytes: an older build as far as the broker check can tell
        os.chmod(old, 0o755)
        first = asgard.ensure_broker(state, old)
        self.addCleanup(lambda: asgard.control(Path(first["socket"]), stop=True))
        old_pid = asgard.broker_pid(first["socket"])
        self.assertTrue(asgard.same_program(old_pid, old))
        # The same build is reused.
        self.assertEqual(asgard.broker_pid(asgard.ensure_broker(state, old)["socket"]), old_pid)
        # A newer build finds the broker busy: a clear refusal, and the broker is left running.
        client = self.spawn("--android-binder-broker", first["socket"], "--android-runtime")
        time.sleep(0.3)
        with self.assertRaisesRegex(ValueError, "older build.*close them.*realm stop"):
            asgard.ensure_broker(state, EMU)
        self.assertEqual(asgard.broker_pid(first["socket"]), old_pid)
        # Once idle it is replaced by one running the new build.
        client.kill()
        client.wait()
        second = asgard.ensure_broker(state, EMU)
        new_pid = asgard.broker_pid(second["socket"])
        self.assertNotEqual(new_pid, old_pid)
        self.assertTrue(asgard.same_program(new_pid, EMU))
        self.assertFalse(Path(f"/proc/{old_pid}").exists() and
                         Path(f"/proc/{old_pid}/stat").read_text().split()[2] != "Z")

    @unittest.skipUnless(EMU.is_file(), "needs a bifrost-emu build (BIFROST_EMU)")
    def test_realm_stop_force_ends_joined_apps_and_restart_replaces_the_broker(self):
        state = self.directory / "state"
        first = asgard.ensure_broker(state, EMU)
        self.addCleanup(lambda: asgard.stop_realm(state, force=True))
        client = self.spawn("--android-binder-broker", first["socket"], "--android-runtime")
        time.sleep(0.3)
        # A plain stop stops the broker and leaves the app process alone.
        stopped = asgard.stop_realm(state)
        self.assertEqual(stopped["state"], "stopped")
        self.assertIsNone(client.poll())
        asgard.ensure_broker(state, EMU)
        # --force ends the joined app process too, and says which.
        stopped = asgard.stop_realm(state, force=True)
        self.assertEqual((stopped["state"], stopped["ended"]), ("stopped", [client.pid]))
        self.assertIsNotNone(client.wait(timeout=10))
        self.assertEqual(asgard.broker_status(state)["state"], "stopped")
        # Stopping a realm that is not running is fine, and so is a command-line restart of it.
        self.assertEqual(asgard.stop_realm(state, force=True)["state"], "stopped")
        result = subprocess.run([sys.executable, str(ROOT / "Scripts/Android/asgard.py"), "--emu", str(EMU),
                                 "--state-root", str(state), "realm", "restart", "--force"],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(asgard.broker_status(state)["state"], "running")
        bad = subprocess.run([sys.executable, str(ROOT / "Scripts/Android/asgard.py"), "--state-root", str(state),
                              "realm", "stop", "--oops"], capture_output=True, text=True, timeout=30)
        self.assertEqual(bad.returncode, 2)
        self.assertIn("realm accepts start, status, stop [--force] or restart [--force]", bad.stderr)


if __name__ == "__main__":
    unittest.main()
