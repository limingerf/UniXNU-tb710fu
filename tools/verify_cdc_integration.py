"""Bind the CDC source archive to the real BSD compile and native LD output."""
import hashlib
import json
import subprocess

from verify_storage_integration import (ROOT, LOCK, digest, force_load_count,
                                        hook_macro_selected, hook_symbol,
                                        load_audit, require)


SOURCES = ('CdcKernelTap', 'CdcLogRing', 'CdcAcmProtocol', 'CdcTransportGuard',
           'CdcDwc3', 'CdcXnu')


def verify(tree, config, native_path):
    audit = load_audit()
    output = tree / 'build/tb710fu-cdc-log'
    archive = output / 'libTb710fuCdcLog.a'
    report_path = output / 'build-report.json'
    report = json.loads(report_path.read_text())
    require(report['archive_sha256'] == digest(archive), 'CDC archive/report mismatch')
    require(report['framework_revision'] == LOCK['unixnu']['commit'] and
            report['xnu_revision'] == LOCK['xnu_commit'], 'Wrong CDC source revisions')
    require(set(report['objects_sha256']) == {name + '.o' for name in SOURCES},
            'Incomplete CDC transport source set')
    # Darwin ar retains a symbol-table member alongside real object members.
    # It is linker metadata, not a seventh compiled transport object.
    members = {name: data for name, data in audit.ar_members(archive.read_bytes())
               if not name.startswith('__.SYMDEF')}
    require(set(members) == set(report['objects_sha256']), 'Unexpected CDC archive members')
    for name, expected in report['objects_sha256'].items():
        data = members[name]
        require(hashlib.sha256(data).hexdigest() == expected, 'CDC member mismatch: ' + name)
        obj = audit.MachO(data)
        require(obj.kind == 1 and obj.subtype & 255 == 2, 'CDC member is not arm64e: ' + name)
    for relative, expected in report['sources'].items():
        require(digest(tree / relative) == expected, 'CDC source hash changed: ' + relative)
    for name, expected in report['compile_commands_sha256'].items():
        require(digest(output / (name.removesuffix('.c') + '.command.json')) == expected,
                'CDC command record mismatch: ' + name)
    records = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    bsd = records / 'bsd/DEVELOPMENT/bsd_init.o'
    compile_path = bsd.with_suffix('.o.json')
    command = json.loads(compile_path.read_text().strip().rstrip(','))
    require(hook_macro_selected(command['arguments'], 'XNU_TB710FU_CDC'),
            'BSD startup does not select the CDC macro')
    symbol = '_tb710fu_cdc_start'
    imports = hook_symbol(audit.MachO(bsd.read_bytes()), symbol)
    require(len(imports) == 1 and imports[0]['kind'] & 14 == 0 and imports[0]['kind'] & 1 and
            not imports[0]['desc'] & 0x40, 'BSD must strongly reference the real CDC startup')
    image = audit.MachO(native_path.read_bytes())
    require(image.kind == 2, 'CDC integration needs the actual native MH_EXECUTE')
    definitions = hook_symbol(image, symbol)
    require(len(definitions) == 1 and definitions[0]['kind'] & 14 == 14 and
            definitions[0]['kind'] & 1 and not definitions[0]['desc'] & 0x80,
            'Native kernel must strongly define CDC startup exactly once')
    symbols = image.nlists()
    for name in ('_tb710fu_dwc3_start', '_tb710fu_dwc3_poll', '_tb710fu_cdc_log_tap_putc',
                 '_tb710fu_cdc_log_tap_copy', '_tb710fu_cdc_log_tap_consume',
                 '_tb710fu_cdc_classify_setup', '_tb710fu_cdc_preflight'):
        require(any(s['name'] == name and s['kind'] & 14 == 14 and not s['kind'] & 0xe0
                    for s in symbols), 'Real CDC transport definition missing: ' + name)
    link = records / 'mach.development.bcm2837.link/mach.development.bcm2837.linkarchives'
    text = link.read_text()
    selected = str(config.get('XNU_EXTRA_LD_ARCHIVES', '')).replace('{tree}', str(tree))
    require(force_load_count(text, archive) == force_load_count(selected, archive) == 1,
            'CDC must be selected and actually force-loaded once')
    return {'schema_version': 1, 'profile': LOCK['os_profile'],
            'device_commit': subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip(),
            'framework_revision': LOCK['unixnu']['commit'], 'xnu_revision': LOCK['xnu_commit'],
            'native_kernel_sha256': digest(native_path), 'archive_sha256': digest(archive),
            'archive_compile_report_sha256': digest(report_path),
            'bsd_object_sha256': digest(bsd), 'bsd_compile_record_sha256': digest(compile_path),
            'bsd_startup_symbol': imports[0], 'native_startup_symbol': definitions[0],
            'linkarchives_sha256': digest(link), 'linkarchives': text,
            'archive_force_load_count': 1, 'linked_into_kernel': True,
            'transport_sources_compiled': True, 'runtime_boot_argument': 'tb710fu-cdc=1',
            'cdc_enumeration_verified': False, 'runtime_qualified': False,
            'hardware_boot_verified': False}
