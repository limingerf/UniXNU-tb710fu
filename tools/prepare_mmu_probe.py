"""Guarded MMU/C-entry diagnostics for the recorded TB710FU prelinked kernel.

Uses the pinned official probe's Mach-O reader, branch guards and assembler.
New probes briefly use the existing physical code alias with MMU/caches off
to paint the GOP, then restore SCTLR. This is a diagnostic, not an MMU fix.
No page tables, stack, persistent RAM, storage or fetched sources are modified.
"""
import argparse
import hashlib
import importlib.util
import json
import lzma
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--framework', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    original = args.kernel.read_bytes()
    if sha(original) != 'cc690c9a0bde84c49e9cebccc288f393393d16da56620d580108f0a62692b31c':
        raise SystemExit('Require the recorded original prelinked kernel')
    if args.output.exists() or args.output.with_suffix('.lzma').exists():
        raise SystemExit('Output exists; preserve prior evidence')
    root = Path(__file__).resolve().parents[1]
    lock = json.loads((root / 'upstream.lock.json').read_text(encoding='utf-8'))
    relative = 'scripts/patch-xnu-entry-probe.py'
    tool = args.framework / relative
    source = subprocess.check_output(['git', '-C', str(args.framework), 'show',
                                      lock['unixnu']['commit'] + ':' + relative])
    if source != tool.read_bytes():
        raise SystemExit('Official probe differs from the pinned source')
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location('official_entry_probe', tool)
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)

    def symbols(kernel):
        result = subprocess.check_output([shutil.which('llvm-nm'), '-n', str(kernel)], text=True)
        found = {}
        for line in result.splitlines():
            fields = line.split()
            if len(fields) == 3 and re.fullmatch('[0-9a-fA-F]+', fields[0]):
                found.setdefault(int(fields[0], 16), fields[2])
        return found

    probe.kernel_symbols = symbols
    data = bytearray(original)
    probe.load_segments(data)
    names = probe.rebase(args.kernel, data)
    entry, before, after = probe.ENTRY_VA, probe.MMU_ENABLE_VA, probe.AFTER_MMU_VA
    arm_init = names['_arm_init']
    word = lambda va: struct.unpack_from('<I', data, probe.file_offset(va))[0]
    expected = {entry: probe.branch(entry, probe.START_FIRST_CPU),
                before: 0xD5181000, before + 4: 0xD5033FDF,
                after: 0xD2A00600, after + 4: 0xD5181040,
                arm_init: 0xD503237F}  # PACIBSP, replayed at the same SP.
    for va, value in expected.items():
        if word(va) != value:
            raise SystemExit('Instruction guard failed at ' + hex(va))
    caves = {1: entry + 0x80, 2: entry + 0x200,
             3: entry + 0x380, 4: entry + 0x500}
    capacity = 0x180
    for va in caves.values():
        begin = probe.file_offset(va)
        if any(data[begin:begin + capacity]):
            raise SystemExit('Probe padding is not empty at ' + hex(va))
    off = '''  mrs x10, sctlr_el1
  bic x11, x10, #1
  bic x11, x11, #4
  bic x11, x11, #0x1000
  msr sctlr_el1, x11
  isb
'''
    on = '''  msr sctlr_el1, x10
  isb
'''
    # Until the first C prologue, startup is register-only after enabling the
    # caches. The probes use no stack and make no normal-memory writes; only
    # GOP stores occur while caches are off. Never apply this to a later site.
    cyan = probe.PAINT.format(args='x20', band=3, lo='0xFFFF', hi='0xFF00')
    yellow = probe.PAINT.format(args='x20', band=4, lo='0xFF00', hi='0xFFFF')
    sources = {1: probe.SOURCES[1], 2: probe.SOURCES[2],
               3: '.text\n.global _start\n_start:\n' + off + cyan + on +
                  '  mov x0, #0x300000\n  b target\n',
               4: '''.text
.global _start
_start:
  adr x9, 8f
  sub x9, x9, x22
  add x9, x9, x23
  br x9
8:
''' + off + yellow + on + '''  adr x9, 9f
  add x9, x9, x22
  sub x9, x9, x23
  br x9
9:
  .inst 0xd503237f
  b target
'''}
    sites = {1: (entry, probe.START_FIRST_CPU), 2: (before, before + 4),
             3: (after, after + 4), 4: (arm_init, arm_init + 4)}
    allowed = []
    code_sizes = {}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='tb710fu-mmu-probe-') as tmp:
        for stage, (site, target) in sites.items():
            code = probe.assemble(sources[stage], caves[stage], {'target': target}, Path(tmp))
            if len(code) > capacity:
                raise SystemExit('Probe exceeds guarded padding')
            at = probe.file_offset(caves[stage])
            data[at:at + len(code)] = code
            struct.pack_into('<I', data, probe.file_offset(site), probe.branch(site, caves[stage]))
            allowed.extend([(at, capacity), (probe.file_offset(site), 4)])
            code_sizes[str(stage)] = len(code)
            args.output.with_suffix('.stage' + str(stage) + '.S').write_text(sources[stage], encoding='utf-8')
    changes = [i for i, (a, b) in enumerate(zip(original, data)) if a != b]
    if not all(any(start <= i < start + size for start, size in allowed) for i in changes):
        raise SystemExit('Unexpected kernel changes')
    with args.output.open('xb') as stream:
        stream.write(data)
    print('Assembled four guarded probes; compressing diagnostic copy', flush=True)
    filters = [{'id': lzma.FILTER_LZMA1, 'dict_size': 8 << 20,
                'lc': 3, 'lp': 0, 'pb': 2, 'preset': 6}]
    encoded = lzma.compress(data, format=lzma.FORMAT_ALONE, filters=filters)
    if lzma.decompress(encoded) != data:
        raise SystemExit('LZMA round-trip failed')
    encoded = encoded[:5] + len(data).to_bytes(8, 'little') + encoded[13:]
    if lzma.decompress(encoded[13:], format=lzma.FORMAT_RAW, filters=filters) != data:
        raise SystemExit('Raw LZMA round-trip failed')
    with args.output.with_suffix('.lzma').open('xb') as stream:
        stream.write(encoded)
    report = {'stage': 'guarded-mmu-c-entry-diagnostic',
              'framework_commit': lock['unixnu']['commit'], 'official_probe_sha256': sha(source),
              'input_sha256': sha(original), 'kernel_sha256': sha(data), 'bytes': len(data),
              'lzma_sha256': sha(encoded), 'lzma_bytes': len(encoded),
              'changed_bytes': len(changes), 'changed_ranges_verified': True,
              'instruction_guards': {hex(k): hex(v) for k, v in expected.items()},
              'sites': {str(k): hex(v[0]) for k, v in sites.items()},
              'caves': {str(k): hex(v) for k, v in caves.items()}, 'code_bytes': code_sizes,
              'markers': {'blue': 'XNU entry', 'magenta': 'before MMU enable',
                          'cyan': 'after MMU enable and startup compare',
                          'yellow': 'arm_init entry reached'},
              'temporary_sctlr_roundtrips': True, 'page_tables_modified': False,
              'automatic_reset_added': False, 'hardware_verified': False,
              'functional_fix_claimed': False}
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
