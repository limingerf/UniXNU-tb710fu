#!/usr/bin/env python3
"""Check reviewed TB710FU source policy; this is not a hardware qualification."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import tomllib


def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--uefi-root', type=Path, help='Optional pinned UEFI base for inherited DSC checks')
    parser.add_argument('--fd', type=Path, help='Optional compiled FD for exact length check')
    args = parser.parse_args()
    board = Path(__file__).resolve().parents[1]
    overlay = board.parents[2]
    config = tomllib.loads((overlay / 'Resources/Configs/tb710fu.toml').read_text(encoding='utf-8'))
    require(config['uefi_fd'] == {'base': 0xA7100000, 'size': 0x300000}, 'FD base or size changed without review')
    require(config['boot_image']['header_version'] == 4, 'Android boot header must remain v4')
    require(config['boot_image']['pagesize'] == 4096, 'Boot page size must remain 4096')

    text = (board / 'Library/MemoryMapLib/MemoryMapLib.c').read_text(encoding='utf-8')
    pattern = r'\{"([^"]+)",\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+)\}'
    regions = []
    for name, base, size, hob, resource, attributes, kind, arm in re.findall(pattern, text):
        regions.append({'name': name, 'base': int(base, 16), 'size': int(size, 16), 'hob': hob, 'resource': resource, 'attributes': attributes, 'kind': kind, 'arm': arm})
    require(len(regions) == 40, f'Expected 40 reviewed regions; found {len(regions)}')
    require(len({r['name'] for r in regions}) == len(regions), 'Duplicate memory region name')
    for region in regions:
        require(region['size'] > 0 and region['base'] + region['size'] <= 1 << 64, 'Invalid range: ' + region['name'])
    ordered = sorted(regions, key=lambda r: r['base'])
    for left, right in zip(ordered, ordered[1:]):
        require(left['base'] + left['size'] <= right['base'], 'Overlapping map regions: ' + left['name'] + '/' + right['name'])
    by_name = {r['name']: r for r in regions}
    expected = {
        'FD_Reserved': (0xA7000000, 0x100000),
        'UEFI_FD': (0xA7100000, 0x300000),
        'Display_Demura': (0xA3600000, 0x2B00000),
        'Display_Reserved': (0xD5100000, 0x2B00000),
        'RAM_High': (0x8C0000000, 0xC0000000),
    }
    for name, pair in expected.items():
        require((by_name[name]['base'], by_name[name]['size']) == pair, 'Critical region changed: ' + name)
    require(by_name['DXE_Heap']['base'] + by_name['DXE_Heap']['size'] == 0xD3000000, 'DXE heap must stop before reference rmtfs')
    protected = [
        ('rmtfs', 0xD3000000, 0x400000),
        ('hwfence', 0xD4E23000, 0x2DD000),
        ('ramoops', 0xAC300000, 0x100000),
        ('demura', 0xA3600000, 0x2B00000),
        ('scanout', 0xD5100000, 0x2B00000),
    ]
    for region in regions:
        if region['kind'] != 'Conv':
            continue
        for name, base, size in protected:
            require(not (region['base'] < base + size and base < region['base'] + region['size']), 'Conventional memory overlaps ' + name)

    dsc = (board / 'tb710fu.dsc').read_text(encoding='utf-8')
    for gate in ('USE_CUSTOM_DISPLAY_DRIVER', 'TB710FU_VENDOR_HW', 'ENABLE_SECUREBOOT'):
        require(re.search(r'^\s*' + gate + r'\s*=\s*0\s*$', dsc, re.M), 'Unsafe gate: ' + gate)
        require('!if $(' + gate + ') != 0' in dsc, 'Missing closed-gate rejection: ' + gate)
    require('gArmPlatformTokenSpaceGuid.PcdCoreCount|6' in dsc, 'SMBIOS six-core override missing')
    require('gQcomPkgTokenSpaceGuid.PcdBuildXblHobs|FALSE' in dsc, 'Stock XBL HOB scan must remain disabled')
    config_map = (board / 'Library/ConfigurationMapLib/ConfigurationMapLib.c').read_text(encoding='utf-8')
    for name in ('NumCpus', 'NumActiveCores'):
        require('{"' + name + '", 8}' in config_map, 'Opaque vendor table changed without consumer review: ' + name)

    fv = '\n'.join((board / p).read_text(encoding='utf-8') for p in ('tb710fu.fdf', 'Include/DXE.inc', 'Include/APRIORI.inc'))
    required = [
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
    for path in required:
        require('INF ' + path in fv, 'Required module missing from FDF: ' + path)
    key_inf = required[-1]
    require(key_inf in dsc, 'Board DSC does not compile the physical-key input')
    apriori = (board / 'Include/APRIORI.inc').read_text(encoding='utf-8')
    require(apriori.index(key_inf) < apriori.index('MdeModulePkg/Universal/BdsDxe/BdsDxe.inf'), 'Input must dispatch before BDS')

    # The source-only baseline must not reference vendor binaries. The one reviewed
    # exception is the opt-in UFS publication chain behind TB710FU_ENABLE_UFS: those
    # modules are the stock Qualcomm PE32 DXE drivers under Binaries/giulia and are
    # gated so the default build is byte-for-byte the audited source-only volume.
    ufs_enabled = re.search(r'^\s*TB710FU_ENABLE_UFS\s*=\s*1\s*$', dsc, re.M) is not None
    if ufs_enabled:
        # Only these exact modules may appear, and only under the UFS gate.
        allowed_vendor = (
            'Binaries/giulia/QcomPkg/Drivers/SmemDxe/SmemDxe.inf',
            'Binaries/giulia/QcomPkg/Drivers/HWIODxe/HWIODxe.inf',
            'Binaries/giulia/QcomPkg/Drivers/CmdDbDxe/CmdDbDxe.inf',
            'Binaries/giulia/QcomPkg/Drivers/PwrUtilsDxe/PwrUtilsDxe.inf',
            'Binaries/giulia/QcomPkg/Drivers/NpaDxe/NpaDxe.inf',
            'Binaries/giulia/QcomPkg/Drivers/RpmhDxe/RpmhDxe.inf',
            'Binaries/giulia/QcomPkg/Drivers/ClockDxe/ClockDxe.inf',
            'Binaries/giulia/QcomPkg/Drivers/UFSDxe/UFSDxe.inf',
        )
        for path in allowed_vendor:
            require('INF ' + path in fv, 'UFS chain module missing from DXE/APRIORI: ' + path)
            require(path in dsc, 'UFS chain module not declared in DSC Components: ' + path)
        # The 2026-11-11 all-vendor black-screen set must stay out even with UFS on.
        still_forbidden = ('DALSYSDxe.inf', 'VcsDxe.inf', 'DisplayDxe.inf',
                           'GpiDxe.inf', 'SPIDxe.inf', 'I2CDxe.inf', 'SdccDxe.inf',
                           'RamManagerDxe.inf')
        for token in still_forbidden:
            require(token not in fv and token not in dsc,
                    'Black-screen vendor module must stay out of the UFS gate: ' + token)
        forbidden = still_forbidden
    else:
        forbidden = ('Binaries/', 'ClockDxe.inf', 'SPMIDxe.inf', 'DisplayDxe.inf', 'RamManagerDxe.inf')

    def strip_ufs_gate(text):
        """Drop everything between !if $(TB710FU_ENABLE_UFS) == 1 and its !endif.

        The UFS chain is opt-in and reviewed separately, so the source-only
        baseline check must judge the volume the default build actually makes.
        """
        out = []
        depth = 0
        for line in text.splitlines(keepends=True):
            stripped = line.strip()
            if depth == 0 and re.match(r'^!if\s+.*TB710FU_ENABLE_UFS.*==\s*1', stripped):
                depth = 1
                continue
            if depth:
                if re.match(r'^!if\b', stripped):
                    depth += 1
                elif re.match(r'^!endif\b', stripped):
                    depth -= 1
                continue
            out.append(line)
        return ''.join(out)

    for path in overlay.rglob('*'):
        if not path.is_file():
            continue
        require(path.stat().st_size > 0, 'Empty placeholder file: ' + str(path.relative_to(overlay)))
        require(path.suffix.lower() not in ('.efi', '.fd', '.img', '.pem', '.key'), 'Binary or key in source overlay')
        if path.suffix.lower() not in ('.c', '.h', '.inf', '.dsc', '.fdf', '.inc', '.toml'):
            continue
        content = strip_ufs_gate(path.read_text(encoding='utf-8'))
        require(not any(token in content for token in forbidden), 'Forbidden firmware path in ' + str(path.relative_to(overlay)))
        require(not re.search(r'(?<![A-Za-z0-9])/(?:opt|home|mnt|Users)/', content) and not re.search(r'(?<![A-Za-z0-9])[A-Za-z]:[/\\]', content), 'Absolute local path in board source')

    if args.uefi_root:
        common = args.uefi_root / 'Silicon/Silicium/SiliciumPkg/SiliciumPkg.dsc.inc'
        inherited = common.read_text(encoding='utf-8')
        for path in required[4:9]:
            require(path in inherited, 'Inherited DSC does not build ' + path)
        require('HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf' in inherited, 'Inherited HobLib differs from audited baseline')
    if args.fd:
        require(args.fd.is_file() and args.fd.stat().st_size == config['uefi_fd']['size'], 'Compiled FD length does not match configured size')
    digest = hashlib.sha256(json.dumps(regions, sort_keys=True).encode()).hexdigest()
    print(json.dumps({'result': 'PASS', 'memory_regions': len(regions), 'memory_table_sha256': digest, 'required_fv_modules': len(required), 'vendor_binary_references': 0, 'hardware_qualified': False}, sort_keys=True))


if __name__ == '__main__':
    try:
        main()
    except (OSError, KeyError, ValueError) as error:
        print('TB710FU preflight: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
