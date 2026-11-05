#!/usr/bin/env python3
#
# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later

"""
Tests of manifold-fetch, the download helper of manifoldd. We run the helper
on one end of a socket pair, like manifoldd does in tests, and let it download
files from a local HTTPS server that has a self-signed certificate.

Usage: test-fetch.py --helper manifold-fetch
"""

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mftest import EXIT_SKIPPED, HttpsServer  # noqa: E402

FETCH = 'org.freedesktop.manifold.fetch.Fetch'


class Helper:
    """A download helper process that is connected to us with a socket pair."""

    def __init__(self, ca_file):
        self.sock, theirs = socket.socketpair()
        argv = [ARGS.helper, '--connection-fd', str(theirs.fileno()), '--verbose']
        if ca_file is not None:
            argv += ['--ca-file', ca_file]
        self.process = subprocess.Popen(argv, pass_fds=(theirs.fileno(),))
        theirs.close()

    def fetch(self, requests, fds):
        """Call Fetch with the 'more' flag, and return all replies. An error reply is
        returned unchanged, no exception is raised for it."""
        call = {'method': FETCH, 'parameters': {'requests': requests}, 'more': True}
        socket.send_fds(self.sock, [json.dumps(call).encode() + b'\0'], fds)
        replies = []
        received = b''
        while True:
            while b'\0' not in received:
                data = self.sock.recv(65536)
                if not data:
                    raise RuntimeError('the helper closed the connection without replying')
                received += data
            message, _, received = received.partition(b'\0')
            reply = json.loads(message)
            if 'error' in reply:
                return reply
            replies.append(reply['parameters'])
            if not reply.get('continues', False):
                return replies

    def close(self):
        self.sock.close()
        return self.process.wait(timeout=10)


class FetchTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.mkdtemp()
        cls.server = HttpsServer(cls.work)
        cls.cert = cls.server.cert
        cls.url = cls.server.url

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()
        shutil.rmtree(cls.work)

    def setUp(self):
        self.server.reset()
        self.helper = Helper(self.cert)

    def tearDown(self):
        self.assertEqual(self.helper.close(), 0)

    def target(self):
        """Return the file descriptor of an empty file in memory. The helper writes a
        download to the file, and the test reads the data back from it."""
        fd = os.memfd_create('target')
        self.addCleanup(os.close, fd)
        return fd

    def test_whole_file(self):
        data = os.urandom(3 * 1024 * 1024)
        self.server.files['/file'] = data
        fd = self.target()

        replies = self.helper.fetch(
            [{'url': f'{self.url}/file', 'fileDescriptor': 0, 'maxSize': len(data)}], [fd]
        )
        self.assertEqual(replies[-1]['results'], [{'size': len(data)}])
        self.assertEqual(os.pread(fd, len(data) + 1, 0), data)
        # progress is reported while the file is downloaded
        self.assertGreater(len(replies), 1)
        self.assertEqual(replies[-1]['received'], len(data))

    def test_ranges_follow_a_redirect_once(self):
        data = os.urandom(64 * 1024)
        self.server.files['/assets/image'] = data
        self.server.redirects['/release/image'] = f'{self.url}/assets/image'
        ranges = [
            {'offset': 0, 'length': 100},
            {'offset': 4096, 'length': 8192},
            {'offset': 60000, 'length': 5536},
        ]
        fd = self.target()

        replies = self.helper.fetch(
            [
                {
                    'url': f'{self.url}/release/image',
                    'fileDescriptor': 0,
                    'maxSize': len(data),
                    'ranges': ranges,
                }
            ],
            [fd],
        )
        self.assertEqual(replies[-1]['results'], [{'size': 100 + 8192 + 5536}])
        for r in ranges:
            self.assertEqual(
                os.pread(fd, r['length'], r['offset']),
                data[r['offset'] : r['offset'] + r['length']],
            )
        # no data was written outside of the requested ranges
        self.assertEqual(os.pread(fd, 100, 200), b'\0' * 100)
        self.assertEqual(self.server.requests.count('/release/image'), 1)
        self.assertEqual(self.server.requests.count('/assets/image'), 3)

    def test_expired_location_is_resolved_again(self):
        data = os.urandom(16 * 1024)
        self.server.files['/assets/image'] = data
        self.server.redirects['/release/image'] = f'{self.url}/assets/image'
        # the redirect target refuses the second request, so only the first range can be downloaded
        # from it
        self.server.forbidden['/assets/image'] = (2,)
        ranges = [{'offset': 0, 'length': 1024}, {'offset': 8192, 'length': 1024}]
        fd = self.target()

        replies = self.helper.fetch(
            [
                {
                    'url': f'{self.url}/release/image',
                    'fileDescriptor': 0,
                    'maxSize': len(data),
                    'ranges': ranges,
                }
            ],
            [fd],
        )
        self.assertEqual(replies[-1]['results'], [{'size': 2048}])
        self.assertEqual(os.pread(fd, 1024, 8192), data[8192:9216])
        self.assertEqual(self.server.requests.count('/release/image'), 2)

    def test_failures(self):
        data = os.urandom(4096)
        self.server.files['/file'] = data
        self.server.files['/no-ranges'] = data
        self.server.no_ranges.add('/no-ranges')
        self.server.redirects['/to-http'] = 'http://127.0.0.1:1/file'
        requests = [
            {'url': f'{self.url}/missing', 'fileDescriptor': 0, 'maxSize': 4096},
            {'url': f'{self.url}/file', 'fileDescriptor': 1, 'maxSize': 1000},
            {
                'url': f'{self.url}/no-ranges',
                'fileDescriptor': 2,
                'maxSize': len(data),
                'ranges': [{'offset': 0, 'length': 10}],
            },
            {'url': f'{self.url}/to-http', 'fileDescriptor': 3, 'maxSize': 4096},
            {'url': f'{self.url}/file', 'fileDescriptor': 4, 'maxSize': 4096},
        ]
        fds = [self.target() for _ in requests]

        results = self.helper.fetch(requests, fds)[-1]['results']
        self.assertEqual(results[0]['failure'], 'http')
        self.assertEqual(results[0]['httpStatus'], 404)
        # a file that is larger than its maximum size is not written beyond that size
        self.assertEqual(results[1]['failure'], 'tooLarge')
        self.assertLessEqual(os.fstat(fds[1]).st_size, 1000)
        # the server sent the whole file instead of the requested range
        self.assertEqual(results[2]['failure'], 'badResponse')
        # we never follow a redirect to plain http
        self.assertEqual(results[3]['failure'], 'badResponse')
        # one request that fails does not stop the others
        self.assertEqual(results[4], {'size': len(data)})
        self.assertEqual(os.pread(fds[4], 4097, 0), data)

    def test_only_https(self):
        reply = self.helper.fetch(
            [{'url': 'http://127.0.0.1/file', 'fileDescriptor': 0, 'maxSize': 1}], [self.target()]
        )
        self.assertEqual(reply['error'], 'org.varlink.service.InvalidParameter')
        self.assertEqual(reply['parameters'], {'parameter': 'url'})

    def test_ranges_stay_below_the_largest_size(self):
        reply = self.helper.fetch(
            [
                {
                    'url': f'{self.url}/file',
                    'fileDescriptor': 0,
                    'maxSize': 100,
                    'ranges': [{'offset': 90, 'length': 11}],
                }
            ],
            [self.target()],
        )
        self.assertEqual(reply['parameters'], {'parameter': 'ranges'})

    def test_untrusted_certificate(self):
        self.server.files['/file'] = b'data'
        helper = Helper(None)
        try:
            results = helper.fetch(
                [{'url': f'{self.url}/file', 'fileDescriptor': 0, 'maxSize': 4}], [self.target()]
            )
        finally:
            self.assertEqual(helper.close(), 0)
        self.assertEqual(results[-1]['results'][0]['failure'], 'network')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--helper', required=True)
    ARGS, remaining = parser.parse_known_args()
    # we need openssl to create the certificate of the server
    if shutil.which('openssl') is None:
        print('SKIP: openssl not available')
        sys.exit(EXIT_SKIPPED)
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
