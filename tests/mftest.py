#
# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later

"""
What the end-to-end tests share: how a daemon is started and stopped, how
it is called, and how bundles are made up. Calls are made with varlinkctl.

The tests are started by `mf-test-sandbox` and run in its sandbox. In the
sandbox, we are root, and the state directories of Manifold are empty.
"""

import hashlib
import json
import os
import platform
import re
import signal
import subprocess
import sysconfig
import time
import zipfile

# the exit status that Meson treats as "skipped"
EXIT_SKIPPED = 77

STORE = '/var/lib/manifold'
RUNTIME_DIR = '/run/manifold'

CORE_ID = 'net.debian.core13'
APP_ID = 'com.example.app'


def host_architecture():
    """Return the architecture of the host, with the name that mf_get_host_architecture() uses.
    On x86 and 32-bit ARM, we return the architecture of the userland, which we derive from
    the multiarch tuple of Python. On all other machines, we return the kernel architecture."""
    userland = {
        'x86_64-linux-gnu': 'x86_64',
        'i386-linux-gnu': 'i386',
        'arm-linux-gnueabihf': 'armhf',
        'arm-linux-gnueabi': 'armel',
    }.get(sysconfig.get_config_var('MULTIARCH'))
    if userland is not None:
        return userland
    machine = platform.machine()
    return 'i386' if re.fullmatch(r'i[3-6]86', machine) else machine


def fake_hash(seed):
    return hashlib.sha256(seed.encode()).hexdigest()


def blake3_hex(data):
    """Return the BLAKE3 hash of data. We run b3sum for it, as Python has no BLAKE3 module."""
    result = subprocess.run(['b3sum', '--no-names'], input=data, capture_output=True, check=True)
    return result.stdout.decode().strip()


class VarlinkError(Exception):
    """A call was replied to with an error. varlinkctl turns the error into a
    message, and prints the parameters of the error, which are kept here."""

    def __init__(self, message, parameters):
        super().__init__(message)
        self.parameters = parameters


class Daemon:
    """A Manifold daemon that listens on the given socket path.

    The daemon logs to a file. We do not use a pipe for its output, as nothing reads the
    pipe while the tests run, so the pipe would fill up and block the daemon."""

    def __init__(self, exe, socket_path, log_path):
        self.exe = exe
        self.socket_path = socket_path
        self.log_path = log_path
        self.process = None

    def start(self):
        if os.path.exists(self.socket_path):
            os.unlink(self.socket_path)
        with open(self.log_path, 'wb') as log:
            self.process = subprocess.Popen(
                [self.exe, '--socket', self.socket_path, '--verbose'],
                stdout=log,
                stderr=subprocess.STDOUT,
            )
        for _ in range(200):
            if os.path.exists(self.socket_path):
                return
            if self.process.poll() is not None:
                raise RuntimeError(f'{self.exe} exited early:\n{self.read_log()}')
            time.sleep(0.05)
        raise RuntimeError(f'{self.exe} did not start listening')

    def log_size(self):
        return os.path.getsize(self.log_path)

    def read_log(self, start=0):
        """Return the log of the daemon from the offset start on, e.g. an earlier log_size()."""
        with open(self.log_path, 'rb') as f:
            f.seek(start)
            return f.read().decode(errors='replace')

    def stop(self):
        """Stop the daemon, and return its log. It is safe to stop the daemon twice."""
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if self.process.returncode != 0:
            raise RuntimeError(
                f'{self.exe} exited with status {self.process.returncode}:\n{self.read_log()}'
            )
        return self.read_log()

    def call(self, method, parameters=None, more=False, fd=None):
        """
        Call a Varlink method, and return the parameters of the reply.

        If `more` is set, a list with the parameters of all replies is returned.
        If `fd` is given, the file descriptor is passed along with the call, and
        the method receives it with index 0. A VarlinkError is raised if the
        method returns an error.
        """
        argv = ['varlinkctl', 'call', '--json=short', '--timeout=infinity']
        if more:
            argv.append('--more')
        if fd is not None:
            argv.append(f'--push-fd={fd}')
        argv += [self.socket_path, method, json.dumps(parameters or {})]
        result = subprocess.run(
            argv,
            capture_output=True,
            text=True,
            check=False,
            pass_fds=() if fd is None else (fd,),
        )
        # with `more`, the replies form a JSON text sequence: each one follows a record separator
        replies = [json.loads(reply) for reply in result.stdout.split('\x1e') if reply.strip()]
        if result.returncode != 0:
            raise VarlinkError(result.stderr.strip(), replies[0] if replies else {})
        return replies if more else replies[0]


class Image:
    """An image for a bundle, with its manifest. Its content is made up."""

    def __init__(
        self,
        component,
        kind,
        version,
        revision=22910000,
        sdk=False,
        base=None,
        seed=None,
        architecture=None,
    ):
        self.component = component
        self.kind = kind
        self.version = version
        self.revision = revision
        self.sdk = sdk
        self.base = base
        self.architecture = architecture or host_architecture()
        self.name = f'{component}_{version}.raw'
        self.identity = fake_hash('identity:' + component)
        self.data = hashlib.sha512((seed or self.name).encode()).digest() * 1024
        self.verity_hash = fake_hash('verity:' + hashlib.sha256(self.data).hexdigest())

    def reference(self):
        return {
            'component': self.component,
            'version': self.version,
            'revision': self.revision,
            'identity': self.identity,
            'content-hash': fake_hash('content:' + self.name),
        }

    def manifest(self):
        m = {
            'format': 1,
            'component': self.component,
            'kind': self.kind,
            'version': self.version,
            'revision': self.revision,
            'architecture': self.architecture,
            'sdk': self.sdk,
            'identity': self.identity,
            'content-hash': fake_hash('content:' + self.name),
            'image': {
                'file': self.name,
                'size': len(self.data),
                'sha256': hashlib.sha256(self.data).hexdigest(),
                'blake3': blake3_hex(self.data),
                'verity-root-hash': self.verity_hash,
                'data-offset': 20480,
                'hash-offset': 22528,
            },
        }
        if self.base is not None:
            m['base'] = self.base.reference()
        if self.kind == 'app' and not self.sdk:
            m['app'] = {'command': 'app'}
        return m


def write_bundle(path, images, signed=True, corrupt_image=None, metainfo_ids=None):
    """Write a .manifold file as the builder would. metainfo_ids maps a component
    to the identifier its MetaInfo claims, when that is to differ."""
    files = {}
    for image in images:
        base = image.name[: -len('.raw')]
        metainfo_id = (metainfo_ids or {}).get(image.component, image.component)
        files[f'{base}.manifold.json'] = json.dumps(image.manifest(), indent=3).encode()
        files[f'{base}.metainfo.xml'] = (
            f'<component><id>{metainfo_id}</id><name>{metainfo_id}</name></component>\n'.encode()
        )
    index = {
        'format': 1,
        'files': [
            {
                'name': name,
                'size': len(data),
                'sha256': hashlib.sha256(data).hexdigest(),
                'blake3': blake3_hex(data),
            }
            for name, data in sorted(files.items())
        ],
    }
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_STORED) as bundle:
        for image in images:
            data = image.data
            if image is corrupt_image:
                data = bytes([data[0] ^ 0xFF]) + data[1:]
            bundle.writestr(image.name, data)
        for name, data in files.items():
            bundle.writestr(name, data)
        bundle.writestr('index.json', json.dumps(index, indent=3))
        if signed:
            bundle.writestr('index.json.sig', 'not a signature\n')
    return path
