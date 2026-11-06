#
# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later

"""
Shared helpers for the end-to-end tests: starting, stopping and calling a
daemon, and making up bundles and repositories. Direct calls are made with
varlinkctl, or with our own small Varlink client if varlinkctl is too old to
pass a file descriptor.

The tests run in the sandbox, which `mf-test-sandbox` enters for them: the
directories Manifold keeps its state in are empty, and we are root.
"""

import functools
import hashlib
import http.server
import json
import os
import platform
import re
import shutil
import signal
import socket
import ssl
import subprocess
import sysconfig
import tempfile
import threading
import time
import zipfile

# the status Meson takes as "skipped"
EXIT_SKIPPED = 77

STORE = '/var/lib/manifold'

# where the filesystem starts in an image
DATA_OFFSET = 20480
RUNTIME_DIR = '/run/manifold'

CORE_ID = 'net.debian.core13'
APP_ID = 'com.example.app'


def host_architecture():
    """The architecture of the host, as Manifold names it (mf_get_host_architecture).
    On x86 and 32-bit ARM, that is the architecture of the userland, which the multiarch
    tuple of Python tells. Elsewhere, it is the one of the kernel."""
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
    """The blake3 hash of data, which Python has no module for: b3sum computes it."""
    result = subprocess.run(['b3sum', '--no-names'], input=data, capture_output=True, check=True)
    return result.stdout.decode().strip()


@functools.cache
def varlinkctl_can_pass_fds():
    """Whether varlinkctl can pass a file descriptor along with a call, which it
    can since systemd 258."""
    result = subprocess.run(['varlinkctl', '--help'], capture_output=True, text=True, check=True)
    return '--push-fd=' in result.stdout


class VarlinkError(Exception):
    """A call was replied to with an error. The message names the error, and the
    parameters of the error are kept here."""

    def __init__(self, message, parameters):
        super().__init__(message)
        self.parameters = parameters


class Daemon:
    """A daemon of Manifold, listening on a socket of our choosing. It logs to a
    file: a pipe that is not read while the tests run would fill up, and block it."""

    def __init__(self, exe, socket_path, log_path, args=()):
        self.exe = exe
        self.socket_path = socket_path
        self.log_path = log_path
        self.args = list(args)
        self.process = None

    def start(self):
        if os.path.exists(self.socket_path):
            os.unlink(self.socket_path)
        with open(self.log_path, 'wb') as log:
            self.process = subprocess.Popen(
                [self.exe, '--socket', self.socket_path, '--verbose', *self.args],
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
        """Stop the daemon, and return what it logged. Stopping it twice is fine."""
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
        Call a Varlink method, and return the parameters of the reply. With
        `more`, every reply is returned, as a list. A file descriptor is passed
        along as the first one, index 0. An error reply raises VarlinkError.
        """
        if fd is not None and not varlinkctl_can_pass_fds():
            return self._call_passing_fd(method, parameters, more, fd)

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
        # a method without output may have its reply printed as nothing at all
        return replies if more else (replies[0] if replies else {})

    def _call_passing_fd(self, method, parameters, more, fd):
        """
        Make a call like varlinkctl does, for a varlinkctl that can't pass a file
        descriptor along. Varlink messages are JSON objects, each ended by a NUL
        byte, and the descriptor is sent with the message of the call.
        """
        call = {'method': method, 'parameters': parameters or {}}
        if more:
            call['more'] = True
        replies = []
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.connect(self.socket_path)
            socket.send_fds(sock, [json.dumps(call).encode() + b'\0'], [fd])
            received = b''
            while True:
                while b'\0' not in received:
                    data = sock.recv(65536)
                    if not data:
                        raise RuntimeError(f'{self.exe} closed the connection without replying')
                    received += data
                message, _, received = received.partition(b'\0')
                reply = json.loads(message)
                if 'error' in reply:
                    raise VarlinkError(reply['error'], reply.get('parameters', {}))
                replies.append(reply.get('parameters', {}))
                # with `more`, every reply but the last one says that more follow
                if not reply.get('continues', False):
                    break
        return replies if more else replies[0]


def make_erofs(files):
    """Make a real EROFS filesystem that holds files, given by their absolute paths."""
    with tempfile.TemporaryDirectory() as work:
        for path, content in files.items():
            target = f'{work}/tree{path}'
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, 'wb') as f:
                f.write(content)
        subprocess.run(
            ['mkfs.erofs', '--quiet', '--all-root', f'{work}/filesystem', f'{work}/tree'],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        with open(f'{work}/filesystem', 'rb') as f:
            return f.read()


class Image:
    """An image for a bundle, with its manifest. Its content is made up, unless it is given
    as data, or it has an integration section, which needs a real filesystem. That section
    maps the kinds of files to their paths and contents. image_files are the files the image
    really holds, if those are to differ from what the manifest records. aliases maps the
    names of aliases to their programs, and needs an integration section."""

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
        integration=None,
        image_files=None,
        aliases=None,
        data=None,
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
        self.integration = integration
        self.aliases = aliases
        if data is not None:
            self.data = data
        elif integration is None:
            self.data = hashlib.sha512((seed or self.name).encode()).digest() * 1024
        else:
            if image_files is None:
                image_files = {p: c for files in integration.values() for p, c in files.items()}
            self.data = bytes(DATA_OFFSET) + make_erofs(image_files)
        self.verity_hash = fake_hash('verity:' + hashlib.sha256(self.data).hexdigest())

    def reference(self):
        return {
            'component': self.component,
            'kind': self.kind,
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
                'data-offset': DATA_OFFSET,
                'hash-offset': max(22528, len(self.data)),
            },
        }
        if self.base is not None:
            m['base'] = self.base.reference()
        if self.kind == 'app' and not self.sdk:
            m['app'] = {'command': 'app'}
        if self.integration is not None:
            m['integration'] = {
                kind: [
                    {
                        'path': path,
                        'sha256': hashlib.sha256(content).hexdigest(),
                        'blake3': blake3_hex(content),
                    }
                    for path, content in files.items()
                ]
                for kind, files in self.integration.items()
            }
            if self.aliases is not None:
                m['integration']['alias'] = [
                    {'name': name, 'command': command} for name, command in self.aliases.items()
                ]
        return m


# the start of a JPEG XL codestream
JXL_DATA = b'\xff\x0afake'


def write_bundle(path, images, signed=True, corrupt_image=None, catalog_ids=None):
    """Write a .manifold file as the builder would, with a catalog that gives
    apps an icon and a screenshot. catalog_ids maps a component to the
    identifier its catalog component claims, when that is to differ."""
    files = {}
    components = []
    for image in images:
        base = image.name[: -len('.raw')]
        catalog_id = (catalog_ids or {}).get(image.component, image.component)
        files[f'{base}.manifold.json'] = json.dumps(image.manifest(), indent=3).encode()
        extra = ''
        if image.kind == 'app' and not image.sdk:
            files[f'catalog/icons/64x64/{image.component}.jxl'] = JXL_DATA
            files[f'catalog/media/{image.component}/screenshot.jxl'] = JXL_DATA
            extra = (
                f'<icon type="cached" width="64" height="64">{image.component}.jxl</icon>'
                '<screenshots><screenshot type="default">'
                f'<image type="source">{image.component}/screenshot.jxl</image>'
                '</screenshot></screenshots>'
            )
        ref = f'{image.architecture}/{image.component}/{image.version}'
        components.append(
            f'<component type="generic"><id>{catalog_id}</id><name>{catalog_id}</name>{extra}'
            f'<bundle type="manifold">{ref}</bundle></component>\n'
        )
    files['catalog/manifold-local.xml'] = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<components version="1.0" origin="manifold-local">\n'
        + ''.join(components)
        + '</components>\n'
    ).encode()
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


def zstd_compress(data):
    """Compress data with zstd, as repositories serve their metadata."""
    try:
        from compression import zstd
    except ImportError:
        return subprocess.run(
            ['zstd', '-q', '-c'], input=data, capture_output=True, check=True
        ).stdout
    return zstd.compress(data)


def zstd_available():
    """Whether we can compress with zstd: Python can since 3.14, before that the zstd tool does."""
    try:
        from compression import zstd  # noqa: F401
    except ImportError:
        return shutil.which('zstd') is not None
    return True


def file_entry(data):
    """Return the entry that an index records for a file of a repository."""
    return {
        'size': len(data),
        'sha256': hashlib.sha256(data).hexdigest(),
        'blake3': blake3_hex(data),
    }


class _HttpsHandler(http.server.BaseHTTPRequestHandler):
    """Serves files whole or one range at a time, and redirects the way GitHub does for release assets."""

    def do_GET(self):
        server = self.server
        server.requests.append(self.path)
        count = server.requests.count(self.path)

        if self.path in server.redirects:
            self.send_response(302)
            self.send_header('Location', server.redirects[self.path])
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if count in server.forbidden.get(self.path, ()):
            self.send_error(403)
            return
        data = server.files.get(self.path)
        if data is None:
            self.send_error(404)
            return

        match = re.fullmatch(r'bytes=(\d+)-(\d+)', self.headers.get('Range', ''))
        if match is None or self.path in server.no_ranges:
            self.send_response(200)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
        else:
            start, end = int(match.group(1)), int(match.group(2))
            self.send_response(206)
            self.send_header('Content-Range', f'bytes {start}-{end}/{len(data)}')
            self.send_header('Content-Length', str(end - start + 1))
            self.end_headers()
            data = data[start : end + 1]
        server.sent[self.path] = server.sent.get(self.path, 0) + len(data)
        self.wfile.write(data)

    def log_message(self, format, *args):
        pass


class _QuietServer(http.server.ThreadingHTTPServer):
    """A server that is quiet about clients that refuse its certificate or hang up early."""

    def handle_error(self, request, client_address):
        pass


class HttpsServer:
    """
    A local https server with a certificate of its own, which openssl makes.
    It serves `files` by their path, follows `redirects`, answers the requests
    to a path whose count is in `forbidden[path]` with 403, and ignores ranges
    for the paths in `no_ranges`. Every request path is logged in `requests`,
    and how many bytes of each path were sent in `sent`.
    """

    def __init__(self, work_dir):
        self.cert = os.path.join(work_dir, 'cert.pem')
        key = os.path.join(work_dir, 'key.pem')
        subprocess.run(
            [
                'openssl',
                'req',
                '-x509',
                '-newkey',
                'ec',
                '-pkeyopt',
                'ec_paramgen_curve:prime256v1',
                '-nodes',
            ]
            + ['-keyout', key, '-out', self.cert, '-days', '1', '-subj', '/CN=127.0.0.1']
            + ['-addext', 'subjectAltName=IP:127.0.0.1'],
            check=True,
            capture_output=True,
        )
        self.server = _QuietServer(('127.0.0.1', 0), _HttpsHandler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(self.cert, key)
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.url = f'https://127.0.0.1:{self.server.server_address[1]}'
        self.reset()
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def reset(self):
        self.server.files = self.files = {}
        self.server.redirects = self.redirects = {}
        self.server.forbidden = self.forbidden = {}
        self.server.no_ranges = self.no_ranges = set()
        self.server.requests = self.requests = []
        self.server.sent = self.sent = {}

    def stop(self):
        self.server.shutdown()
        self.server.server_close()


def _version_key(version):
    """Sort versions the way they compare, closely enough for the versions of the tests."""
    return [
        (0, int(part), '') if part.isdigit() else (1, 0, part)
        for part in re.findall(r'\d+|\D+', version)
    ]


class Repository:
    """
    A repository in the format of docs/repository-format.rst, which an
    HttpsServer serves below /<id>. Components are added to channels, and
    publish() puts their metadata on the server, signed with the key at `key`.

    The repository serves the public key of `key` next to its root index, as
    <id>_key.pub. A component added with add_component() only has metadata,
    which is enough to refresh. One added with add_image() can be installed: the repository
    serves its manifest, and its image as a zchunk file that `zchunk_tool`
    writes, in its pool unless `pool_images` is false. A channel with a path in
    `image_paths` also serves its images there, behind a redirect like the
    one of release assets on GitHub, and names that place as its image-url-prefix.
    """

    def __init__(self, server, repo_id, key, zchunk_tool=None):
        self.server = server
        self.id = repo_id
        self.key = key
        self.zchunk_tool = zchunk_tool
        self.serial = 1
        self.summary = 'Example applications'
        self.publisher = None
        self.valid_until = '2099-06-01T12:00:00Z'
        self.channels = {}
        self.image_paths = {}
        self.pool_images = True
        self.url = f'{server.url}/{repo_id}'
        self._zchunk_files = {}

    def add_component(self, channel, component_id, version='1.0', icon=None):
        """Add a component in one version, which has no manifest or image to download."""
        self.channels.setdefault(channel, {})[component_id] = [
            {'version': version, 'revision': 1, 'icon': icon, 'image': None}
        ]

    def add_image(self, channel, image, icon=None):
        """Add a version of a component that can be installed, next to the versions it has."""
        versions = self.channels.setdefault(channel, {}).setdefault(image.component, [])
        versions[:] = [v for v in versions if v['version'] != image.version]
        versions.append(
            {'version': image.version, 'revision': image.revision, 'icon': icon, 'image': image}
        )
        versions.sort(key=lambda v: (_version_key(v['version']), v['revision']), reverse=True)

    def remove_component(self, channel, component_id):
        del self.channels[channel][component_id]

    def _serve(self, path, data):
        self.server.files[f'/{self.id}/{path}'] = data

    def zchunk_file(self, image):
        """The zchunk file of an image, and its header size and checksum."""
        if image.data not in self._zchunk_files:
            with tempfile.TemporaryDirectory() as work:
                with open(f'{work}/image.raw', 'wb') as f:
                    f.write(image.data)
                result = subprocess.run(
                    [self.zchunk_tool, f'{work}/image.raw', f'{work}/image.raw.zck'],
                    capture_output=True,
                    text=True,
                    check=True,
                )
                with open(f'{work}/image.raw.zck', 'rb') as f:
                    size, sha256 = result.stdout.split()
                    self._zchunk_files[image.data] = (f.read(), int(size), sha256)
        return self._zchunk_files[image.data]

    def image_url(self, channel, image):
        """Return the path of the external copy of an image, which redirects to the image."""
        return f'{self.image_paths[channel]}/{image.verity_hash}.raw.zck'

    def _version_entry(self, channel, component_id, version):
        """Make the entry of a version in the component index, and put the files it names on
        the server."""
        arch = host_architecture()
        image = version['image']
        if image is None:
            return {
                'bundle': f'{arch}/{component_id}/{version["version"]}',
                'version': version['version'],
                'revision': version['revision'],
                'manifest': {
                    'location': f'pool/{component_id}/{fake_hash(component_id + version["version"])}.json.zst',
                    'size': 100,
                    'sha256': fake_hash('sha256'),
                    'blake3': fake_hash('blake3'),
                },
                'zck-header': {'size': 100, 'sha256': fake_hash('zck')},
            }

        manifest = zstd_compress(json.dumps(image.manifest(), indent=3).encode())
        zchunk, header_size, header_sha256 = self.zchunk_file(image)
        self._serve(f'pool/{image.verity_hash}.json.zst', manifest)
        if self.pool_images:
            self._serve(f'pool/{image.verity_hash}.raw.zck', zchunk)
        if channel in self.image_paths:
            location = f'/objects/{image.verity_hash}'
            self.server.redirects[self.image_url(channel, image)] = f'{self.server.url}{location}'
            self.server.files[location] = zchunk
        return {
            'bundle': f'{arch}/{component_id}/{image.version}',
            'version': image.version,
            'revision': image.revision,
            'manifest': {'location': f'pool/{image.verity_hash}.json.zst', **file_entry(manifest)},
            'zck-header': {'size': header_size, 'sha256': header_sha256},
        }

    def _publish_channel(self, name, components):
        arch = host_architecture()
        index = {'components': {}}
        catalog = []
        icons = {}
        for component_id, versions in components.items():
            index['components'][component_id] = {
                'versions': [self._version_entry(name, component_id, v) for v in versions]
            }
            version, icon = versions[0]['version'], versions[0]['icon']
            icon_xml = ''
            if icon is not None:
                location = f'{name}/{arch}/media/{component_id}/{hashlib.sha256(icon).hexdigest()[:16]}.jxl'
                self._serve(location, icon)
                icons[f'64x64/{component_id}.jxl'] = {'location': location, **file_entry(icon)}
                icon_xml = f'<icon type="cached" width="64" height="64">{component_id}.jxl</icon>'
            catalog.append(
                f'<component type="desktop-application"><id>{component_id}</id><name>{component_id}</name>'
                f'<summary>A component</summary><bundle type="manifold">{arch}/{component_id}/{version}</bundle>'
                f'{icon_xml}</component>'
            )
        catalog_xml = (
            f'<components version="1.2" origin="{self.id}-{name}">{"".join(catalog)}</components>'
        ).encode()

        files = {
            'index.json.zst': zstd_compress(json.dumps(index).encode()),
            'components.xml.zst': zstd_compress(catalog_xml),
            'icons.json.zst': zstd_compress(json.dumps(icons).encode()),
        }
        for file_name, data in files.items():
            self._serve(f'{name}/{arch}/{file_name}', data)
        channel = {
            'architectures': {
                arch: {file_name: file_entry(data) for file_name, data in files.items()}
            }
        }
        if name in self.image_paths:
            channel['image-url-prefix'] = f'{self.server.url}{self.image_paths[name]}'
        return channel

    def publish(self):
        """Put the metadata on the server, and return the root index as served."""
        root = {
            'id': self.id,
            'summary': self.summary,
            **({'publisher': self.publisher} if self.publisher is not None else {}),
            'serial': self.serial,
            'date': '2026-11-05T12:00:00Z',
            'valid-until': self.valid_until,
            'channels': {
                name: self._publish_channel(name, components)
                for name, components in self.channels.items()
            },
        }
        data = zstd_compress(json.dumps(root).encode())
        with tempfile.TemporaryDirectory() as work:
            path = os.path.join(work, 'repo-index.json.zst')
            with open(path, 'wb') as f:
                f.write(data)
            subprocess.run(
                ['ssh-keygen', '-q', '-Y', 'sign', '-f', self.key, '-n', 'manifold.repo', path],
                check=True,
                capture_output=True,
            )
            with open(path + '.sig', 'rb') as f:
                self._serve('repo-index.sig', f.read())
        with open(self.key + '.pub', 'rb') as f:
            self._serve(f'{self.id}_key.pub', f.read())
        self._serve('repo-index.json.zst', data)
        return data
