#!/usr/bin/env python3
#
# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later

"""
End-to-end tests of manifoldd and the manifold tool, in the sandbox: bundles
are installed, removed and marked with the manifold tool, and the tests check
its output and the store that manifoldd writes. The store writer itself is
tested in test-store.c.

Usage: mf-test-sandbox test-installer.py --daemon manifoldd --client manifold
"""

import argparse
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
    RUNTIME_DIR,
    STORE,
    Daemon,
    Image,
    VarlinkError,
    host_architecture,
    write_bundle,
)

SOCKET = f'{RUNTIME_DIR}/test-manifoldd.sock'

ARGS = None


def rebuild(image, base=None):
    """Make a rebuild of an image: the same version with the next revision and different
    content. The rebuild is built on base, or on the base of the image if no base is given."""
    return Image(
        image.component,
        image.kind,
        image.version,
        revision=image.revision + 1,
        base=base or image.base,
        seed=f'rebuild of {image.name}',
    )


class InstallerTestCase(unittest.TestCase):
    """The base class of all tests: every class of tests gets its own daemon, and every
    test starts with an empty store."""

    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.mkdtemp(prefix='manifold-test-')
        cls.bundle_count = 0
        cls.daemon = Daemon(ARGS.daemon, SOCKET, os.path.join(cls.work, 'manifoldd.log'))
        cls.daemon.start()

        cls.core = Image(CORE_ID, 'core', '13.4')
        cls.sdk = Image(CORE_ID + '.sdk', 'core', '13.4', sdk=True, base=cls.core)
        cls.kit = Image(CORE_ID + '.7d225c1fe79b1c5e', 'kit', '20260114T0238', base=cls.core)
        cls.kit_sdk = Image(
            cls.kit.component + '.sdk', 'kit', cls.kit.version, sdk=True, base=cls.kit
        )
        cls.app = Image(APP_ID, 'app', '0.1.0', base=cls.kit)
        cls.app2 = Image(APP_ID, 'app', '0.2.0', base=cls.kit)

    @classmethod
    def tearDownClass(cls):
        cls.daemon.stop()
        shutil.rmtree(cls.work, ignore_errors=True)

    def run(self, result=None):
        """Run a test. If the test fails, print the messages that the daemon logged during the
        test."""
        result = result if result is not None else self.defaultTestResult()
        start = self.daemon.log_size()
        problems = len(result.failures) + len(result.errors)
        super().run(result)
        if len(result.failures) + len(result.errors) > problems:
            sys.stderr.write(f'--- the daemon logged:\n{self.daemon.read_log(start)}---\n')
        return result

    def setUp(self):
        """Empty the store."""
        for name in os.listdir(STORE):
            shutil.rmtree(os.path.join(STORE, name))
        for name in ('images/by-verity', 'apps', 'plugins', 'runtimes'):
            os.makedirs(os.path.join(STORE, name))

    def manifold_streams(self, *args, expect=0):
        """Run the manifold tool, check its exit status, and return its standard
        output and its standard error."""
        result = subprocess.run(
            [ARGS.client, '--socket', SOCKET, *args], capture_output=True, text=True, check=False
        )
        self.assertEqual(
            result.returncode,
            expect,
            f'manifold {" ".join(args)}:\n{result.stdout + result.stderr}',
        )
        return result.stdout, result.stderr

    def manifold(self, *args, expect=0):
        """Run the manifold tool, check its exit status, and return its standard output
        followed by its standard error."""
        out, err = self.manifold_streams(*args, expect=expect)
        return out + err

    def bundle(self, *images, **kwargs):
        """Write a bundle of the images, and return its path. The keyword arguments are
        passed on to write_bundle()."""
        type(self).bundle_count += 1
        path = os.path.join(self.work, f'bundle-{self.bundle_count}.manifold')
        return write_bundle(path, images, **kwargs)

    def install(self, *images, expect=0, **kwargs):
        """Install a bundle of the images with the manifold tool, and return the output of the
        tool."""
        return self.manifold('install', self.bundle(*images, **kwargs), expect=expect)

    def install_core(self, expect=0):
        """Install the core and its SDK."""
        return self.install(self.core, self.sdk, expect=expect)

    def install_app(self, expect=0):
        """Install version 0.1.0 of the app, and the kit it builds on."""
        return self.install(self.kit, self.app, expect=expect)

    def install_file(self, bundle):
        """Call InstallFile for a bundle directly, and return every reply."""
        with open(bundle, 'rb') as f:
            return self.daemon.call(
                'org.freedesktop.manifold.InstallFile',
                {'bundleFileDescriptor': 0},
                more=True,
                fd=f.fileno(),
            )

    def remove_component(self, component_id):
        """Call RemoveComponent directly, and return the action and the subject of every
        item it reported progress for."""
        replies = self.daemon.call(
            'org.freedesktop.manifold.RemoveComponent', {'componentId': component_id}, more=True
        )
        return [
            (r['progress']['item']['action'], r['progress']['item']['subject'])
            for r in replies[:-1]
        ]

    def installed(self):
        """Read the installed components from the store on disk, as the daemon has no
        method to list them. Returns a sorted list of (ID, version, mode)."""
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
        """Return the names of the version directories of a component."""
        path = f'{STORE}/{subdir}/{component_id}'
        return sorted(
            name
            for name in os.listdir(path)
            if not name.startswith('.')
            and not os.path.islink(f'{path}/{name}')
            and os.path.isdir(f'{path}/{name}')
        )

    def stored_images(self):
        """Return the verity root hashes of the images in the store."""
        return sorted(name[: -len('.raw')] for name in os.listdir(f'{STORE}/images/by-verity'))

    def mount_tmpfs(self, path):
        """Mount a tmpfs at path until the test ends. The daemon can neither delete nor
        rename a mount point."""
        os.makedirs(path, exist_ok=True)
        subprocess.run(['mount', '-t', 'tmpfs', 'tmpfs', path], check=True)
        self.addCleanup(self.unmount_store)

    def unmount_store(self):
        """Unmount everything below the store. A mount may be at a different path by now,
        if the daemon renamed one of its parent directories."""
        with open('/proc/self/mountinfo', encoding='utf-8') as f:
            mount_points = [line.split()[4] for line in f]
        for path in reversed(mount_points):
            if path.startswith(STORE + '/'):
                subprocess.run(['umount', '-l', path], check=True)


class InstallTest(InstallerTestCase):
    """Tests for installing bundles: which bundles are accepted or refused, and which
    versions an installation supersedes."""

    def test_not_a_bundle(self):
        notes = os.path.join(self.work, 'notes.txt')
        with open(notes, 'w', encoding='utf-8') as f:
            f.write('hello\n')
        self.assertIn('Not a valid bundle', self.manifold('install', notes, expect=1))

    def test_install_core(self):
        out = self.install_core()
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
        self.assertIn(f'needs base {CORE_ID} 13.4', self.install_app(expect=1))
        self.assertEqual(self.stored_images(), [])

    def test_app_brings_its_kit(self):
        self.install_core()
        self.assertIn(f'app      {APP_ID} ', self.install_app())
        self.assertIn((self.kit.component, self.kit.version, 'auto'), self.installed())
        # an app always has the install mode auto, even though the user installed it explicitly
        self.assertIn((APP_ID, '0.1.0', 'auto'), self.installed())
        self.assertNotIn('manual', self.manifold('list', '--kind', 'kit'))
        self.assertNotIn('manual', self.manifold('list', '--kind', 'app'))

    def test_result_leaves_out_installed_components(self):
        self.install_core()
        self.install(self.kit)
        # the kit in the bundle of the app is installed already
        out = self.install_app()
        self.assertIn(f'app      {APP_ID} ', out)
        self.assertNotIn(self.kit.component, out)

    def test_wrong_architecture_is_refused(self):
        other = 'aarch64' if host_architecture() == 'x86_64' else 'x86_64'
        core = Image(CORE_ID, 'core', '13.4', architecture=other)

        self.assertIn(
            f'is built for {other}, but this system is {host_architecture()}',
            self.install(core, expect=1),
        )
        self.assertEqual(self.installed(), [])
        self.assertEqual(self.stored_images(), [])

    def test_id_of_another_kind_is_refused(self):
        app = Image(self.kit.component, 'app', '0.1.0', base=self.kit)
        self.install_core()
        self.install(self.kit)
        installed = self.installed()

        self.assertIn(
            f'{self.kit.component} is installed already as another kind of component (kit)',
            self.install(app, expect=1),
        )
        self.assertEqual(self.installed(), installed)

    def test_install_again_is_refused(self):
        self.install_core()
        self.assertIn('installed already', self.install_core(expect=1))
        self.assertEqual(len(self.stored_images()), 2)

    def test_same_revision_with_other_image_is_refused(self):
        other_kit = Image(self.kit.component, 'kit', self.kit.version, base=self.core, seed='other')
        self.install_core()
        self.install(self.kit)

        self.assertIn('installed already', self.install(other_kit, self.app, expect=1))
        self.assertNotIn(other_kit.verity_hash, self.stored_images())
        self.assertNotIn((APP_ID, '0.1.0', 'auto'), self.installed())

    def test_rebuild_supersedes_older_revisions(self):
        kit = rebuild(self.kit)
        app = rebuild(self.app, base=kit)
        self.install_core()
        self.install_app()

        # the application is auto, and so is the kit that was installed with it
        out = self.install(kit, app)
        self.assertIn('Removed:', out)
        self.assertEqual(self.version_dirs('apps', APP_ID), [f'0.1.0-{app.revision}'])
        self.assertEqual(
            self.version_dirs('runtimes', kit.component), [f'{kit.version}-{kit.revision}']
        )
        self.assertNotIn(self.app.verity_hash, self.stored_images())
        self.assertNotIn(self.kit.verity_hash, self.stored_images())

    def test_superseding_install_reports_progress_once(self):
        kit = rebuild(self.kit)
        app = rebuild(self.app, base=kit)
        self.install_core()
        self.install_app()

        # the progress replies can only be checked on a direct call
        replies = self.install_file(self.bundle(kit, app))
        progress = [r['progress'] for r in replies[:-1]]
        self.assertIn('cleanup', [p['item']['action'] for p in progress if 'item' in p])
        # the removal of superseded versions does not reset the progress
        percents = [p['percent'] for p in progress]
        self.assertEqual(percents, sorted(percents))
        self.assertEqual(percents[-1], 100)

    def test_rebuild_keeps_manual_versions(self):
        core = rebuild(self.core)
        # install the core without its SDK, as the SDK would keep the old core as its exact base
        self.install(self.core)

        self.assertNotIn('Removed', self.install(core))
        self.assertEqual(
            self.version_dirs('runtimes', CORE_ID),
            [f'13.4-{self.core.revision}', f'13.4-{core.revision}'],
        )
        self.assertEqual(
            os.readlink(f'{STORE}/runtimes/{CORE_ID}/current'), f'13.4-{core.revision}'
        )

    def test_rebuild_keeps_revision_used_by_sdk(self):
        kit = rebuild(self.kit)
        app = rebuild(self.app, base=kit)
        self.install_core()
        self.install_app()
        self.install(self.kit_sdk)

        # the SDK needs the exact revision of the kit it extends
        self.install(kit, app)
        self.assertEqual(self.version_dirs('apps', APP_ID), [f'0.1.0-{app.revision}'])
        self.assertEqual(
            self.version_dirs('runtimes', kit.component),
            [f'{kit.version}-{self.kit.revision}', f'{kit.version}-{kit.revision}'],
        )

    def test_newer_version_supersedes_older(self):
        self.install_core()
        self.install_app()

        # the kit stays installed, as the new version uses it too
        out = self.install(self.kit, self.app2)
        self.assertIn(f'app      {APP_ID} 0.1.0', out[out.index('Removed:') :])
        self.assertEqual(self.version_dirs('apps', APP_ID), [f'0.2.0-{self.app2.revision}'])
        self.assertIn((self.kit.component, self.kit.version, 'auto'), self.installed())
        self.assertNotIn(self.app.verity_hash, self.stored_images())

    def test_base_is_found_by_its_identity(self):
        # a kit with a different ID, but the same identity as the kit the app needs
        renamed_kit = Image(CORE_ID + '.renamed', 'kit', '20260118T0000', base=self.core)
        renamed_kit.identity = self.kit.identity
        self.install_core()
        self.install(renamed_kit)

        self.install(self.app)
        self.assertEqual(
            os.readlink(f'{STORE}/apps/{APP_ID}/0.1.0-{self.app.revision}/base'),
            f'../../../runtimes/{renamed_kit.component}/current',
        )

    def test_older_kit_the_app_does_not_use_is_left_out(self):
        newer_kit = Image(
            self.kit.component, 'kit', '20260201T0000', base=self.core, seed='newer kit'
        )
        self.install_core()
        self.install(newer_kit)

        # the app builds on the newer kit, so the older kit in its bundle is not installed
        out = self.install_app()
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
        self.install_core()
        self.assertIn('platform', self.install(platform))
        self.assertEqual(
            os.readlink(f'{STORE}/runtimes/{platform.component}/6.8-22910119/base'),
            f'../../../runtimes/{CORE_ID}/13.4-{self.core.revision}',
        )
        self.assertIn(f'{platform.component} 6.8-22910119', self.manifold('list'))

    def test_exact_base_must_be_installed(self):
        # a different revision of the core provides the same identity, but it is
        # not the revision the platform and the SDK were built against
        core = rebuild(self.core)
        platform = Image(CORE_ID + '.qt6', 'platform', '6.8', base=self.core)
        self.install(core)

        for image in (platform, self.sdk):
            self.assertIn(
                f'needs base {CORE_ID} 13.4-{self.core.revision}',
                self.install(image, expect=1),
            )
            self.assertNotIn(image.verity_hash, self.stored_images())

    def test_platform_must_sit_on_a_core(self):
        platform = Image(CORE_ID + '.qt6', 'platform', '6.8', base=self.kit)
        self.install_core()
        self.install(self.kit)
        self.assertIn(
            'the base of a platform must be a core, not the kit',
            self.install(platform, expect=1),
        )
        self.assertNotIn(platform.verity_hash, self.stored_images())

    def test_corrupt_bundle_leaves_nothing(self):
        self.install_core()
        self.assertIn(
            'could not be verified',
            self.install(self.kit, self.app, corrupt_image=self.app, expect=1),
        )
        self.assertNotIn(self.app.verity_hash, self.stored_images())
        for root, dirs, files in os.walk(STORE):
            for name in dirs + files:
                self.assertFalse(name.startswith('.tmp-'), f'leftover {root}/{name}')

    def test_installed_version_stays_when_it_can_not_become_current(self):
        self.install_core()
        # put a directory at the path of the current link, which a link can not replace
        os.makedirs(f'{STORE}/apps/{APP_ID}/current/in-the-way')

        self.assertIn(f'app      {APP_ID} ', self.install_app())
        self.assertTrue(os.path.isdir(f'{STORE}/apps/{APP_ID}/0.1.0-{self.app.revision}'))

    def test_install_reports_progress(self):
        replies = self.install_file(self.bundle(self.core, self.sdk))
        items = [r['progress']['item'] for r in replies[:-1] if 'item' in r['progress']]
        self.assertEqual([i['action'] for i in items], ['install'] * 4)
        self.assertEqual([i['percent'] for i in items], [0, 100, 0, 100])
        # the metadata, and removing superseded versions, count as one more item
        self.assertEqual(
            [round(r['progress']['percent']) for r in replies[:-1]], [0, 33, 33, 67, 100]
        )
        self.assertEqual(items[0]['bytesTotal'], len(self.core.data))
        result = replies[-1]['result']
        self.assertEqual(result['bytesAdded'], len(self.core.data) + len(self.sdk.data))
        self.assertEqual(
            {c['componentId'] for c in result['installed']}, {CORE_ID, CORE_ID + '.sdk'}
        )

    def test_metainfo_is_kept_and_linked(self):
        self.install_core()
        self.install_app()
        metainfo = f'{STORE}/origins/local/metainfo'
        for directory, component in (('runtimes', CORE_ID), ('apps', APP_ID)):
            name = f'{component}.metainfo.xml'
            self.assertTrue(os.path.isfile(f'{STORE}/{directory}/{component}/current/{name}'))
            self.assertEqual(
                os.readlink(f'{metainfo}/{name}'),
                f'../../../{directory}/{component}/current/{name}',
            )
            with open(f'{metainfo}/{name}', encoding='utf-8') as f:
                self.assertIn(f'<id>{component}</id>', f.read())
        self.manifold('remove', APP_ID)
        self.assertFalse(os.path.lexists(f'{metainfo}/{APP_ID}.metainfo.xml'))
        self.assertTrue(os.path.lexists(f'{metainfo}/{CORE_ID}.metainfo.xml'))

    def test_metainfo_must_describe_the_component(self):
        out = self.install(
            self.core, self.sdk, metainfo_ids={CORE_ID: 'org.example.Other'}, expect=1
        )
        self.assertIn('describes org.example.Other, not', out)
        self.assertEqual(self.installed(), [])


class RemovalTest(InstallerTestCase):
    """Tests for removing components, and for the cleanup of components and images that
    are not needed anymore."""

    def test_remove_takes_the_kit_along(self):
        self.install_core()
        self.install_app()
        out = self.manifold('remove', APP_ID)
        self.assertIn(f'app      {APP_ID} ', out)
        self.assertIn(f'kit      {self.kit.component} ', out)
        self.assertIn('Removed 2 images', out)
        self.assertEqual(
            self.installed(), [(CORE_ID, '13.4', 'manual'), (CORE_ID + '.sdk', '13.4', 'manual')]
        )

    def test_removed_version_is_gone_even_if_not_deleted(self):
        self.install_core()
        self.install_app()
        app_dir = f'{STORE}/apps/{APP_ID}'
        self.mount_tmpfs(f'{app_dir}/0.1.0-{self.app.revision}/in-the-way')

        self.manifold('remove', APP_ID)
        self.assertEqual(self.version_dirs('apps', APP_ID), [])
        self.assertFalse(os.path.lexists(f'{app_dir}/current'))
        self.unmount_store()
        self.manifold('cleanup')
        self.assertEqual(os.listdir(app_dir), [])

    def test_version_that_can_not_be_removed_stays_current(self):
        self.install_core()
        self.install_app()
        app_dir = f'{STORE}/apps/{APP_ID}'
        self.mount_tmpfs(f'{app_dir}/0.1.0-{self.app.revision}')

        self.manifold('remove', APP_ID, expect=1)
        self.assertEqual(os.readlink(f'{app_dir}/current'), f'0.1.0-{self.app.revision}')

    def test_removal_reports_components(self):
        self.install_core()
        self.install_app()
        self.assertEqual(
            set(self.remove_component(APP_ID)),
            {('remove', APP_ID), ('cleanup', self.kit.component)},
        )

    def test_kit_installed_on_purpose_stays(self):
        self.install_core()
        self.install_app()
        self.install(self.kit)
        self.assertIn((self.kit.component, self.kit.version, 'manual'), self.installed())
        self.manifold('remove', APP_ID)
        self.assertIn((self.kit.component, self.kit.version, 'manual'), self.installed())

    def test_component_in_use_is_not_removed(self):
        self.install_core()
        self.install_app()
        self.assertIn('is used by', self.manifold('remove', self.kit.component, expect=1))
        self.assertIn('is used by', self.manifold('remove', CORE_ID, expect=1))
        self.assertIn(
            'org.example.nothing is not installed',
            self.manifold('remove', 'org.example.nothing', expect=1),
        )
        self.assertIn(
            f'{APP_ID} 9.9.9 is not installed', self.manifold('remove', APP_ID, '9.9.9', expect=1)
        )

    def test_unneeded_dependents_are_removed_first(self):
        self.install_core()
        # nothing builds on the SDK, so once it is marked auto, a cleanup would remove it
        self.manifold('mark', 'auto', CORE_ID + '.sdk')
        items = self.remove_component(CORE_ID)
        self.assertEqual(
            list(dict.fromkeys(items)), [('cleanup', CORE_ID + '.sdk'), ('remove', CORE_ID)]
        )
        self.assertEqual(self.installed(), [])

    def test_several_versions(self):
        # installing an older version after a newer one does not supersede any version
        self.install_core()
        self.install(self.kit, self.app2)
        self.install_app()
        self.assertEqual(self.manifold('list', APP_ID).count(APP_ID), 2)

        # without a terminal, the version to remove has to be given if several versions are
        # installed
        out, err = self.manifold_streams('remove', APP_ID, expect=1)
        self.assertEqual(out, '')
        self.assertIn('Several versions', err)
        self.assertIn(f'app      {APP_ID} 0.2.0', err)
        # 0.1.0 is the newest version now, so it stays installed although it is auto
        self.manifold('remove', APP_ID, '0.2.0')
        self.assertEqual(self.manifold('list', APP_ID).count(APP_ID), 1)
        self.assertIn((APP_ID, '0.1.0', 'auto'), self.installed())

    def test_version_with_revision_names_one(self):
        rebuilt = rebuild(self.app)
        old = f'0.1.0-{self.app.revision}'
        new = f'0.1.0-{rebuilt.revision}'
        self.install_core()
        self.install_app()
        # mark the installed version as manual, so that the rebuild does not supersede it
        self.manifold('mark', 'manual', APP_ID)
        self.install(self.kit, rebuilt)
        self.assertEqual(self.version_dirs('apps', APP_ID), [old, new])

        # a full version selects one revision, a version without revision selects all of its
        # revisions
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
        # a version that ends with a dash and a number, so it can be read as the full
        # version of 0.1.0 with a revision
        dashed = Image(
            APP_ID, 'app', f'0.1.0-{self.app.revision}', base=self.kit, seed='dashed app'
        )
        self.install_core()
        self.install_app()
        self.manifold('mark', 'manual', APP_ID)
        self.install(self.kit, dashed)

        out = self.manifold('remove', APP_ID, f'0.1.0-{self.app.revision}', expect=1)
        self.assertIn('is ambiguous', out)
        self.assertEqual(len(self.version_dirs('apps', APP_ID)), 2)

    def test_cleanup_keeps_the_newest_app_version(self):
        self.install_core()
        self.install(self.kit, self.app2)
        self.install_app()

        self.assertIn(f'app      {APP_ID} 0.1.0', self.manifold('cleanup'))
        self.assertIn((APP_ID, '0.2.0', 'auto'), self.installed())
        self.assertNotIn((APP_ID, '0.1.0', 'auto'), self.installed())

    def test_cleanup(self):
        self.install_core()
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
        # no progress is reported if only images are removed, as the hash of an image means nothing
        # to a user
        replies = self.daemon.call('org.freedesktop.manifold.Cleanup', more=True)
        self.assertEqual(len(replies), 1)
        self.assertEqual(replies[-1]['result']['removedImages'], ['f' * 64])
        self.assertEqual(replies[-1]['result']['bytesFreed'], 4096)

    def test_transaction_flags(self):
        self.daemon.call('org.freedesktop.manifold.Cleanup', {'flags': ['allowInteractive']})
        # an unknown flag is an invalid parameter, and the error names the field
        with self.assertRaises(VarlinkError) as refused:
            self.daemon.call('org.freedesktop.manifold.Cleanup', {'flags': ['bogus']})
        self.assertEqual(refused.exception.parameters, {'parameter': 'flags'})


class MarkTest(InstallerTestCase):
    """Marking versions as installed manually or automatically."""

    def test_marked_version_stays(self):
        self.install_core()
        self.install_app()
        # without a version, the current one is marked
        self.assertIn(f'app      {APP_ID} 0.1.0', self.manifold('mark', 'manual', APP_ID))
        self.assertIn((APP_ID, '0.1.0', 'manual'), self.installed())
        self.assertIn('current manual', self.manifold('list', APP_ID))

        # a newer version does not supersede a version that is marked manual
        self.assertNotIn('Removed', self.install(self.kit, self.app2))
        self.assertEqual(self.manifold('list', APP_ID).count(APP_ID), 2)

        # once the old version is marked auto again, a cleanup removes it, as it is not the newest
        # version
        self.manifold('mark', 'auto', APP_ID, '0.1.0')
        self.assertIn((APP_ID, '0.1.0', 'auto'), self.installed())
        self.assertIn(f'app      {APP_ID} 0.1.0', self.manifold('cleanup'))
        self.assertEqual(self.version_dirs('apps', APP_ID), [f'0.2.0-{self.app2.revision}'])

    def test_mark_missing_component(self):
        self.install_core()
        self.assertIn(
            f'{APP_ID} is not installed', self.manifold('mark', 'manual', APP_ID, expect=1)
        )
        self.assertIn(
            f'{CORE_ID} 9.9 is not installed',
            self.manifold('mark', 'auto', CORE_ID, '9.9', expect=1),
        )


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
