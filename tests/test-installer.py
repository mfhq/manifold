#!/usr/bin/env python3
#
# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later

"""
End-to-end tests of manifoldd and the manifold tool, in the sandbox: what
a user sees when installing and removing bundles. The store itself is
tested in test-store.c.

Usage: mf-test-sandbox test-installer.py --daemon manifoldd --client manifold
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mftest import (  # noqa: E402
    APP_ID,
    CORE_ID,
    EXIT_SKIPPED,
    JXL_DATA,
    RUNTIME_DIR,
    STORE,
    Daemon,
    Image,
    VarlinkError,
    host_architecture,
    write_bundle,
)

SOCKET = f'{RUNTIME_DIR}/test-manifoldd.sock'

DESKTOP_ENTRY_PATH = f'/app/share/applications/{APP_ID}.desktop'
DESKTOP_ENTRY = b'[Desktop Entry]\nType=Application\nName=App\nExec=app\n'
DESKTOP_ENTRY_EXPORT = f'{STORE}/exports/share/applications/{APP_ID}.desktop'
ICON_PATH = f'/app/share/icons/hicolor/scalable/apps/{APP_ID}.svg'
ICON = b'<svg xmlns="http://www.w3.org/2000/svg"/>'
ICON_EXPORT = f'{STORE}/exports/share/icons/hicolor/scalable/apps/{APP_ID}.svg'
MIME_PATH = f'/app/share/mime/packages/{APP_ID}.xml'
MIME_PACKAGE = (
    b'<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">'
    b'<mime-type type="application/x-app"><glob pattern="*.appfile"/></mime-type></mime-info>'
)
MIME_EXPORT = f'{STORE}/exports/share/mime/packages/{APP_ID}.xml'
ALIASES = {'app-cli': '/app/bin/app'}
ALIAS_EXPORT = f'{STORE}/exports/bin/app-cli'
REGISTRY = f'{STORE}/integrations/{APP_ID}.json'

ARGS = None


class InstallerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.mkdtemp(prefix='manifold-test-')
        cls.stub_host_tools()
        cls.daemon = Daemon(ARGS.daemon, SOCKET, os.path.join(cls.work, 'manifoldd.log'))
        cls.daemon.start()

        cls.core = Image(CORE_ID, 'core', '13.4')
        cls.sdk = Image(CORE_ID + '.sdk', 'core', '13.4', sdk=True, base=cls.core)
        cls.kit = Image(CORE_ID + '.7d225c1fe79b1c5e', 'kit', '20260114T0238', base=cls.core)
        cls.app = Image(APP_ID, 'app', '0.1.0', base=cls.kit)
        cls.app2 = Image(APP_ID, 'app', '0.2.0', base=cls.kit)

        cls.core_bundle = cls.bundle('core', [cls.core, cls.sdk])
        cls.app_bundle = cls.bundle('app', [cls.kit, cls.app])
        cls.app2_bundle = cls.bundle('app2', [cls.kit, cls.app2])
        cls.kit_bundle = cls.bundle('kit', [cls.kit])

    @classmethod
    def tearDownClass(cls):
        cls.daemon.stop()
        shutil.rmtree(cls.work, ignore_errors=True)

    @classmethod
    def stub_host_tools(cls):
        """Put scripts in front of the tools of the host that update its caches. The
        scripts log how they were called, and fail if the fail_host_tools file exists."""
        cls.host_tools_log = os.path.join(cls.work, 'host-tools.log')
        cls.fail_host_tools = os.path.join(cls.work, 'fail-host-tools')
        stubs = os.path.join(cls.work, 'bin')
        os.mkdir(stubs)
        for tool in ('update-desktop-database', 'update-mime-database'):
            with open(os.path.join(stubs, tool), 'w', encoding='utf-8') as f:
                f.write(
                    '#!/bin/sh\n'
                    f'echo "{tool} $*" >> {cls.host_tools_log}\n'
                    f'test ! -e {cls.fail_host_tools}\n'
                )
            os.chmod(os.path.join(stubs, tool), 0o755)
        os.environ['PATH'] = stubs + os.pathsep + os.environ['PATH']

    @classmethod
    def bundle(cls, name, images, **kwargs):
        return write_bundle(os.path.join(cls.work, name + '.manifold'), images, **kwargs)

    def run(self, result=None):
        """Run a test, and if it fails, show what the daemon logged meanwhile."""
        result = result if result is not None else self.defaultTestResult()
        start = self.daemon.log_size()
        problems = len(result.failures) + len(result.errors)
        super().run(result)
        if len(result.failures) + len(result.errors) > problems:
            sys.stderr.write(f'--- the daemon logged:\n{self.daemon.read_log(start)}---\n')
        return result

    def setUp(self):
        """Every test starts with nothing installed."""
        for name in os.listdir(STORE):
            shutil.rmtree(os.path.join(STORE, name))
        for name in ('images/by-verity', 'apps', 'plugins', 'runtimes'):
            os.makedirs(os.path.join(STORE, name))
        self.host_tool_calls()

    def host_tool_calls(self):
        """The calls of host tools since the last time this was called."""
        if not os.path.exists(self.host_tools_log):
            return []
        with open(self.host_tools_log, encoding='utf-8') as f:
            calls = f.read().splitlines()
        os.unlink(self.host_tools_log)
        return calls

    def manifold_streams(self, *args, expect=0):
        """Run the manifold tool, check its exit status, and return its standard
        output and its standard error."""
        result = subprocess.run(
            [ARGS.client, '--socket', SOCKET, *args],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(
            result.returncode,
            expect,
            f'manifold {" ".join(args)}:\n{result.stdout + result.stderr}',
        )
        return result.stdout, result.stderr

    def manifold(self, *args, expect=0):
        """Run the manifold tool, check its exit status, and return all it printed."""
        out, err = self.manifold_streams(*args, expect=expect)
        return out + err

    def use_own_signing_keys(self):
        """Give the test an empty key directory, and keep it away from the keys of
        whoever runs the tests. Returns the key directory."""
        if shutil.which('ssh-keygen') is None:
            self.skipTest('ssh-keygen is not available')
        config_dir = tempfile.mkdtemp(prefix='config-', dir=self.work)
        patcher = mock.patch.dict(os.environ, {'XDG_CONFIG_HOME': config_dir})
        patcher.start()
        self.addCleanup(patcher.stop)
        os.environ.pop('SSH_AUTH_SOCK', None)
        os.environ.pop('MANIFOLD_SIGNING_KEY', None)
        return os.path.join(config_dir, 'manifold', 'keys')

    def check_bundle_signature(self, bundle, public_key_file):
        """Check that a bundle is signed by the key in public_key_file, and by no other."""
        signers_file = os.path.join(self.work, 'allowed-signers')
        signature_file = os.path.join(self.work, 'index.json.sig')
        with open(public_key_file, encoding='utf-8') as f:
            public_key = f.read()
        with open(signers_file, 'w', encoding='utf-8') as f:
            f.write(f'signer {public_key}')
        with zipfile.ZipFile(bundle) as archive:
            index = archive.read('index.json')
            with open(signature_file, 'wb') as f:
                f.write(archive.read('index.json.sig'))
        subprocess.run(
            ['ssh-keygen', '-Y', 'verify', '-f', signers_file, '-I', 'signer']
            + ['-n', 'manifold.bundle', '-s', signature_file],
            input=index,
            capture_output=True,
            check=True,
        )

    def installed(self):
        """The identifiers of the installed components, with their version and mode,
        as the store on disk records them: there is no call for that."""
        found = []
        for subdir in ('apps', 'plugins', 'runtimes'):
            for component_id in os.listdir(f'{STORE}/{subdir}'):
                for name in os.listdir(f'{STORE}/{subdir}/{component_id}'):
                    version_dir = f'{STORE}/{subdir}/{component_id}/{name}'
                    if name.startswith('.') or os.path.islink(version_dir):
                        continue
                    with open(f'{version_dir}/mode', encoding='utf-8') as f:
                        mode = f.read().strip()
                    found.append((component_id, name.rsplit('-', 1)[0], mode))
        return sorted(found)

    def version_dirs(self, subdir, component_id):
        """The names of the version directories of a component."""
        path = f'{STORE}/{subdir}/{component_id}'
        return sorted(
            name
            for name in os.listdir(path)
            if not name.startswith('.')
            and not os.path.islink(f'{path}/{name}')
            and os.path.isdir(f'{path}/{name}')
        )

    def stored_images(self):
        return sorted(name[: -len('.raw')] for name in os.listdir(f'{STORE}/images/by-verity'))

    def mount_tmpfs(self, path):
        """Mount a tmpfs at path, which the store can neither delete nor move, until the
        test ends."""
        os.makedirs(path, exist_ok=True)
        subprocess.run(['mount', '-t', 'tmpfs', 'tmpfs', path], check=True)
        self.addCleanup(self.unmount_store)

    def unmount_store(self):
        """Unmount everything below the store, wherever a rename has moved it."""
        with open('/proc/self/mountinfo', encoding='utf-8') as f:
            mount_points = [line.split()[4] for line in f]
        for path in reversed(mount_points):
            if path.startswith(STORE + '/'):
                subprocess.run(['umount', '-l', path], check=True)

    # the tool

    def test_usage(self):
        self.assertIn('Manifold version', self.manifold('--version'))
        # 2 is a wrong invocation, 3 a bad input
        self.manifold(expect=2)
        self.assertIn('unknown', self.manifold('frobnicate', expect=2))
        self.manifold('install', expect=3)
        self.manifold('list', '--kind', 'bogus', expect=2)
        self.manifold('cleanup', 'extra', expect=2)
        self.manifold('mark', 'manual', expect=2)
        self.assertIn("'bogus' is not a mode", self.manifold('mark', 'bogus', APP_ID, expect=2))
        self.manifold('mark', 'manual', APP_ID, '0.1.0', 'extra', expect=2)
        self.manifold('remove', 'not/valid', expect=2)
        self.manifold('remove', 'not/valid', '1.0', expect=2)
        self.manifold('install', '/nonexistent.manifold', expect=3)

    def test_not_a_bundle(self):
        notes = os.path.join(self.work, 'notes.txt')
        with open(notes, 'w', encoding='utf-8') as f:
            f.write('hello\n')
        self.assertIn('Not a valid bundle', self.manifold('install', notes, expect=1))

    # installing

    def test_install_core(self):
        out = self.manifold('install', self.core_bundle)
        self.assertIn(f'core     {CORE_ID} ', out)
        self.assertIn('Disk space used', out)
        self.assertEqual(
            self.installed(), [(CORE_ID, '13.4', 'manual'), (CORE_ID + '.sdk', '13.4', 'manual')]
        )
        self.assertEqual(
            self.stored_images(), sorted([self.core.verity_hash, self.sdk.verity_hash])
        )

        out = self.manifold('list')
        self.assertIn(f'core     {CORE_ID} ', out)
        self.assertIn('sdk current manual', out)
        self.assertIn('Nothing is installed', self.manifold('list', '--kind', 'app'))

    def test_app_needs_its_core(self):
        self.assertIn(
            f'needs base {CORE_ID} 13.4', self.manifold('install', self.app_bundle, expect=1)
        )
        self.assertEqual(self.stored_images(), [])

    def test_app_brings_its_kit(self):
        self.manifold('install', self.core_bundle)
        self.assertIn(f'app      {APP_ID} ', self.manifold('install', self.app_bundle))
        self.assertIn((self.kit.component, self.kit.version, 'auto'), self.installed())
        # an app is always auto, though it was installed explicitly
        self.assertIn((APP_ID, '0.1.0', 'auto'), self.installed())
        self.assertNotIn('manual', self.manifold('list', '--kind', 'kit'))
        self.assertNotIn('manual', self.manifold('list', '--kind', 'app'))

    def test_result_names_only_what_was_installed(self):
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.kit_bundle)
        # the kit the app comes with is installed already
        out = self.manifold('install', self.app_bundle)
        self.assertIn(f'app      {APP_ID} ', out)
        self.assertNotIn(self.kit.component, out)

    def test_wrong_architecture_is_refused(self):
        other = 'aarch64' if host_architecture() == 'x86_64' else 'x86_64'
        core = Image(CORE_ID, 'core', '13.4', architecture=other)
        bundle = self.bundle('foreign', [core])

        self.assertIn(
            f'is built for {other}, but this system is {host_architecture()}',
            self.manifold('install', bundle, expect=1),
        )
        self.assertEqual(self.installed(), [])
        self.assertEqual(self.stored_images(), [])

    def test_id_of_another_kind_is_refused(self):
        app = Image(self.kit.component, 'app', '0.1.0', base=self.kit)
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.kit_bundle)
        installed = self.installed()

        self.assertIn(
            f'{self.kit.component} is installed already as another kind of component (kit)',
            self.manifold('install', self.bundle('kit-id-app', [app]), expect=1),
        )
        self.assertEqual(self.installed(), installed)

    def test_install_again_is_refused(self):
        self.manifold('install', self.core_bundle)
        self.assertIn('installed already', self.manifold('install', self.core_bundle, expect=1))
        self.assertEqual(len(self.stored_images()), 2)

    def test_same_revision_with_other_image_is_refused(self):
        other_kit = Image(
            self.kit.component,
            'kit',
            self.kit.version,
            base=self.core,
            seed='other',
        )
        other_bundle = self.bundle('conflict', [other_kit, self.app])
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.kit_bundle)

        self.assertIn('installed already', self.manifold('install', other_bundle, expect=1))
        self.assertNotIn(other_kit.verity_hash, self.stored_images())
        self.assertNotIn((APP_ID, '0.1.0', 'auto'), self.installed())

    def test_rebuild_supersedes_older_revisions(self):
        kit = Image(
            self.kit.component,
            'kit',
            self.kit.version,
            revision=self.kit.revision + 1,
            base=self.core,
            seed='rebuilt kit',
        )
        app = Image(
            APP_ID, 'app', '0.1.0', revision=self.app.revision + 1, base=kit, seed='rebuilt app'
        )
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)

        # the application is auto, and so is the kit that came along with it
        out = self.manifold('install', self.bundle('rebuild', [kit, app]))
        self.assertIn('Removed:', out)
        self.assertEqual(self.version_dirs('apps', APP_ID), [f'0.1.0-{app.revision}'])
        self.assertEqual(
            self.version_dirs('runtimes', kit.component), [f'{kit.version}-{kit.revision}']
        )
        self.assertNotIn(self.app.verity_hash, self.stored_images())
        self.assertNotIn(self.kit.verity_hash, self.stored_images())

    def test_superseding_install_reports_progress_once(self):
        kit = Image(
            self.kit.component,
            'kit',
            self.kit.version,
            revision=self.kit.revision + 1,
            base=self.core,
            seed='rebuilt kit',
        )
        app = Image(
            APP_ID, 'app', '0.1.0', revision=self.app.revision + 1, base=kit, seed='rebuilt app'
        )
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)

        with open(self.bundle('rebuild', [kit, app]), 'rb') as f:
            replies = self.daemon.call(
                'org.freedesktop.manifold.InstallFile',
                {'bundleFileDescriptor': 0},
                more=True,
                fd=f.fileno(),
            )
        progress = [r['progress'] for r in replies[:-1]]
        self.assertIn('cleanup', [p['item']['action'] for p in progress if 'item' in p])
        # removing what the installation supersedes does not start the progress over
        percents = [p['percent'] for p in progress]
        self.assertEqual(percents, sorted(percents))
        self.assertEqual(percents[-1], 100)

    def test_rebuild_keeps_what_was_installed_on_purpose(self):
        core = Image(CORE_ID, 'core', '13.4', revision=self.core.revision + 1, seed='rebuilt core')
        # without its SDK, which would keep the old core as its exact base
        self.manifold('install', self.bundle('core-only', [self.core]))

        self.assertNotIn('Removed', self.manifold('install', self.bundle('core-rebuild', [core])))
        self.assertEqual(
            self.version_dirs('runtimes', CORE_ID),
            [f'13.4-{self.core.revision}', f'13.4-{core.revision}'],
        )
        self.assertEqual(
            os.readlink(f'{STORE}/runtimes/{CORE_ID}/current'), f'13.4-{core.revision}'
        )

    def test_rebuild_keeps_what_is_still_used(self):
        kit_sdk = Image(
            self.kit.component + '.sdk', 'kit', self.kit.version, sdk=True, base=self.kit
        )
        kit = Image(
            self.kit.component,
            'kit',
            self.kit.version,
            revision=self.kit.revision + 1,
            base=self.core,
            seed='rebuilt kit',
        )
        app = Image(
            APP_ID, 'app', '0.1.0', revision=self.app.revision + 1, base=kit, seed='rebuilt app'
        )
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)
        self.manifold('install', self.bundle('kit-sdk', [kit_sdk]))

        # the SDK needs the exact revision of the kit it extends
        self.manifold('install', self.bundle('rebuild', [kit, app]))
        self.assertEqual(self.version_dirs('apps', APP_ID), [f'0.1.0-{app.revision}'])
        self.assertEqual(
            self.version_dirs('runtimes', kit.component),
            [f'{kit.version}-{self.kit.revision}', f'{kit.version}-{kit.revision}'],
        )

    def test_newer_version_supersedes_older(self):
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)

        # the kit stays, the new version uses it too
        out = self.manifold('install', self.app2_bundle)
        self.assertIn(f'app      {APP_ID} 0.1.0', out[out.index('Removed:') :])
        self.assertEqual(self.version_dirs('apps', APP_ID), [f'0.2.0-{self.app2.revision}'])
        self.assertIn((self.kit.component, self.kit.version, 'auto'), self.installed())
        self.assertNotIn(self.app.verity_hash, self.stored_images())

    def test_base_is_what_provides_the_identity(self):
        # a kit with another ID, but the same identity as the kit the app needs
        renamed_kit = Image(CORE_ID + '.renamed', 'kit', '20260118T0000', base=self.core)
        renamed_kit.identity = self.kit.identity
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.bundle('renamed-kit', [renamed_kit]))

        self.manifold('install', self.bundle('app-only', [self.app]))
        self.assertEqual(
            os.readlink(f'{STORE}/apps/{APP_ID}/0.1.0-{self.app.revision}/base'),
            f'../../../runtimes/{renamed_kit.component}/current',
        )

    def test_older_kit_the_app_does_not_use_is_left_out(self):
        newer_kit = Image(
            self.kit.component, 'kit', '20260201T0000', base=self.core, seed='newer kit'
        )
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.bundle('newer-kit', [newer_kit]))

        # the app builds on the newer kit, so the one that came along is not installed
        out = self.manifold('install', self.app_bundle)
        self.assertNotIn(self.kit.version, out)
        self.assertEqual(
            self.version_dirs('runtimes', self.kit.component),
            [f'{newer_kit.version}-{newer_kit.revision}'],
        )
        self.assertNotIn(self.kit.verity_hash, self.stored_images())
        self.assertEqual(
            os.readlink(f'{STORE}/apps/{APP_ID}/0.1.0-{self.app.revision}/base'),
            f'../../../runtimes/{self.kit.component}/current',
        )

    def test_platform_sits_on_its_exact_core(self):
        platform = Image(CORE_ID + '.qt6', 'platform', '6.8', revision=22910119, base=self.core)
        self.manifold('install', self.core_bundle)
        self.assertIn('platform', self.manifold('install', self.bundle('platform', [platform])))
        self.assertEqual(
            os.readlink(f'{STORE}/runtimes/{platform.component}/6.8-22910119/base'),
            f'../../../runtimes/{CORE_ID}/13.4-{self.core.revision}',
        )
        self.assertIn(f'{platform.component} 6.8-22910119', self.manifold('list'))

    def test_exact_base_must_be_installed(self):
        # another revision of the core provides the same identity, but it is
        # not the one the platform and the SDK were built against
        core = Image(CORE_ID, 'core', '13.4', revision=self.core.revision + 1, seed='rebuilt core')
        platform = Image(CORE_ID + '.qt6', 'platform', '6.8', base=self.core)
        self.manifold('install', self.bundle('core-rebuild', [core]))

        for image in (platform, self.sdk):
            self.assertIn(
                f'needs base {CORE_ID} 13.4-{self.core.revision}',
                self.manifold('install', self.bundle(image.component, [image]), expect=1),
            )
            self.assertNotIn(image.verity_hash, self.stored_images())

    def test_platform_must_sit_on_a_core(self):
        platform = Image(CORE_ID + '.qt6', 'platform', '6.8', base=self.kit)
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.kit_bundle)
        self.assertIn(
            f'platform {platform.component} can not build on kit {self.kit.component}',
            self.manifold('install', self.bundle('platform-on-kit', [platform]), expect=1),
        )
        self.assertNotIn(platform.verity_hash, self.stored_images())

    def test_kit_can_not_build_on_a_kit(self):
        kit = Image(CORE_ID + '.0123456789abcdef', 'kit', '20260301T1200', base=self.kit)
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.kit_bundle)
        self.assertIn(
            f'kit {kit.component} can not build on kit {self.kit.component}',
            self.manifold('install', self.bundle('kit-on-kit', [kit]), expect=1),
        )
        self.assertNotIn(kit.verity_hash, self.stored_images())

    def test_app_can_build_on_an_sdk(self):
        # like a development tool that runs on the SDK stack
        app = Image('com.example.OnSdk', 'app', '1.0', base=self.sdk)
        self.manifold('install', self.core_bundle)
        self.assertIn(
            f'app      {app.component} ',
            self.manifold('install', self.bundle('app-on-sdk', [app])),
        )
        self.assertEqual(
            os.readlink(f'{STORE}/apps/{app.component}/1.0-{app.revision}/base'),
            f'../../../runtimes/{self.sdk.component}/current',
        )

    def test_app_sdk_can_build_on_the_kit_sdk(self):
        kit_sdk = Image(
            self.kit.component + '.sdk', 'kit', self.kit.version, sdk=True, base=self.kit
        )
        app_sdk = Image(APP_ID + '.sdk', 'app', '0.1.0', sdk=True, base=kit_sdk)
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.bundle('with-sdks', [self.kit, kit_sdk, self.app, app_sdk]))
        self.assertIn((app_sdk.component, app_sdk.version, 'manual'), self.installed())

    def test_runtime_sdk_extends_its_own_layer(self):
        sdk = Image(self.kit.component + '.sdk', 'kit', self.kit.version, sdk=True, base=self.core)
        self.manifold('install', self.core_bundle)
        self.assertIn(
            f'kit SDK {sdk.component} can not build on core {CORE_ID}',
            self.manifold('install', self.bundle('kit-sdk-on-core', [sdk]), expect=1),
        )
        self.assertNotIn(sdk.verity_hash, self.stored_images())

    def test_failed_component_keeps_the_ones_installed_before(self):
        # the app comes first in the bundle, but is installed after the kit it builds on
        corrupt = self.bundle('corrupt', [self.kit, self.app], corrupt_image=self.app)
        self.manifold('install', self.core_bundle)
        self.assertIn('could not be verified', self.manifold('install', corrupt, expect=1))

        self.assertIn((self.kit.component, self.kit.version, 'auto'), self.installed())
        self.assertIn(self.kit.verity_hash, self.stored_images())
        self.assertNotIn(APP_ID, [component_id for component_id, _, _ in self.installed()])
        self.assertNotIn(self.app.verity_hash, self.stored_images())
        for root, dirs, files in os.walk(STORE):
            for name in dirs + files:
                self.assertFalse(name.startswith('.tmp-'), f'leftover {root}/{name}')

    def test_component_that_can_not_be_recorded_leaves_no_image(self):
        self.manifold('install', self.core_bundle)
        # a file where the directory of the app has to be created
        with open(f'{STORE}/apps/{APP_ID}', 'w', encoding='utf-8'):
            pass

        self.assertIn(
            'Unable to create directory', self.manifold('install', self.app_bundle, expect=1)
        )
        self.assertNotIn(self.app.verity_hash, self.stored_images())
        self.assertIn(self.kit.verity_hash, self.stored_images())
        os.unlink(f'{STORE}/apps/{APP_ID}')
        self.assertIn((self.kit.component, self.kit.version, 'auto'), self.installed())

    def test_installed_version_stays_when_it_can_not_become_current(self):
        self.manifold('install', self.core_bundle)
        # a directory where the current link goes, which no link can replace
        os.makedirs(f'{STORE}/apps/{APP_ID}/current/in-the-way')

        self.assertIn(f'app      {APP_ID} ', self.manifold('install', self.app_bundle))
        self.assertTrue(os.path.isdir(f'{STORE}/apps/{APP_ID}/0.1.0-{self.app.revision}'))

    def test_install_reports_progress(self):
        with open(self.core_bundle, 'rb') as f:
            replies = self.daemon.call(
                'org.freedesktop.manifold.InstallFile',
                {'bundleFileDescriptor': 0},
                more=True,
                fd=f.fileno(),
            )
        items = [r['progress']['item'] for r in replies[:-1] if 'item' in r['progress']]
        self.assertEqual([i['action'] for i in items], ['install'] * 4)
        self.assertEqual([i['percent'] for i in items], [0, 100, 0, 100])
        # an item names the version with its revision, so that the items of two revisions differ
        self.assertEqual(
            [i['version'] for i in items],
            [f'13.4-{self.core.revision}'] * 2 + [f'13.4-{self.sdk.revision}'] * 2,
        )
        # removing what is superseded counts as one item more
        self.assertEqual(
            [round(r['progress']['percent']) for r in replies[:-1]], [0, 33, 33, 67, 100]
        )
        self.assertEqual(items[0]['bytesTotal'], len(self.core.data))
        result = replies[-1]['result']
        self.assertEqual(result['bytesAdded'], len(self.core.data) + len(self.sdk.data))
        self.assertEqual(
            {c['componentId'] for c in result['installed']}, {CORE_ID, CORE_ID + '.sdk'}
        )

    def test_catalog_is_kept_and_linked(self):
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)
        catalog = f'{STORE}/origins/local/catalog'
        for directory, component in (('runtimes', CORE_ID), ('apps', APP_ID)):
            name = f'{component}.xml'
            self.assertTrue(os.path.isfile(f'{STORE}/{directory}/{component}/current/{name}'))
            self.assertEqual(
                os.readlink(f'{catalog}/{name}'), f'../../../{directory}/{component}/current/{name}'
            )
            with open(f'{catalog}/{name}', encoding='utf-8') as f:
                data = f.read()
            self.assertIn(f'<id>{component}</id>', data)
            self.assertIn('origin="manifold-local"', data)
            self.assertIn('<bundle type="manifold">', data)
            # only a repository serves the screenshots
            self.assertNotIn('<screenshot', data)

        # icons are named after their component, so all share a directory per size
        icons = f'{STORE}/origins/local/icons/manifold-local/64x64'
        icon = f'{APP_ID}.jxl'
        self.assertEqual(
            os.readlink(f'{icons}/{icon}'),
            f'../../../../../apps/{APP_ID}/current/icons/64x64/{icon}',
        )
        with open(f'{icons}/{icon}', 'rb') as f:
            self.assertEqual(f.read(), JXL_DATA)

        self.manifold('remove', APP_ID)
        self.assertFalse(os.path.lexists(f'{catalog}/{APP_ID}.xml'))
        self.assertFalse(os.path.lexists(f'{icons}/{icon}'))
        self.assertTrue(os.path.lexists(f'{catalog}/{CORE_ID}.xml'))

    def test_catalog_must_describe_the_component(self):
        bundle = self.bundle(
            'wrong-catalog', [self.core, self.sdk], catalog_ids={CORE_ID: 'org.example.Other'}
        )
        out = self.manifold('install', bundle, expect=1)
        self.assertIn('describes org.example.Other, not', out)
        self.assertEqual(self.installed(), [])

    def test_invalid_manifest_is_an_invalid_bundle(self):
        sdk = Image(CORE_ID + '.sdk', 'core', '13.4', sdk=True)
        out = self.manifold('install', self.bundle('sdk-without-base', [sdk]), expect=1)
        self.assertIn('Not a valid bundle', out)
        self.assertIn('an SDK must name the component it extends', out)

    # integrating with the host

    def test_integration_files_are_kept_with_their_version(self):
        service = f'{APP_ID}-report.service'
        system_path = f'/app/lib/systemd/system/{service}'
        user_path = f'/app/lib/systemd/user/{service}'
        app = Image(
            APP_ID,
            'app',
            '0.1.0',
            base=self.kit,
            integration={
                'desktop-entry': {DESKTOP_ENTRY_PATH: DESKTOP_ENTRY},
                'systemd-unit-system': {system_path: b'[Service]\nExecStart=app --system\n'},
                'systemd-unit-user': {user_path: b'[Service]\nExecStart=app --user\n'},
            },
        )
        self.manifold('install', self.core_bundle)
        out = self.manifold('install', self.bundle('integrated', [self.kit, app]))
        self.assertIn(f'system service: {service} (privileged)\n', out)
        self.assertIn(f'user service: {service}\n', out)
        self.assertIn('Privileged items can gain full access', out)

        exports = f'{STORE}/apps/{APP_ID}/0.1.0-{app.revision}/exports'
        for path, content in (
            (DESKTOP_ENTRY_PATH, DESKTOP_ENTRY),
            (system_path, b'[Service]\nExecStart=app --system\n'),
            (user_path, b'[Service]\nExecStart=app --user\n'),
        ):
            with open(exports + path[len('/app') :], 'rb') as f:
                self.assertEqual(f.read(), content)

    def test_integration_rules_are_checked_before_the_image_is_stored(self):
        self.manifold('install', self.core_bundle)
        images = self.stored_images()
        path = '/app/share/applications/org.example.Other.desktop'
        app = Image(
            APP_ID,
            'app',
            '0.1.0',
            base=self.kit,
            integration={'desktop-entry': {path: DESKTOP_ENTRY}},
        )
        out = self.manifold('install', self.bundle('foreign-entry', [self.kit, app]), expect=1)
        self.assertIn('Not a valid bundle', out)
        self.assertIn(f'is not named like {APP_ID}', out)
        self.assertEqual(self.stored_images(), images)
        # names are only printed once they are known to be harmless
        self.assertNotIn('will install this', out)

    def test_integration_file_has_to_match_its_manifest(self):
        self.manifold('install', self.core_bundle)
        app = Image(
            APP_ID,
            'app',
            '0.1.0',
            base=self.kit,
            integration={'desktop-entry': {DESKTOP_ENTRY_PATH: DESKTOP_ENTRY}},
            image_files={DESKTOP_ENTRY_PATH: DESKTOP_ENTRY + b'Comment=Another file\n'},
        )
        out = self.manifold('install', self.bundle('wrong-entry', [self.kit, app]), expect=1)
        self.assertIn('does not match what its manifest records', out)
        self.assertNotIn(app.verity_hash, self.stored_images())
        self.assertNotIn(APP_ID, [component for component, _, _ in self.installed()])

    def test_integration_file_has_to_be_installable(self):
        self.manifold('install', self.core_bundle)
        entry = b'[Desktop Entry]\nType=Application\nName=App\nExec=/bin/evil\n'
        app = Image(
            APP_ID,
            'app',
            '0.1.0',
            base=self.kit,
            integration={'desktop-entry': {DESKTOP_ENTRY_PATH: entry}},
        )
        out = self.manifold('install', self.bundle('bad-entry', [self.kit, app]), expect=1)
        self.assertIn('has to start with a program of the image', out)
        self.assertNotIn(app.verity_hash, self.stored_images())

    def integrated_app(self, version, comment=b'', revision=22910000):
        """An application with a desktop entry, an icon, a MIME package and an alias. The
        comment is added to the desktop entry, to tell versions apart."""
        return Image(
            APP_ID,
            'app',
            version,
            base=self.kit,
            revision=revision,
            integration={
                'desktop-entry': {DESKTOP_ENTRY_PATH: DESKTOP_ENTRY + comment},
                'icons': {ICON_PATH: ICON},
                'mime-package': {MIME_PATH: MIME_PACKAGE},
            },
            aliases=ALIASES,
        )

    def registry(self):
        with open(REGISTRY, encoding='utf-8') as f:
            return json.load(f)

    def read_export(self, path):
        with open(path, 'rb') as f:
            return f.read()

    def write_foreign_file(self, path):
        """Create a file that does not belong to any component."""
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(b'foreign')

    def test_current_version_is_exported(self):
        self.manifold('install', self.core_bundle)
        app = self.integrated_app('0.1.0')
        out = self.manifold('install', self.bundle('integrated', [self.kit, app]))
        self.assertIn(
            f'{APP_ID} will install this on the system:\n'
            f'  menu entry: {APP_ID}.desktop\n'
            f'  file types: {APP_ID}.xml\n'
            '  icons: 1\n'
            '  command: app-cli\n',
            out,
        )
        self.assertNotIn('rivileged', out)

        entry = self.read_export(DESKTOP_ENTRY_EXPORT)
        self.assertIn(b'/mfrun com.example.app\n', entry)
        self.assertNotIn(b'Exec=app', entry)
        self.assertIn(b'"*.appfile"', self.read_export(MIME_EXPORT))
        # desktop sessions run as ordinary users
        self.assertEqual(os.stat(DESKTOP_ENTRY_EXPORT).st_mode & 0o777, 0o644)
        self.assertIn(b'/mfrun com.example.app "$@"\n', self.read_export(ALIAS_EXPORT))
        self.assertEqual(os.stat(ALIAS_EXPORT).st_mode & 0o777, 0o755)

        current = f'{STORE}/apps/{APP_ID}/current/exports'
        self.assertEqual(os.readlink(ICON_EXPORT), current + ICON_PATH[len('/app') :])
        self.assertEqual(self.read_export(ICON_EXPORT), ICON)

        registry = self.registry()
        self.assertEqual(registry['version'], f'0.1.0-{app.revision}')
        self.assertEqual(
            sorted(f['path'] for f in registry['files']),
            sorted([DESKTOP_ENTRY_EXPORT, ICON_EXPORT, MIME_EXPORT, ALIAS_EXPORT]),
        )

    def test_exports_follow_the_current_version(self):
        self.manifold('install', self.core_bundle)
        old = self.integrated_app('0.2.0', b'Comment=Old\n')
        new = Image(
            APP_ID,
            'app',
            '0.3.0',
            base=self.kit,
            integration={'desktop-entry': {DESKTOP_ENTRY_PATH: DESKTOP_ENTRY + b'Comment=New\n'}},
        )
        self.manifold('install', self.bundle('old', [self.kit, old]))
        self.manifold('mark', 'manual', APP_ID)

        # the new version has no icon, MIME package or alias
        self.manifold('install', self.bundle('new', [self.kit, new]))
        self.assertIn(b'Comment=New', self.read_export(DESKTOP_ENTRY_EXPORT))
        self.assertFalse(os.path.lexists(ICON_EXPORT))
        self.assertFalse(os.path.lexists(MIME_EXPORT))
        self.assertFalse(os.path.lexists(ALIAS_EXPORT))

        # an older version does not become current
        older = self.integrated_app('0.1.0', b'Comment=Older\n')
        self.manifold('install', self.bundle('older', [self.kit, older]))
        self.assertIn(b'Comment=New', self.read_export(DESKTOP_ENTRY_EXPORT))
        self.assertEqual(self.registry()['version'], f'0.3.0-{new.revision}')

        # falling back to a version does not need its image, as the version keeps its own
        # copies of the files in its exports directory
        with open(f'{STORE}/images/by-verity/{old.verity_hash}.raw', 'r+b') as f:
            f.truncate(0)
        self.manifold('remove', APP_ID, '0.3.0')
        self.assertIn(b'Comment=Old\n', self.read_export(DESKTOP_ENTRY_EXPORT))
        self.assertEqual(self.read_export(ICON_EXPORT), ICON)
        self.assertTrue(os.path.exists(ALIAS_EXPORT))
        self.assertEqual(self.registry()['version'], f'0.2.0-{old.revision}')

    def test_removal_leaves_no_exports(self):
        self.manifold('install', self.core_bundle)
        self.manifold(
            'install', self.bundle('integrated', [self.kit, self.integrated_app('0.1.0')])
        )

        self.manifold('remove', APP_ID)
        self.assertFalse(os.path.lexists(DESKTOP_ENTRY_EXPORT))
        self.assertFalse(os.path.lexists(ICON_EXPORT))
        self.assertFalse(os.path.lexists(MIME_EXPORT))
        self.assertFalse(os.path.lexists(ALIAS_EXPORT))
        self.assertFalse(os.path.exists(REGISTRY))

    def test_changed_exports_are_replaced(self):
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.bundle('old', [self.kit, self.integrated_app('0.1.0')]))
        with open(DESKTOP_ENTRY_EXPORT, 'ab') as f:
            f.write(b'Comment=Edited by the admin\n')
        os.unlink(ICON_EXPORT)
        self.write_foreign_file(ICON_EXPORT)

        new = self.integrated_app('0.2.0', b'Comment=New\n')
        self.manifold('install', self.bundle('new', [self.kit, new]))
        self.assertNotIn(b'Edited by the admin', self.read_export(DESKTOP_ENTRY_EXPORT))
        self.assertEqual(self.read_export(ICON_EXPORT), ICON)

        with open(DESKTOP_ENTRY_EXPORT, 'ab') as f:
            f.write(b'Comment=Edited by the admin\n')
        self.manifold('remove', APP_ID)
        self.assertFalse(os.path.lexists(DESKTOP_ENTRY_EXPORT))

    def test_foreign_file_refuses_installation(self):
        self.manifold('install', self.core_bundle)
        self.write_foreign_file(MIME_EXPORT)
        app = self.integrated_app('0.1.0')

        out = self.manifold('install', self.bundle('integrated', [self.kit, app]), expect=1)
        self.assertIn('File conflict', out)
        self.assertIn('does not belong to any component', out)
        self.assertNotIn(app.verity_hash, self.stored_images())
        self.assertEqual(self.read_export(MIME_EXPORT), b'foreign')
        self.assertFalse(os.path.exists(DESKTOP_ENTRY_EXPORT))

        # a symbolic link that points nowhere occupies the path as well
        os.unlink(MIME_EXPORT)
        os.symlink('/nonexistent', MIME_EXPORT)
        self.manifold('install', self.bundle('integrated', [self.kit, app]), expect=1)
        self.assertEqual(os.readlink(MIME_EXPORT), '/nonexistent')

    def test_file_of_another_component_refuses_installation(self):
        self.manifold('install', self.core_bundle)
        path = f'/app/share/applications/{APP_ID}.extra.desktop'
        first = Image(
            APP_ID,
            'app',
            '0.1.0',
            base=self.kit,
            integration={'desktop-entry': {path: DESKTOP_ENTRY}},
        )
        second = Image(
            APP_ID + '.extra',
            'app',
            '0.1.0',
            base=self.kit,
            integration={'desktop-entry': {path: DESKTOP_ENTRY + b'Comment=Second\n'}},
        )

        # two images of a bundle want the same file
        out = self.manifold('install', self.bundle('both', [self.kit, first, second]), expect=1)
        self.assertIn(f'that file belongs to {APP_ID}', out)
        self.assertNotIn(first.verity_hash, self.stored_images())
        self.assertNotIn(second.verity_hash, self.stored_images())

        # an installed component owns the file
        self.manifold('install', self.bundle('first', [self.kit, first]))
        out = self.manifold('install', self.bundle('second', [self.kit, second]), expect=1)
        self.assertIn(f'that file belongs to {APP_ID}', out)
        self.assertNotIn(second.verity_hash, self.stored_images())

        # an installed component owns the file, even if it has disappeared from the host
        os.unlink(f'{STORE}/exports/share/applications/{APP_ID}.extra.desktop')
        out = self.manifold('install', self.bundle('second', [self.kit, second]), expect=1)
        self.assertIn(f'that file belongs to {APP_ID}', out)

    def test_alias_of_another_component_refuses_installation(self):
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.bundle('first', [self.kit, self.integrated_app('0.1.0')]))
        second = Image(
            APP_ID + '.extra',
            'app',
            '0.1.0',
            base=self.kit,
            integration={
                'icons': {f'/app/share/icons/hicolor/scalable/apps/{APP_ID}.extra.svg': ICON}
            },
            aliases=ALIASES,
        )

        out = self.manifold('install', self.bundle('second', [self.kit, second]), expect=1)
        self.assertIn(f'that file belongs to {APP_ID}', out)
        self.assertNotIn(second.verity_hash, self.stored_images())

    def test_foreign_file_survives_falling_back(self):
        self.manifold('install', self.core_bundle)
        new = Image(
            APP_ID,
            'app',
            '0.2.0',
            base=self.kit,
            integration={'desktop-entry': {DESKTOP_ENTRY_PATH: DESKTOP_ENTRY}},
        )
        self.manifold('install', self.bundle('new', [self.kit, new]))

        # installing an older version does not change the host, so nothing is in its way yet
        self.write_foreign_file(MIME_EXPORT)
        older = self.integrated_app('0.1.0')
        self.manifold('install', self.bundle('older', [self.kit, older]))
        self.assertEqual(self.registry()['version'], f'0.2.0-{new.revision}')

        self.manifold('remove', APP_ID, '0.2.0')
        self.assertEqual(self.registry()['version'], f'0.1.0-{older.revision}')
        self.assertEqual(self.read_export(ICON_EXPORT), ICON)
        self.assertEqual(self.read_export(MIME_EXPORT), b'foreign')
        self.assertNotIn(MIME_EXPORT, [f['path'] for f in self.registry()['files']])

        self.manifold('remove', APP_ID)
        self.assertEqual(self.read_export(MIME_EXPORT), b'foreign')

    def test_host_caches_follow_the_exports(self):
        update_applications = f'update-desktop-database --quiet {STORE}/exports/share/applications'
        update_mime = f'update-mime-database {STORE}/exports/share/mime'
        self.manifold('install', self.core_bundle)

        # no application has installed a desktop entry or a MIME package yet, so the
        # directories of these files do not exist, and the tools would fail on them
        first = Image(
            APP_ID, 'app', '0.2.0', base=self.kit, integration={'icons': {ICON_PATH: ICON}}
        )
        self.manifold('install', self.bundle('first', [self.kit, first]))
        self.assertEqual(self.host_tool_calls(), [])

        second = self.integrated_app('0.3.0', b'Comment=Second\n')
        self.manifold('install', self.bundle('second', [self.kit, second]))
        self.assertEqual(self.host_tool_calls(), [update_applications, update_mime])

        # an older version does not change the host
        older = self.integrated_app('0.1.0', b'Comment=Older\n')
        self.manifold('install', self.bundle('older', [self.kit, older]))
        self.assertEqual(self.host_tool_calls(), [])

        # falling back to the older version changes the host, and so does removing that version
        self.manifold('remove', APP_ID, '0.3.0')
        self.assertEqual(self.host_tool_calls(), [update_applications, update_mime])
        self.manifold('remove', APP_ID)
        self.assertEqual(self.host_tool_calls(), [update_applications, update_mime])

    def test_failing_host_tool_is_only_a_warning(self):
        self.manifold('install', self.core_bundle)
        app = self.integrated_app('0.1.0')
        with open(self.fail_host_tools, 'w', encoding='utf-8'):
            pass
        self.addCleanup(os.unlink, self.fail_host_tools)
        start = self.daemon.log_size()

        self.manifold('install', self.bundle('integrated', [self.kit, app]))
        self.assertEqual(self.registry()['version'], f'0.1.0-{app.revision}')
        self.assertIn('update-mime-database failed', self.daemon.read_log(start))

    def test_interrupted_integration_is_finished(self):
        self.manifold('install', self.core_bundle)
        app = self.integrated_app('0.1.0')
        self.manifold('install', self.bundle('integrated', [self.kit, app]))

        # pretend that we crashed before installing the files, and also damage the store by
        # deleting the copy of the desktop entry that the version keeps
        kept = f'{STORE}/apps/{APP_ID}/0.1.0-{app.revision}/exports/share/applications/{APP_ID}.desktop'
        registry = self.registry()
        del registry['version']
        with open(REGISTRY, 'w', encoding='utf-8') as f:
            json.dump(registry, f)
        os.unlink(DESKTOP_ENTRY_EXPORT)
        os.unlink(kept)

        self.manifold('cleanup')
        self.assertEqual(self.registry()['version'], f'0.1.0-{app.revision}')
        self.assertIn(b'/mfrun com.example.app\n', self.read_export(DESKTOP_ENTRY_EXPORT))
        self.assertEqual(self.read_export(kept), DESKTOP_ENTRY)

    def test_first_installation_is_finished_after_a_crash(self):
        self.manifold('install', self.core_bundle)
        self.manifold(
            'install', self.bundle('integrated', [self.kit, self.integrated_app('0.1.0')])
        )

        # pretend that we crashed right after recording the version, before we wrote the
        # registry: nothing has reached the host at that point
        os.unlink(REGISTRY)
        for path in (DESKTOP_ENTRY_EXPORT, ICON_EXPORT, MIME_EXPORT, ALIAS_EXPORT):
            os.unlink(path)

        self.manifold('cleanup')
        self.assertTrue(os.path.exists(DESKTOP_ENTRY_EXPORT))

    def test_only_applications_can_integrate(self):
        self.manifold('install', self.core_bundle)
        kit = Image(
            self.kit.component,
            'kit',
            '20260114T0238',
            base=self.core,
            integration={'desktop-entry': {DESKTOP_ENTRY_PATH: DESKTOP_ENTRY}},
        )
        out = self.manifold('install', self.bundle('integrated-kit', [kit]), expect=1)
        self.assertIn('only applications can use this', out)

    # removing

    def test_remove_takes_the_kit_along(self):
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)
        out = self.manifold('remove', APP_ID)
        self.assertIn(f'app      {APP_ID} ', out)
        self.assertIn(f'kit      {self.kit.component} ', out)
        self.assertIn('Removed 2 images', out)
        self.assertEqual(
            self.installed(), [(CORE_ID, '13.4', 'manual'), (CORE_ID + '.sdk', '13.4', 'manual')]
        )

    def test_removed_version_is_gone_even_if_not_deleted(self):
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)
        app_dir = f'{STORE}/apps/{APP_ID}'
        self.mount_tmpfs(f'{app_dir}/0.1.0-{self.app.revision}/in-the-way')

        self.manifold('remove', APP_ID)
        self.assertEqual(self.version_dirs('apps', APP_ID), [])
        self.assertFalse(os.path.lexists(f'{app_dir}/current'))
        self.unmount_store()
        self.manifold('cleanup')
        self.assertEqual(os.listdir(app_dir), [])

    def test_version_that_can_not_be_removed_stays_current(self):
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)
        app_dir = f'{STORE}/apps/{APP_ID}'
        self.mount_tmpfs(f'{app_dir}/0.1.0-{self.app.revision}')

        self.manifold('remove', APP_ID, expect=1)
        self.assertEqual(os.readlink(f'{app_dir}/current'), f'0.1.0-{self.app.revision}')

    def test_removal_reports_components(self):
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)
        replies = self.daemon.call(
            'org.freedesktop.manifold.RemoveComponent', {'componentId': APP_ID}, more=True
        )
        items = {
            (r['progress']['item']['action'], r['progress']['item']['subject'])
            for r in replies[:-1]
        }
        self.assertEqual(items, {('remove', APP_ID), ('cleanup', self.kit.component)})

    def test_kit_installed_on_purpose_stays(self):
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)
        self.manifold('install', self.kit_bundle)
        self.assertIn((self.kit.component, self.kit.version, 'manual'), self.installed())
        self.manifold('remove', APP_ID)
        self.assertIn((self.kit.component, self.kit.version, 'manual'), self.installed())

    def test_what_is_in_use_stays(self):
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)
        self.assertIn('is used by', self.manifold('remove', self.kit.component, expect=1))
        self.assertIn('is used by', self.manifold('remove', CORE_ID, expect=1))
        self.assertIn(
            'org.example.nothing is not installed',
            self.manifold('remove', 'org.example.nothing', expect=1),
        )
        self.assertIn(
            f'{APP_ID} 9.9.9 is not installed', self.manifold('remove', APP_ID, '9.9.9', expect=1)
        )

    def test_cruft_that_builds_on_it_goes_first(self):
        self.manifold('install', self.core_bundle)
        # nothing builds on the SDK, so once it is marked auto, it is cruft that builds on the core
        self.manifold('mark', 'auto', CORE_ID + '.sdk')
        replies = self.daemon.call(
            'org.freedesktop.manifold.RemoveComponent', {'componentId': CORE_ID}, more=True
        )
        items = [
            (r['progress']['item']['action'], r['progress']['item']['subject'])
            for r in replies[:-1]
        ]
        self.assertEqual(
            list(dict.fromkeys(items)), [('cleanup', CORE_ID + '.sdk'), ('remove', CORE_ID)]
        )
        self.assertEqual(self.installed(), [])

    def test_several_versions(self):
        # an older version installed after a newer one replaces nothing
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app2_bundle)
        self.manifold('install', self.app_bundle)
        self.assertEqual(self.manifold('list', APP_ID).count(APP_ID), 2)

        # without a terminal, a removal of one of several versions has to name it
        out, err = self.manifold_streams('remove', APP_ID, expect=1)
        self.assertEqual(out, '')
        self.assertIn('Several versions', err)
        self.assertIn(f'app      {APP_ID} 0.2.0', err)
        # the version that is newest then stays, though it is auto
        self.manifold('remove', APP_ID, '0.2.0')
        self.assertEqual(self.manifold('list', APP_ID).count(APP_ID), 1)
        self.assertIn((APP_ID, '0.1.0', 'auto'), self.installed())

    def test_version_with_revision_names_one(self):
        rebuilt = Image(
            APP_ID,
            'app',
            '0.1.0',
            revision=self.app.revision + 1,
            base=self.kit,
            seed='rebuilt app',
        )
        old = f'0.1.0-{self.app.revision}'
        new = f'0.1.0-{rebuilt.revision}'
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)
        # kept, so that the rebuild does not supersede it
        self.manifold('mark', 'manual', APP_ID)
        self.manifold('install', self.bundle('rebuild', [self.kit, rebuilt]))
        self.assertEqual(self.version_dirs('apps', APP_ID), [old, new])

        # a version with its revision names that revision, a bare version all of its revisions
        self.manifold('mark', 'manual', APP_ID, new)
        self.assertEqual(
            [i for i in self.installed() if i[0] == APP_ID], [(APP_ID, '0.1.0', 'manual')] * 2
        )
        self.manifold('mark', 'auto', APP_ID, '0.1.0')
        self.assertEqual(
            [i for i in self.installed() if i[0] == APP_ID], [(APP_ID, '0.1.0', 'auto')] * 2
        )
        self.manifold('remove', APP_ID, old)
        self.assertEqual(self.version_dirs('apps', APP_ID), [new])

    def test_version_that_could_be_either_is_refused(self):
        # a version that ends like a revision, so that it is the full version of the other app too
        dashed = Image(
            APP_ID, 'app', f'0.1.0-{self.app.revision}', base=self.kit, seed='dashed app'
        )
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)
        self.manifold('mark', 'manual', APP_ID)
        self.manifold('install', self.bundle('dashed', [self.kit, dashed]))

        out = self.manifold('remove', APP_ID, f'0.1.0-{self.app.revision}', expect=1)
        self.assertIn('is ambiguous', out)
        self.assertEqual(len(self.version_dirs('apps', APP_ID)), 2)

    def test_cleanup_keeps_the_newest_app_version(self):
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app2_bundle)
        self.manifold('install', self.app_bundle)

        self.assertIn(f'app      {APP_ID} 0.1.0', self.manifold('cleanup'))
        self.assertIn((APP_ID, '0.2.0', 'auto'), self.installed())
        self.assertNotIn((APP_ID, '0.1.0', 'auto'), self.installed())

    # marking

    def test_marked_version_stays(self):
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)
        # without a version, the current one is marked
        self.assertIn(f'app      {APP_ID} 0.1.0', self.manifold('mark', 'manual', APP_ID))
        self.assertIn((APP_ID, '0.1.0', 'manual'), self.installed())
        self.assertIn('current manual', self.manifold('list', APP_ID))

        # a newer version supersedes nothing that is marked manual
        self.assertNotIn('Removed', self.manifold('install', self.app2_bundle))
        self.assertEqual(self.manifold('list', APP_ID).count(APP_ID), 2)

        # marked auto again, a cleanup removes it, as it is not the newest version
        reply = self.daemon.call(
            'org.freedesktop.manifold.MarkComponent',
            {'componentId': APP_ID, 'version': '0.1.0', 'mode': 'auto'},
        )
        self.assertEqual(
            [(c['version'], c['installMode']) for c in reply['done']['components']],
            [('0.1.0', 'auto')],
        )
        self.assertIn(f'app      {APP_ID} 0.1.0', self.manifold('cleanup'))
        self.assertEqual(self.version_dirs('apps', APP_ID), [f'0.2.0-{self.app2.revision}'])

    def test_mark_what_is_not_installed(self):
        self.manifold('install', self.core_bundle)
        self.assertIn(
            f'{APP_ID} is not installed', self.manifold('mark', 'manual', APP_ID, expect=1)
        )
        self.assertIn(
            f'{CORE_ID} 9.9 is not installed',
            self.manifold('mark', 'auto', CORE_ID, '9.9', expect=1),
        )

    def test_cleanup(self):
        self.manifold('install', self.core_bundle)
        self.assertIn('Nothing to clean up', self.manifold('cleanup'))

        stray = f'{STORE}/images/by-verity/{"f" * 64}.raw'
        with open(stray, 'wb') as f:
            f.write(b'x' * 4096)
        os.mkdir(f'{STORE}/runtimes/{CORE_ID}/.tmp-9.9.9-1')
        out = self.manifold('cleanup')
        self.assertIn('Removed 1 image', out)
        self.assertFalse(os.path.exists(stray))
        self.assertFalse(os.path.exists(f'{STORE}/runtimes/{CORE_ID}/.tmp-9.9.9-1'))

        with open(stray, 'wb') as f:
            f.write(b'x' * 4096)
        # removing images alone reports no progress, as a hash means nothing to a user
        replies = self.daemon.call('org.freedesktop.manifold.Cleanup', more=True)
        self.assertEqual(len(replies), 1)
        self.assertEqual(replies[-1]['result']['removedImages'], ['f' * 64])
        self.assertEqual(replies[-1]['result']['bytesFreed'], 4096)

    def test_command_groups(self):
        self.assertIn('key new [NAME]', self.manifold('--help'))
        self.assertIn("'key new' command", self.manifold('key', 'new', '--help'))
        self.assertIn("'key' needs a subcommand", self.manifold('key', expect=2))
        self.assertIn("'key' needs a subcommand", self.manifold('key', 'bogus', expect=2))
        self.assertIn("'bogus' is unknown", self.manifold('bogus', 'new', expect=2))

    def test_signing_keys(self):
        key_dir = self.use_own_signing_keys()
        self.assertIn('There are no signing keys', self.manifold('key', 'list'))

        # we don't create a key without a passphrase unless we are told to
        self.assertIn('--no-passphrase', self.manifold('key', 'new', expect=3))
        self.assertFalse(os.path.exists(key_dir))

        out = self.manifold('key', 'new', '--no-passphrase', '--comment', 'our release key')
        self.assertIn("'default'", out)
        with open(f'{key_dir}/default.pub', encoding='utf-8') as f:
            self.assertIn(f.read().strip(), out)
        self.assertEqual(os.stat(key_dir).st_mode & 0o777, 0o700)
        self.assertEqual(os.stat(f'{key_dir}/default').st_mode & 0o777, 0o600)

        self.manifold('key', 'new', '--no-passphrase', 'ci-1.x')
        with open(f'{key_dir}/ci-1.x', 'rb') as f:
            private_key = f.read()
        self.assertIn(
            'already exists', self.manifold('key', 'new', '--no-passphrase', 'ci-1.x', expect=1)
        )
        with open(f'{key_dir}/ci-1.x', 'rb') as f:
            self.assertEqual(f.read(), private_key)

        # these would be taken for a path, a hidden file, a public key or a fingerprint
        for name in ('../key', '.key', 'key.pub', 'SHA256:key'):
            self.manifold('key', 'new', '--no-passphrase', name, expect=2)
        self.assertEqual(
            sorted(os.listdir(key_dir)), ['ci-1.x', 'ci-1.x.pub', 'default', 'default.pub']
        )

        header, *rows = self.manifold('key', 'list').splitlines()
        self.assertEqual(header.split(), ['NAME', 'FINGERPRINT', 'COMMENT'])
        self.assertEqual([row.split()[0] for row in rows], ['ci-1.x', 'default'])
        self.assertTrue(rows[1].split()[1].startswith('SHA256:'))
        self.assertTrue(rows[1].endswith(' our release key'))

    def test_sign_bundle(self):
        key_dir = self.use_own_signing_keys()
        bundle = self.bundle('to-sign', [self.core], signed=False)

        self.manifold('bundle', 'sign', expect=3)
        self.assertIn('manifold key new', self.manifold('bundle', 'sign', bundle, expect=3))

        self.manifold('key', 'new', '--no-passphrase')
        self.manifold('key', 'new', '--no-passphrase', 'other')
        self.assertIn('with default', self.manifold('bundle', 'sign', bundle))
        self.check_bundle_signature(bundle, f'{key_dir}/default.pub')

        # a new signature replaces the old one, and the key can be given by name, or by path
        self.assertIn('with other', self.manifold('bundle', 'sign', '--key=other', bundle))
        self.check_bundle_signature(bundle, f'{key_dir}/other.pub')
        self.manifold('bundle', 'sign', f'--key={key_dir}/default', bundle)
        self.check_bundle_signature(bundle, f'{key_dir}/default.pub')

        self.assertIn(
            'no signing key named', self.manifold('bundle', 'sign', '--key=nope', bundle, expect=3)
        )

        # the key in the environment comes first
        with open(f'{key_dir}/other', encoding='utf-8') as f:
            with mock.patch.dict(os.environ, {'MANIFOLD_SIGNING_KEY': f.read()}):
                self.assertIn('with $MANIFOLD_SIGNING_KEY', self.manifold('bundle', 'sign', bundle))
        self.check_bundle_signature(bundle, f'{key_dir}/other.pub')

        # we stop at a file we can't sign, and say what is left
        missing = os.path.join(self.work, 'missing.manifold')
        out = self.manifold('bundle', 'sign', missing, bundle, expect=1)
        self.assertIn(f'The remaining files were not signed: {bundle}', out)

        # signing rewrites the archive, which has to stay installable
        self.manifold('install', bundle)
        self.assertEqual(self.installed(), [(CORE_ID, '13.4', 'manual')])

    def test_transaction_flags(self):
        self.daemon.call('org.freedesktop.manifold.Cleanup', {'flags': ['allowInteractive']})
        # an unknown flag is an invalid parameter, and the error names the field
        with self.assertRaises(VarlinkError) as refused:
            self.daemon.call('org.freedesktop.manifold.Cleanup', {'flags': ['bogus']})
        self.assertEqual(refused.exception.parameters, {'parameter': 'flags'})


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--daemon', required=True)
    parser.add_argument('--client', required=True)
    ARGS, remaining = parser.parse_known_args()
    # b3sum hashes the bundles the test makes up, and varlinkctl makes the calls
    missing = [tool for tool in ('b3sum', 'varlinkctl') if shutil.which(tool) is None]
    if missing:
        print(f'SKIP: {" and ".join(missing)} not available')
        sys.exit(EXIT_SKIPPED)
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
