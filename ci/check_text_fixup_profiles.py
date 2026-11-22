#!/usr/bin/env python3
"""Make sure board text fixups are isolated to their OS builds."""
import argparse
from pathlib import Path
import sys
from types import SimpleNamespace
from unittest.mock import patch


def fixups(profile):
    return profile.get('payload', {}).get('text_fixups', [])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--top', type=Path, required=True)
    parser.add_argument('--board', type=Path)
    args = parser.parse_args()
    root = args.top / 'UniXNU'
    board_path = args.board or args.top / 'device' / 'board.toml'
    sys.path.insert(0, str(root / 'scripts'))
    from unixnu import feeds, profiles
    from unixnu.config import load_toml

    board = load_toml(board_path)
    if 'text_fixups' in board.get('payload', {}):
        raise SystemExit('Board text fixups must select an exact OS build in os_overrides')

    soc = SimpleNamespace(path=args.top / 'UniXNU-sm8550-common')
    for profile in sorted((root / 'profiles/os').iterdir()):
        path = profile / 'profile.toml'
        if not path.is_file():
            continue
        expected = fixups(load_toml(path))
        override = board.get('os_overrides', {}).get(profile.name, {})
        expected = override.get('payload', {}).get('text_fixups', expected)
        ctx = SimpleNamespace(root=root, device=board_path.parent, raw_board=board,
                              args=SimpleNamespace(os=profile.name))
        with patch.object(feeds, 'resolve_soc', return_value=soc):
            effective = fixups(profiles.load(ctx))
        if effective != expected:
            raise SystemExit(f'{profile.name}: compiled text fixup list differs from its OS config')
        print(f'{profile.name}: {len(effective)} text fixups')


if __name__ == '__main__':
    main()
