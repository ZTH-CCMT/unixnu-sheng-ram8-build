#!/usr/bin/env python3
"""Exercise the pinned Adreno downloader without accessing the network."""
from email.message import Message
import hashlib
import io
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

FETCHER = Path(__file__).resolve().parents[1] / (
    'build-sources/overlay/UniXNU/scripts/fetch-adreno-inputs.py'
)
URL = 'https://inputs.example.test/gpu/sqe.bin'
PAYLOAD = b'pinned GPU input'


def http_error(status):
    return HTTPError(URL, status, f'HTTP {status}', Message(), io.BytesIO())


class FetchAdrenoInputsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fetch = staticmethod(runpy.run_path(str(FETCHER))['fetch'])

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.path = Path(temp.name) / 'cache/sqe.bin'
        expected = {'sha256': hashlib.sha256(PAYLOAD).hexdigest(), 'bytes': len(PAYLOAD)}
        self.item = (self.path, URL, expected, None, None)
        self.urlopen = self.enterContext(patch('urllib.request.urlopen'))
        self.sleep = self.enterContext(patch('time.sleep'))

    def test_503_then_success_writes_verified_input(self):
        self.urlopen.side_effect = [http_error(503), io.BytesIO(PAYLOAD)]

        self.assertEqual(self.fetch(self.item), len(PAYLOAD))

        self.assertEqual(self.path.read_bytes(), PAYLOAD)
        self.assertEqual(self.urlopen.call_count, 2)
        self.sleep.assert_called_once()

    def test_404_is_not_retried(self):
        self.urlopen.side_effect = http_error(404)

        with self.assertRaisesRegex(RuntimeError, '404'):
            self.fetch(self.item)

        self.urlopen.assert_called_once()
        self.sleep.assert_not_called()
        self.assertFalse(self.path.exists())

    def test_persistent_503_stops_after_four_attempts(self):
        self.urlopen.side_effect = [http_error(503) for _ in range(4)]

        with self.assertRaisesRegex(RuntimeError, '503') as failure:
            self.fetch(self.item)

        self.assertIn('inputs.example.test', str(failure.exception))
        self.assertIn('sqe.bin', str(failure.exception))
        self.assertEqual(self.urlopen.call_count, 4)
        self.assertEqual(self.sleep.call_count, 3)
        self.assertFalse(self.path.exists())

    def test_successful_response_with_bad_digest_is_rejected(self):
        self.urlopen.return_value = io.BytesIO(b'x' * len(PAYLOAD))

        with self.assertRaisesRegex(ValueError, 'Pinned input hash/size differs: sqe.bin'):
            self.fetch(self.item)

        self.urlopen.assert_called_once()
        self.sleep.assert_not_called()
        self.assertFalse(self.path.exists())


if __name__ == '__main__':
    unittest.main()
