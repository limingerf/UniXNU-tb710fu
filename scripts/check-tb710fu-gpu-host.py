#!/usr/bin/env python3
"""Host checks for the TB710FU GPU/Metal milestone-1 wiring.

None of this needs a Mac: the checks read the sources, the firmware manifest and
the build/workflow wiring, and state the invariants the hardware run depends on.

  ABI      IOAccelerator is declared as a plain IOService subclass with the
           reviewed 136-byte extent and no virtuals of its own.
  Gating   gpu-metal/gpu-power are read before any object is made, so a default
           boot reaches no new code.
  Metal    the two keys IOGPU's discovery reads carry this port's own plugin
           names, and the service is registered, not merely allocated.
  No MMIO  the power translation unit maps no device memory and writes no
           register in this milestone.
  Payload  the firmware manifest names the four gen70900 images with the
           reference project's sizes and digests, and the addon paths fit the
           160-byte NGPUADD2 entry limit.
  Wiring   defconfig, board.toml and Macos27Services.cpp all carry the new
           archive in the order preModLoad requires; the GPU addon workflow is
           manual-only and reuses the framework's scripts.
"""
import argparse
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'package/Tb710fuGpu'

FIRMWARE = [
    ('gmu_gen70900.bin', 78588, '9685f62c42543befcaee68d25355e16bf55cdb0d573bddcc9f6633f59eec3f72'),
    ('gen70900_sqe.fw', 85300, '8e2c5ce74140de7e161f14654b44de999855bce01b690fdd18c3358a73498cd1'),
    ('gen70900_aqe.fw', 33176, '25237c161d727b8ea92b4beb837a31651b44ea78bfc34d95672af8f346a45825'),
    ('gen70900_zap.mbn', 12088, '890f7e9da007890a40804d3110169ccc887e8a03bdf29e8b7bc3df0016bf9860'),
]
# The address window the reference project records for this SoC. Used only to
# prove the documentation and the census agree; nothing maps them here.
SOC_WINDOWS = {
    'gpu': (0x3d00000, 0x40000),
    'gmu': (0x3d68000, 0x37000),
    'gpucc': (0x3d90000, 0x1c000),
    'kgsl_smmu': (0x3da0000, 0x40000),
}

results = []


def check(name, condition, detail=''):
    results.append({'check': name, 'result': 'PASS' if condition else 'FAIL', 'detail': detail})
    if not condition:
        print(f'FAIL {name}: {detail}')
    else:
        print(f'PASS {name}' + (f': {detail}' if detail else ''))


def read(path):
    return path.read_text(encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, help='write a JSON report here')
    parser.add_argument('--reference', type=Path,
                        default=Path(os.environ['UNIXNU_ZORN_REFERENCE']) if os.environ.get('UNIXNU_ZORN_REFERENCE') else None,
                        help='optional reference checkout (UniXNU-zorn) to cross-check the firmware pins')
    args = parser.parse_args()

    abi = read(PACKAGE / 'Tb710fuGpuABI.h')
    accelerator = read(PACKAGE / 'Tb710fuGpuAccelerator.cpp')
    power = read(PACKAGE / 'Tb710fuGpuPower.cpp')
    header = read(PACKAGE / 'Tb710fuGpu.h')

    # --- ABI -----------------------------------------------------------------
    body = abi.split('IOAccelerator : public IOService', 1)[1].split('};', 1)[0]
    check('abi.inherits_ioservice', 'IOAccelerator : public IOService' in abi)
    check('abi.abstract_structors', 'IOAccelerator(const OSMetaClass *meta) : IOService(meta)' in body)
    check('abi.no_own_virtuals', 'virtual' not in body,
          'the declared base adds no virtual of its own; overrides land on IOService slots')
    check('abi.instance_extent',
          'static_assert(sizeof(IOService) == 136' in abi and 'static_assert(sizeof(IOAccelerator) == 136' in abi)
    check('abi.define_subclass',
          'OSDefineBasicStructors(Tb710fuGpuAccelerator, IOAccelerator)' in accelerator)
    check('abi.deferred_constructor',
          '__GLOBAL__sub_I_Tb710fuGpuAccelerator.cpp' in accelerator)

    # --- Gating --------------------------------------------------------------
    check('gate.metal_boot_arg', 'PE_parse_boot_argn("gpu-metal"' in accelerator)
    first_new = accelerator.find('new Tb710fuGpuAccelerator')
    gate = accelerator.find('PE_parse_boot_argn("gpu-metal"')
    check('gate.gpu_metal_before_allocation', 0 <= gate < first_new,
          f'parse at {gate}, allocation at {first_new}')
    check('gate.early_return', 'if (metal != 1)' in accelerator and 'accelerator not published' in accelerator)
    check('gate.power_boot_arg', 'PE_parse_boot_argn("gpu-power"' in power)
    check('gate.no_unconditional_start', 'if (!tb710fu_gpu_wanted())' in accelerator)
    check('gate.silent_default', 'printf' not in power.split('extern "C" int tb710fu_gpu_wanted(void)')[1].split('}')[0],
          'the wanted() gate prints nothing, so a default boot is unchanged')

    # --- Metal discovery -----------------------------------------------------
    check('metal.plugin_name', 'setProperty("MetalPluginName", "NativeAdrenoMetal")' in accelerator)
    check('metal.plugin_class', 'setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice")' in accelerator)
    check('metal.sandbox_grant', 'setProperty("NativeSandboxGrantProcess", "*")' in accelerator)
    check('metal.registered', 'accelerator->registerService();' in accelerator)
    check('metal.attached_to_platform', 'IOService::getPlatform()' in accelerator and 'accelerator->attach(platform)' in accelerator)
    check('metal.no_start_override', '::start(' not in accelerator,
          'the service is published with attach+registerService; no provider is started in milestone 1')
    check('metal.no_user_client', 'return kIOReturnUnsupported;' in accelerator)
    check('metal.no_console_write_in_open',
          'printf' not in accelerator.split('Tb710fuGpuAccelerator::newUserClient')[1].split('return kIOReturnUnsupported')[0])

    # --- No MMIO in the power milestone -------------------------------------
    mmio_markers = ['IODeviceMemory', 'mapDeviceMemory', 'setMemory', 'IOMemoryMap', 'withRange',
                    'read32(', 'write32(', 'IOPCIDevice', 'volatile uint32_t *', 'reg = base']
    found = [marker for marker in mmio_markers if marker in power]
    check('power.no_mmio', not found, 'markers: ' + (', '.join(found) if found else 'none'))
    check('power.census_only', 'SecureDTLookupEntry' in power and 'SecureDTGetProperty' in power)
    check('power.addon_census', 'native_ram_block_gpu_addon' in power and 'tb710fu_addon_find' in power)
    check('power.domain_gated', 'tb710fu_gpu_stats[TB710FU_GPU_POWER_LEVEL] < 2' in power)

    # --- Firmware manifest ---------------------------------------------------
    manifest = json.loads(read(ROOT / 'scripts/data/tb710fu-gpu-firmware.json'))
    names = {item['name']: (item['bytes'], item['sha256']) for item in manifest['files']}
    for name, size, digest in FIRMWARE:
        check(f'firmware.{name}', names.get(name) == (size, digest),
              f'manifest {names.get(name)} vs reference {(size, digest)}')
    check('firmware.digest_shape',
          all(re.fullmatch(r'[0-9a-f]{64}', item['sha256']) for item in manifest['files']))
    check('firmware.addon_prefix', manifest['addon_prefix'] == 'Gpu/Firmware')
    check('firmware.not_loaded_in_m1', manifest['loads_in_milestone_1'] is False)
    longest = max(len(manifest['addon_prefix']) + 1 + len(item['name']) for item in manifest['files'])
    check('firmware.path_limit', longest < 160, f'longest addon path {longest} bytes')

    # --- Wiring --------------------------------------------------------------
    defconfig = read(ROOT / 'configs/tb710fu_macos_defconfig')
    check('wiring.defconfig_archive',
          '-force_load,{tree}/build/tb710fu-gpu/libTb710fuGpu.a' in defconfig
          or '-force_load,./build/tb710fu-gpu/libTb710fuGpu.a' in defconfig)
    check('wiring.defconfig_define', '-DXNU_TB710FU_GPU=1' in defconfig)
    board = read(ROOT / 'board.toml')
    check('wiring.board_archive', 'build-tb710fu-gpu.py' in board)

    services = read(ROOT / 'package/Macos27Services.cpp')
    # The call sites, not the weak declarations at the top of the file.
    probe = services.find('if (tb710fu_gpu_probe)')
    premod = services.find('OSMetaClass::preModLoad')
    meta = services.find('if (tb710fu_gpu_meta_init)')
    postmod = services.find('OSMetaClass::postModLoad')
    start = services.find('if (tb710fu_gpu_start)')
    check('wiring.services_order',
          0 <= probe < premod < meta < postmod and start > postmod,
          f'probe={probe} preModLoad={premod} meta={meta} postModLoad={postmod} start={start}')
    check('wiring.services_power_meta', 'if (tb710fu_gpu_power_meta_init)' in services)
    check('wiring.services_weak_imports',
          services.count('__attribute__((weak_import))') >= 4,
          'a kernel built without the archive must still load')

    addon_script = read(ROOT / 'scripts/build-tb710fu-gpu-addon.py')
    check('wiring.addon_prefix', "'--prefix', 'GPUBundles'" in addon_script)
    check('wiring.addon_turnip', "'--target', 'macos27'" in addon_script and 'build-turnip-darwin.py' in addon_script)
    check('wiring.addon_plugin', 'build-adreno-metal-plugin.py' in addon_script)
    check('wiring.addon_trust', 'trust-append-module.py' in addon_script)
    check('wiring.addon_no_repo_edit', 'sed -i' not in addon_script and '--local-module' not in addon_script,
          'the framework scripts are called as they are')
    check('wiring.addon_darwin_only', "platform.system() != 'Darwin'" in addon_script)

    workflow = ROOT / '.github/workflows/macos-gpu-addon.yml'
    check('wiring.workflow_present', workflow.is_file())
    if workflow.is_file():
        text = read(workflow)
        check('wiring.workflow_manual_only', 'workflow_dispatch' in text and '\n  push:' not in text and '\n  pull_request:' not in text)
        check('wiring.workflow_calls_device_builder', 'scripts/build-tb710fu-gpu-addon.py' in text)
        check('wiring.workflow_pins',
              '0c1bac3d51a3009dca1708b15a5198f4b43d057a' in text and '6fa7bae2a5b3b523e790eb3e0348af4d98969e4d' in text,
              'framework and Adreno driver revisions are explicit')
        check('wiring.workflow_publish_input', 'inputs.publish' in text)
        check('wiring.workflow_artifact', 'upload-artifact' in text)

    docs = ROOT / 'docs/GPU_METAL_M1.md'
    check('docs.present', docs.is_file())
    if docs.is_file():
        text = read(docs)
        for arg in ('gpu-metal=1', 'gpu-power=', 'gpu-addon=1', 'gpu-bundle-system=1'):
            check(f'docs.boot_arg.{arg}', arg in text)
        check('docs.payload_flag', '--gpu-addon' in text)
        check('docs.no_desktop_claim', 'WindowServer' in text)

    failed = [item for item in results if item['result'] == 'FAIL']
    report = {'schema': 'tb710fu-gpu-host-checks-v1', 'results': results,
              'summary': {'passed': len(results) - len(failed), 'failed': len(failed)},
              'soc_windows': {name: list(value) for name, value in SOC_WINDOWS.items()}}
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    if args.reference and (args.reference / 'native/gpu-owned/firmware-pins.json').is_file():
        pins = json.loads(read(args.reference / 'native/gpu-owned/firmware-pins.json'))
        expected = {name: digest for name, _, digest in FIRMWARE}
        for key, name in (('gmu', 'gmu_gen70900.bin'), ('sqe', 'gen70900_sqe.fw'),
                          ('aqe', 'gen70900_aqe.fw'), ('zap', 'gen70900_zap.mbn')):
            check(f'reference.{key}', pins[key]['sha256'] == expected[name],
                  'cross-checked against the reference project pins')
    print(f'\n{len(results) - len(failed)} passed, {len(failed)} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
