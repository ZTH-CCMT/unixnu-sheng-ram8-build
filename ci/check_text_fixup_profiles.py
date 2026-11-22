#!/usr/bin/env python3
"""Check OS-specific text fixups and the single-copy audio configuration."""
import argparse
from pathlib import Path
import sys
from types import SimpleNamespace
from unittest.mock import patch


SINGLE_COPY_OS = 'ipados-27.0.1-24A446'


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
    if not (root / 'profiles/os' / SINGLE_COPY_OS / 'profile.toml').is_file():
        raise SystemExit(f'Single-copy audio profile is missing: {SINGLE_COPY_OS}')

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
            effective = profiles.load(ctx)
        if fixups(effective) != expected:
            raise SystemExit(f'{profile.name}: compiled text fixup list differs from its OS config')
        payload = effective.get('payload', {})
        runtime = payload.get('runtime', {})
        enabled = 'audio-single-copy=1' in payload.get('kernel_args', [])
        if enabled != (profile.name == SINGLE_COPY_OS):
            raise SystemExit(f'{profile.name}: single-copy audio boot flag is out of scope')
        if enabled and (runtime.get('audio_addon_directory') != 'audio24A446'
                        or runtime.get('audio_system_plugins')
                        or any(fixup.startswith('BTAudioHALPlugin,') for fixup in expected)):
            raise SystemExit(f'{profile.name}: audio runtime and sealed plug-in policy disagree')
        print(f'{profile.name}: {len(expected)} text fixups, single-copy audio={enabled}')


if __name__ == '__main__':
    main()
