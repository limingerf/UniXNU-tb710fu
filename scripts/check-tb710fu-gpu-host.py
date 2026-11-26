#!/usr/bin/env python3
"""Host checks for the TB710FU GPU/Metal milestone-2 wiring.

None of this needs a Mac: the checks read the sources, the firmware manifest and
the build/workflow wiring, and state the invariants the hardware run depends on.

  ABI      IOAccelerator is declared as a plain IOService subclass with the
           reviewed 136-byte extent; the native wire ABI v2 keeps its exact
           record sizes (Turnip and the Metal plugin pass them verbatim).
  Gating   gpu-metal/gpu-power are read before any object is made; every stage of
           the gpu-power chain sits behind its own level, and a default boot
           reaches no new code and prints nothing.
  Metal    the two keys IOGPU's discovery reads carry this port's own plugin
           names, and the service is registered, not merely allocated.
  Power    the staged chain writes only the registers the pinned sources define
           (CX GDSC, GPUCC/ GCC clocks, PLL1, RCGs); GX, the GPU window, the
           SMMU context and the DDRSS AXI branch are never written, and the GMU
           cold start / GX power stages refuse instead of guessing.
  Client   newUserClient serves exactly type 0x4147 once the ABI is live, the
           connection speaks the tunnel and the direct selectors, and every
           method that needs a GPU address or ring refuses.
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
    ('gen70900_sqe.fw', 85364, 'f756bafe5f69a87161c7c53385386374a791861a574c244e845baba677993842'),
    ('gen70900_aqe.fw', 33176, '25237c161d727b8ea92b4beb837a31651b44ea78bfc34d95672af8f346a45825'),
    ('gen70900_zap.mbn', 12088, 'c4f086956467a8bc7a67c3cc5eb8edc17eee6fe9390b9f32dd6305d1e2f0f6cb'),
]
# The windows this port maps, from docs/GPU_REFERENCE_A750.md (live ADT) and
# gpucc-sm8650.c. Used only to prove the documentation and the census agree.
SOC_WINDOWS = {
    'gpu': (0x3d00000, 0x40000),
    'gmu': (0x3d68000, 0x37000),
    'gpucc': (0x3d90000, 0xa000),
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


def between(text, start, end):
    if start not in text:
        return ''
    rest = text.split(start, 1)[1]
    return rest.split(end, 1)[0] if end in rest else rest


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
    hw = read(PACKAGE / 'Tb710fuGpuHw.h')
    mmio = read(PACKAGE / 'Tb710fuGpuMmio.cpp')
    firmware = read(PACKAGE / 'Tb710fuGpuFirmware.cpp')
    client = read(PACKAGE / 'Tb710fuGpuClient.cpp')
    client_header = read(PACKAGE / 'Tb710fuGpuClient.h')
    native_abi = read(PACKAGE / 'Tb710fuGpuNativeABI.h')
    build = read(ROOT / 'scripts/build-tb710fu-gpu.py')

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

    # --- Native wire ABI (must match Turnip/the plugin byte for byte) ---------
    check('abi.native_version', 'NativeAdrenoAbiVersion = 2' in native_abi)
    check('abi.native_methods',
          all(name in native_abi for name in ('NativeAdrenoMethodQuery = 1', 'NativeAdrenoMethodBoAlloc = 2',
                                              'NativeAdrenoMethodBoMap = 3', 'NativeAdrenoMethodBoFree = 4',
                                              'NativeAdrenoMethodSubmit = 5', 'NativeAdrenoMethodWaitFence = 6',
                                              'NativeAdrenoMethodGetTimestamp = 7', 'NativeAdrenoMethodGetStatus = 8',
                                              'NativeAdrenoMethodSetMetadata = 9', 'NativeAdrenoMethodGetMetadata = 10',
                                              'NativeAdrenoMethodCreateQueue = 11', 'NativeAdrenoMethodCloseQueue = 12',
                                              'NativeAdrenoMethodBoCpuMap = 13', 'NativeAdrenoMethodBoCpuUnmap = 14',
                                              'NativeAdrenoMethodGetDeviceInfo = 15', 'NativeAdrenoMethodBoImport = 16',
                                              'NativeAdrenoMethodSetPriority = 17', 'NativeAdrenoMethodNote = 18')),
          'selectors 1..18 are the ABI, not an invented numbering')
    check('abi.native_submit_size',
          'static_assert(sizeof(NativeAdrenoAbiSubmit) == 32 + 24 * NativeAdrenoAbiMaxBuffers' in native_abi)
    check('abi.native_metadata_size', 'static_assert(sizeof(NativeAdrenoAbiMetadata) == 80' in native_abi)
    check('abi.native_device_info_size', 'static_assert(sizeof(NativeAdrenoAbiDeviceInfo) == 64' in native_abi)

    # --- Gating --------------------------------------------------------------
    check('gate.metal_boot_arg', 'PE_parse_boot_argn("gpu-metal"' in accelerator)
    first_new = accelerator.find('new Tb710fuGpuAccelerator')
    gate = accelerator.find('PE_parse_boot_argn("gpu-metal"')
    check('gate.gpu_metal_before_allocation', 0 <= gate < first_new,
          f'parse at {gate}, allocation at {first_new}')
    check('gate.early_return', 'if (metal != 1)' in accelerator and 'accelerator not published' in accelerator)
    check('gate.power_boot_arg', 'PE_parse_boot_argn("gpu-power"' in power)
    check('gate.no_unconditional_start', 'if (!tb710fu_gpu_wanted())' in accelerator)
    check('gate.silent_default',
          'printf' not in between(power, 'extern "C" int tb710fu_gpu_wanted(void)', '}'),
          'the wanted() gate prints nothing, so a default boot is unchanged')
    check('gate.bringup_gated', 'if (level < 2)' in power and 'stage 2: map and read' in power,
          'stage 2 is the first level that maps and touches a device window')
    for level, next_level in ((3, 4), (4, 5), (5, 6), (6, 9)):
        check(f'gate.stage_{level}', f'if (level < {next_level})' in power,
              f'stage {level} work only runs when gpu-power >= {level}')

    # --- Metal discovery -----------------------------------------------------
    check('metal.plugin_name', 'setProperty("MetalPluginName", "NativeAdrenoMetal")' in accelerator)
    check('metal.plugin_class', 'setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice")' in accelerator)
    check('metal.sandbox_grant', 'setProperty("NativeSandboxGrantProcess", "*")' in accelerator)
    check('metal.registered', 'accelerator->registerService();' in accelerator)
    check('metal.attached_to_platform', 'IOService::getPlatform()' in accelerator and 'accelerator->attach(platform)' in accelerator)
    check('metal.no_start_override', '::start(' not in accelerator,
          'the service is published with attach+registerService; no provider is started')

    # --- User client ---------------------------------------------------------
    check('client.open_type', 'type != 0x4147u' in accelerator,
          'only the Metal plugin type (AG) reaches the ABI connection')
    check('client.gated_by_abi', 'tb710fu_gpu_abi_ready()' in accelerator)
    check('client.factory', 'tb710fu_gpu_client_create(' in accelerator and 'tb710fu_gpu_client_create(' in client)
    check('client.deferred_install',
          'tb710fu_gpu_client_static_init()' in accelerator and
          '__GLOBAL__sub_I_Tb710fuGpuClient.cpp' in client_header)
    check('client.metaclass_name', 'AGXDeviceUserClient' in client and 'OSDefineMetaClassAndStructors(AGXDeviceUserClient, IOUserClient)' in client,
          'the sandbox profiles admit this user-client class name')
    check('client.tunnel_magic', '0x4e544e4c' in client and '0xffffffffu' in client,
          'NTNL tunnel and its admission probe')
    check('client.selector_dispatch',
          all(f'NativeAdrenoMethod{name}' in client for name in
              ('Query', 'BoAlloc', 'BoFree', 'Submit', 'WaitFence', 'GetStatus', 'SetMetadata', 'GetMetadata',
               'CreateQueue', 'CloseQueue', 'BoCpuMap', 'BoCpuUnmap', 'SetPriority', 'Note')),
          'every v2 selector is answered, not silently dropped')
    open_body = between(accelerator, 'Tb710fuGpuAccelerator::newUserClient', '\n}\n')
    check('client.no_console_write_in_open', 'printf' not in open_body,
          'newUserClient runs under the service lock with preemption off')
    check('client.no_ring_claim',
          'result = kIOReturnUnsupported' in client and
          all(f'NativeAdrenoMethod{name}' in client for name in ('BoMap', 'Submit', 'GetTimestamp', 'BoImport', 'GetDeviceInfo')),
          'GPU-address and ring methods refuse instead of returning zero-filled success')

    # --- Power chain ---------------------------------------------------------
    check('power.mmio_helper_only',
          'IODeviceMemory' not in power and 'mapDeviceMemory' not in power and 'withRange(' not in power,
          'Power.cpp reaches device memory only through Tb710fuGpuMmio.cpp')
    check('power.mmio_mapping',
          'IODeviceMemory::withRange' in mmio and 'kIOMapInhibitCache' in mmio and 'volatile uint32_t *' in mmio)
    check('power.mmio_barrier', 'dsb sy' in mmio)
    check('power.census_only_reads', 'SecureDTLookupEntry' in power and 'SecureDTGetProperty' in power)
    check('power.addon_census', 'native_ram_block_gpu_addon' in power and 'tb710fu_addon_find' in power)
    check('power.domain_gated', 'tb710fu_gpu_stats[TB710FU_GPU_POWER_LEVEL] < 2' in power)
    check('power.cx_gdsc', 'TBGPU_CC_CX_GDSCR' in power and 'TBGPU_GDSC_SW_COLLAPSE' in power and
          'TBGPU_CC_CX_GDS_HW_CTRL' in power and 'TBGPU_GDSC_RETAIN_FF' in power)
    check('power.cx_clocks', 'TBGPU_CC_AHB_CLK' in power and 'TBGPU_CC_SMMU_VOTE_CLK' in power)
    check('power.smmu_read_only', 'wr(gSmmu' not in power and 'TBGPU_SMMU_SMR' in power and 'TBGPU_SMMU_GFSR' in power,
          'the SMMU inventory reads global space 0 and programs no context')
    check('power.gx_untouched', 'wr(gGpucc, TBGPU_CC_GX' not in power and 'wr(gGpu, ' not in power,
          'GX and the GPU window belong to the GMU, which this milestone does not boot')
    check('power.ddrs_axi_read_only', 'wr(gGcc, TBGPU_GCC_DDRSS_GPU_AXI' not in power,
          'the SM8650 DDRSS AXI offset is not verified, so the branch is read and reported only')
    check('power.gmu_clock_steps', all(marker in power for marker in
                                       ('TBGPU_GCC_PLL_VOTE', 'TBGPU_CC_PLL1', 'TBGPU_CC_HUB_RCGR',
                                        'TBGPU_CC_GMU_RCGR', 'TBGPU_CC_CXO_AON_CLK', 'TBGPU_CC_HUB_CX_INT_CLK')))
    check('power.gmu_stage_refuses',
          'stage7 skipped' in power and 'stage8 skipped' in power,
          'the GMU cold start and GX power state their missing pinned prerequisites')
    check('power.firmware_pins',
          all(name in power and digest in power for name, _, digest in FIRMWARE),
          'stage 6 verifies the four gen70900 images against the manifest pins')
    check('power.abi_stage', 'TB710FU_GPU_ABI] = 1' in power and 'TB710FU_GPU_RING_READY] = 0' in power)
    check('power.windows',
          all(re.search(pattern, hw) for pattern in
              (r'TBGPU_GPU_BASE\s+0x03d00000ull', r'TBGPU_GMU_BASE\s+0x03d68000ull',
               r'TBGPU_GPUCC_BASE\s+0x03d90000ull', r'TBGPU_SMMU_BASE\s+0x03da0000ull',
               r'TBGPU_CC_CX_GDS_HW_CTRL\s+0x9168')),
          'SM8650 windows and the 0x9168 hw-ctrl offset, not SM8550 values')

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

    # --- Build ---------------------------------------------------------
    for name in ('Tb710fuGpuAccelerator.cpp', 'Tb710fuGpuPower.cpp', 'Tb710fuGpuMmio.cpp',
                 'Tb710fuGpuFirmware.cpp', 'Tb710fuGpuClient.cpp'):
        check(f'build.source.{name}', f"PACKAGE / '{name}'" in build)
    check('build.optional_deferral', 'if not renamed' in build and 'return False' in build,
          'a translation unit without a static initialiser is copied unchanged')
    check('build.headers', all(f"'{name}'" in build for name in
                               ('Tb710fuGpuHw.h', 'Tb710fuGpuMmio.h', 'Tb710fuGpuFirmware.h',
                                'Tb710fuGpuClient.h', 'Tb710fuGpuNativeABI.h')))

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

    for docs_name, args_expected in (('docs/GPU_METAL_M1.md', ('gpu-metal=1', 'gpu-power=', 'gpu-addon=1', 'gpu-bundle-system=1')),
                                     ('docs/GPU_METAL_M2.md', ('gpu-metal=1', 'gpu-power=1', 'gpu-power=9', 'stage 7', 'stage 8', 'ring=0'))):
        docs = ROOT / docs_name
        check(f'docs.present.{Path(docs_name).stem}', docs.is_file())
        if docs.is_file():
            text = read(docs)
            for arg in args_expected:
                check(f'docs.{Path(docs_name).stem}.{arg}', arg in text)
            check(f'docs.{Path(docs_name).stem}.no_desktop_claim', 'WindowServer' in text)
    check('docs.m2.user_client', (ROOT / 'docs/GPU_METAL_M2.md').is_file() and
          'AGXDeviceUserClient' in read(ROOT / 'docs/GPU_METAL_M2.md'))

    failed = [item for item in results if item['result'] == 'FAIL']
    report = {'schema': 'tb710fu-gpu-host-checks-v2', 'results': results,
              'summary': {'passed': len(results) - len(failed), 'failed': len(failed)},
              'soc_windows': {name: list(value) for name, value in SOC_WINDOWS.items()},
              'milestone': 2}
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
