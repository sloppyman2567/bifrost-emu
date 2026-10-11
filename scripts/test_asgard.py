#!/usr/bin/env python3
"""Exercise independent Bionic Binder processes, mixed engines and peer death."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import time
import socket
import struct
from asgard import HEADER, MAGIC, VERSION, receive

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emu', type=Path, default=ROOT / 'bifrost-emu')
    parser.add_argument('--rootfs', type=Path, default=ROOT / 'tools/android-runtime/rootfs')
    args = parser.parse_args()
    rootfs, emu = args.rootfs.resolve(), args.emu.resolve()
    fixture = rootfs / 'data/local/tmp/android_binder_process'
    fixture.parent.mkdir(parents=True, exist_ok=True)
    for name in ('android_binder_process', 'android_binder_features'):
        subprocess.run(['clang', '--target=aarch64-linux-android35', '-fuse-ld=lld',
                        '-nostdlib', '-fPIE', '-pie', '-O2', '-D__errno_location=__errno',
                        '-isystem', str(ROOT / 'tools/aarch64-linux-musl-cross/aarch64-linux-musl/include'),
                        '-Wl,-z,max-page-size=16384', '-Wl,--dynamic-linker=/system/bin/linker64',
                        '-Wl,-rpath,/apex/com.android.runtime/lib64/bionic', '-Wl,--allow-shlib-undefined',
                        str(ROOT / 'ctest/android_bionic_start.S'), str(ROOT / f'ctest/{name}.c'),
                        str(rootfs / 'apex/com.android.runtime/lib64/bionic/libc.so'), '-o', str(rootfs / f'data/local/tmp/{name}')], check=True)
    env = dict(os.environ, BIFROST_GUEST_ENV='LD_LIBRARY_PATH=/apex/com.android.runtime/lib64/bionic')
    with tempfile.TemporaryDirectory(prefix='asgard-test-') as directory:
        state = Path(directory)
        control = ['python3', str(ROOT / 'scripts/asgard.py'), '--emu', str(emu), '--state-root', str(state), 'realm']
        import json
        broker = json.loads(subprocess.check_output(control + ['start'], text=True))
        base = [str(emu), '--android-runtime', '--rootfs', str(rootfs), '--android-binder-broker', broker['socket']]

        def wait_for(log, marker, process):
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                log.seek(0)
                text = log.read()
                if marker in text:
                    return
                if process.poll() is not None:
                    raise RuntimeError(text)
                time.sleep(.02)
            raise TimeoutError(marker)

        try:
            # Exercise 1000 ioctls on the same stream, with a separate lifetime
            # channel, so a server that still closes after one RPC fails here.
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as lifetime, socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as stream:
                lifetime.settimeout(3)
                stream.settimeout(3)
                lifetime.connect(broker['socket'])
                opening = struct.pack('<IIIi', 1, os.O_RDWR, 10001, os.getpid())
                lifetime.sendall(HEADER.pack(MAGIC, VERSION, 1, len(opening), 0, 0) + opening)
                header = HEADER.unpack(receive(lifetime, HEADER.size))
                receive(lifetime, header[3])
                stream.connect(broker['socket'])
                body = struct.pack('<QIIIIII', 0x10000, 0xc0046209, os.getpid(), os.O_RDWR, 1, 0, 0)
                body += struct.pack('<QII', 0x10000, 4, 0)
                for _ in range(1000):
                    stream.sendall(HEADER.pack(MAGIC, VERSION, 3, len(body), *header[4:]) + body)
                    response = HEADER.unpack(receive(stream, HEADER.size))
                    data = receive(stream, response[3])
                    if struct.unpack('<qIIQIIi', data) != (0, 1, 0, 0x10000, 4, 1, 8):
                        raise RuntimeError('persistent Binder channel returned an invalid version reply')
            print('Asgard: 1000 ioctls on one persistent RPC connection passed', flush=True)
            for mode in ('jit', 'interp'):
                result = subprocess.run(base + ([] if mode == 'jit' else ['--no-jit']) +
                    ['/data/local/tmp/android_binder_features'], env=env, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
                if result.returncode or 'checks passed' not in result.stdout:
                    raise RuntimeError(result.stdout)
                print(f'Asgard: Binder feature contracts {mode} passed', flush=True)
            for server_mode, client_mode, death in [('jit', 'jit', False), ('interp', 'jit', False),
                                                    ('jit', 'interp', False), ('interp', 'interp', False),
                                                    ('jit', 'jit', True), ('jit', 'jit', 'notify'), ('interp', 'interp', 'notify')]:
                with tempfile.TemporaryFile(mode='w+') as log, tempfile.TemporaryFile(mode='w+') as client_log:
                    server = subprocess.Popen(base + ([] if server_mode == 'jit' else ['--no-jit']) +
                                              ['--android-app-id', '10002', '/data/local/tmp/android_binder_process',
                                               'server'] + (['notify'] if death == 'notify' else ['death'] if death else []), env=env,
                                              stdout=log, stderr=subprocess.STDOUT)
                    client = None
                    try:
                        wait_for(log, 'PROCESS_SERVER_READY', server)
                        client = subprocess.Popen(base + ([] if client_mode == 'jit' else ['--no-jit']) +
                                                  ['--android-app-id', '10003', '/data/local/tmp/android_binder_process',
                                                   'client'] + (['notify'] if death == 'notify' else ['death'] if death else []), env=env,
                                                  stdout=client_log, stderr=subprocess.STDOUT)
                        if death:
                            if death == 'notify':
                                wait_for(client_log, 'PROCESS_NOTIFY_READY', client)
                            wait_for(log, 'PROCESS_SERVER_WAITING', server)
                            server.kill()
                        result = client.wait(timeout=20)
                        client_log.seek(0)
                        text = client_log.read()
                        expected = 'PROCESS_NOTIFY_OK' if death == 'notify' else 'PROCESS_DEATH_OK' if death else 'PROCESS_CLIENT_OK'
                        if result or expected not in text:
                            raise RuntimeError(text)
                        server_result = server.wait(timeout=10)
                        log.seek(0)
                        text = log.read()
                        if not death and (server_result or 'PROCESS_SERVER_OK' not in text):
                            raise RuntimeError(text)
                        print(f'Asgard: {server_mode} server / {client_mode} client death={death} passed', flush=True)
                    finally:
                        for process in (client, server):
                            if process and process.poll() is None:
                                process.kill()
                                process.wait()
                # Disconnect cleanup must release the context manager before the next process opens it.
                time.sleep(.1)
            with tempfile.TemporaryFile(mode='w+') as log:
                server = subprocess.Popen(base + ['/data/local/tmp/android_binder_process', 'server', 'stress'],
                    env=env, stdout=log, stderr=subprocess.STDOUT)
                clients = []
                logs = []
                started = time.monotonic()
                try:
                    wait_for(log, 'PROCESS_SERVER_READY', server)
                    for index in range(8):
                        client_log = tempfile.TemporaryFile(mode='w+')
                        logs.append(client_log)
                        clients.append(subprocess.Popen(base + ['--android-app-id', str(11000 + index),
                            '/data/local/tmp/android_binder_process', 'client', 'stress'],
                            env=env, stdout=client_log, stderr=subprocess.STDOUT))
                    for client, client_log in zip(clients, logs):
                        result = client.wait(timeout=30)
                        client_log.seek(0)
                        text = client_log.read()
                        if result or 'PROCESS_STRESS_CLIENT_OK' not in text:
                            raise RuntimeError(text)
                    if server.wait(timeout=10):
                        log.seek(0)
                        raise RuntimeError(log.read())
                    print(f'Asgard: 8 concurrent processes / 1600 RPCs passed in {time.monotonic()-started:.2f}s', flush=True)
                finally:
                    for process in [server, *clients]:
                        if process.poll() is None:
                            process.kill()
                            process.wait()
                    for client_log in logs:
                        client_log.close()
            subprocess.run(control + ['stop'], check=True, stdout=subprocess.DEVNULL)
            subprocess.run(control + ['start'], check=True, stdout=subprocess.DEVNULL)
            print('Asgard: broker stop/restart passed', flush=True)
        finally:
            subprocess.run(control + ['stop'], check=True, stdout=subprocess.DEVNULL)
    print('ASGARD_PROCESS_OK')


if __name__ == '__main__':
    main()
