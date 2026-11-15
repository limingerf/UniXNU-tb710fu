"""Append an auditable terminal exception reporter to the exact bring-up fileset.

No kernel instruction is bypassed on the default path. The original PACIBSP is
replayed. Explicit self-test and optional checkpoint modes are separate from
normal early execution. No source cache, firmware or device is modified.
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


def sha(data):
    return hashlib.sha256(data).hexdigest()


def run(cmd, cwd=None):
    subprocess.run(list(map(str, cmd)), cwd=cwd, check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel', type=Path, required=True)
    p.add_argument('--framework', type=Path, required=True)
    p.add_argument('--qr-source', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--checkpoint-before-vm', action='store_true',
                   help='Explicit diagnostic stop; default continues the original arm_vm_init call')
    args = p.parse_args()
    root = Path(__file__).resolve().parents[1]
    src = root / 'diagnostics/early-report'
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    candidate = output / 'kernel-early-report.macho'
    if candidate.exists():
        raise SystemExit('Output exists; preserve previous evidence')
    original = args.kernel.read_bytes()
    if sha(original) != 'cc690c9a0bde84c49e9cebccc288f393393d16da56620d580108f0a62692b31c':
        raise SystemExit('Require original recorded fileset')
    manifest = json.loads((src / 'qr-sources.json').read_text())
    qr = args.qr_source.resolve()
    for record in manifest['files']:
        if sha((qr / record['path']).read_bytes()) != record['sha256']:
            raise SystemExit('QR source hash mismatch: ' + record['path'])
    lock = json.loads((root / 'upstream.lock.json').read_text())
    tool_path = args.framework / 'scripts/patch-xnu-entry-probe.py'
    pinned = subprocess.check_output(['git', '-C', str(args.framework), 'show',
                                     lock['unixnu']['commit'] + ':scripts/patch-xnu-entry-probe.py'])
    if tool_path.read_bytes() != pinned:
        raise SystemExit('Official input parser/probe differs')
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location('official_probe', tool_path)
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    def symbols(kernel):
        found = {}
        for line in subprocess.check_output(['llvm-nm', '-n', str(kernel)], text=True).splitlines():
            fields = line.split()
            if len(fields) == 3 and re.fullmatch('[0-9a-fA-F]+', fields[0]):
                found.setdefault(int(fields[0], 16), fields[2])
        return found
    tool.kernel_symbols = symbols
    data = bytearray(original)
    tool.load_segments(data)
    names = tool.rebase(args.kernel, data)
    arm = names['_arm_init']
    if struct.unpack_from('<I', data, tool.file_offset(arm))[0] != 0xd503237f:
        raise SystemExit('arm_init PACIBSP guard failed')
    # Record the VM call identity without stopping normal execution.
    kernel_symbols = symbols(args.kernel)
    call, callee = tool.find_call(data, kernel_symbols, '_arm_init', '_arm_vm_init', 1)
    original_call = struct.unpack_from('<I', data, tool.file_offset(call))[0]
    if original_call >> 26 != 0b100101:
        raise SystemExit('Checkpoint must replace a verified direct BL')
    traced_names = ['_PE_init_platform', '_configure_timer_apple_regs', '_wfe_timeout_configure',
                    '_wfe_timeout_init', '_configure_misc_apple_boot_args', '_configure_misc_apple_regs',
                    '_ml_parse_cpu_topology', '_thread_bootstrap', '_cpu_bootstrap',
                    '_rtclock_early_init', '_kernel_startup_bootstrap', '_cpu_init', '_processor_bootstrap',
                    '_arm_vm_init']
    if args.checkpoint_before_vm:
        traced_names.remove('_arm_vm_init')
    traced = []
    trace_source = '.section .text.calltrace,"ax"\n'
    definitions = []
    for index, name in enumerate(traced_names):
        site, target = tool.find_call(data, kernel_symbols, '_arm_init', name, 1)
        if site > call:
            raise SystemExit('Trace point lies beyond final diagnostic stop')
        label, symbol, resume = 'diag_trace_' + str(index), 'diag_original_' + str(index), 'diag_return_' + str(index)
        trace_source += ('.global ' + label + '\n.hidden ' + symbol + '\n.hidden ' + resume + '\n' +
                         label + ':\n  adrp x16,diag_last_stage\n  add x16,x16,:lo12:diag_last_stage\n' +
                         '  mov x17,#' + str(index * 2 + 1) + '\n  str x17,[x16]\n' +
                         '  bl ' + symbol + '\n' +
                         '  adrp x16,diag_last_stage\n  add x16,x16,:lo12:diag_last_stage\n' +
                         '  mov x17,#' + str(index * 2 + 2) + '\n  str x17,[x16]\n' +
                         '  adrp x30,' + resume + '\n  add x30,x30,:lo12:' + resume + '\n  b ' + resume + '\n')
        definitions += ['--defsym=' + symbol + '=' + hex(target), '--defsym=' + resume + '=' + hex(site + 4)]
        traced.append({'name': name, 'site': hex(site), 'callee': hex(target), 'label': label,
                       'before_stage': index * 2 + 1, 'after_stage': index * 2 + 2})
    ncmds, sizeofcmds = struct.unpack_from('<II', data, 16)
    command_at = 32 + sizeofcmds
    if any(data[command_at:command_at + 72]):
        raise SystemExit('Root Mach-O has no guarded spare load-command space')
    last = max(va + size for va, size, off, fs in tool.SEGMENTS)
    base = (last + 0x3fff) & ~0x3fff
    compat = output / 'compat'
    for relative in ['Uefi.h', 'Protocol/GraphicsOutput.h', 'Library/BaseMemoryLib.h',
                     'Library/DebugLib.h', 'Library/MemoryAllocationLib.h', 'Library/UefiBootServicesTableLib.h']:
        path = compat / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "Compat.h"\n')
    shutil.copyfile(qr / 'MsGraphicsPkg/Include/Library/QrEncoderLib.h', compat / 'Library/QrEncoderLib.h')
    flags = ['--target=aarch64-none-elf', '-std=c11', '-Oz', '-fpie', '-fvisibility=hidden',
             '-fno-stack-protector', '-fno-builtin', '-ffreestanding', '-mgeneral-regs-only',
             '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-I' + str(src), '-I' + str(compat)]
    sources = [src / 'Report.c', qr / 'MsGraphicsPkg/Library/QrEncoderLib/QrEncoderLib.c',
               qr / 'MsGraphicsPkg/Library/QrEncoderLib/QrEncoderTables.c']
    objects = []
    for index, source in enumerate(sources):
        obj = output / ('part' + str(index) + '.o')
        run(['clang'] + flags + ['-DDIAG_LINK_BASE=' + hex(base) + 'ULL', '-c', source, '-o', obj])
        objects.append(obj)
    entry_obj = output / 'Entry.o'
    run(['clang', '--target=aarch64-none-elf', '-c', src / 'Entry.S', '-o', entry_obj])
    trace_file, trace_object = output / 'calltrace.S', output / 'calltrace.o'
    trace_file.write_text(trace_source)
    run(['clang', '--target=aarch64-none-elf', '-c', trace_file, '-o', trace_object])
    linker = output / 'layout.ld'
    linker.write_text('''SECTIONS {
  . = ''' + hex(base) + '''; diag_image_base = .;
  .text : { *(.text.entry) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  . = ALIGN(16); diag_rela_start = .;
  .rela.dyn : { *(.rela.dyn) } diag_rela_end = .;
  . = ALIGN(16);
  .data : { *(.data.state) *(.data .data.*) *(.got .got.*) *(.bss .bss.*) *(COMMON) BYTE(0) }
  diag_stack_end = diag_stack + 16384;
  diag_image_end = .;
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) *(.dynamic) }
}
''')
    elf = output / 'report.elf'
    run(['ld.lld', '-pie', '--no-dynamic-linker', '-Bsymbolic', '-T', linker,
         '--defsym=arm_next=' + hex(arm + 4),
         '--defsym=diag_kernel_vectors=' + hex(names['_ExceptionVectorsBase']),
         '-e', 'diag_init', entry_obj, trace_object] +
         definitions + objects + ['-o', elf])
    payload_path = output / 'report.bin'
    run(['llvm-objcopy', '-O', 'binary', elf, payload_path])
    payload = payload_path.read_bytes()
    table = {}
    for line in subprocess.check_output(['llvm-nm', '-n', str(elf)], text=True).splitlines():
        f = line.split()
        if len(f) == 3 and re.fullmatch('[0-9a-fA-F]+', f[0]):
            table[f[2]] = int(f[0], 16)
    relocations = subprocess.check_output(['llvm-readelf', '-r', str(elf)], text=True)
    if any('R_AARCH64_' in x and 'R_AARCH64_RELATIVE' not in x for x in relocations.splitlines()):
        raise SystemExit('Unsupported runtime relocation')
    (output / 'relocations.txt').write_text(relocations)
    if table['diag_image_base'] != base or len(payload) > 0x100000:
        raise SystemExit('Unexpected diagnostic image layout/size')
    fileoff = (len(data) + 0x3fff) & ~0x3fff
    size = (len(payload) + 0x3fff) & ~0x3fff
    if base + size - min(va for va, vs, off, fs in tool.SEGMENTS) >= 0x10000000 - 0x200000:
        raise SystemExit('Diagnostic exceeds loader kernel size constraint')
    data += bytes(fileoff - len(data)) + payload + bytes(size - len(payload))
    segment = struct.pack('<II16sQQQQiiII', 0x19, 72, b'__KDIAG', base, size, fileoff, size, 7, 7, 0, 0)
    data[command_at:command_at + 72] = segment
    struct.pack_into('<II', data, 16, ncmds + 1, sizeofcmds + 72)
    struct.pack_into('<I', data, tool.file_offset(arm), tool.branch(arm, table['diag_init']))
    # Synchronous EL1 faults enter our own VBAR table installed at arm_init.
    panic = names['_panic']
    panic_word = struct.unpack_from('<I', original, tool.file_offset(panic))[0]
    struct.pack_into('<I', data, tool.file_offset(panic), tool.branch(panic, table['diag_panic']))
    # No default BRK: exception/panic reports must describe the real kernel.
    if args.checkpoint_before_vm:
        struct.pack_into('<I', data, tool.file_offset(call), 0xd4200000 | (0xb713 << 5))
    for item in traced:
        site = int(item['site'], 16)
        struct.pack_into('<I', data, tool.file_offset(site), tool.branch(site, table[item['label']]))
    # Reuse three already instruction-tested compact entry/MMU checkpoints.
    compact = src
    for stage, site, target, cave in [(1, tool.ENTRY_VA, tool.START_FIRST_CPU, tool.ENTRY_VA + 0x80),
                                     (2, tool.MMU_ENABLE_VA, tool.MMU_ENABLE_VA + 4, tool.ENTRY_VA + 0x200),
                                     (3, tool.AFTER_MMU_VA, tool.AFTER_MMU_VA + 4, tool.ENTRY_VA + 0x380)]:
        code_source = (compact / ('Compact' + str(stage) + '.S')).read_text()
        code = tool.assemble(code_source, cave, {'target': target}, output)
        at = tool.file_offset(cave)
        if any(original[at:at + 0x180]) or len(code) > 0x180:
            raise SystemExit('Compact checkpoint padding guard failed')
        data[at:at + len(code)] = code
        struct.pack_into('<I', data, tool.file_offset(site), tool.branch(site, cave))
    candidate.write_bytes(data)
    print('Reporter linked with original QrEncodeData; compressing', flush=True)
    filters = [{'id': lzma.FILTER_LZMA1, 'dict_size': 8 << 20, 'lc': 3, 'lp': 0, 'pb': 2, 'preset': 6}]
    encoded = lzma.compress(data, format=lzma.FORMAT_ALONE, filters=filters)
    if lzma.decompress(encoded) != data:
        raise SystemExit('Compression round-trip')
    encoded = encoded[:5] + len(data).to_bytes(8, 'little') + encoded[13:]
    if lzma.decompress(encoded[13:], format=lzma.FORMAT_RAW, filters=filters) != data:
        raise SystemExit('Raw stream round-trip')
    candidate.with_suffix('.lzma').write_bytes(encoded)
    record = {'stage': 'terminal-early-exception-and-Onesend-diagnostic', 'original_sha256': sha(original),
              'kernel_sha256': sha(data), 'kernel_bytes': len(data), 'lzma_sha256': sha(encoded),
              'lzma_bytes': len(encoded), 'segment_va': hex(base), 'segment_fileoff': fileoff,
              'segment_bytes': size, 'payload_bytes': len(payload),
              'arm_init_va': hex(arm), 'arm_init_original_instruction': 'PACIBSP replayed',
              'panic_va': hex(panic), 'panic_original_word': hex(panic_word),
              'checkpoint_va': hex(call), 'checkpoint_original_bl': hex(original_call),
              'checkpoint_callee': hex(callee),
              'checkpoint_installed': args.checkpoint_before_vm,
              'checkpoint': 'explicit BRK b713 before arm_vm_init' if args.checkpoint_before_vm else 'none on default path',
              'call_trace': traced,
              'symbols': {k: hex(v) for k, v in table.items() if k.startswith('diag_')},
              'qr_source': manifest,
              'reporter_sources': {str(p.relative_to(root)): sha(p.read_bytes()) for p in sorted(src.rglob('*')) if p.is_file() and '__pycache__' not in p.parts},
              'user_display_request': 'one compact strip; fault replaces screen with report',
              'report_is_full_console_log': False, 'static_single_OS_frame': True,
              'hardware_verified': False, 'functional_fix_claimed': False}
    (output / 'build-report.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({k: record[k] for k in ['kernel_sha256', 'kernel_bytes', 'segment_bytes', 'checkpoint_va']}, indent=2))


if __name__ == '__main__':
    main()
