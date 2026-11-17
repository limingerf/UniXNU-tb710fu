#!/usr/bin/env python3
"""Build the device source-storage archive at its ordered schema2 recipe slot.

The fixed framework copies scripts into the resolved tree and supplies
UNIXNU_DEVICE. Replay the device-owned recipe from its original repository so
its lock, patches and policy inputs remain there; never replace an official
script or fetch an interface component during kernel compilation.
"""
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def revision(path):
    top = Path(subprocess.check_output(
        ['git', '-C', str(path), 'rev-parse', '--show-toplevel'], text=True).strip()).resolve()
    if top != path.resolve():
        raise SystemExit(f'Initialize the pinned component before native build: {path}')
    return subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()


def generated_records(tree):
    # Invalidation is confined to the fixed generated XNU object directory.
    # Refuse a symlink that would redirect it into any fetched source/cache.
    xnu = tree / 'build/xnu'
    records = xnu / 'ios-obj/DEVELOPMENT_ARM64_BCM2837'
    if xnu.resolve() != xnu or records.resolve() != records:
        raise SystemExit('Generated XNU link inputs must remain inside resolved tree/build/xnu')
    return records


def verify_link_rule(tree, expected_revision):
    source = tree / '.cache/xnu-work'
    if revision(source) != expected_revision:
        raise SystemExit('Prepared XNU source differs from the device source lock')
    makefile = source / 'makedefs/MakeInc.kernel'
    text = makefile.read_text()
    rule = '$(TARGET)/$(KERNEL_FILE_NAME).unstripped.noctf: $(addprefix $(TARGET)/,'
    command = ('$(LD) $(LDFLAGS_KERNEL) $(LDFLAGS_KERNEL_ONLY) -filelist link.filelist '
               '$(filter %.o,$+) -o $@ $(LD_KERNEL_LIBS) $(LD_KERNEL_ARCHIVES)')
    if text.count(rule) != 1 or command not in text:
        raise SystemExit('Pinned XNU actual LD output rule changed; re-audit link invalidation')
    return {'makefile_sha256': sha(makefile), 'ld_output_pattern': rule.split(':', 1)[0],
            'ld_command_template': command}


def record_link_input(tree, identity):
    records = generated_records(tree)
    target = records / 'mach.development.bcm2837.unstripped.noctf'
    stamp = records / '.tb710fu-source-storage-link-input.json'
    if (target.is_symlink() or stamp.is_symlink() or
            (target.exists() and not target.is_file()) or (stamp.exists() and not stamp.is_file())):
        raise SystemExit('Refusing redirected generated XNU link target or input record')
    text = json.dumps(identity, indent=2) + '\n'
    previous_text = stamp.read_text() if stamp.is_file() else ''
    try:
        previous = json.loads(previous_text)
    except ValueError:
        previous = {}
    if not isinstance(previous, dict):
        previous = {}
    changed = previous_text != text
    existed = target.is_file()
    result = {'link_target': str(target), 'previous_archive_sha256': previous.get('archive_sha256'),
              'archive_sha256': identity['archive_sha256'], 'input_changed': changed,
              'link_invalidation_requested': changed, 'link_target_removed': changed and existed,
              'kernel_link_completed': False}
    if not changed:
        return result
    # XNU ac9718f:makedefs/MakeInc.kernel links this exact target with $(LD).
    # .unstripped is its CTF-derived copy. The fixed framework appends
    # XNU_EXTRA_LD_ARCHIVES without adding them to XNU_EXTERNAL_KERNEL_ARCHIVES,
    # so an unchanged path with new archive bytes otherwise need not relink.
    target.unlink(missing_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', dir=records, prefix=stamp.name + '.',
                                         delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(text)
        os.replace(temporary, stamp)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    print(f'TB710FU source-storage input changed: invalidated {target}')
    return result


def main():
    if os.environ.get('XNU_SOURCE_STORAGE', '0') != '1':
        return
    if os.environ.get('XNU_NATIVE_UFS', '0') != '0':
        raise SystemExit('This source recipe starts its service without automatic UFS personalities')
    if os.environ.get('XNU_SOURCE_CORECRYPTO14') != '1':
        raise SystemExit('The source-storage archive needs the selected real source crypto provider')
    device = Path(os.environ['UNIXNU_DEVICE']).resolve()
    tree = Path.cwd().resolve()
    catalog = tree / 'components.toml'
    if not catalog.is_symlink():
        raise SystemExit('Expected the fixed framework catalog link in the resolved tree')
    framework = catalog.resolve().parent
    lock = json.loads((device / 'upstream.lock.json').read_text())
    if revision(framework) != lock['unixnu']['commit']:
        raise SystemExit('Framework revision differs from the device source lock')
    link_rule = verify_link_rule(tree, lock['xnu_commit'])
    for relative in ('kits/VerifiedRoot', 'drivers/VirtioMmioBlock'):
        expected = subprocess.check_output(
            ['git', '-C', str(framework), 'rev-parse', lock['unixnu']['commit'] + ':' + relative],
            text=True).strip()
        if revision(framework / relative) != expected:
            raise SystemExit(f'Initialize the pinned interface before native build: {relative}')
    output = tree / 'build/tb710fu-source-storage'
    if output.resolve() != output:
        raise SystemExit('Source-storage output must remain inside the resolved tree/build directory')
    input_report = output / 'archive-link-input.json'
    input_report.unlink(missing_ok=True)
    archive = output / 'libTb710fuSourceStorage.a'
    flags = shlex.split(os.environ.get('XNU_EXTRA_CFLAGS', ''))
    if '-DXNU_TB710FU_SOURCE_STORAGE=1' not in flags:
        raise SystemExit('Source storage requires the explicit strong bsd_init hook macro')
    link_flags = shlex.split(os.environ.get('XNU_EXTRA_LD_ARCHIVES', ''))
    if link_flags.count('-Wl,-force_load,' + str(archive)) != 1:
        raise SystemExit('Select the exact device source-storage archive once in EXTRA_LD_ARCHIVES')
    records = generated_records(tree)
    for relative in ('osfmk/DEVELOPMENT/startup.o.json', 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'):
        if not (records / relative).is_file():
            raise SystemExit(f'Native bootstrap did not provide the required compile record: {relative}')
    recipe = device / 'scripts/build-tb710fu-source-storage.py'
    # Bootstrap errors are not handled here. Both real records are required;
    # every provider compiler failure propagates, as must the final kernel link.
    subprocess.run([sys.executable, str(recipe), '--tree', str(tree),
                    '--framework', str(framework)], check=True)
    report_path = output / 'build-report.json'
    report = json.loads(report_path.read_text())
    digest = sha(archive)
    if report['archive_sha256'] != digest or report['framework_revision'] != lock['unixnu']['commit']:
        raise SystemExit('The freshly built archive does not match its pinned build report')
    identity = {'archive': str(archive.relative_to(tree)), 'archive_sha256': digest,
                'framework_revision': lock['unixnu']['commit'], 'xnu_revision': lock['xnu_commit'],
                'recipe_sha256': sha(recipe), 'wrapper_sha256': sha(Path(__file__)),
                'selected_link_flags': link_flags, 'startup_cflags': flags, 'link_rule': link_rule}
    invalidation = record_link_input(tree, identity)
    input_report.write_text(json.dumps(
        {**identity, **invalidation, 'archive_report_sha256': sha(report_path)}, indent=2) + '\n')


if __name__ == '__main__':
    main()
