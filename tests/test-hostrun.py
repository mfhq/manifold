#!/usr/bin/env python3
#
# Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later

"""
Tests of manifold-hostrun, which runs programs of the host for applications.
We start a runner for every connection to a socket, like systemd does, and
talk to it like mfhost-run does.

The runner recognizes an application by the record file in its mount tree,
which processes of the host do not have. To test this, a second process of
this script acts as the application. It enters a new mount namespace, creates
a record file there, and calls the runner from inside the namespace.

Usage: test-hostrun.py --runner manifold-hostrun
"""

import argparse
import ctypes
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mftest import APP_ID, EXIT_SKIPPED, call_more  # noqa: E402

INTERFACE = 'org.freedesktop.manifold.host'

CLONE_NEWNS = 0x00020000
CLONE_NEWUSER = 0x10000000
MS_REC = 0x4000
MS_PRIVATE = 1 << 18

ARGS = None


def run_as_application(socket_path, permissions):
    """Entry point of the process that acts as an application. It creates the record
    file of a mount tree for itself, runs a program on the host, and prints the result
    as JSON."""
    libc = ctypes.CDLL(None, use_errno=True)
    uid, gid = os.getuid(), os.getgid()
    if libc.unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0:
        print(f'SKIP: unable to create a user namespace: {os.strerror(ctypes.get_errno())}')
        return EXIT_SKIPPED
    for name, content in (
        ('setgroups', 'deny'),
        ('uid_map', f'{uid} {uid} 1'),
        ('gid_map', f'{gid} {gid} 1'),
    ):
        with open(f'/proc/self/{name}', 'w', encoding='utf-8') as f:
            f.write(content)
    if (
        libc.mount(None, b'/', None, MS_REC | MS_PRIVATE, None) != 0
        or libc.mount(b'tmpfs', b'/run', b'tmpfs', 0, None) != 0
    ):
        raise OSError(ctypes.get_errno(), 'Unable to mount a /run of our own')
    os.makedirs('/run/manifold')
    with open('/run/manifold/environment.json', 'w', encoding='utf-8') as f:
        json.dump(
            {
                'componentId': APP_ID,
                'permissions': json.loads(permissions),
                'environment': {'MF_TEST_MANAGED': 'for the tree'},
            },
            f,
        )

    runtime_dir = os.environ['XDG_RUNTIME_DIR']
    environment = [
        f'PATH=/app/bin:/usr/bin:{runtime_dir}/manifold/host-bin/0123456789abcdef:/run/host-bin',
        'MANIFOLD_HOST_PATH=/home/user/bin:/usr/bin',
        'MF_TEST_MANAGED=for the tree',
        'MF_TEST_OTHER=from the application',
    ]
    script = 'echo "$PATH|${MF_TEST_MANAGED-unset}|$MF_TEST_OTHER|${MANIFOLD_HOST_PATH-unset}"'
    output_read, output_write = os.pipe()
    cwd_fd = os.open('/', os.O_PATH)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.connect(socket_path)
        result = call_more(
            sock,
            f'{INTERFACE}.Run',
            {
                'argv': ['sh', '-c', script],
                'workingDirectory': '/',
                'environment': environment,
                'fileDescriptors': [0, 1, 1],
                'workingDirectoryFileDescriptor': 2,
            },
            [os.open('/dev/null', os.O_RDONLY), output_write, cwd_fd],
        )
    os.close(output_write)
    with os.fdopen(output_read, 'r', encoding='utf-8') as f:
        print(json.dumps({'result': result, 'output': f.read()}))
    return 0


class HostRunTest(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.mkdtemp(prefix='manifold-test-')
        self.addCleanup(shutil.rmtree, self.work)
        self.runtime_dir = f'{self.work}/runtime'
        os.makedirs(self.runtime_dir)
        self.socket_path = f'{self.work}/host.sock'
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(self.socket_path)
        self.listener.listen()
        self.addCleanup(self.listener.close)
        self.runners = []
        self.addCleanup(self.stop_runners)

    def stop_runners(self):
        for runner in self.runners:
            self.assertEqual(runner.wait(timeout=10), 0)

    def start_runner(self):
        """Start a runner for the next connection to the socket, like systemd does."""
        connection, _ = self.listener.accept()
        environment = dict(os.environ, XDG_RUNTIME_DIR=self.runtime_dir)
        environment.pop('MF_TEST_MANAGED', None)
        self.runners.append(
            subprocess.Popen(
                [ARGS.runner, '--connection-fd', str(connection.fileno())],
                pass_fds=(connection.fileno(),),
                env=environment,
            )
        )
        connection.close()

    def connect(self):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(self.socket_path)
        self.start_runner()
        return sock

    def run_program(self, argv, stdin=b'', cwd='/', cwd_fd_path=None, environment=()):
        """Run a program on the host. Returns the result of the call, the standard output
        of the program and the standard error of the program."""
        stdin_read, stdin_write = os.pipe()
        stdout_read, stdout_write = os.pipe()
        stderr_read, stderr_write = os.pipe()
        cwd_fd = os.open(cwd_fd_path or cwd, os.O_PATH)
        os.write(stdin_write, stdin)
        os.close(stdin_write)
        with self.connect() as sock:
            result = call_more(
                sock,
                f'{INTERFACE}.Run',
                {
                    'argv': argv,
                    'workingDirectory': cwd,
                    'environment': list(environment),
                    'fileDescriptors': [0, 1, 2],
                    'workingDirectoryFileDescriptor': 3,
                },
                [stdin_read, stdout_write, stderr_write, cwd_fd],
            )
        for fd in (stdin_read, stdout_write, stderr_write, cwd_fd):
            os.close(fd)
        with os.fdopen(stdout_read, 'rb') as out, os.fdopen(stderr_read, 'rb') as err:
            return result, out.read(), err.read()

    def test_program_runs_with_the_files_of_the_caller(self):
        result, out, err = self.run_program(
            ['sh', '-c', 'cat; echo "$MF_TEST_VAR" >&2; exit 7'],
            stdin=b'from the caller',
            environment=['MF_TEST_VAR=hello'],
        )
        self.assertGreater(result[0]['pid'], 0)
        self.assertEqual(result[-1], {'exitStatus': 7})
        self.assertEqual(out, b'from the caller')
        self.assertEqual(err, b'hello\n')

    def test_working_directory(self):
        result, out, _ = self.run_program(['pwd'], cwd=self.work)
        self.assertEqual(result[-1], {'exitStatus': 0})
        self.assertEqual(out.decode().strip(), os.path.realpath(self.work))

        # the working directory of the caller has the same path as a directory of the host,
        # but it is a different directory
        result, _, _ = self.run_program(['pwd'], cwd=self.work, cwd_fd_path=self.runtime_dir)
        self.assertEqual(result['error'], f'{INTERFACE}.NoSuchWorkingDirectory')
        self.assertEqual(result['parameters'], {'path': self.work})

    def test_no_such_program(self):
        result, _, _ = self.run_program(['mf-test-no-such-program'])
        self.assertEqual(result['error'], f'{INTERFACE}.NoSuchProgram')
        self.assertEqual(result['parameters'], {'program': 'mf-test-no-such-program'})

    def test_signals_are_passed_on(self):
        signal_read, signal_write = os.pipe()
        cwd_fd = os.open('/', os.O_PATH)
        null_fd = os.open('/dev/null', os.O_RDWR)
        with self.connect() as sock:
            call = {
                'method': f'{INTERFACE}.Run',
                'more': True,
                'parameters': {
                    'argv': ['sleep', '100'],
                    'workingDirectory': '/',
                    'environment': [],
                    'fileDescriptors': [0, 0, 0],
                    'workingDirectoryFileDescriptor': 1,
                    'signalFileDescriptor': 2,
                },
            }
            socket.send_fds(
                sock, [json.dumps(call).encode() + b'\0'], [null_fd, cwd_fd, signal_read]
            )
            started = json.loads(sock.recv(65536).rstrip(b'\0'))
            self.assertTrue(started['continues'])
            os.write(signal_write, bytes([signal.SIGUSR1]))
            ended = json.loads(sock.recv(65536).rstrip(b'\0'))
            self.assertEqual(ended['parameters'], {'signal': signal.SIGUSR1})
        for fd in (signal_read, signal_write, cwd_fd, null_fd):
            os.close(fd)

    def test_program_ends_with_the_connection(self):
        cwd_fd = os.open('/', os.O_PATH)
        null_fd = os.open('/dev/null', os.O_RDWR)
        with self.connect() as sock:
            call = {
                'method': f'{INTERFACE}.Run',
                'more': True,
                'parameters': {
                    'argv': ['sleep', '100'],
                    'workingDirectory': '/',
                    'environment': [],
                    'fileDescriptors': [0, 0, 0],
                    'workingDirectoryFileDescriptor': 1,
                },
            }
            socket.send_fds(sock, [json.dumps(call).encode() + b'\0'], [null_fd, cwd_fd])
            pid = json.loads(sock.recv(65536).rstrip(b'\0'))['parameters']['pid']
        os.close(cwd_fd)
        os.close(null_fd)

        deadline = time.monotonic() + 10
        while self.is_running(pid):
            self.assertLess(time.monotonic(), deadline, 'the program is still running')
            time.sleep(0.05)

    def is_running(self, pid):
        """Check if a process is still running. A zombie process does not count as running.

        The runner exits right after it has terminated its program, so the program becomes
        an orphan. In a container, there may be no init process that reaps orphans, and the
        terminated program then remains as a zombie."""
        try:
            with open(f'/proc/{pid}/stat', encoding='utf-8') as f:
                # the process state is the field after the program name, which is in parentheses
                return f.read().rsplit(')', 1)[1].split()[0] != 'Z'
        except (FileNotFoundError, ProcessLookupError):
            return False

    def call_as_application(self, permissions):
        """Run a program on the host from a process that is in the mount tree of an
        application with the given permissions."""
        application = subprocess.Popen(
            [sys.executable, __file__, '--application', self.socket_path, json.dumps(permissions)],
            stdout=subprocess.PIPE,
            text=True,
            env=dict(os.environ, XDG_RUNTIME_DIR=self.runtime_dir),
        )
        self.listener.settimeout(10)
        try:
            self.start_runner()
        except TimeoutError:
            # the application did not connect, its exit status is checked below
            pass
        output, _ = application.communicate(timeout=10)
        if application.returncode == EXIT_SKIPPED:
            self.skipTest(output.strip())
        self.assertEqual(application.returncode, 0, output)
        return json.loads(output)

    def test_application_needs_the_permission(self):
        reply = self.call_as_application([])
        self.assertEqual(reply['result']['error'], f'{INTERFACE}.NotPermitted')
        self.assertEqual(reply['result']['parameters'], {'componentId': APP_ID})
        self.assertEqual(reply['output'], '')

    def test_application_gets_the_environment_of_the_host(self):
        """The program gets the environment variables of the application, except for the
        variables that are only set for the mount tree. Directories that only exist in
        the mount tree are removed from its PATH."""
        reply = self.call_as_application(['host-commands'])
        self.assertEqual(reply['result'][-1], {'exitStatus': 0})
        self.assertEqual(
            reply['output'], '/usr/bin:/home/user/bin|unset|from the application|unset\n'
        )


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--runner')
    parser.add_argument('--application', nargs=2, metavar=('SOCKET', 'PERMISSIONS'))
    ARGS, remaining = parser.parse_known_args()
    if ARGS.application is not None:
        sys.exit(run_as_application(*ARGS.application))
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
