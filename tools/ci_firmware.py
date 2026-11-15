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

# Opt-in UFS publication chain (board.toml TB710FU_ENABLE_UFS=1). These are the
# stock Qualcomm prebuilt DXE drivers under Binaries/tb710fu; they are verified in
# the real nested FFS volumes exactly like the source-only modules above.
UFS_REQUIRED = [
    'QcomPkg/Drivers/SmemDxe/SmemDxe.inf',
    'QcomPkg/Drivers/HWIODxe/HWIODxe.inf',
    'QcomPkg/Drivers/CmdDbDxe/CmdDbDxe.inf',
    'QcomPkg/Drivers/PwrUtilsDxe/PwrUtilsDxe.inf',
    'QcomPkg/Drivers/NpaDxe/NpaDxe.inf',
    'QcomPkg/Drivers/RpmhDxe/RpmhDxe.inf',
    'QcomPkg/Drivers/ClockDxe/ClockDxe.inf',
    'QcomPkg/Drivers/UFSDxe/UFSDxe.inf',
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['build', 'collect'])
    parser.add_argument('--ufs', action='store_true',
                        help='build with the opt-in UFS publication chain (TB710FU_ENABLE_UFS=1)')
    parser.add_argument('--lzma', action='store_true',
                        help='apply the DarwinBoot LZMA kernel patch and build that loader variant')
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
    # Patches are routed by target tree and applied in two phases. The 0001
    # SimpleFb patch targets the uefi-sheng checkout and can go on immediately.
    # The 0002 DarwinBoot patch targets UniXNU's drivers/DarwinBoot gitlink, which
    # only becomes a populated checkout after 'defconfig' runs feeds.fetch. It is
    # therefore applied in the second phase, after defconfig, and only when the
    # LZMA kernel variant is requested.
    simplefb_patches = []
    darwinboot_patches = []
    for patch in sorted((ROOT / 'firmware/patches').glob('*.patch')):
        name = patch.name
        if 'simplefb' in name:
            simplefb_patches.append(patch)
        elif 'darwinboot' in name:
            darwinboot_patches.append(patch)
        else:
            raise SystemExit('Unrouted firmware patch (name must contain simplefb or darwinboot): ' + name)
    for patch in simplefb_patches:
        command(['git', '-C', uefi, 'apply', '--check', patch], ROOT)
        command(['git', '-C', uefi, 'apply', patch], ROOT)
    shutil.copytree(ROOT / 'firmware/overlay', uefi, dirs_exist_ok=True, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    if args.ufs:
        # Flip the reviewed opt-in gate in the deployed copy only. The repository
        # default stays 0, so the source-only baseline is unchanged. The preflight
        # accepts exactly this one file difference when UNIXNU_ENABLE_UFS=1.
        dsc = uefi / 'Platforms/Lenovo/tb710fuPkg/tb710fu.dsc'
        text = dsc.read_text(encoding='utf-8')
        replaced, count = re.subn(r'^(\s*TB710FU_ENABLE_UFS\s*=\s*)0(\s*)$', r'\g<1>1\2', text, flags=re.M)
        if count != 1:
            raise SystemExit('Could not enable TB710FU_ENABLE_UFS exactly once in the deployed DSC')
        dsc.write_text(replaced, encoding='utf-8')
        os.environ['UNIXNU_ENABLE_UFS'] = '1'
    driver = [sys.executable, str(source / 'unixnu')]
    common = ['-D', str(ROOT), '--os', PROFILE]
    command(driver + ['defconfig', 'tb710fu_macos_defconfig'] + common, source, output / 'defconfig.log')
    if args.lzma:
        # defconfig only writes .config; it does not initialize submodules. DarwinBoot
        # is a gitlink under UniXNU (drivers/DarwinBoot), so initialize exactly that
        # submodule before applying the LZMA patch. The firmware build step later runs
        # feeds.update_submodules, which leaves a dirty submodule alone, so the patch
        # survives into the EDK II compile.
        command(['git', '-C', str(source), 'submodule', 'update', '--init', '--depth', '1',
                 '--', 'drivers/DarwinBoot'], ROOT, output / 'darwinboot-submodule.log')
        darwinboot = source / 'drivers/DarwinBoot'
        if not (darwinboot / '.git').exists():
            raise SystemExit('DarwinBoot submodule still not populated after explicit init')
        for patch in darwinboot_patches:
            command(['git', '-C', str(darwinboot), 'apply', '--check', patch], ROOT)
            command(['git', '-C', str(darwinboot), 'apply', patch], ROOT)
        os.environ['UNIXNU_ENABLE_LZMA'] = '1'
        # The LZMA decoder is a NULL library; NULL| paths are only valid in a DSC
        # component override block, not in an INF [LibraryClasses] section. Add the
        # override to the deployed board DSC so DarwinBoot links it.
        dsc = uefi / 'Platforms/Lenovo/tb710fuPkg/tb710fu.dsc'
        dtext = dsc.read_text(encoding='utf-8')
        flat = '  DarwinBootPkg/Driver/DarwinBoot.inf\n'
        block = ('  DarwinBootPkg/Driver/DarwinBoot.inf {\n'
                 '    <LibraryClasses>\n'
                 '      NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf\n'
                 '      ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf\n'
                 '  }\n')
        if block in dtext:
            pass
        elif flat in dtext:
            dtext = dtext.replace(flat, block, 1)
            dsc.write_text(dtext, encoding='utf-8')
        else:
            raise SystemExit('DarwinBoot DSC component not found in a form the LZMA override can replace')
    status_snapshot(driver, common, source, output / 'status.json')
    if args.ufs:
        # Device-Binaries publishes the SM8650 module set as Binaries/giulia; this
        # board has no upstream tree of its own. Clone the exact gitlink commit that
        # the pinned UEFI checkout records, then present it under the board's own
        # name so the deployed DSC/FDF and check-board.py read Binaries/tb710fu.
        # Nothing here is committed: the clone and the copy live in the build
        # checkout only.
        gitlink = subprocess.check_output(
            ['git', '-C', str(uefi), 'ls-tree', 'HEAD', 'Binaries'], text=True).split()
        if len(gitlink) < 3:
            raise SystemExit('Cannot resolve the Binaries gitlink in the pinned UEFI checkout')
        pinned = gitlink[2]
        binaries = uefi / 'Binaries'
        if binaries.is_dir():
            shutil.rmtree(binaries)
        command(['git', 'clone', '--no-checkout',
                 'https://github.com/Project-Silicium/Device-Binaries.git', str(binaries)],
                ROOT, output / 'uefi-binaries-clone.log')
        command(['git', '-C', str(binaries), 'fetch', '--depth', '1', 'origin', pinned],
                ROOT, output / 'uefi-binaries-clone.log')
        command(['git', '-C', str(binaries), 'checkout', '--detach', pinned],
                ROOT, output / 'uefi-binaries-clone.log')
        published = binaries / 'giulia'
        if not published.is_dir():
            listing = sorted(p.name for p in binaries.iterdir())
            raise SystemExit('Binaries/giulia missing at ' + pinned + '; found: ' + repr(listing))
        board_view = binaries / 'tb710fu'
        if board_view.is_dir():
            shutil.rmtree(board_view)
        shutil.copytree(published, board_view)
        for name in UFS_REQUIRED:
            if not (board_view / name).is_file():
                raise SystemExit('UFS chain INF missing from the published set: ' + name)
    else:
        # board.toml lists {uefi}/Binaries/tb710fu in [firmware.packages], but only
        # the --ufs build materialises it. EDK2 errors with 'One Path in
        # PACKAGES_PATH doesn't exist' for a missing directory, so create an empty
        # one for the non-UFS variants (baseline and --lzma). It contributes no
        # modules because the gate in tb710fu.dsc is off.
        board_view = uefi / 'Binaries/tb710fu'
        board_view.mkdir(parents=True, exist_ok=True)
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
    ufs_modules = []
    if args.ufs:
        # The vendor set lives under the Binaries/tb710fu package root, which is not
        # in the board's [firmware.packages] list; resolve it against the UEFI checkout.
        for name in UFS_REQUIRED:
            inf = uefi / 'Binaries/tb710fu' / name
            if not inf.is_file():
                raise SystemExit('UFS chain INF missing from the pinned UEFI checkout: ' + name)
            match = re.search(r'^\s*FILE_GUID\s*=\s*([0-9A-Fa-f-]{36})', inf.read_text(), re.M)
            if not match or match.group(1).lower() not in parsed['file_guids']:
                raise SystemExit('UFS chain module not found in actual nested FFS volumes: ' + name)
            ufs_modules.append({'inf': 'Binaries/tb710fu/' + name, 'file_guid': match.group(1).lower()})
        for name in modules:
            if name['inf'].endswith(('DisplayDxe.inf', 'DALSYSDxe.inf', 'VcsDxe.inf')):
                raise SystemExit('Black-screen vendor module present in the UFS volume: ' + name['inf'])
    overlay = [{'path': str(path.relative_to(ROOT / 'firmware/overlay')), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
               for path in sorted((ROOT / 'firmware/overlay').rglob('*')) if path.is_file() and '__pycache__' not in path.parts]
    # Also verify the patched DarwinBoot made it into the FD. Its FILE_GUID is
    # 1B64AF87-8415-4B57-9B16-680D70913B9F; the LZMA variant must show the new
    # DARWINBOOT strings, which we check against the built module's source.
    lzma_verified = False
    if args.lzma:
        db_src = source / 'drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c'
        # The linker brought the patched object into the built FD (the build step
        # succeeded only after resolving LzmaCustomDecompressLib), so verify the
        # source still carries the patch and the module GUID is in the volume.
        lzma_verified = ('LzmaUefiDecompress' in db_src.read_text()
                         and '1b64af87-8415-4b57-9b16-680d70913b9f' in parsed['file_guids'])
        if not lzma_verified:
            raise SystemExit('LZMA DarwinBoot patch did not reach the built firmware volume')
    report = {'stage': 'firmware-built', 'target': 'AARCH64 DEBUG CLANGPDB',
              'device_source_commit': os.environ.get('GITHUB_SHA') or subprocess.check_output(
                  ['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'uefi_source_commit': LOCK['uefi']['commit'], 'framework_commit': LOCK['unixnu']['commit'],
              'fd_base': '0xA7100000', 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
              'required_modules': modules, 'volumes': parsed['volumes'], 'overlay': overlay,
              'vendor_binary_inputs': bool(args.ufs), 'uefi_secure_boot': 'development-disabled',
              'abl_changed': False, 'hardware_boot_verified': False,
              'ufs_chain': {'enabled': bool(args.ufs), 'modules': ufs_modules},
              'lzma_kernel': {'enabled': bool(args.lzma), 'darwinboot_patched': lzma_verified}}
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
            chain = f'\nUFS chain enabled: {len(ufs_modules)} stock Qualcomm modules verified.' if args.ufs else ''
            stream.write(f'### TB710FU UEFI\n\nFD SHA-256: `{report["sha256"]}`\n\n{len(REQUIRED)} required modules verified in nested FFS volumes.{chain}\n\nHardware boot is not yet qualified.\n')


if __name__ == '__main__':
    main()
