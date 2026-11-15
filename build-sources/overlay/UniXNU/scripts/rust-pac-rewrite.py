#!/usr/bin/env python3
"""Route the indirect branches of the linked Rust code of an arm64e image through the stripping trampolines.

  rust-pac-rewrite.py IMAGE MAP ARCHIVE_NAME

rustc's arm64e code calls function pointers with a plain blr/br, which faults on the signed pointers the rest of an arm64e process hands
out (drivers/AdrenoMetal/bridge/RustPacBoundary.c). Every blr Xn / br Xn inside a function the link map attributes to an object of
ARCHIVE_NAME becomes bl / b to nmb_pac_call_xN (xpaci; br), in place, before the image is signed. A br x30 uses the same tail trampoline.
A blr x30 instead branches without linking to a per-site trampoline which sets the original return PC."""
import re
import struct
import sys
from pathlib import Path

if len(sys.argv) != 4:
    sys.exit(__doc__)
image, link_map, archive = Path(sys.argv[1]), Path(sys.argv[2]).read_text(errors='replace'), sys.argv[3]
data = bytearray(image.read_bytes())

files, text, symbols, part = set(), None, [], None
for line in link_map.splitlines():
    if line.startswith('# Object files:'): part = 'files'; continue
    if line.startswith('# Sections:'): part = 'sections'; continue
    if line.startswith('# Symbols:'): part = 'symbols'; continue
    if line.startswith('# Dead Stripped'): part = None; continue
    if line.startswith('#') or not line.strip(): continue
    if part == 'files':
        m = re.match(r'\[\s*(\d+)\]\s+(.*)$', line)
        if m and archive in m.group(2): files.add(int(m.group(1)))
    elif part == 'sections':
        f = line.split()
        if len(f) >= 4 and f[2] == '__TEXT' and f[3] == '__text': text = (int(f[0], 16), int(f[1], 16))
    elif part == 'symbols':
        m = re.match(r'(0x[0-9A-Fa-f]+)\s+(0x[0-9A-Fa-f]+)\s+\[\s*(\d+)\]\s+(.*)$', line)
        if m: symbols.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(3)), m.group(4)))
if not files or not text:
    sys.exit('the map names no object of %s or no __TEXT,__text' % archive)
tramp = {}
for address, size, index, name in symbols:
    m = re.fullmatch(r'_nmb_pac_call_x(\d+)', name)
    if m: tramp[int(m.group(1))] = address
if sorted(tramp) != list(range(31)):
    sys.exit('trampolines missing: %s' % sorted(set(range(31)) - set(tramp)))

# Locate the executable reservation by its linked section, never by a gap in __text.
ncmds = struct.unpack_from('<I', data, 16)[0]
at = 32
pool = None
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', data, at)
    if cmd == 0x19 and data[at + 8:at + 24].rstrip(b'\0') == b'__TEXT':
        vmaddr, vmsize, fileoff, filesize, _, initprot, nsects = struct.unpack_from('<4Q3I', data, at + 24)
        for index in range(nsects):
            section = at + 72 + index * 80
            if data[section:section + 16].rstrip(b'\0') == b'__nmb_x30':
                pool = struct.unpack_from('<QQI', data, section + 32)
                addr, length, offset = pool
                if (not initprot & 4 or (addr | offset) & 3 or length % 20
                        or not vmaddr <= addr < addr + length <= vmaddr + vmsize
                        or not fileoff <= offset < offset + length <= min(fileoff + filesize, len(data))
                        or offset - fileoff != addr - vmaddr):
                    sys.exit('invalid executable __nmb_x30 reservation')
        break
    at += size
if pool is None:
    sys.exit('missing executable __nmb_x30 reservation')

pool_addr, pool_size, pool_off = pool
used = 0


def call_x30_trampoline(pc):
    global used
    if used + 20 > pool_size:
        sys.exit('out of x30 call trampolines at %#x' % pc)
    address = pool_addr + used
    offset = pool_off + used
    if struct.unpack_from('<5I', data, offset) != (0xd4200000,) * 5:
        sys.exit('x30 call trampoline slot is not empty at %#x' % address)

    # The incoming x30 is the destination. Use IP0, the linker scratch
    # register, and place the continuation back in x30 before branching.
    resume = pc + 4
    pages = (resume >> 12) - ((address + 4) >> 12)
    if not -(1 << 20) <= pages < (1 << 20):
        sys.exit('x30 call continuation out of ADRP range at %#x' % pc)
    adrp = 0x9000001e | ((pages & 3) << 29) | (((pages >> 2) & 0x7ffff) << 5)
    add = 0x910003de | ((resume & 0xfff) << 10)
    struct.pack_into('<5I', data, offset,
                     0xaa1e03f0, adrp, add, 0xdac143f0, 0xd61f0200)
    used += 20
    return address


lo, hi = text[0], text[0] + text[1]
counts = {'blr': 0, 'br': 0, 'blr_x30': 0, 'br_x30': 0}
for address, size, index, name in symbols:
    if index not in files or not lo <= address < hi:
        continue
    for pc in range(address, min(address + size, hi), 4):
        off = pc - vmaddr + fileoff
        word = struct.unpack_from('<I', data, off)[0]
        kind = {0xD63F0000: 'blr', 0xD61F0000: 'br'}.get(word & 0xFFFFFC1F)
        if not kind:
            continue
        reg = (word >> 5) & 31
        if reg == 31:
            sys.exit('invalid indirect branch register at %#x' % pc)
        link = kind == 'blr' and reg != 30
        if kind == 'blr' and reg == 30:
            target = call_x30_trampoline(pc)
        else:
            target = tramp[reg]
        delta = (target - pc) >> 2
        if not -(1 << 25) <= delta < (1 << 25):
            sys.exit('trampoline out of branch range at %#x' % pc)
        struct.pack_into('<I', data, off, (0x94000000 if link else 0x14000000) | (delta & 0x3FFFFFF))
        counts[kind + '_x30' if reg == 30 else kind] += 1
image.write_bytes(data)
print('PASS: Rust indirect branches routed through PAC trampolines: '
      '%(blr)d blr, %(br)d br, %(blr_x30)d blr x30, %(br_x30)d br x30' % counts)
