#!/usr/bin/env python3
"""Verify pinned public checkouts and apply the RAM8 source overlay."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

STAGE = Path(__file__).resolve().parents[1]
LOCK = STAGE / 'build-sources/manifest.json'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def git(repo, *args, check=True):
    return subprocess.run(['git', '-C', str(repo), *args], capture_output=True, text=True, check=check)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--top', type=Path, required=True)
    args = parser.parse_args()
    top = args.top.resolve()
    lock = json.loads(LOCK.read_bytes())
    data = lock['data']
    if digest((STAGE / data['path']).read_bytes()) != data['sha256']:
        raise SystemExit('Original generic Data seed does not match its lock')
    for path, commit in lock['base_pins'].items():
        repo = top / path
        if git(repo, 'rev-parse', 'HEAD').stdout.strip() != commit:
            raise SystemExit(f'{path}: expected checkout {commit}')
        if git(repo, 'diff', '--ignore-submodules=all', '--quiet', 'HEAD', '--', check=False).returncode:
            raise SystemExit(f'{path}: tracked files differ before staging')

    writes = []
    for key, spec in lock['files'].items():
        target = top / spec['repository'] / spec['path']
        if target != top / key:
            raise SystemExit(f'{key}: inconsistent source destination')
        content = (STAGE / 'build-sources/overlay' / key).read_bytes()
        if digest(content) != spec['sha256']:
            raise SystemExit(f'{key}: overlay file hash differs')
        before = digest(target.read_bytes()) if target.is_file() else None
        if before != spec['before_sha256']:
            raise SystemExit(f'{key}: clean source file hash differs')
        writes.append((target, content))

    for target, content in writes:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)

    # The author's feeds.update_submodules leaves modified tracked submodules in place.
    expected = {
        spec['repository'] for spec in lock['files'].values()
        if '/' in spec['repository']
    }
    dirty = set()
    for parent in ('UniXNU', 'device'):
        paths = git(top / parent, 'submodule', 'foreach', '--quiet',
                    'git diff --quiet HEAD -- 2>/dev/null || echo "$sm_path"').stdout.split()
        dirty.update(f'{parent}/{path}' for path in paths)
    if not expected <= dirty:
        raise SystemExit(f'Overlay submodules missing from update guard: {sorted(expected - dirty)}')
    for target, content in writes:
        if target.read_bytes() != content:
            raise SystemExit(f'{target.relative_to(top)}: write-back differs')
    receipt = {
        'status': 'verified',
        'source_lock_sha256': digest(LOCK.read_bytes()),
        'seed_sha256': data['sha256'],
        'overlaid_files': len(writes),
        'base_pins': lock['base_pins'],
        'preserved_dirty_submodules': sorted(expected),
    }
    (top / 'materialized-sources.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(f'Applied and verified {len(writes)} source files; protected {len(expected)} submodules.')


if __name__ == '__main__':
    main()
