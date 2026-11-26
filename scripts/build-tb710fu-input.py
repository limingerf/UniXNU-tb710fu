#!/usr/bin/env python3
"""Build the TB710FU input archive (Novatek NT36532 touch over GENI SPI + PMIC keys).

Same schema as the board's other archives: the C transport/format layer and the
single IOKit translation unit are compiled with the actual XNU compile records
(arm64e-apple-ios27.0), the objects are PAC/vtable checked with the framework's
kernel_target helper, and the XNU link target is invalidated when the archive
changes. Nothing here publishes a service unless the firmware passes
tb710fu-touch=1..3 / tb710fu-keys=1, so the default boot path is unchanged.

The IOHIDDevice vtable this port adopts is read out of this board's prelinked
kernel, not guessed: --emit-hid-slots regenerates
package/Tb710fuInput/Tb710fuInputHidSlots.h from the pinned
.local/prelink-kernel.macho, re-asserts the named slots by hashing their mangled
method names, and refuses to write a table whose count or names moved.

Host-side source checks (GENI framing, NT36532 parsing, tracking, HID reports,
key state machine) run separately: scripts/check-tb710fu-input-host.py.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DEVICE = Path(os.environ['UNIXNU_DEVICE']).resolve() if os.environ.get('UNIXNU_DEVICE') else ROOT
PACKAGE = DEVICE / 'package/Tb710fuInput'
OUT = ROOT / 'build/tb710fu-input'

RECORD_C = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
RECORD_CPP = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
TARGET = 'arm64e-apple-ios27.0'

C_SOURCES = [PACKAGE / name for name in (
    'Tb710fuInputConfig.c',
    'Tb710fuGeni.c',
    'Tb710fuNvt.c',
    'Tb710fuNvtFirmware.c',
    'Tb710fuTracker.c',
    'Tb710fuHid.c',
    'Tb710fuSpmi.c',
    'Tb710fuKeys.c',
)]
CPP_SOURCES = [PACKAGE / 'Tb710fuInputProbe.cpp']
HEADERS = [PACKAGE / name for name in (
    'Tb710fuInputHardware.h', 'Tb710fuInputConfig.h', 'Tb710fuInputHidSlots.h',
    'Tb710fuInput.h', 'Tb710fuMem.h', 'Tb710fuGeni.h', 'Tb710fuNvt.h',
    'Tb710fuNvtFirmware.h', 'Tb710fuTracker.h', 'Tb710fuHid.h', 'Tb710fuSpmi.h',
    'Tb710fuKeys.h',
)]

# The table is generated from this image; check-tb710fu-input-host.py re-proves it.
KERNEL_IMAGE = DEVICE / '.local/prelink-kernel.macho'
HID_VTABLE = 0xfffffff0085ef6d0
HID_SLOTS_HEADER = PACKAGE / 'Tb710fuInputHidSlots.h'

# Mangled names of the slots the probe relies on and their expected slot index.
# A method that overrides an inherited virtual keeps the base class's mangling
# (call sites are typed by the base): start/stop hash as IOService, while
# newReportDescriptor and handleReport are IOHIDDevice's own virtuals.
NAMED_SLOTS = (
    ('GetMetaClass', 7, None),  # clang pins getMetaClass to diversity 0xfffc
    ('Start', 190, '_ZN9IOService5startEPS_'),
    ('Stop', 191, '_ZN9IOService4stopEPS_'),
    ('NewReportDescriptor', 284, '_ZNK11IOHIDDevice19newReportDescriptorEPP18IOMemoryDescriptor'),
    ('HandleReport', 285, '_ZN11IOHIDDevice12handleReportEP18IOMemoryDescriptor15IOHIDReportTypej'),
)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def reported(path):
    for base in (DEVICE, ROOT):
        try:
            return str(Path(path).relative_to(base))
        except ValueError:
            pass
    return str(path)


def helper(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def compile_command(record, source, output, includes, extra):
    arguments = record['arguments']
    command = []
    index = 0
    replaced = False
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
            replaced = True
        elif value == '-o':
            command.extend(['-o', str(output)])
            index += 1
        elif value in ('-MF', '-MT', '-MQ', '-MJ'):
            index += 1
        elif value in ('-MD', '-MMD', '-MP'):
            pass
        elif value == '-Xclang' and index + 1 < len(arguments) and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    if not replaced:
        raise SystemExit(f'{record["file"]}: compile record does not name its source')
    command = ['--target=' + TARGET if value.startswith('--target=') else value for value in command]
    command += ['-Wall', '-Wextra', '-Werror', '-Wno-overriding-option']
    for include in includes:
        command += ['-I', str(include)]
    command += extra
    return command


def defer_constructor(obj, late, init, required=True):
    """Move the C++ static initialiser section to __mod_deferred.

    Left in __mod_init_func it would run at kernel entry, before IOKit exists.
    This translation unit deliberately has only trivially constructed globals,
    so `required=False` accepts an object without a constructor at all."""
    data = bytearray(obj.read_bytes())
    if struct.unpack_from('<I', data)[0] != 0xfeedfacf:
        raise SystemExit(f'{obj}: not a 64-bit Mach-O object')
    count = struct.unpack_from('<I', data, 16)[0]
    symtab, renamed, offset = None, 0, 32
    for _ in range(count):
        command, size = struct.unpack_from('<II', data, offset)
        if command == 0x19:
            sections = struct.unpack_from('<I', data, offset + 64)[0]
            for index in range(sections):
                section = offset + 72 + index * 80
                if data[section:section + 16].rstrip(b'\0') == b'__mod_init_func':
                    data[section:section + 16] = b'__mod_deferred\0\0'
                    flags = struct.unpack_from('<I', data, section + 64)[0]
                    struct.pack_into('<I', data, section + 64, flags & ~0xff)
                    renamed += 1
        elif command == 0x2:
            symtab = struct.unpack_from('<IIII', data, offset + 8)
        offset += size
    if not renamed:
        if not required:
            return False
        raise SystemExit(f'{obj}: expected eager metaclass constructor section missing')
    if symtab is None:
        raise SystemExit(f'{obj}: no symbol table')
    symoff, nsyms, stroff, _ = symtab
    external = 0
    for index in range(nsyms):
        entry = symoff + index * 16
        string, n_type = struct.unpack_from('<IB', data, entry)
        name = bytes(data[stroff + string:]).split(b'\0', 1)[0]
        if name == init.encode():
            struct.pack_into('<B', data, entry + 4, n_type | 0x01)
            external += 1
    if not external and required:
        raise SystemExit(f'{obj}: expected deferred constructor {init} missing from symbols')
    late.write_bytes(bytes(data))
    return True


def invalidate_link(digest):
    records = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    target = records / 'mach.development.bcm2837.unstripped.noctf'
    stamp = records / '.tb710fu-input-link-input.json'
    text = json.dumps({'archive': 'build/tb710fu-input/libTb710fuInput.a',
                       'archive_sha256': digest}, indent=2) + '\n'
    previous = stamp.read_text() if stamp.is_file() else ''
    if previous == text:
        return False
    target.unlink(missing_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', dir=records, prefix=stamp.name + '.', delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(text)
    os.replace(temporary, stamp)
    return True


# ---- IOHIDDevice vtable extraction (also used by the host check) ----

def macho_segments(image):
    data = image.read_bytes()
    if struct.unpack_from('<I', data)[0] != 0xfeedfacf:
        raise SystemExit(f'{image}: not a 64-bit Mach-O image')
    count = struct.unpack_from('<I', data, 16)[0]
    segments, offset = [], 32
    for _ in range(count):
        command, size = struct.unpack_from('<II', data, offset)
        if command == 0x19:
            va, vs, fo, fs = struct.unpack_from('<QQQQ', data, offset + 24)
            segments.append((va, vs, fo, fs))
        offset += size
    return data, segments


def read_va(data, segments, address, length):
    for va, vs, fo, fs in segments:
        if va <= address and address + length <= va + min(vs, fs) and fo:
            start = fo + address - va
            return data[start:start + length]
    raise SystemExit(f'0x{address:x} is not backed by a file range in the kernel image')


def vtable_rows(image, vtable=HID_VTABLE):
    """Entries of __ZTV<class>: authenticated pointers, diversity in bits 32..47.

    The table ends at the first zero word or at the first entry whose bit 63 is
    clear (a non-pointer, i.e. the next object's typeinfo slot)."""
    data, segments = macho_segments(image)
    rows = []
    for slot in range(400):
        word = struct.unpack_from('<Q', read_va(data, segments, vtable + 16 + slot * 8, 8))[0]
        if word == 0 or word >> 63 != 1:
            break
        rows.append({'slot': slot,
                     'target': 0xfffffff000000000 | (word & 0xffffffff),
                     'diversity': (word >> 32) & 0xffff})
    return rows


def ptrauth_string_discriminator(name):
    """clang's ptrauth_string_discriminator: SipHash-2-4, fixed key, mod 0xffff+1."""
    mask = (1 << 64) - 1
    rotl = lambda x, b: ((x << b) | (x >> (64 - b))) & mask
    key = struct.unpack('<2Q', bytes([0xb5, 0xd4, 0xc9, 0xeb, 0x79, 0x10, 0x4a, 0x79,
                                      0x6f, 0xec, 0x8b, 0x1b, 0x42, 0x87, 0x81, 0xd4]))
    v = [0x736f6d6570736575 ^ key[0], 0x646f72616e646f6d ^ key[1],
         0x6c7967656e657261 ^ key[0], 0x7465646279746573 ^ key[1]]

    def rounds(n):
        for _ in range(n):
            v[0] = (v[0] + v[1]) & mask; v[1] = rotl(v[1], 13) ^ v[0]; v[0] = rotl(v[0], 32)
            v[2] = (v[2] + v[3]) & mask; v[3] = rotl(v[3], 16) ^ v[2]
            v[0] = (v[0] + v[3]) & mask; v[3] = rotl(v[3], 21) ^ v[0]
            v[2] = (v[2] + v[1]) & mask; v[1] = rotl(v[1], 17) ^ v[2]; v[2] = rotl(v[2], 32)

    data = name.encode()
    full = len(data) & ~7
    for i in range(0, full, 8):
        m = struct.unpack_from('<Q', data, i)[0]
        v[3] ^= m; rounds(2); v[0] ^= m
    tail = (len(data) << 56) & mask
    for i, byte in enumerate(data[full:]):
        tail |= byte << (8 * i)
    v[3] ^= tail; rounds(2); v[0] ^= tail; v[2] ^= 0xff; rounds(4)
    return (v[0] ^ v[1] ^ v[2] ^ v[3]) % 0xffff + 1


def verify_named_slots(rows):
    seen = []
    for label, slot, mangled in NAMED_SLOTS:
        if slot >= len(rows):
            raise SystemExit(f'{label}: slot {slot} is past the end of {len(rows)} entries')
        got = rows[slot]['diversity']
        if mangled is None:
            if got != 0xfffc:
                raise SystemExit(f'{label}: slot {slot} carries 0x{got:04x}, expected 0xfffc')
        else:
            want = ptrauth_string_discriminator(mangled)
            if want != got:
                raise SystemExit(f'{label}: slot {slot} carries 0x{got:04x}, {mangled} hashes to 0x{want:04x}')
        seen.append((label, slot, got))
    return seen


def emit_slots_header(image=KERNEL_IMAGE, output=HID_SLOTS_HEADER, slots=334):
    rows = vtable_rows(image)
    if len(rows) != slots:
        raise SystemExit(f'{image}: {len(rows)} slots, the pinned profile has {slots}')
    verify_named_slots(rows)
    digest = sha(image)
    lines = [
        '/* SPDX-License-Identifier: BSD-2-Clause-Patent */',
        '/* GENERATED - do not edit by hand.  Regenerate with',
        ' * scripts/build-tb710fu-input.py --emit-hid-slots when the pinned OS profile',
        ' * changes; scripts/check-tb710fu-input-host.py re-proves the table against',
        ' * .local/prelink-kernel.macho and fails if a single diversity moved.',
        ' *',
        ' * What this is: the IOHIDDevice vtable of the macOS 27.3 (26C5035r) kernel',
        ' * this board prelinks, read out of the prelinked fileset itself.',
        f' *   source image   : prelink-kernel.macho, sha256 {digest}',
        f' *   vtable symbol  : __ZTV11IOHIDDevice at 0x{HID_VTABLE:x}',
        f' *   address point  : 0x{HID_VTABLE + 16:x} (slot 0)',
        f' *   slot count     : {len(rows)}',
        ' *   entry encoding : authenticated pointer, diversity in bits 32..47',
        ' *',
        ' * How it is used: the probe copies the live vtable into its own table and',
        ' * re-signs every entry for the new slot address with the diversity below,',
        ' * because ptrauth_blend_discriminator mixes the slot address into the',
        ' * signature.  Every slot is verified to strip to kernel text before the',
        ' * table is published; a mismatch refuses the device instead of calling',
        ' * through a poisoned pointer.',
        ' *',
        ' * Named slots were identified by hashing the mangled method name (clang\'s',
        ' * ptrauth_string_discriminator: SipHash-2-4 with the fixed key, mod 0xffff',
        ' * plus one) and matching the result against the entry\'s diversity; the',
        ' * generator asserts each one, and the host check repeats it.',
        ' */',
        '#ifndef TB710FU_INPUT_HIDSLOTS_H',
        '#define TB710FU_INPUT_HIDSLOTS_H',
        '',
        '#include <stdint.h>',
        '',
        f'#define TB710FU_HID_VTABLE_SLOTS {len(rows)}u',
        '#define TB710FU_HID_SLOT_GET_META_CLASS     7u',
        '#define TB710FU_HID_SLOT_START              190u',
        '#define TB710FU_HID_SLOT_STOP               191u',
        '#define TB710FU_HID_SLOT_NEW_REPORT_DESC    284u',
        '#define TB710FU_HID_SLOT_HANDLE_REPORT      285u',
        '#define TB710FU_HID_INSTANCE_BYTES          0xD0u',
        '',
        'static const uint16_t tb710fu_hid_slot_diversity[TB710FU_HID_VTABLE_SLOTS] = {',
    ]
    values = [f'0x{row["diversity"]:04x}' for row in rows]
    for index in range(0, len(rows), 12):
        lines.append('    ' + ', '.join(values[index:index + 12]) + ',')
    lines += ['};', '', '#endif /* TB710FU_INPUT_HIDSLOTS_H */']
    output.write_text('\n'.join(lines) + '\n')
    return digest, len(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emit-hid-slots', action='store_true',
                        help='regenerate package/Tb710fuInput/Tb710fuInputHidSlots.h and exit')
    parser.add_argument('--kernel', type=Path, default=KERNEL_IMAGE,
                        help='prelinked kernel image the slot table is read from')
    args = parser.parse_args()

    if args.emit_hid_slots:
        digest, count = emit_slots_header(args.kernel)
        print(f'wrote {reported(HID_SLOTS_HEADER)} ({count} slots, image {digest[:16]})')
        return 0

    device = Path(os.environ['UNIXNU_DEVICE']).resolve()
    tree = Path.cwd().resolve()
    catalog = tree / 'components.toml'
    if not catalog.is_symlink():
        raise SystemExit('Expected the fixed framework catalog link in the resolved tree')
    framework = catalog.resolve().parent
    lock = json.loads((device / 'upstream.lock.json').read_text())
    revision = subprocess.check_output(['git', '-C', str(framework), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != lock['unixnu']['commit']:
        raise SystemExit('Framework revision differs from upstream.lock.json')

    for record in (RECORD_C, RECORD_CPP):
        if not record.is_file():
            raise SystemExit(f'{record} missing; build the minimal XNU kernel first')
    c_record = json.loads(RECORD_C.read_text().strip().rstrip(','))
    cpp_record = json.loads(RECORD_CPP.read_text().strip().rstrip(','))

    adapter = helper('tb710fu_ufs_adapter', device / 'scripts/build-tb710fu-ufs-adapter.py')
    target = helper('tb710fu_source_target', framework / 'scripts/kernel_target.py')

    OUT.mkdir(parents=True, exist_ok=True)
    archive = OUT / 'libTb710fuInput.a'
    archive.unlink(missing_ok=True)
    objects, commands = [], {}
    includes = [PACKAGE, tree / '.cache/xnu-work/osfmk', tree / '.cache/xnu-work/libkern']

    for source in C_SOURCES:
        output = OUT / (source.stem + '.o')
        command = compile_command(c_record, source, output, includes, ['-mgeneral-regs-only'])
        commands[source.name] = command
        result = subprocess.run(command, cwd=c_record['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (OUT / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(f'{source.name}:\nCMD: {" ".join(command)}\n{result.stdout[-6000:]}')
        target.check_object(output.read_bytes(), source.name)
        objects.append(output)

    for source in CPP_SOURCES:
        output = OUT / (source.stem + '.o')
        command = adapter.compile_command(cpp_record, source, output, includes, [])
        command = ['--target=' + TARGET if value.startswith('--target=') else value for value in command]
        commands[source.name] = command
        result = subprocess.run(command, cwd=cpp_record['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (OUT / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(f'{source.name}:\n{result.stdout[-6000:]}')
        target.check_object(output.read_bytes(), source.name)
        late = output.with_suffix('.late.o')
        if defer_constructor(output, late, '__GLOBAL__sub_I_' + source.name, required=False):
            target.check_object(late.read_bytes(), source.name + ' deferred')
            objects.append(late)
        else:
            objects.append(output)

    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    digest = sha(archive)
    slots = vtable_rows(args.kernel) if args.kernel.is_file() else None
    report = {
        'archive': reported(archive),
        'archive_sha256': digest,
        'framework_revision': revision,
        'objects': {obj.name: sha(obj) for obj in objects},
        'sources': {reported(path): sha(path) for path in C_SOURCES + CPP_SOURCES + HEADERS},
        'compile_commands_sha256': {name: hashlib.sha256(json.dumps(cmd).encode()).hexdigest()
                                    for name, cmd in commands.items()},
        'host_checks': 'scripts/check-tb710fu-input-host.py',
        'boot_arguments': {'touch': 'tb710fu-touch=0..3', 'keys': 'tb710fu-keys=0..1',
                           'fuse': 'tb710fu-touch-fuse=1', 'irq': 'tb710fu-touch-irq=1',
                           'trace': 'tb710fu-input-verbose=1'},
        'hid_vtable': {'slots': len(slots) if slots else None,
                       'named': [{'slot': s, 'diversity': f'0x{d:04x}'}
                                 for _, s, d in (verify_named_slots(slots) if slots else [])]},
        'publishes_digitizer': False,  # stage 3 only; needs tb710fu-touch=3 at boot
        'publishes_keys': False,       # needs tb710fu-keys=1 at boot
        'firmware_upload': False,      # validated offline, never written to the controller
    }
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    if invalidate_link(digest):
        print('TB710FU input archive changed: invalidated the XNU link target')
    print(f'TB710FU input archive passed: {archive} ({digest[:12]})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
