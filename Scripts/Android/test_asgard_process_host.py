#!/usr/bin/env python3
"""Service children end with the supervisor that spawned them.

A supervisor killed in the middle of shutting a session down (a terminal tab interrupted, a
SIGKILL) used to leave its Play services and system helper processes running: two of them were
still resident 44 minutes after their game crashed, about 490 MB. `ProcessHost.spawn` now passes
`--asgard-service-parent <supervisor pid>`, so the emulator asks the kernel to SIGKILL the child
when its parent dies (`bind_asgard_parent` in Source/Main.cpp), as it already does for the broker.

The first test checks the command `spawn` builds. The others use the real emulator: a supervisor
spawns a child from a worker thread (as ProcessHost's own thread does, and the parent-death signal
follows the spawning thread), the supervisor is killed, and the child must be gone. A control run
without the flag shows the check can see a leak.
"""
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import android_processes

ROOT = Path(__file__).resolve().parents[2]
EMU = Path(os.environ.get("BIFROST_EMU", ROOT / "bifrost-emu"))
TOYBOX = ROOT / "Tests/Apps/toybox"

SUPERVISOR = """
import os, subprocess, sys, threading, time
emulator, toybox, pidfile, bound = sys.argv[1:5]
def spawn():
    command = [emulator] + (['--asgard-service-parent', str(os.getpid())] if bound == '1' else []) + [toybox, 'sleep', '120']
    child = subprocess.Popen(command, start_new_session=True)
    with open(pidfile, 'w') as stream: stream.write(str(child.pid))
    time.sleep(120)  # this thread must outlive the test: it is the child's parent for the kernel
worker = threading.Thread(target=spawn)
worker.start(); worker.join()
"""


def alive(pid):
    """Running, not a zombie waiting for its new parent to reap it."""
    try:
        with open(f"/proc/{pid}/stat") as stream:
            return stream.read().rsplit(")", 1)[1].split()[0] != "Z"
    except (FileNotFoundError, ProcessLookupError):
        return False


class ServiceChildTests(unittest.TestCase):
    def setUp(self):
        self.pids = []
        self.addCleanup(self.kill_leftovers)

    def kill_leftovers(self):
        for pid in self.pids:
            try:
                os.killpg(pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass

    def test_spawn_binds_the_child_to_its_supervisor(self):
        with tempfile.TemporaryDirectory(prefix="asgard-host-") as directory:
            root = Path(directory)
            record = root / "argv"
            stub = root / "emulator"
            stub.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$ARGV_RECORD"\nexec sleep 30\n')
            stub.chmod(0o755)
            host = android_processes.ProcessHost(
                root / "rootfs", stub, ["--android-binder-broker", str(root / "binder")],
                dict(os.environ, ARGV_RECORD=str(record)), "/data/local/tmp/x.dex", root / "logs")
            try:
                reply = host.spawn(dict(key="a" * 32, controller="asgard.process.1", uid=10001))
                self.pids.append(reply["pid"])
                deadline = time.monotonic() + 5
                while not (record.exists() and record.read_text().endswith("\n")) and time.monotonic() < deadline:
                    time.sleep(0.02)
                arguments = record.read_text().split("\n")
                self.assertIn("--asgard-service-parent", arguments)
                self.assertEqual(arguments[arguments.index("--asgard-service-parent") + 1], str(os.getpid()))
            finally:
                host.close()

    def run_supervisor(self, bound):
        """Start the supervisor, wait for its child, kill the supervisor; return the child's pid."""
        if not EMU.is_file() or not TOYBOX.is_file():
            self.skipTest("needs the emulator and Tests/Apps/toybox")
        with tempfile.TemporaryDirectory(prefix="asgard-parent-") as directory:
            pidfile = Path(directory) / "child"
            supervisor = subprocess.Popen([sys.executable, "-I", "-c", SUPERVISOR, str(EMU), str(TOYBOX),
                                           str(pidfile), "1" if bound else "0"], start_new_session=True)
            self.addCleanup(lambda: (supervisor.kill(), supervisor.wait()))
            deadline = time.monotonic() + 10
            while not pidfile.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertTrue(pidfile.exists(), "the supervisor never spawned its child")
            child = int(pidfile.read_text())
            self.pids.append(child)
            time.sleep(1.0)  # let the emulator start and bind
            self.assertTrue(alive(child), "the child exited by itself before the supervisor was killed")
            supervisor.kill()
            supervisor.wait()
            return child

    def gone_within(self, pid, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if not alive(pid):
                return True
            time.sleep(0.05)
        return not alive(pid)

    def test_child_dies_with_its_supervisor(self):
        child = self.run_supervisor(bound=True)
        self.assertTrue(self.gone_within(child, 5), "the child outlived its killed supervisor")

    def test_control_without_the_flag_the_child_survives(self):
        child = self.run_supervisor(bound=False)
        self.assertFalse(self.gone_within(child, 2), "the control child died by itself: the check cannot see a leak")


if __name__ == "__main__":
    unittest.main()
