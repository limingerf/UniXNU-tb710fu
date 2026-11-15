"""Prepare an unsigned, guarded entry-bar probe using the pinned official tool.

The official script is not edited. This adapter only strips llvm-nm fileset
headings that its three-column parser mistakes for symbols. Its assembly,
instruction checks, empty-padding checks and branch-range checks remain active.
No device commands, signing, firmware build or source-cache writes occur.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--framework', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--sha256', required=True, help='Required original kernel identity')
    parser.add_argument('--output', type=Path, required=True, help='New Mach-O copy; never overwritten')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    lock = json.loads((root / 'upstream.lock.json').read_text(encoding='utf-8'))
    revision = lock['unixnu']['commit']
    relative = 'scripts/patch-xnu-entry-probe.py'
    tool = args.framework / relative
    pinned = subprocess.check_output(['git', '-C', str(args.framework), 'show', revision + ':' + relative])
    if sha(tool.read_bytes()) != sha(pinned):
        raise SystemExit('Official probe differs from the pinned framework source')
    original = args.kernel.read_bytes()
    if sha(original) != args.sha256.lower():
        raise SystemExit('Kernel identity mismatch')
    if args.output.resolve() == args.kernel.resolve():
        raise SystemExit('Refuse to modify the original kernel')
    if args.output.exists():
        raise SystemExit('Output exists; choose a new path to preserve earlier evidence')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('xb') as stream:
        stream.write(original)

    spec = importlib.util.spec_from_file_location('pinned_entry_probe', tool)
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)

    def fileset_symbols(kernel):
        nm = shutil.which('llvm-nm')
        if not nm:
            raise SystemExit('llvm-nm required')
        result = subprocess.run([nm, '-n', str(kernel)], capture_output=True, text=True, check=True)
        symbols = {}
        for line in result.stdout.splitlines():
            fields = line.split()
            if len(fields) == 3 and re.fullmatch(r'[0-9a-fA-F]+', fields[0]):
                symbols.setdefault(int(fields[0], 16), fields[2])
        return symbols

    print('Using pinned official probe; adapting llvm-nm fileset headings only', flush=True)
    probe.kernel_symbols = fileset_symbols
    previous = sys.argv
    try:
        sys.argv = [str(tool), str(args.output), '--no-reset']
        probe.main()
    finally:
        sys.argv = previous
    changed = args.output.read_bytes()
    if len(original) != len(changed):
        raise SystemExit('Unexpected kernel size change')
    differences = sum(a != b for a, b in zip(original, changed))
    if differences == 0:
        raise SystemExit('No entry probe was produced')
    report = {
        'stage': 'unsigned-entry-bar-probe', 'framework_commit': revision,
        'official_tool': relative, 'official_tool_sha256': sha(pinned),
        'input_sha256': sha(original), 'output_sha256': sha(changed),
        'bytes': len(changed), 'changed_bytes': differences,
        'kernel_entry_va': hex(probe.ENTRY_VA),
        'start_first_cpu_va': hex(probe.START_FIRST_CPU),
        'before_mmu_enable_va': hex(probe.MMU_ENABLE_VA),
        'markers': {'blue': 'XNU entry executed', 'magenta': 'reached instruction before MMU enable'},
        'automatic_reset_added': False, 'hardware_verified': False,
        'device_operations': False, 'signed': False,
    }
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
