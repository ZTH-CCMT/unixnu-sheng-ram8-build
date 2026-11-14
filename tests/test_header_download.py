"""Exercise the actual Apple-header downloader with no REST API dependency.

Optional integration inputs:
  UNIXNU_HEADER_CORPUS: verified raw headers under driverkit/ and trustcache/
  UNIXNU_HEADER_BASELINE_REPO: original UniXNU Git checkout
No network access is permitted by these tests.
"""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'build-sources/overlay/UniXNU/scripts/prepare-xnu-source.py'
INDEX = SCRIPT.parent / 'data/xnu-restored-header-inputs.json'
BASELINE_COMMIT = 'db32481cd5f17c98244a1f96eb00e41753e09987'


def blob(data):
    return hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()


class HeaderDownloadTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='.header-test-', dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.index = json.loads(INDEX.read_text(encoding='utf-8'))
        self.payloads = {}
        for kind, source in self.index['sources'].items():
            for name in source['files']:
                data = ('/* synthetic header fixture: ' + kind + '/' + name + ' */\n').encode()
                self.payloads[kind, name] = data
                source['files'][name] = blob(data)

    def execute(self, source_text=None, *, existing=None, corrupt=None,
                forbid_api=True, tags=None, work_name='candidate'):
        work = self.work / work_name
        scripts = work / 'scripts'
        (scripts / 'data').mkdir(parents=True)
        (scripts / 'data/xnu-restored-header-inputs.json').write_text(
            json.dumps(self.index), encoding='utf-8')
        destination = work / '.cache/xnu-work'
        (destination / 'iokit/DriverKit').mkdir(parents=True)
        for path, data in (existing or {}).items():
            target = destination / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        requests = []
        raw = {}
        catalogs = {}
        for kind, source in self.index['sources'].items():
            groups = {}
            for name, sha in source['files'].items():
                payload = self.payloads[kind, name]
                for revision in (source['tag'], source['commit']):
                    url = (self.index['repository'].replace('https://github.com/',
                           'https://raw.githubusercontent.com/') + '/' + revision + '/'
                           + source['directory'] + '/' + name)
                    raw[url] = b'corrupted' if (kind, name) == corrupt else payload
                folder, _, basename = name.rpartition('/')
                url = (self.index['repository'].replace('https://github.com/',
                       'https://raw.githubusercontent.com/') + '/' + source['tag'] + '/'
                       + source['directory'] + '/' + name)
                groups.setdefault(folder, []).append({
                    'type': 'file', 'name': basename, 'sha': sha, 'download_url': url})
            for folder, entries in groups.items():
                path = source['directory'] + ('/' + folder if folder else '')
                api = ('https://api.github.com/repos/apple-oss-distributions/xnu/contents/'
                       + path + '?ref=' + source['tag'])
                catalogs[api] = json.dumps(entries).encode()

        def urlopen(request, timeout=None):
            url = request.full_url
            requests.append(url)
            if urlsplit(url).hostname == 'api.github.com':
                if forbid_api:
                    raise AssertionError('REST API must not be called: ' + url)
                return io.BytesIO(catalogs[url])
            if url not in raw:
                raise AssertionError('Unexpected request: ' + url)
            return io.BytesIO(raw[url])

        environment = {'GITHUB_TOKEN': '', 'GH_TOKEN': '',
            'UNIXNU_DRIVERKIT_TAG': self.index['sources']['driverkit']['tag'],
            'UNIXNU_TRUSTCACHE_TAG': self.index['sources']['trustcache']['tag']}
        environment.update(tags or {})
        code = source_text if source_text is not None else SCRIPT.read_text(encoding='utf-8')
        namespace = {'__name__': '__main__', '__file__': str(scripts / SCRIPT.name)}
        with mock.patch.dict(os.environ, environment), mock.patch('urllib.request.urlopen', urlopen), \
             contextlib.chdir(work), contextlib.redirect_stdout(io.StringIO()):
            exec(compile(code, str(SCRIPT), 'exec'), namespace)
        files = {p.relative_to(destination).as_posix(): p.read_bytes()
                 for p in destination.rglob('*') if p.is_file()}
        receipt = json.loads((work / '.cache/sources/xnu-restored-headers.json').read_text())
        return files, receipt, requests

    def test_complete_main_restores_headers_without_api_or_token(self):
        files, receipt, requests = self.execute()
        self.assertEqual(len(files), sum(len(s['files']) for s in self.index['sources'].values()))
        self.assertEqual(len(requests), len(files))
        for kind, source in self.index['sources'].items():
            for name, expected in source['files'].items():
                data = files[source['directory'] + '/' + name]
                self.assertEqual(blob(data), expected)
                record = next(r for r in receipt if r['name'] == name)
                self.assertEqual(record['sha256'], hashlib.sha256(data).hexdigest())
                self.assertEqual(record['git_blob'], expected)
        self.assertIn('iokit/DriverKit/crypto/aes.h', files)
        self.assertIn('EXTERNAL_HEADERS/TrustCache/API.h', files)
        self.assertTrue(all(urlsplit(u).hostname == 'raw.githubusercontent.com' for u in requests))
        self.assertTrue(all('/xnu-' not in u for u in requests))

    def test_existing_worktree_header_is_preserved(self):
        preserved = b'/* current XNU header, must not be replaced */\n'
        path = 'iokit/DriverKit/IOTypes.h'
        files, receipt, requests = self.execute(existing={path: preserved})
        self.assertEqual(files[path], preserved)
        self.assertFalse(any(u.endswith('/IOTypes.h') for u in requests))
        self.assertFalse(any(r['name'] == 'IOTypes.h' for r in receipt))

    def test_corrupt_download_is_rejected_before_write(self):
        with self.assertRaisesRegex(ValueError, 'Git blob hash mismatch'):
            self.execute(corrupt=('driverkit', 'crypto/aes.h'))
        self.assertFalse((self.work / 'candidate/.cache/xnu-work/iokit/DriverKit/crypto/aes.h').exists())

    def test_bootstrap_uses_ipados27_profile_header_tag(self):
        tag = 'xnu-12377.121.6'
        source = self.index['sources']['driverkit']
        selected = source['alternate'][tag]
        self.assertEqual(selected['commit'], 'ac9718fb1af618d5ce8678d0dc6e8a58f252216f')
        self.assertEqual(len(selected['files']), 21)
        self.assertNotEqual(selected['files']['IOTypes.h'],
                            json.loads(INDEX.read_bytes())['sources']['driverkit']['files']['IOTypes.h'])
        path = 'iokit/DriverKit/IOTypes.h'
        content = b'/* fixture for the newer DriverKit IOTypes */\n'
        other_headers = {
            'iokit/DriverKit/' + name: self.payloads['driverkit', name]
            for name in source['files'] if name != 'IOTypes.h'
        }
        selected['files']['IOTypes.h'] = blob(content)
        source['files']['IOTypes.h'] = blob(content)
        self.payloads['driverkit', 'IOTypes.h'] = content
        source['commit'] = selected['commit']
        files, receipt, requests = self.execute(
            existing=other_headers, tags={'UNIXNU_DRIVERKIT_TAG': tag})
        self.assertEqual(files[path], content)
        driverkit = [url for url in requests if '/iokit/DriverKit/' in url]
        self.assertEqual(driverkit, [
            'https://raw.githubusercontent.com/apple-oss-distributions/xnu/'
            + selected['commit'] + '/' + path])
        self.assertEqual(next(r['source_tag'] for r in receipt if r['name']=='IOTypes.h'), tag)

    def test_unpinned_tag_fails_before_download(self):
        with self.assertRaisesRegex(ValueError, 'Unpinned Apple header tag'):
            self.execute(tags={'UNIXNU_DRIVERKIT_TAG': 'xnu-unreviewed'})
        self.assertEqual(list((self.work / 'candidate/.cache/xnu-work').rglob('*.h')), [])

    @unittest.skipUnless(os.environ.get('UNIXNU_HEADER_CORPUS') and
                         os.environ.get('UNIXNU_HEADER_BASELINE_REPO'),
                         'optional verified real-header corpus and original Git source not supplied')
    def test_real_headers_match_original_downloader_outputs(self):
        corpus = Path(os.environ['UNIXNU_HEADER_CORPUS'])
        self.index = json.loads(INDEX.read_text(encoding='utf-8'))
        for kind, source in self.index['sources'].items():
            for name, expected in source['files'].items():
                data = (corpus / kind / name).read_bytes()
                self.assertEqual(blob(data), expected)
                self.payloads[kind, name] = data
        original = subprocess.check_output(['git', '-C', os.environ['UNIXNU_HEADER_BASELINE_REPO'],
            'show', BASELINE_COMMIT + ':scripts/prepare-xnu-source.py']).decode('utf-8')
        before, before_receipt, before_requests = self.execute(original, forbid_api=False, work_name='original')
        after, after_receipt, after_requests = self.execute(work_name='updated')
        self.assertEqual(before, after)
        self.assertEqual(sorted(before_receipt, key=lambda r:r['name']),
                         sorted(after_receipt, key=lambda r:r['name']))
        self.assertEqual(sum(urlsplit(u).hostname == 'api.github.com' for u in before_requests), 3)
        self.assertEqual(sum(urlsplit(u).hostname == 'api.github.com' for u in after_requests), 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
