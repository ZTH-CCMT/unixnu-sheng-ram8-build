#!/usr/bin/env python3
"""Key the iPadOS GPU addon by its locked inputs and the runner toolchain."""
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess


TOUCH_SOURCES = (
    'device/drivers/ShengNovatekTouch',
    'device/drivers/ShengHidInput',
    'device/package/Touch',
)


def cache_key(lock, toolchain):
    inputs = {
        'base_pins': {path: pin for path, pin in lock['base_pins'].items()
                      if path != 'uefi-sheng' and not path.startswith(TOUCH_SOURCES)},
        'overlay': {path: spec['sha256'] for path, spec in lock['files'].items()
                    if not path.startswith(('uefi-sheng/', *TOUCH_SOURCES))},
        'toolchain': toolchain,
    }
    encoded = json.dumps(inputs, sort_keys=True).encode()
    return 'sheng-ios27-gpu-v1-' + hashlib.sha256(encoded).hexdigest()


def main():
    lock_path = Path(__file__).resolve().parents[1] / 'build-sources/manifest.json'
    lock = json.loads(lock_path.read_bytes())
    toolchain = {
        'arch': platform.machine(),
        'runner_image': os.environ.get('ImageVersion', ''),
        'xcode': subprocess.check_output(['xcodebuild', '-version'], text=True),
        'ios_sdk': subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-build-version'], text=True),
        'rust': subprocess.check_output(['rustc', '+1.98.0', '--version'], text=True),
        'formulae': subprocess.check_output(
            ['brew', 'list', '--versions', 'llvm', 'glslang', 'shaderc', 'spirv-tools'], text=True),
    }
    print(cache_key(lock, toolchain))


if __name__ == '__main__':
    main()
