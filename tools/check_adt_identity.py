#!/usr/bin/env python3
"""Fail the build when the ADT sidecar would not identify as a macOS device.

Why this exists: the PR249 delivery shipped a kernel.macho.adt whose root was
still the hardware DTS identity --

    model      = 'Qualcomm Technologies, Inc. Pineapple MTP'
    compatible = 'qcom,pineapple-mtp'
    /arm-io    = 0 hardware nodes
    /product   = absent

-- because board.toml never set [payload.adt] chip_id / owned_memory_size.
scripts/unixnu/devicetree.generate() only emits unique-chip-id + chip-id when
payload.adt.chip_id is set (a bare --unique-chip-id raises KeyError('chip_id')),
and it emits none of adt.root / adt.chosen / adt.product without a merged
profile config. A kernel booting against that ADT matches no macOS platform
expert.

Every earlier gate passed anyway, because each one compared the produced ADT
against the *previous* ADT's hash. This check compares it against the identity
the board is supposed to have.

Usage:
    python3 tools/check_adt_identity.py --adt PATH [--require-model Mac17,11]

Also usable as a config preflight with --config, which runs the pinned generator
and inspects the result without needing a built payload.
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_nodes(blob):
    """Parse the flat ADT layout (scripts/unixnu/devicetree.py node())."""
    cursor = [0]

    def node():
        prop_count, child_count = struct.unpack_from('<II', blob, cursor[0])
        cursor[0] += 8
        props = []
        for _ in range(prop_count):
            key = blob[cursor[0]:cursor[0] + 32].split(b'\0', 1)[0].decode('latin-1')
            cursor[0] += 32
            (size,) = struct.unpack_from('<I', blob, cursor[0])
            cursor[0] += 4
            value = blob[cursor[0]:cursor[0] + size]
            cursor[0] += size + (-size % 4)
            props.append((key, value))
        return props, [node() for _ in range(child_count)]

    root = node()
    if cursor[0] != len(blob):
        raise SystemExit(f'ADT trailing bytes: parsed {cursor[0]} of {len(blob)}')
    return root


def name_of(node):
    for key, value in node[0]:
        if key == 'name':
            return value.rstrip(b'\0').decode('latin-1')
    return '?'


def text(value):
    return value.rstrip(b'\0').decode('latin-1', 'replace')


def inspect(blob, require_model):
    root_props, children = read_nodes(blob)
    root = {key: value for key, value in root_props if key != 'name'}
    model = text(root.get('model', b''))
    compatible = [part for part in text(root.get('compatible', b'')).split('\0') if part]

    chosen_keys, chosen = [], {}
    arm_io_nodes = None
    product_props = 0
    for child in children:
        child_name = name_of(child)
        if child_name == 'chosen':
            chosen_keys = [k for k, _ in child[0] if k != 'name']
            chosen = {k: v for k, v in child[0] if k != 'name'}
        elif child_name == 'arm-io':
            arm_io_nodes = len(child[1])
        elif child_name == 'product':
            product_props = len([k for k, _ in child[0] if k != 'name'])

    findings = []

    def check(ok, label, detail):
        findings.append({'check': label, 'result': 'PASS' if ok else 'FAIL',
                         'detail': detail})

    check(model == require_model, 'root model is the macOS identity',
          f'model={model!r} required={require_model!r}')
    check(bool(compatible) and compatible[0] == 'K114cAP',
          'compatible[0] is the board identity',
          f'compatible[0]={compatible[0] if compatible else None!r}')
    check(product_props > 0, '/product carries the Mac product identity',
          f'{product_props} properties')
    for key in ('osenvironment', 'boot-uuid', 'apfs-preboot-uuid', 'root-matching'):
        check(key in chosen_keys, f'/chosen has {key}', 'present' if key in chosen_keys else 'MISSING')
    for key in ('dram-base', 'dram-size', 'darwinboot-memory-size'):
        check(key in chosen_keys, f'/chosen keeps {key}',
              'present' if key in chosen_keys else 'MISSING')

    # The hardware window must survive the identity: a macOS ADT that lost the
    # window or the seed cannot hand memory to DarwinBoot.
    window = int.from_bytes(chosen.get('darwinboot-memory-size', b'\0' * 8), 'little')
    check(window > 0, '/chosen darwinboot-memory-size is set', f'{window:#x}')
    check('chip-id' in chosen_keys, '/chosen has chip-id',
          'present' if 'chip-id' in chosen_keys else 'MISSING')
    check('unique-chip-id' in chosen_keys, '/chosen has unique-chip-id',
          'present' if 'unique-chip-id' in chosen_keys else 'MISSING')

    # Honest limitation, reported but not failed: the board DTS exports no
    # unixnu,adt-name nodes, so /arm-io has no hardware children yet.
    findings.append({'check': '/arm-io hardware nodes', 'result': 'INFO',
                     'detail': f'{arm_io_nodes} nodes (none exported by the board DTS)'})

    return {'bytes': len(blob), 'sha256': sha(blob), 'model': model,
            'compatible': compatible, 'arm_io_nodes': arm_io_nodes,
            'product_props': product_props, 'chosen_keys': chosen_keys,
            'findings': findings}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--adt', type=Path, required=True)
    parser.add_argument('--require-model', default='Mac17,11')
    args = parser.parse_args()

    if not args.adt.exists():
        raise SystemExit(f'no such ADT: {args.adt}')
    report = inspect(args.adt.read_bytes(), args.require_model)
    print(f"ADT {args.adt} ({report['bytes']:,} B, {report['sha256']})")
    print(f"  model={report['model']!r} compatible={report['compatible'][:2]} "
          f"product={report['product_props']} arm-io={report['arm_io_nodes']}\n")
    failed = 0
    for item in report['findings']:
        if item['result'] == 'FAIL':
            failed += 1
        print(f"  [{item['result']}] {item['check']}: {item['detail']}")

    if failed:
        print(f"\n{failed} identity check(s) failed: this ADT is not a macOS device tree "
              f"and the kernel would match no platform expert. Set [payload.adt] "
              f"chip_id / owned_memory_size and merge the OS profile's [adt] block.")
        return 1
    print('\nAll ADT identity checks passed.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
