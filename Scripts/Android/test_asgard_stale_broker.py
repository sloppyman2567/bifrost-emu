#!/usr/bin/env python3
"""A broker that an upgrade left running: detected by its program, restarted when idle, refused when busy."""
import io
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

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


class BrokerStartFailureTests(unittest.TestCase):
    """A broker that cannot start says how it ended and what it printed, on both start paths (the realm's
    `ensure_broker` and a launched app's private broker in `ProcessHost`), instead of a bare 'did not start'."""

    def setUp(self):
        self.directory = Path(tempfile.mkdtemp(prefix="asgard-start-fail-"))
        self.addCleanup(shutil.rmtree, self.directory, True)

    def fake_emulator(self, body):
        path = self.directory / "emulator"
        path.write_text("#!/bin/sh\n" + body)
        path.chmod(0o755)
        return path

    def test_descriptions_name_the_loader_and_signals(self):
        self.assertIn("127", asgard.exit_description(127))
        self.assertIn("loader", asgard.exit_description(127))
        self.assertEqual(asgard.exit_description(-signal.SIGKILL), "was killed by SIGKILL")
        self.assertEqual(asgard.exit_description(1), "exited with status 1")

    def test_log_tail_is_only_this_starts_output(self):
        log = self.directory / "broker.log"
        log.write_text("an earlier start\n")
        start = log.stat().st_size
        with log.open("a") as stream:
            stream.write("".join(f"line {number}\n" for number in range(40)))
        tail = asgard.log_tail(log, start).splitlines()
        self.assertEqual((len(tail), tail[0], tail[-1]), (15, "  line 25", "  line 39"))
        self.assertIn("no output", asgard.log_tail(log, log.stat().st_size))

    def test_realm_broker_failure_carries_the_loader_message(self):
        emulator = self.fake_emulator("echo 'error while loading shared libraries: libgone.so.1: cannot open' >&2\nexit 127\n")
        with self.assertRaises(ValueError) as caught:
            asgard.ensure_broker(self.directory / "state", emulator)
        message = str(caught.exception)
        self.assertIn("status 127", message)
        self.assertIn("libgone.so.1", message)

    def test_realm_broker_killed_by_a_signal_says_so(self):
        emulator = self.fake_emulator("kill -KILL $$\n")
        with self.assertRaises(ValueError) as caught:
            asgard.ensure_broker(self.directory / "state", emulator)
        self.assertIn("killed by SIGKILL", str(caught.exception))

    def test_private_broker_failure_carries_the_message_too(self):
        import android_processes
        emulator = self.fake_emulator("echo 'Asgard: socket path is too long' >&2\nexit 2\n")
        with self.assertRaises(RuntimeError) as caught:
            android_processes.ProcessHost(self.directory / "root", emulator, [], {}, None, self.directory / "logs")
        message = str(caught.exception)
        self.assertIn("status 2", message)
        self.assertIn("socket path is too long", message)


def process_running(pid):
    """Alive and not a zombie (the broker is this test's child: it stays a zombie until reaped)."""
    try:
        return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0] != "Z"
    except OSError:
        return False


def open_channel(path):
    """Attach to the broker the way an app's /dev/binder open does: an OPEN request whose connection stays open."""
    channel = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    channel.settimeout(5)
    channel.connect(str(path))
    body = struct.pack("<IIIi", 0, os.O_RDWR, 10000, os.getpid())
    channel.sendall(asgard.HEADER.pack(asgard.MAGIC, asgard.VERSION, 1, len(body), 0, 0) + body)
    magic, _, operation, size, _, _ = asgard.HEADER.unpack(asgard.receive(channel, asgard.HEADER.size))
    reply = asgard.receive(channel, size)
    assert (magic, operation) == (asgard.MAGIC, 1) and struct.unpack("<q", reply[:8])[0] == 0
    return channel


@unittest.skipUnless(EMU.is_file(), "needs a bifrost-emu build (BIFROST_EMU)")
class RealmIdleTests(unittest.TestCase):
    """A realm with no app attached ends after ASGARD_REALM_IDLE_SECONDS; the next launch starts a fresh one."""

    def setUp(self):
        self.state = Path(tempfile.mkdtemp(prefix="asgard-idle-")) / "state"
        self.addCleanup(shutil.rmtree, self.state.parent, True)
        self.addCleanup(lambda: asgard.stop_realm(self.state, force=True))

    def start(self, idle):
        with mock.patch.dict(os.environ, {"ASGARD_REALM_IDLE_SECONDS": str(idle)}):
            status = asgard.ensure_broker(self.state, EMU)
        return status, asgard.broker_pid(status["socket"])

    def wait_until_gone(self, pid, seconds):
        deadline = time.monotonic() + seconds
        while process_running(pid) and time.monotonic() < deadline:
            time.sleep(0.1)
        return not process_running(pid)

    def test_an_idle_realm_ends_and_the_next_launch_starts_another(self):
        status, pid = self.start(2)
        self.assertTrue(self.wait_until_gone(pid, 12), "the idle broker is still running")
        self.assertFalse(Path(status["socket"]).exists(), "its socket was left behind")
        again, new_pid = self.start(600)
        self.assertEqual(again["state"], "running")
        self.assertNotEqual(new_pid, pid)

    def test_an_attached_app_keeps_the_realm_until_it_leaves(self):
        status, pid = self.start(2)
        channel = open_channel(status["socket"])
        time.sleep(5)  # more than twice the idle time; no control request in between, which would restart the clock
        self.assertTrue(process_running(pid), "the broker ended with an app attached")
        channel.close()
        self.assertTrue(self.wait_until_gone(pid, 12), "the broker stayed after the app left")

    def test_a_control_request_restarts_the_clock(self):
        status, pid = self.start(4)
        for _ in range(4):
            time.sleep(2)
            self.assertEqual(asgard.broker_status(self.state)["state"], "running")
        self.assertTrue(process_running(pid))

    def test_zero_keeps_the_realm_until_it_is_stopped(self):
        status, pid = self.start(0)
        time.sleep(3)
        self.assertTrue(process_running(pid))

    def test_an_emulator_without_the_option_still_starts_a_realm(self):
        wrapper = self.state.parent / "old-emulator"
        wrapper.write_text(f'#!/bin/sh\nfor a in "$@"; do [ "$a" = --asgard-idle ] && exit 2; done\nexec {EMU} "$@"\n')
        wrapper.chmod(0o755)
        with mock.patch.dict(os.environ, {"ASGARD_REALM_IDLE_SECONDS": "600"}), \
                mock.patch("sys.stderr", new_callable=io.StringIO) as note:
            status = asgard.ensure_broker(self.state, wrapper)
        self.assertEqual(status["state"], "running")
        self.assertIn("predates the realm idle timeout", note.getvalue())

    def test_a_bad_setting_is_refused(self):
        for value in ("soon", "-1", "604801"):
            with mock.patch.dict(os.environ, {"ASGARD_REALM_IDLE_SECONDS": value}):
                with self.assertRaises(ValueError):
                    asgard.realm_idle_seconds()
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("ASGARD_REALM_IDLE_SECONDS", None)
            self.assertEqual(asgard.realm_idle_seconds(), asgard.REALM_IDLE_SECONDS)


if __name__ == "__main__":
    unittest.main()
