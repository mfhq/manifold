#!/usr/bin/env python3
#
# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later

"""
End-to-end tests of remotes, in the sandbox: the manifold tool refreshes
the metadata of a repository on a local https server, and installs
components from it. manifoldd downloads everything with the download
helper, which it starts itself.

Usage: mf-test-sandbox test-remote.py --daemon manifoldd --fetch-helper manifold-fetch
           --client manifold --remotes-dir DIR --zchunk-tool mf-test-zchunk
"""

import argparse
import json
import os
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
    HttpsServer,
    Image,
    Repository,
    host_architecture,
    zstd_available,
    zstd_compress,
)


def make_key(path):
    subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', path], check=True)
    return path


class RemoteTest(unittest.TestCase):
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

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()
        shutil.rmtree(cls.work)

    def setUp(self):
        for path in (STORE, ARGS.remotes_dir):
            for name in os.listdir(path):
                full = os.path.join(path, name)
                (
                    shutil.rmtree(full)
                    if os.path.isdir(full) and not os.path.islink(full)
                    else os.unlink(full)
                )
        self.server.reset()
        self.repo = Repository(self.server, 'example', self.key, ARGS.zchunk_tool)
        self.daemon = Daemon(
            ARGS.daemon,
            f'{RUNTIME_DIR}/test-manifoldd.sock',
            os.path.join(self.work, 'manifoldd.log'),
            ['--fetch-helper', ARGS.fetch_helper, '--fetch-ca-file', self.server.cert],
        )
        self.daemon.start()

    def tearDown(self):
        self.daemon.stop()

    def run(self, result=None):
        """Run a test, and if it fails, show what the daemon logged."""
        result = result if result is not None else self.defaultTestResult()
        problems = len(result.failures) + len(result.errors)
        super().run(result)
        if len(result.failures) + len(result.errors) > problems:
            with open(os.path.join(self.work, 'manifoldd.log'), encoding='utf-8') as f:
                sys.stderr.write(f'--- the daemon logged:\n{f.read()}---\n')
        return result

    def configure_remote(self, remote_id='example', channels=None, key=None):
        """
        Write the configuration of a remote for the repository, like an administrator
        does by hand. The remote trusts the public key of the key at `key`, or no key
        at all if `key` is False.
        """
        remote_dir = os.path.join(ARGS.remotes_dir, remote_id)
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

    def manifold(self, *args, expect=0):
        """Run the manifold tool, check its exit status, and return all it printed."""
        result = subprocess.run(
            [ARGS.client, '--socket', self.daemon.socket_path, *args],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, expect, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def refresh(self, *remotes, expect=0):
        """Run 'manifold refresh' for the given remotes, or for all remotes."""
        return self.manifold('refresh', *remotes, expect=expect)

    def install(self, component_id, origin=None, expect=0):
        """Run 'manifold install' for a component, from the given origin if there is one."""
        options = ['--origin', origin] if origin is not None else []
        return self.manifold('install', *options, component_id, expect=expect)

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

    def installed(self):
        """The installed components with their version, mode and origin, as the store records them."""
        found = []
        for subdir in ('apps', 'plugins', 'runtimes'):
            for component_id in os.listdir(f'{STORE}/{subdir}'):
                for name in os.listdir(f'{STORE}/{subdir}/{component_id}'):
                    version_dir = f'{STORE}/{subdir}/{component_id}/{name}'
                    if name.startswith('.') or os.path.islink(version_dir):
                        continue
                    with open(f'{version_dir}/mode', encoding='utf-8') as f:
                        mode = f.read().strip()
                    with open(f'{version_dir}/origin', encoding='utf-8') as f:
                        origin = f.read().strip()
                    found.append((component_id, name.rsplit('-', 1)[0], mode, origin))
        return sorted(found)

    def publish_app(self, channels=('main',)):
        """Publish the application and the kit and core it builds on, and refresh."""
        for channel in channels:
            for image in (self.core, self.kit, self.app):
                self.repo.add_image(channel, image)
        self.repo.publish()
        self.configure_remote(channels=list(channels))
        self.refresh()

    def image_requests(self, path):
        """The requests for zchunk files below path."""
        return [p for p in self.server.requests if p.startswith(path) and p.endswith('.raw.zck')]

    def served(self, path):
        return self.server.files[f'/example/{path}']

    def stored(self, path):
        with open(os.path.join(STORE, path), 'rb') as f:
            return f.read()

    def test_refresh_keeps_metadata_and_catalog(self):
        arch = host_architecture()
        self.repo.add_component('main', 'org.example.App', icon=JXL_DATA + b'app')
        self.repo.add_component('main', 'org.example.Runtime')
        self.repo.publish()
        self.configure_remote()
        self.refresh()

        # the metadata stays as the repository serves it, so it can be checked again
        for name in ('repo-index.json.zst', 'repo-index.sig'):
            self.assertEqual(self.stored(f'remotes/example/{name}'), self.served(name))
        for name in ('index.json.zst', 'icons.json.zst'):
            self.assertEqual(
                self.stored(f'remotes/example/main/{name}'), self.served(f'main/{arch}/{name}')
            )
        # the catalog is where AppStream finds it, with the icons of its origin
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

        # an older root index would hide updates
        self.repo.serial = 4
        self.repo.publish()
        self.assertIn('Verification failed', self.refresh(expect=1))
        # so would a different one with the same serial
        self.repo.serial = 5
        self.repo.summary = 'Another'
        self.repo.publish()
        self.assertIn('Verification failed', self.refresh(expect=1))
        # and one that expired says so
        self.repo.serial = 6
        self.repo.valid_until = '2020-01-01T00:00:00Z'
        self.repo.publish()
        self.assertIn('expired at 2020-01-01', self.refresh(expect=1))

        # the cached root index is unchanged
        self.assertEqual(self.stored('remotes/example/repo-index.json.zst'), kept)

    def test_what_is_gone_is_removed(self):
        self.repo.add_component('main', 'org.example.App', icon=JXL_DATA + b'app')
        self.repo.add_component('beta', 'org.example.App', version='2.0', icon=JXL_DATA + b'beta')
        self.repo.publish()
        self.configure_remote(channels=['main', 'beta'])
        self.refresh()
        xml_dir = os.path.join(STORE, 'catalog', 'xml')
        self.assertEqual(
            sorted(os.listdir(xml_dir)), ['example-beta.xml.zst', 'example-main.xml.zst']
        )

        # a channel that the remote no longer installs from is removed
        self.configure_remote(channels=['main'])
        self.refresh()
        self.assertEqual(os.listdir(xml_dir), ['example-main.xml.zst'])
        self.assertFalse(os.path.exists(os.path.join(STORE, 'catalog', 'icons', 'example-beta')))
        self.assertFalse(os.path.exists(os.path.join(STORE, 'remotes', 'example', 'beta')))

        # a channel the repository does not have is only a warning
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

    def test_install_with_bases(self):
        self.publish_app()
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

    def test_install_from_the_external_location(self):
        self.repo.image_paths['main'] = '/releases/download/images'
        self.repo.pool_images = False
        self.publish_app()
        self.server.requests.clear()
        self.install(APP_ID)

        self.assertEqual(len(self.installed()), 3)
        self.assertEqual(self.image_requests('/example/pool/'), [])
        self.assertTrue(self.image_requests('/releases/download/images/'))
        self.assertTrue([p for p in self.server.requests if p.startswith('/objects/')])

    def test_install_falls_back_to_the_repository(self):
        self.repo.image_paths['main'] = '/releases/download/images'
        self.publish_app()
        # the external copy of the core is a different image, and the external copy of the kit is gone
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
        self.publish_app()
        # the repository replaces the application with a new version, and we do not refresh
        self.server.reset()
        self.repo.serial += 1
        self.repo.remove_component('main', APP_ID)
        self.repo.add_image('main', self.app2)
        self.repo.publish()
        self.install(APP_ID)
        self.assertIn((APP_ID, '0.2.0', 'auto', 'example-main'), self.installed())
        self.assertEqual(self.server.requests.count('/example/repo-index.json.zst'), 1)

        # a file that does not match while the root index stays the same is not tried again
        app3 = Image(APP_ID, 'app', '0.3.0', base=self.kit)
        self.repo.serial += 1
        self.repo.add_image('main', app3)
        self.repo.publish()
        self.refresh()
        path = f'/example/pool/{app3.verity_hash}.raw.zck'
        damaged = bytearray(self.server.files[path])
        damaged[-1] ^= 0xFF
        self.server.files[path] = bytes(damaged)
        self.server.requests.clear()
        self.assertIn('Verification failed', self.install(APP_ID, expect=1))
        self.assertEqual(self.server.requests.count('/example/repo-index.json.zst'), 1)
        self.assertIn((APP_ID, '0.2.0', 'auto', 'example-main'), self.installed())

    def test_install_needs_one_origin(self):
        self.publish_app(channels=('main', 'beta'))
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
            'No remote has org.example.Missing', self.install('org.example.Missing', expect=1)
        )

    def test_install_tells_bundle_files_from_components(self):
        self.publish_app()

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
            'No remote has app.manifold', self.manifold('install', 'app.manifold', expect=1)
        )

        # we stop at the first component that fails, and name the ones that were not installed
        out = self.manifold('install', 'org.example.Missing', APP_ID, expect=1)
        self.assertIn(f'The rest was not installed: {APP_ID}', out)

    def test_install_checks_what_was_kept_again(self):
        self.publish_app()

        # the cached component index is not the one that the root index records
        with open(f'{STORE}/remotes/example/main/index.json.zst', 'wb') as f:
            f.write(zstd_compress(b'{"components": {}}'))
        self.assertIn("run 'manifold refresh'", self.install(APP_ID, expect=1))
        self.refresh()

        # the key that signed the root index is not trusted anymore
        self.configure_remote(key=self.other_key)
        self.assertIn('Verification failed', self.install(APP_ID, origin='example-main', expect=1))
        self.assertEqual(self.installed(), [])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--daemon', required=True)
    parser.add_argument('--fetch-helper', required=True)
    parser.add_argument('--client', required=True)
    parser.add_argument('--remotes-dir', required=True)
    parser.add_argument('--zchunk-tool', required=True)
    ARGS, remaining = parser.parse_known_args()
    # these tools make up the repository
    missing = [tool for tool in ('b3sum', 'openssl', 'ssh-keygen') if shutil.which(tool) is None]
    if not zstd_available():
        missing.append('zstd')
    if missing:
        print(f'SKIP: {" and ".join(missing)} not available')
        sys.exit(EXIT_SKIPPED)
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
