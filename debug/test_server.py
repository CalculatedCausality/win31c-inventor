"""Run: python3 -m unittest discover -s debug -p 'test_*.py'."""
import hashlib
import json
import threading
import unittest
from urllib.request import Request, urlopen
from urllib.error import HTTPError
import server


class HostTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.http = server.ThreadingHTTPServer(('127.0.0.1', 0), server.Handler)
        cls.url = f'http://127.0.0.1:{cls.http.server_port}'
        threading.Thread(target=cls.http.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.http.shutdown()
        cls.http.server_close()

    def test_disk_matches_repository_and_is_uncached(self):
        if not server.DISK.exists():
            self.skipTest('out/disk.img absent')
        with urlopen(self.url + '/api/disk?t=123') as r:
            disk = r.read()
            self.assertEqual(r.headers['Cache-Control'], 'no-store')
        self.assertEqual(disk, server.DISK.read_bytes())
        with urlopen(self.url + '/api/status') as r:
            status = json.load(r)
        self.assertEqual(status['disk']['sha256'], hashlib.sha256(disk).hexdigest())

    def test_isolated_view(self):
        with urlopen(self.url) as r:
            html = r.read().decode()
        self.assertIn('machine-placeholder', html)
        self.assertNotIn('<iframe', html)
        self.assertNotIn('googletagmanager', html)

    def test_no_file_server_or_open_proxy(self):
        for path in ['/house.c', '/debug/.cache/x', '/etc/passwd',
                     '/proxy?url=https://example.org', '/machines/pcx86/other.js',
                     '/%2e%2e/house.c', '//example.org/a', '/api/disk/../../house.c']:
            with self.subTest(path=path), self.assertRaises(HTTPError) as error:
                urlopen(self.url + path)
            self.assertIn(error.exception.code, [400, 404])

    def test_rebinding_and_post_rejected(self):
        for request, code in [(Request(self.url, headers={'Host': 'evil.example'}), 403),
                              (Request(self.url + '/api/status', data=b''), 501)]:
            with self.assertRaises(HTTPError) as error:
                urlopen(request)
            self.assertEqual(error.exception.code, code)

    def test_allowlist(self):
        for path in ['https://example.org', '/../x', '/harddisks/other.json']:
            with self.assertRaises(ValueError):
                server.asset(path)


if __name__ == '__main__':
    unittest.main()
