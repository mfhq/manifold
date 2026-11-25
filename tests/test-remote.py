#!/usr/bin/env python3
#
# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later

"""
End-to-end tests of remotes, which run in the sandbox. The manifold tool adds
and removes remotes, refreshes the metadata of a repository on a local HTTPS
server, and installs and updates components from the repository. manifoldd
starts the download helper itself, and uses it for all downloads.

The tests use the manifold tool where it can show the behavior, and only
call manifoldd directly for the details that the tool does not print.

Usage: mf-test-sandbox test-remote.py --daemon manifoldd --fetch-helper manifold-fetch
           --client manifold --remotes-dir DIR --vendor-remotes-dir DIR
           --zchunk-tool mf-test-zchunk
"""

import argparse
import contextlib
import hashlib
import os
import pty
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mftest import (  # noqa: E402
    APP_ID,
    CORE_ID,
    EXIT_SKIPPED,
    JXL_DATA,
    RUNTIME_DIR,
    STORE,
    Daemon,
    DaemonTestCase,
    HttpsServer,
    Image,
    Repository,
    VarlinkError,
    host_architecture,
    installed_components,
    zstd_available,
    zstd_compress,
)

INSPECT_REMOTE = 'org.freedesktop.manifold.InspectRemote'
ADD_REMOTE = 'org.freedesktop.manifold.AddRemote'
BLOCK_SIZE = 4096


def incompressible_data(seed, blocks):
    """Return data that zstd can not compress. A repository splits an image with such
    data into chunks of 16 blocks."""
    return b''.join(
        hashlib.sha256(f'{seed}:{i}'.encode()).digest() for i in range(blocks * BLOCK_SIZE // 32)
    )


def make_key(path):
    subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', path], check=True)
    return path


def public_key(path):
    """Return the public key of the private key file `path`, without its comment."""
    with open(f'{path}.pub', encoding='utf-8') as f:
        return ' '.join(f.read().split()[:2])


def fingerprint(path):
    """Return the fingerprint of the key file `path`, in the format that ssh-keygen prints."""
    result = subprocess.run(
        ['ssh-keygen', '-l', '-E', 'sha256', '-f', f'{path}.pub'],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.split()[1]


class RemoteTestCase(DaemonTestCase):
    """The base class of all tests: every class of tests gets its own HTTPS server, and
    every test starts with an empty store, an empty repository and no remotes."""

    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.mkdtemp()
        cls.server = HttpsServer(cls.work)
        cls.key = make_key(os.path.join(cls.work, 'repo'))
        cls.other_key = make_key(os.path.join(cls.work, 'other'))
        cls.core = Image(CORE_ID, 'core', '13.4')
        cls.kit = Image(CORE_ID + '.7d225c1fe79b1c5e', 'kit', '20260114T0238', base=cls.core)
        cls.app = Image(APP_ID, 'app', '0.1.0', base=cls.kit)
        cls.app2 = Image(APP_ID, 'app', '0.2.0', base=cls.kit)
        cls.core2 = Image(CORE_ID, 'core', '13.5', seed='core 13.5')
        cls.public_key = public_key(cls.key)
        cls.daemon = Daemon(
            ARGS.daemon,
            f'{RUNTIME_DIR}/test-manifoldd.sock',
            os.path.join(cls.work, 'manifoldd.log'),
            ['--fetch-helper', ARGS.fetch_helper, '--fetch-ca-file', cls.server.cert],
        )

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()
        shutil.rmtree(cls.work)

    def setUp(self):
        for path in (STORE, ARGS.remotes_dir, ARGS.vendor_remotes_dir):
            for name in os.listdir(path):
                full = os.path.join(path, name)
                if os.path.isdir(full) and not os.path.islink(full):
                    shutil.rmtree(full)
                else:
                    os.unlink(full)
        self.server.reset()
        self.repo = Repository(self.server, 'example', self.key, ARGS.zchunk_tool)
        self.daemon.start()

    def tearDown(self):
        self.daemon.stop()

    def configure_remote(self, remote_id='example', channels=None, key=None, vendor=False):
        """
        Write the configuration of a remote for the repository. The configuration is
        written to the directory for administrators, or to the directory for vendors
        if `vendor` is set. The remote trusts the public key of the private key file
        `key`, or no key at all if `key` is False.
        """
        remote_dir = os.path.join(
            ARGS.vendor_remotes_dir if vendor else ARGS.remotes_dir, remote_id
        )
        os.makedirs(os.path.join(remote_dir, 'keys'), exist_ok=True)
        config = f'[Remote]\nURL={self.server.url}/{remote_id}\n'
        if channels is not None:
            config += f'Channels={";".join(channels)};\n'
        with open(os.path.join(remote_dir, 'remote.conf'), 'w', encoding='utf-8') as f:
            f.write(config)
        if key is not False:
            shutil.copy(
                f'{key or self.key}.pub', os.path.join(remote_dir, 'keys', f'{remote_id}.pub')
            )

    def manifold(self, *args, expect=0, answer=None):
        """
        Run the manifold tool, check its exit status, and return its output. If `answer`
        is given, the tool runs with a terminal as its input, and `answer` is typed on
        the terminal as the reply to the question of the tool.
        """
        terminal = None
        stdin = subprocess.DEVNULL
        if answer is not None:
            terminal, stdin = pty.openpty()
            os.write(terminal, f'{answer}\n'.encode())
        try:
            result = subprocess.run(
                [ARGS.client, '--socket', self.daemon.socket_path, *args],
                stdin=stdin,
                capture_output=True,
                text=True,
                check=False,
            )
        finally:
            if terminal is not None:
                os.close(terminal)
                os.close(stdin)
        self.assertEqual(result.returncode, expect, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def refresh(self, *remotes, expect=0):
        """Run 'manifold refresh' for the given remotes, or for all remotes."""
        return self.manifold('refresh', *remotes, expect=expect)

    def install(self, component_id, origin=None, expect=0):
        """Run 'manifold install' for a component, with --origin if `origin` is given."""
        options = ['--origin', origin] if origin is not None else []
        return self.manifold('install', *options, component_id, expect=expect)

    def update(self, *component_ids, expect=0):
        """Run 'manifold update' with --no-refresh, so the update only uses the metadata
        that the test refreshed explicitly."""
        return self.manifold('update', '--no-refresh', *component_ids, expect=expect)

    def remote_add(self, key=None, channels=(), expect=0, answer=None):
        """Run 'manifold remote add' for the repository, and trust the public key of the
        private key file `key`. By default, `key` is the key that signs the repository.
        If `key` is False, no key is given, and the tool asks whether to trust the key
        that the repository serves."""
        options = ['--key', f'{key or self.key}.pub'] if key is not False else []
        for channel in channels:
            options += ['--channel', channel]
        return self.manifold('remote', 'add', *options, self.repo.url, expect=expect, answer=answer)

    def remote_remove(self, remote_id, expect=0):
        """Run 'manifold remote remove'."""
        return self.manifold('remote', 'remove', remote_id, expect=expect)

    def inspect(self, url=None, key=None):
        """Call InspectRemote directly, and return the description of the repository."""
        parameters = {'url': url or self.repo.url}
        if key is not None:
            parameters['publicKey'] = key
        return self.daemon.call(INSPECT_REMOTE, parameters)['remote']

    def call_add_remote(self, key=None, channels=None, remote_id='example'):
        """Call AddRemote directly for the repository, and return its result. By default,
        the remote trusts the key that signs the repository."""
        parameters = {
            'remoteId': remote_id,
            'url': self.repo.url,
            'publicKey': key or self.public_key,
        }
        if channels is not None:
            parameters['channels'] = channels
        return self.daemon.call(ADD_REMOTE, parameters)['result']

    @contextlib.contextmanager
    def refused(self, error=None, parameter=None, message=None):
        """Expect the call in the block to be refused. The name of the error, the invalid
        parameter and a part of the error message are checked if they are given."""
        with self.assertRaises(VarlinkError) as refused:
            yield
        if error is not None:
            self.assertIn(error, str(refused.exception))
        if parameter is not None:
            self.assertEqual(
                refused.exception.parameters.get('parameter'), parameter, refused.exception
            )
        if message is not None:
            self.assertIn(message, refused.exception.parameters.get('message', ''))

    def listed(self, output, heading):
        """Return the (ID, version) of the components that the tool listed below a heading
        like 'Installed:' in its output. The version is returned without its revision."""
        lines = output.splitlines()
        if heading not in lines:
            return []
        rows = []
        for line in lines[lines.index(heading) + 1 :]:
            columns = line.split()
            if len(columns) < 3 or columns[0] not in ('app', 'core', 'kit', 'platform'):
                break
            rows.append((columns[1], columns[2].rsplit('-', 1)[0]))
        return rows

    def updated(self, output):
        """Return the (ID, previous version, new version) of the components that the tool
        listed as updated in its output. The versions are returned without their revision."""
        lines = output.splitlines()
        if 'Updated:' not in lines:
            return []
        rows = []
        for line in lines[lines.index('Updated:') + 1 :]:
            columns = line.split()
            if len(columns) != 5 or columns[3] != '→':
                break
            rows.append((columns[1], columns[2].rsplit('-', 1)[0], columns[4].rsplit('-', 1)[0]))
        return rows

    def installed(self):
        """Return the (ID, version, install mode, origin) of the installed components."""
        return installed_components(with_origin=True)

    def publish(self, *images, channels=('main',)):
        """Publish images in the channels, configure the remote for the channels, and
        refresh. Without images, the application and the kit and core it builds on are
        published."""
        for channel in channels:
            for image in images or (self.core, self.kit, self.app):
                self.repo.add_image(channel, image)
        self.repo.publish()
        self.configure_remote(channels=list(channels))
        self.refresh()

    def republish(self, *images, refresh=True):
        """Publish a new state of the repository with more images in the main channel, and
        refresh unless `refresh` is False."""
        self.repo.serial += 1
        for image in images:
            self.repo.add_image('main', image)
        self.repo.publish()
        if refresh:
            self.refresh()

    def image_requests(self, path):
        """Return the requests that the server received for zchunk files below `path`."""
        return [p for p in self.server.requests if p.startswith(path) and p.endswith('.raw.zck')]

    def served(self, path):
        return self.server.files[f'/example/{path}']

    def stored(self, path):
        with open(os.path.join(STORE, path), 'rb') as f:
            return f.read()


class RemoteConfigTest(RemoteTestCase):
    """Adding, inspecting, listing and removing remotes."""

    def test_inspect_remote(self):
        self.repo.publisher = {'name': 'Example Inc.', 'email': 'repo@example.org'}
        self.repo.add_component('main', 'org.example.App')
        self.repo.add_component('beta', 'org.example.App', version='2.0')
        self.repo.publish()

        # the key that the repository serves verifies its root index
        remote = self.inspect(f'{self.repo.url}/')
        self.assertEqual(remote['id'], 'example')
        self.assertEqual(remote['url'], self.repo.url)
        self.assertEqual(remote['summary'], 'Example applications')
        self.assertEqual(remote['publisherName'], 'Example Inc.')
        self.assertEqual(remote['publisherEmail'], 'repo@example.org')
        self.assertEqual(remote['validUntil'], '2099-06-01T12:00:00Z')
        self.assertEqual(remote['channels'], ['main', 'beta'])
        self.assertEqual(remote['publicKey'], self.public_key)
        self.assertEqual(remote['fingerprint'], fingerprint(self.key))
        # inspecting a repository changes nothing
        self.assertEqual(os.listdir(ARGS.remotes_dir), [])
        self.assertFalse(os.path.exists(f'{STORE}/remotes/example'))

        # if the caller passes a key, the key has to verify the root index, and the repository
        # does not need to serve a key
        with self.refused('VerificationFailed'):
            self.inspect(key=public_key(self.other_key))
        del self.server.files['/example/example_key.pub']
        with self.refused('DownloadFailed'):
            self.inspect()
        self.assertEqual(self.inspect(key=self.public_key)['id'], 'example')

        # the key that the repository serves has to be the key that signed its root index
        with open(f'{self.other_key}.pub', 'rb') as f:
            self.server.files['/example/example_key.pub'] = f.read()
        with self.refused('VerificationFailed'):
            self.inspect()
        self.server.files['/example/example_key.pub'] = b'not a key\n'
        with self.refused('InvalidRepository'):
            self.inspect()

        # a repository with expired metadata is refused, as no component could be installed from it
        self.repo.valid_until = '2020-01-01T00:00:00Z'
        self.repo.publish()
        with self.refused('Expired'):
            self.inspect()

        # a malformed call is refused before anything is downloaded
        self.server.requests.clear()
        with self.refused(parameter='url'):
            self.inspect('http://example.org')
        with self.refused(parameter='publicKey'):
            self.inspect(key='not a key')
        self.assertEqual(self.server.requests, [])

    def test_add_remote(self):
        for image in (self.core, self.kit, self.app):
            self.repo.add_image('main', image)
        self.repo.add_component('beta', 'org.example.Other')
        self.repo.publish()

        # the key has to be the one that signed the repository, and the channels have to exist
        self.assertIn('not made by a trusted key', self.remote_add(key=self.other_key, expect=1))
        self.assertIn(
            'Unable to read',
            self.manifold('remote', 'add', '--key', 'missing.pub', self.repo.url, expect=3),
        )
        self.assertIn(
            'has no channel extra for this system', self.remote_add(channels=['extra'], expect=1)
        )

        # the user is not asked to trust a key that is given with --key
        out = self.remote_add()
        self.assertNotIn('Warning', out)
        self.assertNotIn('[y/N]', out)
        remote_dir = os.path.join(ARGS.remotes_dir, 'example')
        with open(os.path.join(remote_dir, 'remote.conf'), encoding='utf-8') as f:
            config = f.read()
        self.assertIn(f'URL={self.repo.url}\n', config)
        self.assertIn('Channels=main;\n', config)
        with open(os.path.join(remote_dir, 'keys', 'example.pub'), encoding='utf-8') as f:
            self.assertEqual(f.read(), f'{self.public_key}\n')

        # the remote is refreshed, so its components can be installed right away
        self.assertEqual(
            self.stored('remotes/example/repo-index.json.zst'), self.served('repo-index.json.zst')
        )
        self.install(APP_ID)
        self.assertEqual(len(self.installed()), 3)

        # a remote that is configured already is not added again
        self.assertIn('There is a remote example already', self.remote_add(expect=1))

        # a remote that was removed can be added again, with other channels
        self.remote_remove('example')
        out = self.remote_add(channels=['main', 'beta'])
        self.assertIn('Added the remote example, which installs from main, beta.', out)
        with open(os.path.join(remote_dir, 'remote.conf'), encoding='utf-8') as f:
            self.assertIn('Channels=main;beta;\n', f.read())

    def test_add_remote_refused(self):
        # the tool inspects the repository and checks most of this itself before it calls
        # AddRemote, so the checks of the daemon are only reached with a direct call
        arch = host_architecture()
        self.repo.add_component('main', 'org.example.App')
        self.repo.add_component('beta', 'org.example.App', version='2.0')
        self.repo.publish()

        # a key that did not sign the root index
        with self.refused('VerificationFailed'):
            self.call_add_remote(key=public_key(self.other_key))
        # a repository with a different ID than the one that the user confirmed
        with self.refused('VerificationFailed', message='has the ID example, not other'):
            self.call_add_remote(remote_id='other')
        with self.refused(parameter='remoteId'):
            self.call_add_remote(remote_id='not/valid')
        # a channel that the repository does not have, and a channel twice
        with self.refused('NotFound', message=f'no channel extra for {arch}, only: main, beta'):
            self.call_add_remote(channels=['main', 'extra'])
        with self.refused(parameter='channels'):
            self.call_add_remote(channels=['main', 'main'])
        self.assertEqual(os.listdir(ARGS.remotes_dir), [])

        # a remote that can not be refreshed stays, and the result says why
        del self.server.files[f'/example/beta/{arch}/index.json.zst']
        result = self.call_add_remote(channels=['main', 'beta'])
        self.assertTrue(
            any('example was added, but refreshing it failed' in w for w in result['warnings']),
            result,
        )
        self.assertTrue(os.path.isfile(os.path.join(ARGS.remotes_dir, 'example', 'remote.conf')))
        self.assertFalse(os.path.exists(f'{STORE}/remotes/example'))

        # a remote that is configured already is not replaced
        with self.refused('RemoteExists'):
            self.call_add_remote(channels=['main'])
        with open(os.path.join(ARGS.remotes_dir, 'example', 'remote.conf'), encoding='utf-8') as f:
            self.assertIn('Channels=main;beta;\n', f.read())

    def test_remote_add_asks_before_it_trusts_a_key(self):
        self.repo.publisher = {'name': 'Example Inc.', 'email': 'repo@example.org'}
        self.repo.add_component('main', 'org.example.App')
        self.repo.add_component('beta', 'org.example.App', version='2.0')
        self.repo.publish()
        config = os.path.join(ARGS.remotes_dir, 'example', 'remote.conf')

        # without a terminal, the key has to be trusted with --yes, or given with --key
        self.assertIn('--yes', self.remote_add(key=False, expect=2))
        self.assertEqual(self.server.requests, [])

        # the user is asked to trust the key that the repository serves, by its fingerprint
        out = self.remote_add(key=False, answer='n', expect=1)
        self.assertIn('example: Example applications', out)
        self.assertIn('Publisher: Example Inc. <repo@example.org>', out)
        # only the channels that would be enabled for the remote are listed
        self.assertIn('Channels: main\n', out)
        self.assertIn(f'Key: {fingerprint(self.key)}', out)
        self.assertIn('Trust this key, and add the remote? [y/N]', out)
        self.assertIn('Aborted.', out)
        self.assertFalse(os.path.exists(config))

        out = self.remote_add(key=False, answer='y')
        self.assertIn('Added the remote example, which installs from main.', out)
        self.assertTrue(os.path.exists(config))

        # with --yes, the key is trusted without a question, and no terminal is needed
        self.remote_remove('example')
        out = self.manifold('remote', 'add', '--yes', self.repo.url)
        self.assertIn('Added the remote example, which installs from main.', out)
        self.assertNotIn('[y/N]', out)

    def test_remove_remote(self):
        self.publish()
        self.install(APP_ID)
        installed = self.installed()

        self.assertIn('Removed the remote example.', self.remote_remove('example'))
        self.assertFalse(os.path.exists(os.path.join(ARGS.remotes_dir, 'example')))
        self.assertFalse(os.path.exists(f'{STORE}/remotes/example'))
        self.assertEqual(os.listdir(f'{STORE}/catalog/xml'), [])
        self.assertFalse(os.path.exists(f'{STORE}/catalog/icons/example-main'))
        # components that were installed from the remote stay installed
        self.assertEqual(self.installed(), installed)
        self.assertIn('There is no remote example', self.remote_remove('example', expect=1))
        self.assertIn(
            'The ID of the remote is not valid', self.remote_remove('../example', expect=1)
        )

        # a remote that the vendor configured can not be deleted, it is masked instead
        self.configure_remote(vendor=True)
        self.refresh()
        self.remote_remove('example')
        config = os.path.join(ARGS.remotes_dir, 'example', 'remote.conf')
        self.assertEqual(os.readlink(config), '/dev/null')
        self.assertIn('There are no remotes', self.manifold('remote', 'list'))
        self.assertFalse(os.path.exists(f'{STORE}/remotes/example'))
        self.remote_remove('example', expect=1)

        # adding the masked remote again replaces the mask with a new configuration
        self.remote_add()
        self.assertFalse(os.path.islink(config))
        self.assertIn('example: Example applications', self.manifold('remote', 'list'))

    def test_refresh_and_list_remotes(self):
        self.assertIn('There are no remotes', self.manifold('remote', 'list'))
        # refreshing without remotes is not an error
        self.refresh()

        self.repo.publisher = {'name': 'Example Inc.', 'email': 'repo@example.org'}
        self.repo.add_component('main', 'org.example.App')
        self.repo.publish()
        self.configure_remote()
        out = self.manifold('remote', 'list')
        self.assertIn('example\n', out)
        self.assertIn('Not refreshed yet', out)

        self.refresh()
        out = self.manifold('remote', 'list')
        self.assertIn('example: Example applications', out)
        self.assertIn('Publisher: Example Inc. <repo@example.org>', out)
        self.assertIn(f'URL: {self.server.url}/example', out)
        self.assertIn('Channels: main', out)
        self.assertIn('Valid until: 2099-06-01', out)

        # a remote can be named, and one that is not defined is an error
        self.refresh('example')
        self.assertIn('There is no remote other', self.refresh('other', expect=1))


class RefreshTest(RemoteTestCase):
    """Refreshing the cached metadata and the catalog of a remote."""

    def test_refresh_stores_metadata_and_catalog(self):
        arch = host_architecture()
        self.repo.add_component('main', 'org.example.App', icon=JXL_DATA + b'app')
        self.repo.add_component('main', 'org.example.Runtime')
        self.repo.publish()
        self.configure_remote()
        self.refresh()

        # cached metadata is stored as the repository serves it, so it can be verified again
        for name in ('repo-index.json.zst', 'repo-index.sig'):
            self.assertEqual(self.stored(f'remotes/example/{name}'), self.served(name))
        for name in ('index.json.zst', 'icons.json.zst'):
            self.assertEqual(
                self.stored(f'remotes/example/main/{name}'), self.served(f'main/{arch}/{name}')
            )
        # the catalog and its icons are stored in the directory layout of AppStream
        self.assertEqual(
            self.stored('catalog/xml/example-main.xml.zst'),
            self.served(f'main/{arch}/components.xml.zst'),
        )
        self.assertEqual(
            self.stored('catalog/icons/example-main/64x64/org.example.App.jxl'), JXL_DATA + b'app'
        )
        self.assertEqual(
            sorted(os.listdir(os.path.join(STORE, 'catalog', 'icons'))), ['example-main']
        )

        # refreshing again only downloads the root index, as nothing has changed
        self.server.requests.clear()
        self.refresh('example')
        self.assertEqual(
            sorted(self.server.requests),
            ['/example/repo-index.json.zst', '/example/repo-index.sig'],
        )

    def test_only_changed_icons_are_downloaded(self):
        self.repo.add_component('main', 'org.example.App', icon=JXL_DATA + b'app')
        self.repo.add_component('main', 'org.example.Other', icon=JXL_DATA + b'other')
        self.repo.publish()
        self.configure_remote()
        self.refresh()

        self.repo.serial += 1
        self.repo.add_component(
            'main', 'org.example.App', version='1.1', icon=JXL_DATA + b'new app'
        )
        self.repo.remove_component('main', 'org.example.Other')
        self.repo.publish()
        self.server.requests.clear()
        self.refresh()

        icons = os.path.join(STORE, 'catalog', 'icons', 'example-main', '64x64')
        self.assertEqual(os.listdir(icons), ['org.example.App.jxl'])
        self.assertEqual(
            self.stored('catalog/icons/example-main/64x64/org.example.App.jxl'),
            JXL_DATA + b'new app',
        )
        self.assertEqual(len([path for path in self.server.requests if '/media/' in path]), 1)

        # an icon that did not change is kept, not downloaded again
        self.repo.serial += 1
        self.repo.summary = 'Changed'
        self.repo.publish()
        self.server.requests.clear()
        self.refresh()
        self.assertEqual([path for path in self.server.requests if '/media/' in path], [])
        self.assertTrue(os.path.isfile(os.path.join(icons, 'org.example.App.jxl')))

    def test_older_or_expired_index_is_refused(self):
        self.repo.add_component('main', 'org.example.App')
        self.repo.serial = 5
        kept = self.repo.publish()
        self.configure_remote()
        self.refresh()

        # a root index with an older serial is refused, as it would hide updates
        self.repo.serial = 4
        self.repo.publish()
        self.assertIn('Verification failed', self.refresh(expect=1))
        # a different root index with the same serial is refused for the same reason
        self.repo.serial = 5
        self.repo.summary = 'Another'
        self.repo.publish()
        self.assertIn('Verification failed', self.refresh(expect=1))
        # an expired root index is refused with a message that names the expiry date
        self.repo.serial = 6
        self.repo.valid_until = '2020-01-01T00:00:00Z'
        self.repo.publish()
        self.assertIn('expired at 2020-01-01', self.refresh(expect=1))

        # the cached root index is unchanged
        self.assertEqual(self.stored('remotes/example/repo-index.json.zst'), kept)

    def test_refresh_removes_disabled_channels_and_remotes(self):
        self.repo.add_component('main', 'org.example.App', icon=JXL_DATA + b'app')
        self.repo.add_component('beta', 'org.example.App', version='2.0', icon=JXL_DATA + b'beta')
        self.repo.publish()
        self.configure_remote(channels=['main', 'beta'])
        self.refresh()
        xml_dir = os.path.join(STORE, 'catalog', 'xml')
        self.assertEqual(
            sorted(os.listdir(xml_dir)), ['example-beta.xml.zst', 'example-main.xml.zst']
        )

        # a channel that is disabled for the remote loses its cached metadata and catalog
        self.configure_remote(channels=['main'])
        self.refresh()
        self.assertEqual(os.listdir(xml_dir), ['example-main.xml.zst'])
        self.assertFalse(os.path.exists(os.path.join(STORE, 'catalog', 'icons', 'example-beta')))
        self.assertFalse(os.path.exists(os.path.join(STORE, 'remotes', 'example', 'beta')))

        # a configured channel that the repository does not have only causes a warning
        self.configure_remote(channels=['main', 'extra'])
        self.assertRegex(self.refresh(), 'Warning: .*no channel extra')

        # a remote that is not configured anymore is removed, while the local catalog stays
        with open(
            os.path.join(xml_dir, 'manifold-local-org.example.Local.xml'), 'w', encoding='utf-8'
        ) as f:
            f.write('<components/>')
        shutil.rmtree(os.path.join(ARGS.remotes_dir, 'example'))
        self.refresh()
        self.assertEqual(os.listdir(xml_dir), ['manifold-local-org.example.Local.xml'])
        self.assertEqual(os.listdir(os.path.join(STORE, 'catalog', 'icons')), [])
        self.assertEqual(os.listdir(os.path.join(STORE, 'remotes')), [])


class InstallTest(RemoteTestCase):
    """Installing components from a remote."""

    def test_install_with_bases(self):
        self.publish()
        out = self.install(APP_ID)

        self.assertEqual(
            sorted(component_id for component_id, _ in self.listed(out, 'Installed:')),
            sorted([APP_ID, self.kit.component, CORE_ID]),
        )
        self.assertEqual(
            self.installed(),
            sorted(
                [
                    (APP_ID, '0.1.0', 'auto', 'example-main'),
                    (self.kit.component, self.kit.version, 'auto', 'example-main'),
                    (CORE_ID, '13.4', 'auto', 'example-main'),
                ]
            ),
        )
        self.assertIn('Disk space used', out)
        self.assertEqual(len(self.image_requests('/example/pool/')), 6)
        # the catalog of the remote describes the components, they have no catalog data of their own
        self.assertEqual(os.listdir(f'{STORE}/catalog/xml'), ['example-main.xml.zst'])
        self.assertFalse(os.path.exists(f'{STORE}/catalog/icons/manifold-local'))

        # no image is downloaded for a component that is installed already
        self.server.requests.clear()
        self.assertIn('is installed already', self.install(APP_ID, expect=1))
        self.assertEqual(self.image_requests('/'), [])

    def test_install_sdk_with_the_sdk_below(self):
        sdk = Image(CORE_ID + '.sdk', 'core', '13.4', sdk=True, base=self.core)
        kit_sdk = Image(
            self.kit.component + '.sdk',
            'kit',
            self.kit.version,
            sdk=True,
            base=self.kit,
            base_sdk=sdk,
        )
        self.publish(self.core, sdk, self.kit, kit_sdk)

        # the SDK brings the kit it extends, and the SDK of the core for the layer below
        self.install(kit_sdk.component)
        self.assertEqual(
            self.installed(),
            sorted(
                [
                    (kit_sdk.component, kit_sdk.version, 'manual', 'example-main'),
                    (self.kit.component, self.kit.version, 'auto', 'example-main'),
                    (sdk.component, '13.4', 'auto', 'example-main'),
                    (CORE_ID, '13.4', 'auto', 'example-main'),
                ]
            ),
        )
        self.assertEqual(
            os.readlink(
                f'{STORE}/runtimes/{kit_sdk.component}/{kit_sdk.version}-{kit_sdk.revision}/base-sdk'
            ),
            f'../../../runtimes/{sdk.component}/13.4-{sdk.revision}',
        )

    def test_exact_base_is_installed_next_to_a_newer_version(self):
        sdk = Image(CORE_ID + '.sdk', 'core', '13.4', sdk=True, base=self.core)
        self.publish(self.core, self.core2, sdk)
        self.install(CORE_ID)

        # the newer core provides the same identity, but the SDK needs the core it was built on
        self.install(sdk.component)
        self.assertIn((CORE_ID, '13.4', 'auto', 'example-main'), self.installed())
        self.assertTrue(os.path.isdir(f'{STORE}/runtimes/{sdk.component}/13.4-{sdk.revision}/base'))
        self.assertEqual(
            os.readlink(f'{STORE}/runtimes/{CORE_ID}/current'), f'13.5-{self.core2.revision}'
        )

    def test_install_from_the_external_location(self):
        self.repo.image_paths['main'] = '/releases/download/images'
        self.repo.pool_images = False
        self.publish()
        self.server.requests.clear()
        self.install(APP_ID)

        self.assertEqual(len(self.installed()), 3)
        self.assertEqual(self.image_requests('/example/pool/'), [])
        self.assertTrue(self.image_requests('/releases/download/images/'))
        self.assertTrue([p for p in self.server.requests if p.startswith('/objects/')])

    def test_install_falls_back_to_the_repository(self):
        self.repo.image_paths['main'] = '/releases/download/images'
        self.publish()
        # at the image-url-prefix, the file of the core is a different image, and the file of the
        # kit is missing
        self.server.files[f'/objects/{self.core.verity_hash}'] = self.repo.zchunk_file(self.app)[0]
        del self.server.files[f'/objects/{self.kit.verity_hash}']
        self.server.requests.clear()
        self.install(APP_ID)

        self.assertEqual(len(self.installed()), 3)
        fallbacks = self.image_requests('/example/pool/')
        self.assertIn(f'/example/pool/{self.core.verity_hash}.raw.zck', fallbacks)
        self.assertIn(f'/example/pool/{self.kit.verity_hash}.raw.zck', fallbacks)
        self.assertNotIn(f'/example/pool/{self.app.verity_hash}.raw.zck', fallbacks)

    def test_install_refreshes_once_when_the_repository_changed(self):
        self.publish()
        # the repository replaces the application with a new version, and we do not refresh
        self.server.reset()
        self.repo.serial += 1
        self.repo.remove_component('main', APP_ID)
        self.repo.add_image('main', self.app2)
        self.repo.publish()
        self.install(APP_ID)
        self.assertIn((APP_ID, '0.2.0', 'auto', 'example-main'), self.installed())
        self.assertEqual(self.server.requests.count('/example/repo-index.json.zst'), 1)

        # a download that does not match is not retried if the root index is unchanged
        app3 = Image(APP_ID, 'app', '0.3.0', base=self.kit)
        self.republish(app3)
        path = f'/example/pool/{app3.verity_hash}.raw.zck'
        damaged = bytearray(self.server.files[path])
        damaged[-1] ^= 0xFF
        self.server.files[path] = bytes(damaged)
        self.server.requests.clear()
        self.assertIn('Verification failed', self.install(APP_ID, expect=1))
        self.assertEqual(self.server.requests.count('/example/repo-index.json.zst'), 1)
        self.assertIn((APP_ID, '0.2.0', 'auto', 'example-main'), self.installed())

    def test_install_needs_one_origin(self):
        self.publish(channels=('main', 'beta'))
        # several origins have the application, and the error says how to choose
        out = self.install(APP_ID, expect=1)
        self.assertIn('example-main, example-beta', out)
        self.assertIn('--origin', out)

        # the bases come from the same origin
        self.install(APP_ID, origin='example-beta')
        self.assertEqual({origin for *_, origin in self.installed()}, {'example-beta'})

        # an origin without the component, and one that does not exist
        self.install('org.example.Missing', origin='example-main', expect=1)
        self.install(APP_ID, origin='other-main', expect=1)
        self.assertIn(
            'No remote has the component org.example.Missing',
            self.install('org.example.Missing', expect=1),
        )

    def test_install_tells_bundle_files_from_components(self):
        self.publish()

        # an argument that is not a file has to be a component, and only components have an origin
        self.assertIn(
            'not a valid component identifier', self.manifold('install', 'not/valid', expect=3)
        )
        self.manifold('install', '--origin', 'example-main', './app.manifold', expect=2)
        # only a path that starts with ./ or / is a bundle file
        self.assertIn(
            'Unable to open ./app.manifold', self.manifold('install', './app.manifold', expect=3)
        )
        self.assertIn(
            'No remote has the component app.manifold',
            self.manifold('install', 'app.manifold', expect=1),
        )

        # we stop at the first component that fails, and name the ones that were not installed
        out = self.manifold('install', 'org.example.Missing', APP_ID, expect=1)
        self.assertIn(f'The remaining components were not installed: {APP_ID}', out)

    def test_install_verifies_the_cached_metadata(self):
        self.publish()

        # the cached component index is not the one that the root index records
        with open(f'{STORE}/remotes/example/main/index.json.zst', 'wb') as f:
            f.write(zstd_compress(b'{"components": {}}'))
        self.assertIn("run 'manifold refresh'", self.install(APP_ID, expect=1))
        self.refresh()

        # the key that signed the root index is not trusted anymore
        self.configure_remote(key=self.other_key)
        self.assertIn('Verification failed', self.install(APP_ID, origin='example-main', expect=1))
        self.assertEqual(self.installed(), [])


class UpdateTest(RemoteTestCase):
    """Updating installed components from their remotes."""

    def test_update_reuses_chunks(self):
        old_data = incompressible_data('app', 64)
        new_data = bytearray(old_data)
        new_data[24 * BLOCK_SIZE : 25 * BLOCK_SIZE] = incompressible_data('changed', 1)
        app = Image(APP_ID, 'app', '0.1.0', base=self.kit, data=old_data)
        app2 = Image(APP_ID, 'app', '0.2.0', base=self.kit, data=bytes(new_data))
        self.publish(self.core, self.kit, app)
        self.install(APP_ID)

        self.republish(app2)
        self.server.sent.clear()
        out = self.update()

        # the old version is part of the update, and is not listed as removed
        self.assertEqual(self.updated(out), [(APP_ID, '0.1.0', '0.2.0')])
        self.assertNotIn('Installed:', out)
        self.assertNotIn('Removed', out)
        self.assertEqual(out.count('Disk space'), 1)
        self.assertEqual(
            [(cid, version) for cid, version, *_ in self.installed() if cid == APP_ID],
            [(APP_ID, '0.2.0')],
        )
        # one of four chunks changed, so we only downloaded that chunk and the header of the image
        zchunk = self.repo.zchunk_file(app2)[0]
        sent = self.server.sent[f'/example/pool/{app2.verity_hash}.raw.zck']
        self.assertLess(sent, len(zchunk) / 2)
        self.assertGreater(sent, 16 * BLOCK_SIZE)

        # with everything up to date, nothing is downloaded
        self.server.requests.clear()
        self.assertIn('There are no updates.', self.update())
        self.assertEqual(self.image_requests('/'), [])
        self.assertFalse([p for p in self.server.requests if p.endswith('.json.zst')])

    def test_update_dependencies(self):
        platform = Image(CORE_ID + '.qt6', 'platform', '6.8', base=self.core)
        self.publish(self.core, self.kit, self.app, platform)
        self.install(platform.component)

        # the platform has an exact base, so a newer core in the repository is not an update for it
        self.republish(self.core2)
        self.assertIn('There are no updates.', self.update())

        # the kit of the application accepts any core with the same identity, so the core is updated
        self.install(APP_ID)
        self.assertEqual(self.updated(self.update()), [(CORE_ID, '13.4', '13.5')])
        self.assertIn((CORE_ID, '13.5', 'auto', 'example-main'), self.installed())
        self.assertIn((CORE_ID, '13.4', 'auto', 'example-main'), self.installed())
        self.assertTrue(os.readlink(f'{STORE}/runtimes/{CORE_ID}/current').startswith('13.5-'))

    def test_update_skips_failing_components(self):
        self.publish()
        self.install(APP_ID)

        # a new core and a new application, but the manifest of the application is gone
        self.republish(self.core2, self.app2)
        del self.server.files[f'/example/pool/{self.app2.verity_hash}.json.zst']
        self.server.requests.clear()
        out = self.update()

        self.assertEqual(self.updated(out), [(CORE_ID, '13.4', '13.5')])
        self.assertIn(f'Warning: Unable to update {APP_ID}', out)
        self.assertIn((APP_ID, '0.1.0', 'auto', 'example-main'), self.installed())
        # the root index was downloaded once more, and it had not changed
        self.assertEqual(self.server.requests.count('/example/repo-index.json.zst'), 1)

        # a component from a remote that is not configured anymore is not updated, with a warning
        shutil.rmtree(os.path.join(ARGS.remotes_dir, 'example'))
        out = self.update()
        self.assertEqual(self.updated(out), [])
        self.assertIn(
            f'Warning: {APP_ID} was installed from example-main, which is no longer configured', out
        )

        # a component that is named has to be installed
        self.assertIn(
            'org.example.Missing is not installed', self.update('org.example.Missing', expect=1)
        )

    def test_update_refreshes_first(self):
        self.publish()
        self.install(APP_ID)
        self.assertIn('There are no updates.', self.manifold('update'))

        # without a refresh, we do not see the new version
        self.republish(self.app2, refresh=False)
        self.server.requests.clear()
        self.assertIn('There are no updates.', self.update())
        self.assertEqual(self.server.requests, [])
        out = self.manifold('update', APP_ID)
        self.assertEqual(self.updated(out), [(APP_ID, '0.1.0', '0.2.0')])

        # a remote that fails to refresh does not stop the update
        app3 = Image(APP_ID, 'app', '0.3.0', base=self.kit)
        self.republish(app3)
        del self.server.files['/example/repo-index.json.zst']
        out = self.manifold('update')
        self.assertIn('Warning: Unable to refresh, updating from the cached metadata', out)
        self.assertEqual(self.updated(out), [(APP_ID, '0.2.0', '0.3.0')])

        self.assertIn(
            'not a valid component identifier', self.manifold('update', 'not/valid', expect=2)
        )


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--daemon', required=True)
    parser.add_argument('--fetch-helper', required=True)
    parser.add_argument('--client', required=True)
    parser.add_argument('--remotes-dir', required=True)
    parser.add_argument('--vendor-remotes-dir', required=True)
    parser.add_argument('--zchunk-tool', required=True)
    ARGS, remaining = parser.parse_known_args()
    # we need these tools to create the repository, and varlinkctl to call the daemon
    missing = [tool for tool in ('b3sum', 'varlinkctl', 'openssl') if shutil.which(tool) is None]
    if not zstd_available():
        missing.append('zstd')
    if missing:
        print(f'SKIP: {" and ".join(missing)} not available')
        sys.exit(EXIT_SKIPPED)
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
