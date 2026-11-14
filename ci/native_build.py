#!/usr/bin/env python3
"""Build and verify a generic RAM8 boot update for local personalization."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tomllib
import urllib.request


PROFILE = 'ipados-27.0.1-24A446'
DUMMY_CHIP = '13579bdf'
RAM_BYTES = 8 * 1024**3
DATA_BYTES = 215 * 1024**3
VERSION = 'V5.3-WebKit'
GPU_SOURCE_SHA = 'ee9ba0f8730a22ab83970160e99115de7b42e7a6c4c456081cf1978fe980b7c7'
GPU_BOARD_SHA = {'GpuPowerProbe.cpp': 'c4a82503b75915bccad79598846d92488caaeab93f44521f356aef9fd336c542', 'GpuSqeCapture.h': '0aaa5455e3f992ce53a8d61b1e5cf0bb04834e1e39ca4c6ba74f04ffcdd3f8c4', 'BootLog.c': '642d62a50f9b75b4b5bc7d395027fa838f92837f7f0a6b1e7e86598b200706ac'}
TURNIP_KMD = 'src/freedreno/vulkan/tu_knl_darwin.cc'
TURNIP_KMD_SHA = '28dbfb342333cbfc70da733ad26102ed64ec502ca002f12c0940a7175627df1c'
SEED_SHA = 'e1bfa6971ef9fe6a27b53474c509cd518325459484230daf7f679642a04323b3'
XNU_DEFAULT_CFLAGS = (
    '-Wno-reserved-identifier -Wno-pre-c11-compat -Wno-missing-designated-field-initializers '
    '-Wno-suggest-override -Wno-unnecessary-virtual-specifier '
    '-Wno-suggest-destructor-override -Wno-ms-bitfield-padding'
)
WIFI_BASE = 'https://gitlab.com/kernel-firmware/linux-firmware/-/raw/20261019'
WIFI_PREFIX = 'ath12k/WCN7850/hw2.0/'
WIFI_MANIFEST_SHA = 'f474f2a5a157bc59c804029532231a9a1cee16725018d37212972f11a0868570'
WIFI_NOTICES = {
    'LICENSE.QualcommAtheros_ath10k': ('LICENSES/LICENSE.QualcommAtheros_ath10k',
        2713, '337a55102138d7baa143ee4a4c6c91693e0113fece35d380b2a12109e8c23b3f'),
    'WCN7850-Notice.txt': (WIFI_PREFIX + 'Notice.txt',
        142991, '515bf4c9d620a87458e4447fe01a0e9bc384d1c3e0037cc4c3d2037b1ff25525'),
}


def require(ok, detail):
    if not ok:
        raise SystemExit(detail)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def json_file(path):
    return json.loads(path.read_text())


def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def get_locked(path, source, size, sha):
    if path.exists():
        require(path.stat().st_size == size and digest(path) == sha,
                f'Existing Wi-Fi input differs from lock: {path.name}')
        return
    with urllib.request.urlopen(f'{WIFI_BASE}/{source}', timeout=60) as response:
        content = response.read(size + 1)
    require(len(content) == size and hashlib.sha256(content).hexdigest() == sha,
            f'Wi-Fi input differs from lock: {path.name}')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def prepare_wifi(top, device, env):
    firmware = device / 'firmware/wcn7850'
    manifest = firmware / 'manifest.json'
    require(digest(manifest) == WIFI_MANIFEST_SHA, 'Unexpected WCN7850 firmware manifest')
    files = json_file(manifest)['files']
    require(set(files) == {'amss.bin', 'm3.bin', 'board-2.bin'}, 'Unexpected WCN7850 inputs')
    for name, expected in files.items():
        get_locked(firmware / name, WIFI_PREFIX + name, expected['bytes'], expected['sha256'])
    notices = top / 'wifi-notices'
    for name, (source, size, sha) in WIFI_NOTICES.items():
        get_locked(notices / name, source, size, sha)
    wifi = device / 'drivers/Ath12kWifi'
    sparse = dict(env, GIT_CONFIG_COUNT='1', GIT_CONFIG_KEY_0='core.sparseCheckoutCone',
                  GIT_CONFIG_VALUE_0='false')
    run('bash', wifi / 'tools/fetch-sources.sh', env=sparse)
    for name in ('linux', 'freebsd'):
        run('git', '-C', wifi / 'deps' / name, 'sparse-checkout', 'reapply',
            '--no-cone', '--no-sparse-index', env=sparse)
    return notices


def validate_tools():
    names = ('clang', 'xcodebuild', 'unifdef', 'ninja', 'cmake', 'dtc', 'fdtget',
             'fdtput', 'aea', 'compression_tool', 'hdiutil', 'codesign', 'mformat',
             'mcopy', 'ipsw', 'lld-link', 'llvm-lib', 'llvm-rc', 'llvm-nm', 'llvm-objcopy')
    missing = [name for name in names if shutil.which(name) is None]
    require(not missing, 'Missing build tools: ' + ', '.join(missing))
    for sdk in ('iphoneos', 'macosx'):
        path = run('xcrun', '--sdk', sdk, '--show-sdk-path', capture_output=True, text=True).stdout.strip()
        require(Path(path).is_dir(), f'Missing Xcode {sdk} SDK')


def require_record(path, record):
    require(path.stat().st_size == record['bytes'] and digest(path) == record['sha256'],
            f'Author manifest verification failed: {path.name}')


def fat_readback(image, relative, target):
    run('mcopy', '-i', image, f'::/EFI/UniXNU/{relative}', target,
        env=dict(os.environ, MTOOLS_SKIP_CHECK='1'))


def property_bytes(name, value):
    data = struct.pack('<Q', value)
    return struct.pack('<32sI', name.encode(), len(data)) + data



GPU_EVIDENCE_SYMBOL = '_native_adreno_switch_evidence_copy'


def archived_object(archive, obj):
    members = run('xcrun', 'ar', 't', archive, capture_output=True, text=True).stdout.splitlines()
    require(members.count(obj.name) == 1, f'Expected one {obj.name} in {archive.name}')
    archived = run('xcrun', 'ar', 'p', archive, obj.name, capture_output=True).stdout
    require(archived == obj.read_bytes(), f'Archive member differs from {obj.name}')
    return {'object_sha256': digest(obj), 'archive_sha256': digest(archive)}


def object_symbol(obj, symbol):
    output = run('xcrun', 'nm', '-m', obj, capture_output=True, text=True).stdout
    lines = [line.strip() for line in output.splitlines() if re.search(symbol, line)]
    require(len(lines) == 1, f'Expected one {symbol} in {obj.name}')
    return lines[0]


def gpu_evidence_objects(tree):
    board = tree / 'kits/sheng'
    for name, expected in GPU_BOARD_SHA.items():
        require(digest(board / name) == expected, f'GPU board source differs: {name}')

    adreno = tree / 'build/adreno740'
    submit = adreno / 'NativeAdrenoSubmit.o'
    submit_meta = archived_object(adreno / 'libAdreno740.a', submit)
    definition = object_symbol(submit, rf'\s{GPU_EVIDENCE_SYMBOL}$')
    require('(__TEXT,__text)' in definition and 'external' in definition
            and 'weak' not in definition, 'Adreno evidence copy is not a strong text definition')

    legacy = tree / 'build/native-legacy-display'
    probe = legacy / 'board/GpuPowerProbe.o'
    probe_meta = archived_object(legacy / 'libNativeLegacyDisplay.late.a', probe)
    callback = object_symbol(probe, r'\s\S*boardSqeSnapshot\S*$')
    require('(__TEXT,__text)' in callback, 'GpuPowerProbe lacks the SQE callback definition')

    storage = tree / 'build/sheng-source-storage'
    bootlog = storage / 'BootLog.o'
    bootlog_meta = archived_object(storage / 'libShengSourceStorage.a', bootlog)
    call = object_symbol(bootlog, rf'\s{GPU_EVIDENCE_SYMBOL}$')
    require('(undefined)' in call, 'BootLog does not reference the Adreno evidence copy')
    return {'board_source_sha256': GPU_BOARD_SHA,
            'NativeAdrenoSubmit.o': dict(submit_meta, symbol=definition),
            'GpuPowerProbe.o': dict(probe_meta, symbol=callback),
            'BootLog.o': dict(bootlog_meta, symbol=call)}


def gpu_diagnostic_symbols(tree, kernel):
    sys.path.insert(0, str(tree / 'kits/XnuPrelink'))
    from kc_image import Image, LC_SYMTAB

    raw = kernel.read_bytes()
    fileset = Image(raw)
    image = Image(raw, fileset.entries['com.apple.kernel'][1])
    symbol_command = image.command(LC_SYMTAB)
    require(symbol_command is not None, 'Packaged kernel lacks a symbol table')
    symoff, count, stroff, _ = struct.unpack_from('<4I', raw, symbol_command[0] + 8)
    names = ('_native_adreno_reset_epoch', '_native_adreno_switch_timeout_copy',
             GPU_EVIDENCE_SYMBOL)
    found = {name.encode(): [] for name in names}
    for index in range(count):
        strx, kind, _, desc, address = struct.unpack_from('<IBBHQ', raw, symoff + 16 * index)
        if kind & 0xe0 or kind & 0x0e != 0x0e:
            continue
        start = stroff + strx
        end = raw.find(b'\0', start)
        name = raw[start:end]
        if name in found:
            found[name].append((address, desc))
    addresses = []
    for name in names:
        matches = found[name.encode()]
        require(len(matches) == 1, f'Packaged kernel must define {name} exactly once')
        address, desc = matches[0]
        section = image.section(address)
        require(section and (section['segment'], section['name']) == ('__TEXT_EXEC', '__text'),
                f'{name} is not executable kernel code')
        if name == GPU_EVIDENCE_SYMBOL:
            require((desc & 0x0080) == 0, 'Packaged Adreno evidence copy resolved to the weak fallback')
            instructions = list(struct.unpack('<3I', image.read(address, 12)))
            if instructions[0] in (0xd503241f, 0xd503245f, 0xd503249f, 0xd50324df):
                instructions.pop(0)
            require(instructions[:2] not in ([0x52800000, 0xd65f03c0], [0x2a1f03e0, 0xd65f03c0]),
                    'Packaged Adreno evidence copy is a return-zero stub')
        image.offset(address, 4)
        addresses.append(hex(address))
    return addresses


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--top', type=Path, required=True)
    top = parser.parse_args().top.resolve()
    require(sys.platform == 'darwin', 'This build requires a macOS runner')
    validate_tools()
    core, device, uefi = (top / name for name in ('UniXNU', 'device', 'uefi-sheng'))
    seed = top / 'pipeline/data/data.dbr'
    require(digest(seed) == SEED_SHA, 'Preserved generic seed SHA-256 mismatch')
    with seed.open('rb') as stream:
        magic, version, block, logical, _ = struct.unpack('<8sIIQQ', stream.read(32))
    require((magic, version, block, logical) == (b'DBRDISK1', 1, 4096, DATA_BYTES),
            'Preserved generic seed logical layout differs')
    board = tomllib.loads((device / 'board.toml').read_text())
    require(board['board']['os_profile'] == PROFILE, 'Unexpected OS profile')
    require(int(str(board['payload']['adt']['owned_memory_size']), 0) == RAM_BYTES,
            'iPadOS 27 memory selection differs from 8 GiB')
    spec = board['payload']['runtime']
    require(spec['data_gib'] == 215 and spec['wifi_launchd_cache'] and spec['appsync']
            and spec['bluetooth_tool'], 'Required author runtime features differ')

    sys.path.insert(0, str(core / 'scripts'))
    from unixnu.runtime import seed_uuid
    expected_uuid = seed_uuid(core, seed)
    env = dict(os.environ, UNIXNU_UNIQUE_CHIP_ID=DUMMY_CHIP)
    # The retained 24A446 fileset has a fixed executable window for XNU.
    cflags = env.get('XNU_EXTRA_CFLAGS') or XNU_DEFAULT_CFLAGS
    env['XNU_EXTRA_CFLAGS'] = f'{cflags} -Os'
    print('Native iPadOS XNU optimization: -Os', flush=True)
    env.pop('UNIXNU_WIFI_JOIN', None)
    receipt = top / 'materialized-sources.json'
    require(json_file(receipt)['seed_sha256'] == SEED_SHA, 'Source staging seed identity differs')
    print(f'Available before Wi-Fi preparation: {shutil.disk_usage(top).free / 1024**3:.1f} GiB', flush=True)
    notices = prepare_wifi(top, device, env)
    print(f'Available before native package: {shutil.disk_usage(top).free / 1024**3:.1f} GiB', flush=True)
    run('bash', device / 'scripts/package.sh', '--os', '27', '--uefi', uefi,
        '--local-module', f'soc:sm8550={top / "UniXNU-sm8550-common"}',
        '--data-ramdisk', seed, cwd=device, env=env)

    config_name = board['package']['defconfig'][PROFILE].removesuffix('_defconfig')
    print(f'Available after native package: {shutil.disk_usage(top).free / 1024**3:.1f} GiB', flush=True)
    work = core / 'out/sheng' / PROFILE / config_name
    tree = work / 'tree'
    runtime = work / 'runtime'
    boot = work / 'boot/boot.img'
    payload = work / 'payload/payload.img'
    kernel = work / 'kernel/kernel.macho'
    manifest = work / 'package.manifest.json'
    records = json_file(manifest)
    require(records['os_profile'] == PROFILE, 'Author manifest selected another OS')
    for name, path in (('boot.img', boot), ('payload.img', payload), ('kernel.macho', kernel)):
        require_record(path, records['outputs'][name])
    boot_report = work / 'boot/boot.json'
    require(json_file(boot_report)['image_sha256'] == records['outputs']['boot.img']['sha256'],
            'Author boot readback digest differs')
    with kernel.open('rb') as stream:
        require(struct.unpack('<II', stream.read(8)) == (0xfeedfacf, 0x100000c),
                'Kernel output is not an ARM64 Mach-O')

    inputs = json_file(runtime / 'inputs.json')
    require(inputs['data_volume_uuid'].upper() == expected_uuid, 'Runtime Data volume UUID differs')
    config = work / '.config'
    require(f'CONFIG_XNU_OWNER_DATA_VOLUME_UUID="{expected_uuid}"' in config.read_text().splitlines(),
            'Compiled Data UUID differs')
    runtime_files = {}
    for key in ('data_ramdisk', 'trust_cache', 'gpu_addon'):
        item = inputs['inputs'][key]
        path = Path(item['path'])
        require(digest(path) == item['sha256'], f'Runtime input digest differs: {key}')
        runtime_files[key] = path
    require(digest(runtime_files['data_ramdisk']) == SEED_SHA, 'Runtime repacked the original Data seed')

    turnip = json_file(tree / 'build/mesa-tu-ios27/builtin-build-profile.json')
    plugin = json_file(tree / 'build/adreno-metal-plugin27/NativeAdrenoMetal.bundle/gpu-build.json')
    require(plugin['target'] == 'ios27' and plugin['turnip'] == turnip
            and turnip['profile']['extra_sources'][TURNIP_KMD] == TURNIP_KMD_SHA,
            'GPU addon did not use the V4.1 Turnip source')
    print(f'Turnip ios27 source profile {turnip["source_id"]}', flush=True)

    gpu_trace_path = tree / 'build/adreno740/gpu-trace-build.json'
    gpu_trace = json_file(gpu_trace_path)
    require(gpu_trace['source_sha256'] == GPU_SOURCE_SHA
            and gpu_trace['trace_source_id'] == GPU_SOURCE_SHA[:16]
            and gpu_trace['target'] == 'arm64e-apple-ios27.0',
            'Compiled Adreno source identity differs from the V5.3 candidate')
    print(f'Adreno V5.3 source ID {gpu_trace["trace_source_id"]}', flush=True)

    stage = top / 'os27-ram8-handoff.partial'
    stage.mkdir()
    adt = stage / 'kernel.macho.adt'
    fat_readback(payload, 'Payloads/kernel.macho.adt', adt)
    adt_bytes = adt.read_bytes()
    require(adt_bytes.count(property_bytes('darwinboot-memory-size', RAM_BYTES)) == 1,
            'Packed ADT does not contain the 8 GiB memory request')
    require(adt_bytes.count(property_bytes('unique-chip-id', int(DUMMY_CHIP, 16))) == 1,
            'Packed ADT does not contain the dummy identity')
    payload_report = work / 'payload/payload.json'
    payload_info = json_file(payload_report)
    require(digest(adt) == payload_info['adt_sha256'], 'Packed ADT digest differs')
    payload_kernel = stage / 'payload-kernel-check.macho'
    fat_readback(payload, 'Payloads/kernel.macho', payload_kernel)
    require(digest(payload_kernel) == payload_info['payload_kernel_sha256'], 'Packed kernel digest differs')
    gpu_objects = gpu_evidence_objects(tree)
    gpu_reset_address, gpu_timeout_address, gpu_evidence_address = gpu_diagnostic_symbols(tree, payload_kernel)
    gpu_link_path = stage / 'gpu-sqe-link-evidence.json'
    gpu_objects.update(status='VERIFIED_STRONG_PACKAGED_GPU_SQE_COPY',
                       packaged_symbol=gpu_evidence_address,
                       packaged_kernel_sha256=digest(payload_kernel))
    gpu_link_path.write_text(json.dumps(gpu_objects, indent=2) + '\n')
    print(f'GPU SQE evidence copy linked strongly at {gpu_evidence_address}', flush=True)
    print(f'GPU reset generation linked in packaged kernel at {gpu_reset_address}', flush=True)
    payload_kernel.unlink()
    for label, member in (('gpu_addon', 'kernel.macho.gpuaddon'),
                          ('trust_cache', 'kernel.macho.trustcache')):
        packed = stage / 'gpu-runtime-check.bin'
        fat_readback(payload, f'Payloads/{member}', packed)
        require(digest(packed) == digest(runtime_files[label]), f'Packed {label} differs')
        packed.unlink()
    packed_seed = stage / 'packed-seed-check.dbr'
    fat_readback(payload, 'Payloads/scratch-data.img', packed_seed)
    require(digest(packed_seed) == SEED_SHA, 'Packed Data seed differs')
    packed_seed.unlink()
    touch = tree / 'build/native-legacy-display/touch-link-evidence.json'
    touch_sources = tree / 'build/sheng-source-drivers/report.json'
    evidence = json_file(touch)
    require(evidence['status'] == 'COMPILED_OBJECTS_IN_ACTUAL_LATE_ARCHIVE'
            and evidence['source_report_sha256'] == digest(touch_sources), 'Touch compilation/late archive evidence differs')
    files = {
        'provisional/DO-NOT-BOOT-boot.img': boot,
        'provisional/DO-NOT-BOOT-payload.img': payload,
        'kernel/kernel.macho': kernel,
        '.config': config,
        'source-staging-receipt.json': receipt,
        'author-package.manifest.json': manifest,
        'author-boot-report.json': boot_report,
        'author-payload-report.json': payload_report,
        'gpu-trace-build.json': gpu_trace_path,
        'touch-link-evidence.json': touch,
        'touch-source-report.json': touch_sources,
        'data.dbr': runtime_files['data_ramdisk'],
        'trust-cache.bin': runtime_files['trust_cache'],
        'gpu-addon.bin': runtime_files['gpu_addon'],
        'root-policies.bin': tree / 'build/sheng-source-storage/root-policies.bin',
        'SILICIUM_UEFI.fd': uefi / board['firmware']['fd'].format(target='DEBUG'),
        'sheng.dtb': tree / board['bootimg']['dtb'],
        **{name: runtime / name for name in ('wifi-launchd-cache.json', 'bluetooth-service.json', 'appsync-service.json')},
        **{f'LICENSES/{name}': notices / name for name in WIFI_NOTICES},
    }
    for name, path in files.items():
        target = stage / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
    handoff = {
        'status': 'PROVISIONAL_DO_NOT_BOOT_OR_FLASH',
        'profile': PROFILE, 'synthetic_platform_seed': DUMMY_CHIP,
        'candidate_version': VERSION, 'gpu_reset_epoch_symbol': gpu_reset_address,
        'gpu_switch_timeout_copy_symbol': gpu_timeout_address,
        'gpu_switch_evidence_copy_symbol': gpu_evidence_address,
        'gpu_sqe_link_evidence_sha256': digest(gpu_link_path),
        'gpu_board_source_sha256': GPU_BOARD_SHA,
        'gpu_source_sha256': GPU_SOURCE_SHA,
        'gpu_trace_source_id': gpu_trace['trace_source_id'],
        'turnip_source_id': turnip['source_id'], 'turnip_kmd_sha256': TURNIP_KMD_SHA,
        'personalization_required': True, 'hardware_tested': False,
        'memory_request_bytes': RAM_BYTES, 'data_logical_bytes': DATA_BYTES,
        'data_volume_uuid': expected_uuid,
        'files': {p.relative_to(stage).as_posix(): {'bytes': p.stat().st_size, 'sha256': digest(p)}
                  for p in sorted(stage.rglob('*')) if p.is_file()},
    }
    (stage / 'handoff.json').write_text(json.dumps(handoff, indent=2) + '\n')
    stage.rename(top / 'os27-ram8-handoff')
    print('Verified generic 8 GiB boot handoff. Personalize locally before any boot attempt.')


if __name__ == '__main__':
    main()
