"""Assemble reviewed source inputs, invoke official firmware build, verify the actual FD."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

from ci_kernel import ROOT, LOCK, PROFILE, command, status_snapshot
from uefi_fv import inspect

REQUIRED = [
    'UniXNUPkg/Application/UniXNU/UniXNU.inf',
    'UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf',
    'UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.inf',
    'DarwinBootPkg/Driver/DarwinBoot.inf',
    'MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf',
    'MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf',
    'FatPkg/EnhancedFatDxe/Fat.inf',
    'MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf',
    'SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf',
    'tb710fuPkg/Drivers/Tb710fuKeyInputDxe/Tb710fuKeyInputDxe.inf',
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['build', 'collect'])
    args = parser.parse_args()
    source, uefi = ROOT / '.work/UniXNU', ROOT / '.work/uefi-sheng'
    output = ROOT / '.work/firmware-artifacts'
    output.mkdir(parents=True, exist_ok=True)
    work = source / 'out/tb710fu' / PROFILE / 'tb710fu_macos'
    if args.mode == 'collect':
        if (work / 'logs').is_dir():
            shutil.copytree(work / 'logs', output / 'logs', dirs_exist_ok=True)
        for item in (ROOT / 'upstream.lock.json', ROOT / 'unixnu.lock'):
            if item.is_file():
                shutil.copy2(item, output / item.name)
        return
    for path, expected in ((source, LOCK['unixnu']['commit']), (uefi, LOCK['uefi']['commit'])):
        actual = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != expected:
            raise SystemExit('Unexpected source revision: ' + str(path))
    key_test = ROOT / 'firmware/overlay/Platforms/Lenovo/tb710fuPkg/Drivers/Tb710fuKeyInputDxe/Test/tb710fu_keys_host.c'
    key_binary = output / 'tb710fu-keys-host'
    command(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-fsanitize=address,undefined',
             '-fno-omit-frame-pointer', key_test, '-o', key_binary], ROOT, output / 'input-test-build.log')
    command([key_binary], ROOT, output / 'input-test.log')
    for patch in sorted((ROOT / 'firmware/patches').glob('*.patch')):
        command(['git', '-C', uefi, 'apply', '--check', patch], ROOT)
        command(['git', '-C', uefi, 'apply', patch], ROOT)
    shutil.copytree(ROOT / 'firmware/overlay', uefi, dirs_exist_ok=True, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    driver = [sys.executable, str(source / 'unixnu')]
    common = ['-D', str(ROOT), '--os', PROFILE]
    command(driver + ['defconfig', 'tb710fu_macos_defconfig'] + common, source, output / 'defconfig.log')
    status_snapshot(driver, common, source, output / 'status.json')
    command(driver + ['build', 'firmware', '--uefi', str(uefi), '--target', 'DEBUG', '-j', '3'] + common, source, output / 'build-firmware.log')
    fd = uefi / 'Build/tb710fuPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd'
    data = fd.read_bytes()
    if len(data) != 0x300000:
        raise SystemExit('FD size does not match the reviewed 3 MiB layout')
    sys.path.insert(0, str(source / 'scripts'))
    from unixnu.cli import parser as cli_parser
    from unixnu.config import Context
    from unixnu import feeds
    ctx = Context(cli_parser().parse_args(['status'] + common))
    feeds.resolve(ctx)
    edk2 = ctx.modules[('refer', 'edk2')].path
    fields = {'uefi': str(uefi), 'tree': str(ctx.tree), 'edk2': str(edk2)}
    packages = [Path(item.format(**fields)) for item in ctx.board['firmware']['packages']]
    parsed = inspect(data)
    modules = []
    for name in REQUIRED:
        inf = next((root / name for root in packages if (root / name).is_file()), None)
        if inf is None:
            raise SystemExit('Required INF source not found: ' + name)
        match = re.search(r'^\s*FILE_GUID\s*=\s*([0-9A-Fa-f-]{36})', inf.read_text(), re.M)
        if not match or match.group(1).lower() not in parsed['file_guids']:
            raise SystemExit('Required module not found in actual nested FFS volumes: ' + name)
        modules.append({'inf': name, 'file_guid': match.group(1).lower()})
    overlay = [{'path': str(path.relative_to(ROOT / 'firmware/overlay')), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
               for path in sorted((ROOT / 'firmware/overlay').rglob('*')) if path.is_file() and '__pycache__' not in path.parts]
    report = {'stage': 'firmware-built', 'target': 'AARCH64 DEBUG CLANGPDB',
              'device_source_commit': os.environ.get('GITHUB_SHA') or subprocess.check_output(
                  ['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'uefi_source_commit': LOCK['uefi']['commit'], 'framework_commit': LOCK['unixnu']['commit'],
              'fd_base': '0xA7100000', 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
              'required_modules': modules, 'volumes': parsed['volumes'], 'overlay': overlay,
              'vendor_binary_inputs': False, 'uefi_secure_boot': 'development-disabled',
              'abl_changed': False, 'hardware_boot_verified': False}
    report['input'] = {'driver': REQUIRED[-1], 'host_model_test': 'PASS',
                       'physical_keys_verified': False, 'pmic_configuration_writes': False}
    (output / 'firmware-result.json').write_text(json.dumps(report, indent=2) + '\n')
    shutil.copy2(fd, output / 'SILICIUM_UEFI.fd')
    command(['git', '-C', str(uefi), 'submodule', 'status', '--recursive'], ROOT, output / 'uefi-submodules.log')
    for pattern in ('*.map', '*.txt', 'Guid.xref'):
        for path in fd.parent.glob(pattern):
            shutil.copy2(path, output / path.name)
    print(json.dumps({key: report[key] for key in ('stage', 'bytes', 'sha256', 'hardware_boot_verified')}, indent=2))
    summary = os.environ.get('GITHUB_STEP_SUMMARY')
    if summary:
        with open(summary, 'a') as stream:
            stream.write(f'### TB710FU UEFI\n\nFD SHA-256: `{report["sha256"]}`\n\n{len(REQUIRED)} required modules verified in nested FFS volumes. Hardware boot is not yet qualified.\n')


if __name__ == '__main__':
    main()
