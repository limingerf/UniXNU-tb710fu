"""Bounded source-only cloud compilation of the early reporter, not an XNU rebuild."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def main():
    src = ROOT / 'diagnostics/early-report'
    manifest = json.loads((src / 'qr-sources.json').read_text())
    work = ROOT / '.local/early-fault-qr'
    upstream, compat = work / 'upstream', work / 'compat'
    output = ROOT / '.work/early-report-artifacts'
    output.mkdir(parents=True, exist_ok=True)
    def fetch(record):
        url = 'https://raw.githubusercontent.com/' + manifest['repository'] + '/' + manifest['commit'] + '/' + record['path']
        with urllib.request.urlopen(url, timeout=45) as response:
            data = response.read(1 << 20)
        if hashlib.sha256(data).hexdigest() != record['sha256']:
            raise ValueError('Pinned QR source identity mismatch')
        path = upstream / record['path']
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    with ThreadPoolExecutor(max_workers=3) as pool:
        list(pool.map(fetch, manifest['files']))
    for name in ['Uefi.h', 'Protocol/GraphicsOutput.h', 'Library/BaseMemoryLib.h',
                 'Library/DebugLib.h', 'Library/MemoryAllocationLib.h', 'Library/UefiBootServicesTableLib.h']:
        path = compat / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "Compat.h"\n')
    shutil.copyfile(upstream / 'MsGraphicsPkg/Include/Library/QrEncoderLib.h', compat / 'Library/QrEncoderLib.h')
    qr = upstream / 'MsGraphicsPkg/Library/QrEncoderLib'
    sources = [src / 'Report.c', qr / 'QrEncoderLib.c', qr / 'QrEncoderTables.c']
    flags = ['-std=c11', '-Oz', '-fpie', '-fvisibility=hidden', '-fno-stack-protector',
             '-fno-builtin', '-ffreestanding', '-mgeneral-regs-only',
             '-I' + str(src), '-I' + str(compat)]
    for i, source in enumerate(sources):
        subprocess.run(['clang', '--target=aarch64-none-elf'] + flags +
                       ['-c', str(source), '-o', str(output / ('source-' + str(i) + '.o'))], check=True)
    subprocess.run(['clang', '--target=aarch64-none-elf', '-c', str(src / 'Entry.S'),
                    '-o', str(output / 'entry.o')], check=True)
    subprocess.run(['cc', '-std=c11', '-O1', '-fPIC', '-shared', '-fno-builtin',
                    '-DTB_REPORT_HOST_TEST', '-I' + str(src), '-I' + str(compat)] +
                   list(map(str, sources)) + ['-o', str(work / 'report-host.so')], check=True)
    result = subprocess.run([sys.executable, str(src / 'Test/host_report.py')],
                             cwd=ROOT, text=True, capture_output=True, check=True)
    (output / 'host-test.log').write_text(result.stdout)
    shutil.copyfile(work / 'host-check.json', output / 'host-check.json')
    report = {'stage': 'source-only-reporter-compiled-and-host-tested',
              'device_source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'compiler': subprocess.check_output(['clang', '--version'], text=True).splitlines()[0],
              'qr_source': manifest, 'kernel_rebuilt': False, 'device_operations': False,
              'keys_used': False, 'hardware_verified': False}
    (output / 'cloud-result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
