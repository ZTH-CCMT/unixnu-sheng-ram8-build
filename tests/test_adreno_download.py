"""Regression checks for raw, hash-pinned firmware transport (no network)."""
import base64
import copy
import hashlib
import io
import json
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'build-sources/overlay/UniXNU/scripts/fetch-adreno-inputs.py'
PAYLOAD = b'\x00firmware\xff\x10'
BLOB = hashlib.sha1(b'blob ' + str(len(PAYLOAD)).encode() + b'\0' + PAYLOAD).hexdigest()
EXPECTED = {'bytes': len(PAYLOAD), 'sha256': hashlib.sha256(PAYLOAD).hexdigest(),
            'git_blob_sha1': BLOB}
PINS = {
    'linux': {'repository': 'https://github.com/example/linux', 'commit': 'a' * 40,
              'files': {'source.c': EXPECTED}},
    'mesa': {'mirror': 'https://chromium.googlesource.com/mesa', 'commit': 'b' * 40,
             'files': {'source.h': EXPECTED}},
    'firmware': {'repository': 'https://github.com/ianchb/sheng-firmware',
                 'tree_sha': '2c1e2729a085c7f0470c855d236e49455c4601f0',
                 'files': {'qcom/a740_sqe.fw': EXPECTED}},
}


def load(name='adreno_test'):
    original_read = Path.read_text
    def read(path, *args, **kwargs):
        if path == SCRIPT.parent / 'data/adreno-inputs.json':
            return json.dumps(PINS)
        return original_read(path, *args, **kwargs)
    with patch.object(Path, 'read_text', read), patch.dict('os.environ', {}, clear=True):
        return runpy.run_path(str(SCRIPT), run_name=name)


class AdrenoDownloadTests(unittest.TestCase):
    def setUp(self):
        self.work = ROOT / '.test-work'
        self.work.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='adreno-', dir=self.work)
        self.directory = Path(self.temp.name).resolve()
        assert self.directory.is_relative_to(self.work.resolve())
        self.path = self.directory / 'qcom/a740_sqe.fw'
        self.fetch = load()['fetch']
        self.item = (self.path, 'https://raw.githubusercontent.com/ianchb/sheng-firmware/'
                     + PINS['firmware']['tree_sha'] + '/qcom/a740_sqe.fw',
                     copy.deepcopy(EXPECTED), BLOB, None)

    def tearDown(self):
        self.temp.cleanup()

    def test_raw_firmware_not_json_decoded(self):
        with patch('urllib.request.urlopen', return_value=io.BytesIO(PAYLOAD)) as opening:
            self.assertEqual(self.fetch(self.item), len(PAYLOAD))
        self.assertEqual(self.path.read_bytes(), PAYLOAD)
        self.assertNotIn('api.github.com', opening.call_args.args[0].full_url)

    def test_existing_valid_cache_avoids_network(self):
        self.path.parent.mkdir()
        self.path.write_bytes(PAYLOAD)
        with patch('urllib.request.urlopen', side_effect=AssertionError('network')):
            self.assertEqual(self.fetch(self.item), len(PAYLOAD))

    def test_sha256_mismatch_rejected_without_cache_write(self):
        with patch('urllib.request.urlopen', return_value=io.BytesIO(b'bad')):
            with self.assertRaisesRegex(ValueError, 'hash/size'):
                self.fetch(self.item)
        self.assertFalse(self.path.exists())

    def test_size_mismatch_rejected(self):
        self.item[2]['bytes'] += 1
        with patch('urllib.request.urlopen', return_value=io.BytesIO(PAYLOAD)):
            with self.assertRaisesRegex(ValueError, 'hash/size'):
                self.fetch(self.item)
        self.assertFalse(self.path.exists())

    def test_git_blob_mismatch_rejected(self):
        item = self.item[:3] + ('0' * 40, None)
        with patch('urllib.request.urlopen', return_value=io.BytesIO(PAYLOAD)):
            with self.assertRaisesRegex(ValueError, 'Git blob identity'):
                self.fetch(item)
        self.assertFalse(self.path.exists())

    def test_corrupt_existing_cache_rejected(self):
        self.path.parent.mkdir()
        self.path.write_bytes(b'bad')
        with patch('urllib.request.urlopen', side_effect=AssertionError('network')):
            with self.assertRaisesRegex(ValueError, 'hash/size'):
                self.fetch(self.item)

    def test_gitiles_source_decoding_unchanged(self):
        item = self.item[:3] + (None, 'gitiles-base64')
        with patch('urllib.request.urlopen', return_value=io.BytesIO(base64.b64encode(PAYLOAD))):
            self.assertEqual(self.fetch(item), len(PAYLOAD))
        self.assertEqual(self.path.read_bytes(), PAYLOAD)

    def test_main_builds_immutable_firmware_raw_url_with_blob_check(self):
        captured = []
        class Executor:
            def __init__(self, **kwargs): pass
            def __enter__(self): return self
            def __exit__(self, *args): pass
            def map(self, func, jobs):
                captured.extend(jobs)
                return [job[2]['bytes'] for job in jobs]
        with patch('concurrent.futures.ThreadPoolExecutor', Executor):
            load('__main__')
        firmware = captured[-1]
        self.assertEqual(firmware[1], self.item[1])
        self.assertEqual(firmware[3], BLOB)
        self.assertIsNone(firmware[4])
        self.assertFalse(any('api.github.com' in job[1] for job in captured))


if __name__ == '__main__':
    unittest.main()
