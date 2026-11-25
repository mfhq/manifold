#!/usr/bin/env python3
#
# Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later

"""
End-to-end tests of manifold-run. The runner has to enter the mount tree that
it receives from the compose service, and run the program of the component in
the mount tree.

This test mocks the compose service, as the real service can not mount images
in the sandbox. The mount tree of the mocked service is a tmpfs with a bind
mount of the host's /usr, and a shell script as the program of the component.

The test runs in two halves, as the runner can enter a mount tree in two ways.
The outer half runs as the user who started the test. It starts the inner half
as root in the sandbox, and then runs the runner outside of the sandbox, where
the runner enters the mount tree in a new user namespace. The inner half
creates the mount tree, starts the mocked service, and runs the runner in the
sandbox, where the runner enters the mount tree directly. The socket of the
service is in a temporary directory of the host, which both halves can access.

Usage: test-runner.py --sandbox mf-test-sandbox --runner manifold-run
"""

import argparse
import array
import ctypes
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mftest import APP_ID, EXIT_SKIPPED, RUNTIME_DIR  # noqa: E402

TREE = f'{RUNTIME_DIR}/test-tree'
# an application with the permission to run programs of the host
HOST_APP_ID = 'com.example.hostapp'
INTERFACE = 'org.freedesktop.manifold.run'

MS_BIND = 0x1000
MS_REC = 0x4000
SYS_OPEN_TREE = 428
OPEN_TREE_CLONE = 1
AT_RECURSIVE = 0x8000
AT_FDCWD = -100

PROBE = '''#!/bin/sh
echo "marker=$(cat /manifold-test-marker)"
echo "program=$0"
echo "arg=$1"
echo "cwd=$(pwd)"
echo "var=$MF_TEST_VAR"
echo "unset=${MF_TEST_UNSET-absent}"
echo "uid=$(id -u)"
'''

ARGS = None
libc = ctypes.CDLL(None, use_errno=True)


def mount(source, target, fstype, flags=0):
    def encode(value):
        return None if value is None else value.encode()

    if libc.mount(encode(source), encode(target), encode(fstype), flags, None) != 0:
        raise OSError(
            ctypes.get_errno(), f'Unable to mount {target}: {os.strerror(ctypes.get_errno())}'
        )


def open_tree(path):
    libc.syscall.restype = ctypes.c_int
    fd = libc.syscall(SYS_OPEN_TREE, AT_FDCWD, path.encode(), OPEN_TREE_CLONE | AT_RECURSIVE)
    if fd < 0:
        raise OSError(
            ctypes.get_errno(), f'Unable to clone {path}: {os.strerror(ctypes.get_errno())}'
        )
    return fd


def make_tree():
    """Create a mount tree like the compose service provides. The tree is a root filesystem
    with the programs that are needed to run a shell script."""
    os.makedirs(TREE)
    mount('tmpfs', TREE, 'tmpfs')
    for name in ('usr', 'proc', 'app/bin', 'tmp', 'home'):
        os.makedirs(f'{TREE}/{name}')
    for name in ('bin', 'sbin', 'lib', 'lib64', 'lib32'):
        if os.path.islink(f'/{name}'):
            os.symlink(os.readlink(f'/{name}'), f'{TREE}/{name}')
    mount('/usr', f'{TREE}/usr', None, MS_BIND | MS_REC)
    mount('/proc', f'{TREE}/proc', None, MS_BIND | MS_REC)
    with open(f'{TREE}/manifold-test-marker', 'w', encoding='utf-8') as f:
        f.write('in the tree')
    with open(f'{TREE}/app/bin/probe', 'w', encoding='utf-8') as f:
        f.write(PROBE)
    os.chmod(f'{TREE}/app/bin/probe', 0o755)
    # a directory with the name of a program, the runner has to skip it when it searches for the
    # program
    os.makedirs(f'{TREE}/app/bin/sh')


class FakeService(threading.Thread):
    """A mocked compose service. It answers PrepareLaunch calls with a clone of the mount
    tree, like manifold-rund does."""

    def __init__(self, socket_path):
        super().__init__(daemon=True)
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(socket_path)
        self.listener.listen()

    def run(self):
        while True:
            connection, _ = self.listener.accept()
            with connection:
                self.serve(connection)

    def serve(self, connection):
        buffer = b''
        while b'\0' not in buffer:
            chunk = connection.recv(65536)
            if not chunk:
                return
            buffer += chunk
        request = json.loads(buffer.split(b'\0', 1)[0])
        assert request['method'] == f'{INTERFACE}.PrepareLaunch', request
        component_id = request['parameters']['componentId']

        if component_id not in (APP_ID, HOST_APP_ID):
            reply = {
                'error': f'{INTERFACE}.NoSuchComponent',
                'parameters': {'componentId': component_id},
            }
            connection.sendall(json.dumps(reply).encode() + b'\0')
            return

        mount_fd = open_tree(TREE)
        reply = {
            'parameters': {
                'mountFileDescriptor': 0,
                'executable': '/app/bin/probe',
                'environment': ['MF_TEST_VAR=hello', 'MF_TEST_UNSET='],
            }
        }
        if component_id == HOST_APP_ID:
            reply['parameters']['hostCommands'] = {
                'directory': '/run/host-bin',
                'systemPaths': ['/usr/bin', '/bin'],
            }
        fds = array.array('i', [mount_fd])
        connection.sendmsg(
            [json.dumps(reply).encode() + b'\0'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, fds)]
        )
        os.close(mount_fd)


class RunnerTest(unittest.TestCase):
    """Tests of the runner. They run once in each half of the test, with the settings below."""

    socket_path = None
    uid = None  # the user ID that the program in the tree is expected to run as
    # drop all capabilities first, so root has to use a user namespace too
    without_capabilities = False

    def run_runner(self, *args, cwd='/', search_path=None, runtime_dir=None):
        argv = [ARGS.runner, '--socket', self.socket_path, *args]
        if self.without_capabilities:
            argv = ['setpriv', '--bounding-set=-all', '--inh-caps=-all', *argv]
        environment = dict(os.environ, MF_TEST_UNSET='from the caller', HOME='/home')
        if search_path is not None:
            environment['PATH'] = search_path
        if runtime_dir is not None:
            environment['XDG_RUNTIME_DIR'] = runtime_dir
        return subprocess.run(
            argv, capture_output=True, text=True, check=False, cwd=cwd, env=environment
        )

    def assert_launched(self, *options, cwd, expected_cwd):
        """Run the program of the application in the working directory `cwd`, with the
        given options of the runner, and check what the program reports about itself."""
        result = self.run_runner(*options, APP_ID, 'first', cwd=cwd)
        self.assertEqual(result.returncode, 0, result)
        lines = dict(line.split('=', 1) for line in result.stdout.splitlines())
        self.assertEqual(
            lines,
            {
                'marker': 'in the tree',
                'program': '/app/bin/probe',
                'arg': 'first',
                'cwd': expected_cwd,
                'var': 'hello',
                'unset': 'absent',
                'uid': str(self.uid),
            },
            result.stderr,
        )

    def test_launch(self):
        self.assert_launched(cwd='/usr/bin', expected_cwd='/usr/bin')

    def test_cwd_falls_back_to_home(self):
        """If the working directory does not exist in the tree, the program starts in the home
        directory."""
        self.assert_launched(cwd='/run', expected_cwd='/home')

    def test_other_command(self):
        """Any program in the tree can be run instead of the program of the component."""
        result = self.run_runner(
            '--command=sh', APP_ID, '-c', 'echo "shell=$0"; cat /manifold-test-marker'
        )
        self.assertEqual(result.returncode, 0, result)
        self.assertEqual(result.stdout, 'shell=sh\nin the tree')
        self.assert_launched('--command=/app/bin/probe', cwd='/usr/bin', expected_cwd='/usr/bin')
        result = self.run_runner('--command=no-such-program', APP_ID)
        self.assertEqual(result.returncode, 127, result)
        self.assertIn('no-such-program', result.stderr)

    def test_search_path(self):
        """The directories of the application come first in the PATH. Entries that are
        empty, relative or duplicated are removed."""
        result = self.run_runner(
            '--command=sh',
            APP_ID,
            '-c',
            'echo "$PATH"',
            search_path='/usr/bin:/home/user/bin::./bin:bin:/usr/bin/:/home/user/bin',
        )
        self.assertEqual(result.returncode, 0, result)
        self.assertEqual(result.stdout, '/app/bin:/usr/bin:/home/user/bin\n')

    def test_host_commands(self):
        """A PATH directory of the caller that does not exist in the tree is replaced by
        a directory with a link for each of its programs. The directory with the links is
        created in the runtime directory of the user."""
        work = tempfile.mkdtemp(prefix='manifold-test-')
        self.addCleanup(shutil.rmtree, work)
        user_bin = f'{work}/bin'
        runtime_dir = f'{work}/runtime'
        os.makedirs(user_bin)
        os.makedirs(runtime_dir)
        search_path = f'/usr/bin:{user_bin}:{work}/missing'

        def make_program(name):
            with open(f'{user_bin}/{name}', 'w', encoding='utf-8') as f:
                f.write('#!/bin/sh\n')
            os.chmod(f'{user_bin}/{name}', 0o755)

        def launch():
            result = self.run_runner(
                '--command=sh',
                HOST_APP_ID,
                '-c',
                'echo "$PATH"; echo "$MANIFOLD_HOST_PATH"',
                search_path=search_path,
                runtime_dir=runtime_dir,
            )
            self.assertEqual(result.returncode, 0, result)
            self.assertEqual(result.stderr, '')
            (links_name,) = os.listdir(f'{runtime_dir}/manifold/host-bin')
            links_dir = f'{runtime_dir}/manifold/host-bin/{links_name}'
            self.assertEqual(
                result.stdout, f'/app/bin:/usr/bin:{links_dir}:/run/host-bin\n{search_path}\n'
            )
            return links_dir

        make_program('first')
        links_dir = launch()
        self.assertEqual(os.listdir(links_dir), ['first'])
        self.assertEqual(os.readlink(f'{links_dir}/first'), '/usr/bin/mfhost-run')

        # the links are updated when the programs change, in the same directory, as running
        # applications still use it
        make_program('second')
        os.unlink(f'{user_bin}/first')
        self.assertEqual(launch(), links_dir)
        self.assertEqual(os.listdir(links_dir), ['second'])

        # for an application without the permission, the PATH directories of the caller are not
        # replaced
        result = self.run_runner(
            '--command=sh',
            APP_ID,
            '-c',
            'echo "$PATH"; echo "${MANIFOLD_HOST_PATH-absent}"',
            search_path=search_path,
            runtime_dir=runtime_dir,
        )
        self.assertEqual(result.returncode, 0, result)
        self.assertEqual(result.stdout, f'/app/bin:{search_path}\nabsent\n')

    def test_not_installed(self):
        result = self.run_runner('com.example.missing')
        self.assertEqual(result.returncode, 125, result)
        self.assertIn('Component com.example.missing is not installed', result.stderr)


def run_inside(socket_path):
    """Run the inner half of the test, as root in the sandbox. The runner enters the tree
    directly."""
    make_tree()
    FakeService(socket_path).start()
    RunnerTest.socket_path = socket_path
    RunnerTest.uid = 0
    result = unittest.main(argv=[sys.argv[0]], exit=False, verbosity=2).result
    if not result.wasSuccessful():
        return 1

    # keep the service running until the outer half is done, it closes our stdin to tell us
    print('ready', flush=True)
    sys.stdin.read()
    return 0


def run_outside():
    """Run the outer half of the test. It starts the inner half, and runs the runner
    outside of the sandbox."""
    work = tempfile.mkdtemp(prefix='manifold-test-')
    socket_path = os.path.join(work, 'run.sock')
    inside = subprocess.Popen(
        [ARGS.sandbox, sys.executable, __file__, '--inside', socket_path, *sys.argv[1:]],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
    )
    try:
        for line in inside.stdout:
            sys.stdout.write(line)
            if line.rstrip() == 'ready':
                break
        else:
            return inside.wait()

        # without capabilities, the runner creates a user namespace of its own
        if os.geteuid() == 0:
            if shutil.which('setpriv') is None:
                print('SKIP: running as root, and setpriv is not available to drop capabilities')
                return EXIT_SKIPPED
            RunnerTest.without_capabilities = True
        RunnerTest.socket_path = socket_path
        RunnerTest.uid = os.getuid()
        result = unittest.main(argv=[sys.argv[0]], exit=False, verbosity=2).result
    finally:
        inside.stdin.close()
        status = inside.wait()
        shutil.rmtree(work, ignore_errors=True)
    return 0 if result.wasSuccessful() and status == 0 else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--sandbox', required=True)
    parser.add_argument('--runner', required=True)
    parser.add_argument('--inside', metavar='SOCKET')
    ARGS = parser.parse_args()
    if ARGS.inside:
        os._exit(run_inside(ARGS.inside))  # exit directly, so we do not wait for the service thread
    sys.exit(run_outside())
