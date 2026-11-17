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
    p.add_argument('--build-suffix', default='qr8',
                   help='Short display suffix that distinguishes post-link diagnostic candidates')
    p.add_argument('--expect-kernel-sha256',
                   help='Require this exact input digest (refuses a different accepted fileset)')
    p.add_argument('--trace-past-switch', action='store_true',
                   help='Also trace _set_mmu_ttb and _flush_mmu_tlb after the TTBR1 '
                        'switch. Only valid with the 0006/0009 diagnostic segment mappings.')
    p.add_argument('--trace-switch-probe', action='store_true',
                   help='Trace the noinline 0007 marker helper with separate QR frames '
                        'before entry and after it returns. Requires --trace-past-switch.')
    p.add_argument('--trace-platform-startup', action='store_true',
                   help='Extend the continuing trace from ramoops through arm_init\'s '
                        'pre-scheduler platform calls; stop tracing at machine_startup entry.')
    args = p.parse_args()
    if args.trace_switch_probe and not args.trace_past_switch:
        raise SystemExit('--trace-switch-probe requires --trace-past-switch')
    if args.trace_platform_startup and not args.trace_switch_probe:
        raise SystemExit('--trace-platform-startup requires --trace-switch-probe')
    root = Path(__file__).resolve().parents[1]
    src = root / 'diagnostics/early-report'
    source_snapshot = {str(q.relative_to(root)): sha(q.read_bytes()) for q in sorted(src.rglob('*'))
                       if q.is_file() and '__pycache__' not in q.parts}
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    candidate = output / 'kernel-early-report.macho'
    if candidate.exists():
        raise SystemExit('Output exists; preserve previous evidence')
    original = args.kernel.read_bytes()
    # The reporter was originally pinned to the single cc690c9a fileset. Later
    # accepted kernels are listed explicitly, and the caller must always name the
    # exact digest with --expect-kernel-sha256, so wiring the reporter to a new
    # kernel is deliberate and never accidental. Every hook point is re-verified
    # below regardless of which input is used.
    allowed = {
        # original prelink with the ramoops console patch
        'cc690c9a0bde84c49e9cebccc288f393393d16da56620d580108f0a62692b31c',
        # PR249 bootstrap pool (16 -> 22 pages)
        '9daf5064924a2a085b1e8ee4e0871df5b9fc86dac516c8d7de9a85484f2ca8ee',
        # PR249 + 0006 diagnostic-segment mapping (run 43806716346)
        '6adc7e159137d073bfeebcc1d3385bd78d08e312544949b03287ec9129231a32',
        # PR249 + 0006 keyed on the segment name, no boot argument (run 43813385537)
        '6c71e39ca6c58a48a614c1ca4927a723d7b782d8fde516215c18ee7a49aae96e',
        # PR249 + 0006 + 0007 identity-mapped switch markers (run 43819437775)
        '860834c55878c484be9c444588ae9c70e9bc01f40f89750e055fe4d361211bb7',
        # PR249 + 0006 + 0007 + 0008 in-window log region (run 43827454902)
        '4ad08f1b166ca16e340bdfd9325d6e2ce1a76045036251ae95f2d4c47a23db00',
        # Paired RX/RW-NX diagnostic mappings + noinline switch helper (run 43844823907)
        '045bfc7cfad9ed2539a586043dd43211263099734f4978abd8092c213b7d7d95',
        # Fileset-aware diagnostic-segment lookup (run 43917723572)
        'eff36305a28a6bf89c453734a07a9fe0e878e9041773864aeb205fc88483c3e3',
        # Six-source CDC and source-storage integration (run 44088845841).
        # Its 0006/0009 hashes match the fileset-aware guard11 input.
        '503599a026ee362e9523aba1808f8af01601ae1323c7404fe7ecd1ed9a5690e2',
    }
    digest = sha(original)
    if digest not in allowed:
        raise SystemExit(f'Require a recorded fileset, got {digest}')
    if digest == '503599a026ee362e9523aba1808f8af01601ae1323c7404fe7ecd1ed9a5690e2' and not args.expect_kernel_sha256:
        raise SystemExit('The CDC reporter derivative requires its explicit expected input SHA-256')
    if args.expect_kernel_sha256 and digest != args.expect_kernel_sha256:
        raise SystemExit(f'Kernel is {digest}, not the requested {args.expect_kernel_sha256}')
    if not re.fullmatch(r'[a-z0-9-]{1,16}', args.build_suffix):
        raise SystemExit('Build suffix must contain 1-16 lowercase letters, digits, or hyphens')
    build_label = 'src-' + digest[:8] + '-' + args.build_suffix
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
    # Returning to appended code after TTBR1 changes requires both RX and RW-NX
    # segment mappings; 0006/0009 establish them before this call site.
    specs = [('_arm_init', name, 1) for name in traced_names]
    switch_target = '_set_mmu_ttb_alternate_probed' if args.trace_switch_probe else '_set_mmu_ttb_alternate'
    if args.trace_switch_probe and switch_target not in kernel_symbols.values():
        raise SystemExit('Kernel lacks the noinline 0009 marker helper; rebuild with patches/xnu/0009')
    if args.trace_switch_probe:
        switch_va = next(address for address, name in kernel_symbols.items() if name == switch_target)
        switch_fileoff = tool.file_offset(switch_va)
        switch_words = struct.unpack_from('<12I', data, switch_fileoff)
        expected_switch_words = (
            0xd503245f, 0xd29e0008, 0xf2a7ffe8, 0xf2c00128,
            0x52800029, 0x39000109, 0xd5033f9f, 0xd5182020,
            0xd5033fdf, 0x52800049, 0x39000509, 0xd65f03c0,
        )
        if switch_words != expected_switch_words:
            raise SystemExit('Built marker helper does not bracket TTBR1 MSR/ISB with the expected stores')
    specs += [('_arm_vm_init', name, occurrence) for name, occurrence in [
        ('_early_random', 1), ('_SecureDTLookupEntry', 1), ('_SecureDTGetProperty', 1),
        ('_SecureDTGetProperty', 2), ('_arm_vm_outside_add', 1),
        ('_getsegdatafromheader', 1), ('_vm_set_page_size', 1),
        ('_arm_vm_prot_init', 1), ('_qsort', 1), (switch_target, 1)]]
    switch_site, _ = tool.find_call(data, kernel_symbols, '_arm_vm_init', '_set_mmu_ttb_alternate', 1)
    _, next_vm_target = tool.find_call(data, kernel_symbols, '_arm_vm_init', '_set_mmu_ttb', 1)
    if args.trace_past_switch:
        # Patches 0006/0009 map diagnostic code RX and mutable state/stack RW-NX
        # during arm_vm_prot_init, before the switch.
        # _set_mmu_ttb is the first call arm_vm_init makes after the TTBR1
        # switch; reaching its entry proves execution survived the switch.
        # _flush_mmu_tlb is a macro here, so it is not a call site.
        specs += [
            ('_arm_vm_init', '_set_mmu_ttb', 1),
            ('_arm_vm_init', '_pmap_bootstrap', 1),
            ('_arm_vm_init', '__enable_preemption', 1),
        ]
        if args.trace_switch_probe:
            # This call is immediately after arm_vm_init returns, so its entry
            # checkpoint proves the entire function returned to its caller.
            specs += [('_arm_init', '_tb710fu_ramoops_init', 1)]
    if args.trace_platform_startup:
        # All these calls precede scheduling. fiq_context_init still asserts
        # interrupts disabled; machine_startup never returns, and only its entry
        # is reported. Do not extend this shared-stack/GOP reporter into BSD or
        # a CDC worker, where interrupts, scheduling and concurrent CPUs apply.
        specs += [('_arm_init', name, occurrence) for name, occurrence in (
            ('_PE_consistent_debug_inherit', 1), ('_serial_init', 1),
            ('_kernel_startup_initialize_upto', 1), ('_PE_create_console', 1),
            ('_PE_init_printf', 1), ('_cpu_machine_idle_init', 1),
            ('_PE_init_platform', 2), ('_pe_arm_debug_init_early', 1),
            ('_ml_map_cpu_pio', 1), ('_cpu_timebase_init', 1),
            ('_kptimer_curcpu_up', 1), ('_PE_init_cpu', 1),
            ('_apt_msg_init', 1), ('_apt_msg_init_cpu', 1),
            ('_fiq_context_init', 1), ('_early_random', 1),
            ('_machine_startup', 1),
        )]
    traced = []
    trace_source = '.section .text.calltrace,"ax"\n'
    definitions = []
    for index, (parent, name, occurrence) in enumerate(specs):
        site, target = tool.find_call(data, kernel_symbols, parent, name, occurrence)
        if parent == '_arm_vm_init' and site > switch_site and not args.trace_past_switch:
            raise SystemExit('Trace must not enter appended code after TTBR1 switch '
                             '(pass --trace-past-switch only with the 0006 segment mapping)')
        # Tail transfers reproduce the original code's branch shape. Only the two
        # call sites the kernel itself reaches with `b` may be treated as tails:
        # _set_mmu_ttb_alternate is entered that way so the reporter stops
        # before TTBR1 changes. The noinline marker helper is an ordinary call.
        #
        # _set_mmu_ttb is an ordinary `bl` call made *after* the switch. It is not
        # in this set, so it goes through `bl`, the wrapper regains control and
        # writes stage 49. Listing it while it was treated as a tail is why stage
        # 49 never appeared on the device.
        tail = name in ('_arm_vm_init', '_set_mmu_ttb_alternate', '_machine_startup')
        handoff_before = name == '_set_mmu_ttb_alternate'
        handoff_after = name == '_set_mmu_ttb_alternate_probed'
        label, symbol, resume = 'diag_trace_' + str(index), 'diag_original_' + str(index), 'diag_return_' + str(index)
        description = name[1:] if parent == '_arm_init' else 'vm/' + name[1:] + '#' + str(occurrence)
        report_entry = name != '__enable_preemption'
        trace_source += ('.global ' + label + '\n.hidden ' + symbol + '\n.hidden ' + resume + '\n' +
                         label + ':\n  adrp x16,diag_last_stage\n  add x16,x16,:lo12:diag_last_stage\n' +
                         '  mov x17,#' + str(index * 2 + 1) + '\n  str x17,[x16]\n' +
                         (('  adr x17,' + label + '_enter_text\n  bl diag_live_call\n')
                          if report_entry else ''))
        if handoff_before:
            trace_source += ('  adrp x16,diag_kernel_vectors\n  add x16,x16,:lo12:diag_kernel_vectors\n' +
                             '  msr vbar_el1,x16\n  isb\n')
        elif args.trace_switch_probe and name == switch_target:
            trace_source += (
                '  adrp x16,diag_guard_status\n  add x16,x16,:lo12:diag_guard_status\n' +
                '  ldr x16,[x16]\n  cmp x16,#2\n  b.eq 1f\n' +
                '  adrp x16,diag_kernel_vectors\n  add x16,x16,:lo12:diag_kernel_vectors\n' +
                '  msr vbar_el1,x16\n  isb\n1:\n')
        if tail:
            trace_source += ('  adrp x30,' + resume + '\n  add x30,x30,:lo12:' + resume + '\n  b ' + symbol + '\n')
        else:
            trace_source += ('  bl ' + symbol + '\n')
            if handoff_after:
                trace_source += '.global diag_target_report_resume\ndiag_target_report_resume:\n'
            trace_source += (
                             '  adrp x16,diag_last_stage\n  add x16,x16,:lo12:diag_last_stage\n' +
                             '  mov x17,#' + str(index * 2 + 2) + '\n  str x17,[x16]\n' +
                             '  adr x17,' + label + '_return_text\n  bl diag_live_call\n' +
                             (('  adrp x16,diag_kernel_vectors\n  add x16,x16,:lo12:diag_kernel_vectors\n' +
                               '  msr vbar_el1,x16\n  isb\n' +
                               '  stp x0,x1,[sp,#-16]!\n  bl diag_guard_deactivate\n' +
                               '  ldp x0,x1,[sp],#16\n') if handoff_after else '') +
                             '  adrp x30,' + resume + '\n  add x30,x30,:lo12:' + resume + '\n  b ' + resume + '\n')
        trace_source += (label + '_enter_text:\n  .asciz "XNU enter: ' + description + '"\n' +
                         label + '_return_text:\n  .asciz "XNU returned: ' + description + '"\n.balign 4\n')
        definitions += ['--defsym=' + symbol + '=' + hex(target), '--defsym=' + resume + '=' + hex(site + 4)]
        traced.append({'name': name, 'parent': parent, 'occurrence': occurrence,
                       'site': hex(site), 'callee': hex(target), 'label': label,
                       'before_stage': index * 2 + 1, 'after_stage': None if tail else index * 2 + 2,
                       'entry_checkpoint_reported': report_entry,
                       'original_LR_tail_transfer': tail,
                       'restore_kernel_VBAR': handoff_before or handoff_after,
                       'kernel_VBAR_handoff': ('before-call' if handoff_before else
                                               'after-return-report' if handoff_after else None)})
    switch_helper_target = next(address for address, name in kernel_symbols.items()
                                if name == switch_target)
    definitions += [
        '--defsym=diag_target_switch=' + hex(switch_helper_target),
        '--defsym=diag_target_kernel_resume=' + hex(switch_site + 4),
        '--defsym=diag_target_kernel_vectors=' + hex(names['_ExceptionVectorsBase']),
        '--defsym=diag_target_set_mmu_ttb=' + hex(next_vm_target),
    ]
    if not args.trace_switch_probe:
        definitions.append('--defsym=diag_target_report_resume=' + hex(switch_site + 4))
    ncmds, sizeofcmds = struct.unpack_from('<II', data, 16)
    command_at = 32 + sizeofcmds
    if any(data[command_at:command_at + 144]):
        raise SystemExit('Root Mach-O has no guarded spare space for RX and RW diagnostic segments')
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
             '-fno-stack-protector', '-fno-builtin', '-ffreestanding', '-mgeneral-regs-only', '-mstrict-align',
             '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
             '-DDIAG_BUILD_LABEL="' + build_label + '"', '-I' + str(src), '-I' + str(compat)]
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
  . = ALIGN(0x4000); diag_code_end = .; diag_data_start = .;
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
    code_size = table['diag_data_start'] - base
    data_file_size = len(payload) - code_size
    data_size = (data_file_size + 0x3fff) & ~0x3fff
    if code_size <= 0 or code_size % 0x4000 or data_file_size <= 0:
        raise SystemExit('Diagnostic RX/RW segment split is not 16 KiB aligned or has an empty side')
    total_size = code_size + data_size
    if (base | fileoff | code_size | data_size) & 0x3fff:
        raise SystemExit('Diagnostic Mach-O segments are not 16 KiB aligned')
    relocation_count = 0
    for at in range(table['diag_rela_start'] - base, table['diag_rela_end'] - base, 24):
        target, info, _ = struct.unpack_from('<QQq', payload, at)
        if info & 0xffffffff != 1027 or target < base + code_size or target + 8 > base + total_size:
            raise SystemExit('Runtime relocation target is not wholly inside the RW-NX segment')
        relocation_count += 1
    if relocation_count == 0:
        raise SystemExit('Expected PIE relocations into mutable diagnostic state')
    if base + total_size - min(va for va, vs, off, fs in tool.SEGMENTS) >= 0x10000000 - 0x200000:
        raise SystemExit('Diagnostic exceeds loader kernel size constraint')
    data += bytes(fileoff - len(data)) + payload + bytes(total_size - len(payload))
    code_segment = struct.pack('<II16sQQQQiiII', 0x19, 72, b'__KDIAG',
                               base, code_size, fileoff, code_size, 5, 5, 0, 0)
    state_segment = struct.pack('<II16sQQQQiiII', 0x19, 72, b'__KDATRW',
                                base + code_size, data_size, fileoff + code_size,
                                data_file_size, 3, 3, 0, 0)
    data[command_at:command_at + 144] = code_segment + state_segment
    struct.pack_into('<II', data, 16, ncmds + 2, sizeofcmds + 144)
    patch_spans = []

    def patch_bytes(at, replacement, kind):
        if at < 0 or at + len(replacement) > len(original):
            raise SystemExit('Diagnostic instruction patch is outside the original input')
        patch_spans.append({'offset': at, 'before_hex': original[at:at + len(replacement)].hex(),
                            'after_hex': replacement.hex(), 'kind': kind})
        data[at:at + len(replacement)] = replacement

    patch_bytes(tool.file_offset(arm), struct.pack('<I', tool.branch(arm, table['diag_init'])), 'arm_init')
    # Synchronous EL1 faults enter our own VBAR table installed at arm_init.
    panic = names['_panic']
    panic_word = struct.unpack_from('<I', original, tool.file_offset(panic))[0]
    # Keep the original panic entry executable after diagnostic TTBR1 handoff.
    # Pre-handoff synchronous exceptions still use diag_vectors.
    # No default BRK: exception/panic reports must describe the real kernel.
    if args.checkpoint_before_vm:
        patch_bytes(tool.file_offset(call), struct.pack('<I', 0xd4200000 | (0xb713 << 5)), 'explicit_checkpoint')
    for item in traced:
        site = int(item['site'], 16)
        patch_bytes(tool.file_offset(site), struct.pack('<I', tool.branch(site, table[item['label']])), 'call_trace')
    # Reuse three already instruction-tested compact entry/MMU checkpoints.
    compact = src
    compact_checkpoints = []
    for stage, site, target, cave in [(1, tool.ENTRY_VA, tool.START_FIRST_CPU, tool.ENTRY_VA + 0x80),
                                     (2, tool.MMU_ENABLE_VA, tool.MMU_ENABLE_VA + 4, tool.ENTRY_VA + 0x200),
                                     (3, tool.AFTER_MMU_VA, tool.AFTER_MMU_VA + 4, tool.ENTRY_VA + 0x380)]:
        code_source = (compact / ('Compact' + str(stage) + '.S')).read_text()
        code = tool.assemble(code_source, cave, {'target': target}, output)
        at = tool.file_offset(cave)
        if any(original[at:at + 0x180]) or len(code) > 0x180:
            raise SystemExit('Compact checkpoint padding guard failed')
        expected_word = {1: tool.branch(site, target), 2: 0xd5181000, 3: 0xd2a00600}[stage]
        if struct.unpack_from('<I', original, tool.file_offset(site))[0] != expected_word:
            raise SystemExit('Compact checkpoint original-instruction guard failed')
        patch_bytes(at, code, 'compact_code')
        patch_bytes(tool.file_offset(site), struct.pack('<I', tool.branch(site, cave)), 'compact_branch')
        compact_checkpoints.append({'stage': stage, 'site': hex(site), 'target': hex(target),
                                    'cave': hex(cave), 'code_bytes': len(code), 'code_sha256': sha(code)})
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
    if source_snapshot != {str(q.relative_to(root)): sha(q.read_bytes()) for q in sorted(src.rglob('*'))
                           if q.is_file() and '__pycache__' not in q.parts}:
        raise SystemExit('Reporter sources changed during build; do not distribute this output')
    record = {'stage': 'terminal-early-exception-and-Onesend-diagnostic', 'original_sha256': sha(original),
              'kernel_sha256': sha(data), 'kernel_bytes': len(data), 'lzma_sha256': sha(encoded),
              'lzma_bytes': len(encoded), 'segment_va': hex(base), 'segment_fileoff': fileoff,
              'segment_bytes': total_size, 'payload_bytes': len(payload),
              'payload_sha256': sha(payload), 'patch_spans': patch_spans,
              'compact_checkpoints': compact_checkpoints,
              'reporter_tool_sha256': sha(Path(__file__).read_bytes()),
              'code_segment': {'name': '__KDIAG', 'va': hex(base), 'bytes': code_size,
                               'fileoff': fileoff, 'filesize': code_size, 'protection': 'RX'},
              'state_segment': {'name': '__KDATRW', 'va': hex(base + code_size),
                                'bytes': data_size, 'fileoff': fileoff + code_size,
                                'filesize': data_file_size, 'protection': 'RW-NX'},
              'rw_runtime_relocations': relocation_count,
              'arm_init_va': hex(arm), 'arm_init_original_instruction': 'PACIBSP replayed',
              'panic_va': hex(panic), 'panic_original_word': hex(panic_word),
              'panic_hook_installed': False, 'reporter_has_split_RX_and_RW_NX_segments': True,
              'checkpoint_va': hex(call), 'checkpoint_original_bl': hex(original_call),
              'checkpoint_callee': hex(callee),
              'checkpoint_installed': args.checkpoint_before_vm,
              'checkpoint': 'explicit BRK b713 before arm_vm_init' if args.checkpoint_before_vm else 'none on default path',
              'call_trace': traced,
              'symbols': {k: hex(v) for k, v in table.items() if k.startswith('diag_')},
              'qr_source': manifest,
              'reporter_sources': source_snapshot,
              'user_display_request': 'live QR through pre-scheduler platform startup' if args.trace_platform_startup else 'live QR at TTBR1 switch, post-switch VM milestones, and arm_vm_init return',
              'display_build_label': build_label,
              'report_is_full_console_log': False, 'static_single_OS_frame': True,
              'strict_alignment': True, 'live_stage_text': True, 'live_stage_QR': True,
              'live_QR_first_stage': 47, 'live_QR_return_stage': 48,
              'live_QR_first_post_switch_stage': 49, 'VM_boundary_stage': 47,
              'stable_transfer_at_final_checkpoint_only': False,
              'live_checkpoint_stages': [47, 48, 49, 50, 51, 52, 54, 55, 56] + (list(range(57, 90)) if args.trace_platform_startup else []),
              'platform_startup_trace': args.trace_platform_startup,
              'platform_trace_boundary': 'machine_startup entry only; no BSD/CDC worker instrumentation',
              'stage_47_page_table_snapshot': {
                  'root_argument': 'ARG0 before set_mmu_ttb_alternate',
                  'walk': 'TTBR1 4 KiB, T1SZ=26/TG1=2 only; reads L1/L2/L3 descriptors through the old TTBR1 linear alias after AT S1E1R confirms the physical page',
                  'targets': ['switch helper', 'arm_vm_init return PC', 'kernel SP',
                              'kernel exception vectors', 'reporter post-helper continuation',
                              'first post-switch _set_mmu_ttb'],
                  'segments': 'every __KDIAG RX and __KDATRW RW-NX page, including expected physical address and access permissions',
                  'candidate_root_is_read_only': True,
                  'guard': 'Builds a low identity TTBR0 map from the currently active old-TTBR1 mappings, independently of the candidate root, checks read/write translations and walked execute permissions, then takes and returns from BRK #0xb712 before the TTBR1 helper. On success, a fault in the helper uses the physical alias vector/state/stack; TTBR0 and VBAR are restored after helper return. Mapping or self-test failure restores the old roots and falls back to the original vector path.',
                  'guard_changes_ttbr0_temporarily': True,
                  'guard_changes_tcr': False,
                  'hardware_verified': False,
              },
              'normal_QR_replay_seconds': 2, 'normal_QR_draws': 4,
              'QR_cached_variants': 4, 'QR_replay_interval_ms': 500,
              'explicit_export_boot_arg': 'tb-report-export=1',
              'explicit_export_intentionally_pauses_before_VM_switch': True,
              'kernel_vectors': hex(names['_ExceptionVectorsBase']),
              'hardware_verified': False, 'functional_fix_claimed': False}
    (output / 'build-report.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({k: record[k] for k in ['kernel_sha256', 'kernel_bytes', 'segment_bytes', 'checkpoint_va']}, indent=2))


if __name__ == '__main__':
    main()
