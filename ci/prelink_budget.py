#!/usr/bin/env python3
"""Report the executable window used by the 24A446 fileset emitter."""
import json
from pathlib import Path
import sys
import tomllib


def measure(cache, native, roots):
    from fileset_emit import dependency_closure
    from fileset_layout import align
    from kc_image import Image

    closure = dependency_closure(cache, roots)
    segments = {segment['name']: segment for segment in cache.segments}
    code = segments['__TEXT_EXEC']
    code_start = code['va']
    code_limit = code_start + code['size']
    last_end, last_owner = code_start, None
    for identifier, (_, offset) in cache.entries.items():
        if identifier == 'com.apple.kernel' or (closure is not None and identifier not in closure):
            continue
        for segment in Image(cache.data, offset).segments:
            if not segment['size'] or segment['name'] == '__LINKEDIT':
                continue
            address = segment['va']
            end = address + segment['size']
            if code_start <= address < code_limit and end > last_end:
                last_end, last_owner = end, identifier

    native_segments = {segment['name']: segment for segment in native.segments}
    text_bytes = native_segments['__TEXT_EXEC']['size']
    kld_bytes = native_segments['__KLD']['size']
    capacity = segments['__DATA']['va'] - align(last_end)
    used = align(text_bytes) + kld_bytes
    return {
        'last_retained_code_owner': last_owner,
        'capacity_bytes': capacity,
        'native_text_bytes': text_bytes,
        'native_kld_bytes': kld_bytes,
        'available_after_placement_bytes': capacity - used,
    }


def main():
    core, board, cache_path, native_path = map(Path, sys.argv[1:])
    sys.path.insert(0, str(core / 'kits/XnuPrelink'))
    from kc_image import Image

    config = tomllib.loads(board.read_text())
    roots = config['os_overrides']['ipados-27.0.1-24A446']['kernel']['prelink']['roots']
    budget = measure(Image(cache_path.read_bytes()), Image(native_path.read_bytes()), roots)
    print('24A446 prelink code budget:', json.dumps(budget, sort_keys=True))


if __name__ == '__main__':
    main()
