#!/usr/bin/env python3
"""Fetch only SHA-pinned Adreno source and firmware; never execute firmware."""
import base64
import concurrent.futures
import hashlib
import http.client
import json
import os
from pathlib import Path
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
RETRYABLE_STATUS = {408, 429, 500, 502, 503, 504}


# Anonymous GitHub API calls share a small per-address quota that hosted CI runners exhaust
# (HTTP 403); a token in GITHUB_TOKEN or GH_TOKEN is sent to GitHub hosts only.
TOKEN = os.environ.get('GITHUB_TOKEN') or os.environ.get('GH_TOKEN')


def download(url, headers, name):
    request = urllib.request.Request(url, headers=headers)
    host = urllib.parse.urlsplit(url).hostname
    for delay in (2, 5, 10, None):
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                return response.read()
        except (urllib.error.URLError, TimeoutError, ConnectionError, http.client.IncompleteRead) as error:
            if isinstance(error, urllib.error.HTTPError):
                error.close()
                reason = f'HTTP {error.code}'
                retryable = error.code in RETRYABLE_STATUS
            else:
                reason = str(error)
                retryable = True
            if not retryable or delay is None:
                raise RuntimeError(f'{name}: download from {host} failed ({reason})') from error
            print(f'{name}: {host} download failed ({reason}), retrying in {delay}s',
                  file=sys.stderr, flush=True)
            time.sleep(delay)


def fetch(item):
    path, url, expected, blob, encoding = item
    if path.exists():
        data = path.read_bytes()
    else:
        headers = {'User-Agent': 'sheng-offline-adreno-audit'}
        if TOKEN and urllib.parse.urlsplit(url).hostname in ('api.github.com', 'raw.githubusercontent.com'):
            headers['Authorization'] = f'Bearer {TOKEN}'
        data = download(url, headers, path.name)
        if encoding == 'gitiles-base64':
            data = base64.b64decode(data, validate=False)
    if hashlib.sha256(data).hexdigest() != expected['sha256'] or (expected.get('bytes') and len(data) != expected['bytes']):
        raise ValueError('Pinned input hash/size differs: ' + path.name)
    if blob and hashlib.sha1(('blob ' + str(len(data)) + '\0').encode() + data).hexdigest() != blob:
        raise ValueError('Pinned Git blob identity differs: ' + path.name)
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return len(data)


if __name__ == '__main__':
    pins = json.loads((ROOT / 'scripts/data/adreno-inputs.json').read_text())
    jobs = []
    linux = pins['linux']
    for name, expected in linux['files'].items():
        url = linux['repository'].replace('https://github.com/', 'https://raw.githubusercontent.com/') + '/' + linux['commit'] + '/' + name
        jobs.append((ROOT / '.cache/adreno-source' / name, url, expected, None, None))
    mesa = pins['mesa']
    for name, expected in mesa['files'].items():
        url = mesa['mirror'] + '/+/' + mesa['commit'] + '/' + name + '?format=TEXT'
        jobs.append((ROOT / '.cache/adreno-source/mesa' / name, url, expected, None, 'gitiles-base64'))
    firmware = pins['firmware']
    for name, expected in firmware['files'].items():
        blob = expected['git_blob_sha1']
        # The lock's tree_sha identifies the immutable firmware revision. Raw
        # transport avoids hosted-runner REST quotas; both content hashes below
        # still validate exactly the same firmware bytes as the Git blob route.
        url = firmware['repository'].replace('https://github.com/', 'https://raw.githubusercontent.com/') + '/' + firmware['tree_sha'] + '/' + name
        jobs.append((ROOT / '.cache/firmware/sheng-gpu' / name, url, expected, blob, None))
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        sizes = list(executor.map(fetch, jobs))
    print(f'PASS: {len(sizes)} exact source/firmware inputs, {sum(sizes)} bytes; no GPU execution/authenticity claim')
