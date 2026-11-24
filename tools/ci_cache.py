#!/usr/bin/env python3
"""Deterministic cache identity and restore checks for the macOS cloud kernel build.

A cold cloud run spends six to eight minutes compiling ~950 XNU objects twice (one
bootstrap pass for the compile records, one real pass with the archive set).  Only a
content-addressed compile cache removes that cost, and such a cache is trustworthy
only when three properties hold:

* the key names every input that can change an object file - the pinned UniXNU and
  XNU commits, the resolved device/OS patch series, the kernel defconfig, the board,
  the frozen feed lock and the runner toolchain - and names nothing else.  Editing
  ``package/`` or ``scripts/build-tb710fu-*.py`` must keep all objects valid because
  those inputs only reach the kernel through the archive link, not through a header.
* a restored tree is proven to belong to the current key before it is used, and is
  thrown away (cold build) when any part of it does not match.
* make's date comparison stays deterministic after restore: the archive action does
  not contract to preserve mtimes, so restored sources and objects are pinned to
  fixed epochs with a strict order (sources older than objects).

``key``, ``adopt`` and ``record`` implement that contract; ``selftest`` exercises it
on a synthetic tree without touching a real XNU build.

Provenance: the cache only ever stores compiler output that a *successful* build
produced, the state record carries the exact input manifest, the patch signature the
driver wrote, the produced kernel digest and the digest of every archive link-input
stamp.  Nothing in the cache is trusted without that record.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import tomllib
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# A key prefix carries the schema, so a change to this tool invalidates every entry
# instead of reinterpreting an old one.
SCHEMA = 1
KEY_PREFIX = 'tb710fu-xnu-obj'
FEED_PREFIX = 'tb710fu-feeds'
IPSW_PREFIX = 'tb710fu-kernelcache'

# XNU development object directory pinned by the macOS 27 profile.
XNU_CONFIG = 'DEVELOPMENT_ARM64_BCM2837'
XNU_RECORDS = Path('tree/build/xnu/ios-obj') / XNU_CONFIG
# Fallback kernel file name; the authoritative value is read from the link stamps.
FALLBACK_KERNEL = 'mach.development.bcm2837'
LINK_OUTPUT_SUFFIXES = ('.unstripped.noctf', '.unstripped', '.ctf', '.dSYM', '')

# Epochs pinned on a restored tree.  2001-01-01T00:00:00Z is older than any file a
# build writes today, and objects sit one minute above sources so make never sees a
# restored source as newer than a restored object.
FIXED_SOURCE_MTIME = 978307200
OBJECT_MTIME_OFFSET = 60

# Files that describe the build inputs, per cache identity.  unixnu.lock is rewritten
# by `unixnu fetch`; an absent lock is a distinct (cold) identity rather than an error.
FIXED_INPUTS = {
    'xnu': ('upstream.lock.json', 'unixnu.lock', 'board.toml', 'feeds.toml'),
    'feeds': ('upstream.lock.json', 'unixnu.lock', 'feeds.toml'),
    'ipsw': ('upstream.lock.json', 'board.toml'),
}
XNU_TREES = ('patches/xnu', 'patches/pre-os')
# Inputs a run rewrites itself; see stable_digest().
VOLATILE_INPUTS = ('unixnu.lock',)
TOOLCHAIN_ENV = (
    ('xcode', 'CI_XCODE_VERSION'),
    ('macos_sdk', 'CI_MACOS_SDK'),
    ('iphoneos_sdk', 'CI_IPHONEOS_SDK'),
    ('host', 'CI_HOST'),
)


def say(message):
    print(message, flush=True)


def warn(message):
    print(f'::warning::{message}', flush=True)


def error(message):
    print(f'::error::{message}', flush=True)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text())


def human(size):
    value = float(size)
    for unit in ('B', 'KiB', 'MiB', 'GiB'):
        if value < 1024 or unit == 'GiB':
            return f'{value:.1f} {unit}'
        value /= 1024
    return f'{value:.1f} GiB'


def lock_inputs(root):
    return read_json(Path(root) / 'upstream.lock.json')


def board_inputs(root):
    return tomllib.loads((Path(root) / 'board.toml').read_text())


def defconfig_name(root):
    board = board_inputs(root)
    try:
        return board['kernel']['defconfig']
    except KeyError:
        raise SystemExit('board.toml has no [kernel] defconfig; the cache key cannot be computed')


def toolchain_identity():
    """Runner compiler identity, from the workflow environment.

    A rolling `macos-26` image changes Xcode and the SDKs under a mutable tag.  Objects
    compiled against the old SDK must not be reused against a new one, so the identity
    is part of the key.  Missing values still produce a stable key, but they lose that
    protection, which is worth a warning.
    """
    values = {}
    missing = []
    for name, variable in TOOLCHAIN_ENV:
        value = os.environ.get(variable, '').strip()
        if value:
            values[name] = value
        else:
            values[name] = 'unset'
            missing.append(variable)
    values['developer_dir'] = os.environ.get('DEVELOPER_DIR', '').strip() or 'unset'
    if missing:
        warn('runner toolchain identity is unset (' + ', '.join(missing)
             + '); the cache key cannot detect a runner image change')
    return values


def iter_tree(base):
    for path in sorted(Path(base).rglob('*')):
        if path.is_file() and '__pycache__' not in path.parts:
            yield path


def input_manifest(root, kind):
    """Name/value pairs that define the cache identity, in a stable order."""
    root = Path(root)
    entries = []
    for relative in FIXED_INPUTS[kind]:
        path = root / relative
        if path.is_file():
            entries.append((relative, sha256_file(path)))
        elif relative == 'unixnu.lock':
            entries.append((relative, 'absent'))
        else:
            raise SystemExit(f'cache identity input missing: {relative}')
    if kind == 'xnu':
        relative = f'configs/{defconfig_name(root)}'
        path = root / relative
        if not path.is_file():
            raise SystemExit(f'cache identity input missing: {relative}')
        entries.append((relative, sha256_file(path)))
        for tree in XNU_TREES:
            base = root / tree
            if not base.is_dir():
                raise SystemExit(f'cache identity input missing: {tree}')
            for path in iter_tree(base):
                entries.append((path.relative_to(root).as_posix(), sha256_file(path)))
        for name, value in toolchain_identity().items():
            entries.append((f'toolchain:{name}', value))
    return entries


def identity_digest(entries):
    return hashlib.sha256(''.join(f'{name}\0{value}\n' for name, value in entries).encode()).hexdigest()


def stable_digest(entries):
    """Digest over the inputs a cached run cannot rewrite behind its own record.

    `unixnu fetch` rewrites unixnu.lock during the very run whose objects are being cached,
    so the bytes seen at record time are not the bytes the next clean checkout carries.  The
    key still covers the checkout-time content, and adopt compares the key, so dropping only
    the volatile entry keeps both guarantees without a permanent cache miss.
    """
    return identity_digest([(name, value) for name, value in entries if name not in VOLATILE_INPUTS])


def cache_keys(root):
    root = Path(root)
    lock = lock_inputs(root)
    if not lock.get('xnu_commit') or not lock.get('os_profile'):
        raise SystemExit('upstream.lock.json has no xnu_commit/os_profile; the cache key is undefined')
    label = f'{sys.platform}-{platform.machine()}'
    xnu = input_manifest(root, 'xnu')
    feeds = input_manifest(root, 'feeds')
    ipsw = input_manifest(root, 'ipsw')
    return {
        'label': label,
        'xnu': f'{KEY_PREFIX}-v{SCHEMA}-{label}-{identity_digest(xnu)[:16]}',
        'xnu-digest': identity_digest(xnu),
        'xnu-manifest': xnu,
        'feeds': f'{FEED_PREFIX}-v{SCHEMA}-{label}-{identity_digest(feeds)[:16]}',
        'feeds-digest': identity_digest(feeds),
        'ipsw': f'{IPSW_PREFIX}-v{SCHEMA}-{label}-{identity_digest(ipsw)[:16]}',
    }


def write_outputs(path, values):
    if not path:
        return
    with open(path, 'a') as stream:
        for name, value in values.items():
            stream.write(f'{name}={value}\n')


def profile_kernel_path(root, profile):
    """`[kernel.prelink].kernel` from the pinned OS profile, when it is checked out."""
    profile_file = Path(root) / '.work/UniXNU/profiles/os' / profile / 'profile.toml'
    if not profile_file.is_file():
        return None
    try:
        data = tomllib.loads(profile_file.read_text())
        return data['kernel']['prelink']['kernel']
    except (KeyError, tomllib.TOMLDecodeError):
        return None


def kernel_name(records):
    """Base kernel file name: read from a link-input stamp, then the profile, then the constant."""
    for stamp in sorted(Path(records).glob('.tb710fu-*-link-input.json')):
        try:
            target = read_json(stamp).get('link_target')
        except (OSError, ValueError):
            continue
        if target:
            name = Path(target).name
            if name.endswith('.unstripped.noctf'):
                return name[: -len('.unstripped.noctf')]
    return FALLBACK_KERNEL


def link_outputs(records, base):
    records = Path(records)
    for path in sorted(records.glob(f'{base}*')):
        name = path.name
        suffix = name[len(base):]
        if suffix in LINK_OUTPUT_SUFFIXES:
            yield path


def resolve_work(root, warn_only=False):
    root = Path(root)
    resolved = root / '.work/resolved-work.txt'
    if resolved.is_file():
        work = Path(resolved.read_text().strip())
        if work.is_dir():
            return work
        if not warn_only:
            raise SystemExit(f'resolved work directory is missing: {work}')
    if not warn_only:
        raise SystemExit('.work/resolved-work.txt is missing; run tools/ci_kernel.py configure first')
    return None


def restored_work_dirs(root):
    """All resolved work directories a cache restore could have produced."""
    base = Path(root) / '.work/UniXNU/out/tb710fu'
    if not base.is_dir():
        return []
    return [path.parent for path in sorted(base.glob('*/tb710fu_macos/tree'))]


def discard(root, work, records, reasons, github_output=None):
    """Throw the restored state away and let the build run cold."""
    tree = Path(work) / 'tree'
    for path in (records, tree / '.cache/xnu-work', tree / '.cache/fakeroot'):
        if path.exists():
            shutil.rmtree(path, ignore_errors=True)
    for path in (Path(work) / 'xnu-inputs.json', Path(work) / '.ci-cache'):
        if path.is_file() or path.is_symlink():
            path.unlink(missing_ok=True)
        elif path.is_dir():
            shutil.rmtree(path, ignore_errors=True)
    for reason in reasons:
        warn(f'restored XNU state rejected: {reason}')
    say('== ci_cache: cold build (restored state discarded)')
    write_outputs(github_output, {'xnu-state': 'rejected'})
    return 0


def git_head(work):
    """Detached HEAD of the cached XNU work tree without paying for a git process when possible."""
    dotgit = Path(work) / '.git'
    if not dotgit.is_dir():
        return None
    head = dotgit / 'HEAD'
    if head.is_file():
        text = head.read_text().strip()
        if len(text) == 40 and ' ' not in text:
            return text
    try:
        return subprocess.run(['git', '-C', str(work), 'rev-parse', 'HEAD'], check=True,
                              text=True, capture_output=True, timeout=30).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None


def file_totals(paths):
    files = 0
    size = 0
    for path in paths:
        try:
            files += 1
            size += path.stat().st_size
        except OSError:
            continue
    return files, size


def pin_mtimes(bases, epoch):
    """Pin every file and directory (except git metadata) to one epoch."""
    touched = 0
    skipped = 0
    for base in bases:
        path = Path(base)
        if not path.exists():
            continue
        for directory, dirnames, names in os.walk(path):
            dirnames[:] = [name for name in dirnames if name != '.git']
            for name in names:
                candidate = Path(directory) / name
                if candidate.is_symlink():
                    continue
                try:
                    os.utime(candidate, (epoch, epoch))
                    touched += 1
                except OSError:
                    skipped += 1
            try:
                os.utime(directory, (epoch, epoch))
            except OSError:
                skipped += 1
    return touched, skipped


def stamp_payload(records):
    """File name to sha256 of every device archive link-input stamp."""
    return {stamp.name: sha256_file(stamp)
            for stamp in sorted(Path(records).glob('.tb710fu-*-link-input.json'))}


def adopt(root, key, github_output=None):
    root = Path(root)
    work = resolve_work(root, warn_only=True)
    if work is None:
        for candidate in restored_work_dirs(root):
            records = candidate / XNU_RECORDS
            if records.is_dir():
                discard(root, candidate, records, ['.work/resolved-work.txt is missing'], github_output)
        write_outputs(github_output, {'xnu-state': 'cold'})
        return 0
    records = work / XNU_RECORDS
    state_file = work / '.ci-cache/xnu-state.json'
    if not records.is_dir():
        say('== ci_cache: cold XNU object tree (nothing restored, a full build will produce one)')
        write_outputs(github_output, {'xnu-state': 'cold'})
        return 0

    reasons = []
    state = None
    if not state_file.is_file():
        reasons.append('the cache identity record .ci-cache/xnu-state.json is missing')
    else:
        try:
            state = read_json(state_file)
        except (OSError, ValueError) as problem:
            reasons.append(f'the cache identity record is unreadable: {problem}')
    if state:
        if state.get('schema') != SCHEMA:
            reasons.append(f'cache schema {state.get("schema")} differs from {SCHEMA}')
            state = None
        else:
            if state.get('key') != key:
                reasons.append('cache key differs from the restored tree identity')
            expected = stable_digest(input_manifest(root, 'xnu'))
            if state.get('inputs', {}).get('stable_sha256') != expected:
                reasons.append('input manifest differs from the restored tree identity')
            lock = lock_inputs(root)
            if state.get('source', {}).get('xnu_commit') != lock.get('xnu_commit'):
                reasons.append('XNU commit differs from upstream.lock.json')
            stamp_file = work / 'xnu-inputs.json'
            if not stamp_file.is_file():
                reasons.append('xnu-inputs.json is missing, so the driver would re-clone the patched sources')
            else:
                if sha256_file(stamp_file) != state.get('xnu_inputs_sha256'):
                    reasons.append('xnu-inputs.json differs from the record')
    if reasons:
        return discard(root, work, records, reasons, github_output)

    head = git_head(work / 'tree/.cache/xnu-work')
    if head != state['source']['xnu_work_head']:
        return discard(root, work, records, [
            f'patched source HEAD {head} differs from the record {state["source"]["xnu_work_head"]}'], github_output)
    if not (records / 'osfmk/DEVELOPMENT/startup.o.json').is_file():
        return discard(root, work, records, ['the startup compile record is missing, so the bootstrap pass would run'],
                       github_output)

    base = kernel_name(records)
    dropped = []
    for path in link_outputs(records, base):
        files, size = file_totals([path]) if path.is_file() else (0, 0)
        if path.is_dir():
            files, size = file_totals(list(iter_tree(path)))
        dropped.append((path.name, files, size))
        if path.is_dir():
            shutil.rmtree(path, ignore_errors=True)
        else:
            path.unlink(missing_ok=True)

    # The stamps carry the commit label, the absolute link flags and the archive digest
    # of the run that recorded them, so a restored stamp is stale by construction and
    # must not be compared across runs.  Dropping it leaves every builder with no
    # previous input, which is the same branch that requests the relink.
    stale = [stamp.name for stamp in sorted(records.glob('.tb710fu-*-link-input.json'))]
    for name in stale:
        (records / name).unlink(missing_ok=True)

    sources, source_skipped = pin_mtimes([work / 'tree/.cache/xnu-work', work / 'tree/.cache/fakeroot'], FIXED_SOURCE_MTIME)
    objects, object_skipped = pin_mtimes([records], FIXED_SOURCE_MTIME + OBJECT_MTIME_OFFSET)

    object_files, object_size = file_totals(list(iter_tree(records)))
    say(f'== ci_cache: adopted XNU compile cache {key}')
    say(f'   objects   {object_files} files, {human(object_size)} (pinned to epoch +{OBJECT_MTIME_OFFSET}s)')
    say(f'   sources   {sources} paths pinned to epoch (patched xnu-work and fakeroot headers)')
    say(f'   patch set {len(state["xnu_inputs"].get("patches", []))} patches at XNU {state["source"]["xnu_commit"][:12]}')
    for name, files, size in dropped:
        say(f'   dropped kernel link output {name}: {files} files, {human(size)} (linked fresh from this run\'s archives)')
    if stale:
        say(f'   dropped {len(stale)} stale archive link-input stamps (the builders rewrite them for this run)')
    if source_skipped or object_skipped:
        warn(f'{source_skipped + object_skipped} restored paths could not be re-stamped; make may rebuild them')
    if not (work / 'tree/.cache/fakeroot').is_dir():
        warn('the restored fakeroot headers are missing; installhdrs will run and rebuild every dependent object')
    write_outputs(github_output, {'xnu-state': 'hit'})
    return 0


def audit_link_inputs(tree, records, base):
    """Prove the linked kernel cannot predate the archives that are on disk now.

    Every device archive builder rewrites its stamp when the produced archive digest
    changes and deletes `mach...unstripped.noctf` in the same branch, which is what
    makes a changed package/ archive reach the link.  This audit checks the two
    observable halves of that contract: the stamps describe the archives on disk, and
    the link output is newer than every stamp.
    """
    tree = Path(tree)
    records = Path(records)
    reports = {}
    errors = []
    notes = []
    for report in sorted((tree / 'build').glob('*/build-report.json')):
        try:
            data = read_json(report)
        except (OSError, ValueError):
            continue
        archive = data.get('archive')
        digest = data.get('archive_sha256')
        if not archive or not digest:
            continue
        path = Path(archive)
        if not path.is_absolute():
            path = tree / archive
        if not path.is_file():
            notes.append(f'{report.parent.name}: report archive {path} is not on disk')
            continue
        actual = sha256_file(path)
        if actual != digest:
            errors.append(f'{report.parent.name}: report archive digest {digest[:12]} differs from {actual[:12]}')
        else:
            reports[digest] = str(path)
    stamps = {}
    stamp_mtimes = {}
    for stamp in sorted(records.glob('.tb710fu-*-link-input.json')):
        try:
            data = read_json(stamp)
        except (OSError, ValueError) as problem:
            errors.append(f'{stamp.name}: unreadable ({problem})')
            continue
        stamps[stamp.name] = {'archive': data.get('archive'),
                              'archive_sha256': data.get('archive_sha256'),
                              'sha256': sha256_file(stamp)}
        stamp_mtimes[stamp.name] = stamp.stat().st_mtime
        digest = data.get('archive_sha256')
        if not digest:
            errors.append(f'{stamp.name}: has no archive digest, so it cannot invalidate anything')
            continue
        archive = data.get('archive')
        if archive:
            path = Path(archive)
            if not path.is_absolute():
                path = tree / archive
            if not path.is_file():
                errors.append(f'{stamp.name}: archive {path} is missing')
            elif sha256_file(path) != digest:
                errors.append(f'{stamp.name}: archive digest {digest[:12]} differs from the archive on disk')
        elif digest not in reports:
            notes.append(f'{stamp.name}: digest {digest[:12]} has no build report; freshness cannot be proven')
    kernel = records / f'{base}.unstripped.noctf'
    if not kernel.is_file():
        errors.append(f'{kernel.name} is missing; the kernel link did not run')
    else:
        newest = max(stamp_mtimes.values()) if stamp_mtimes else None
        if newest and newest > kernel.stat().st_mtime + 5:
            notes.append('a link-input stamp is newer than the linked kernel; the relink may not have happened')
    return {'ok': not errors, 'errors': errors, 'notes': notes, 'stamps': stamps}


def record(root, key, github_output=None):
    root = Path(root)
    work = resolve_work(root)
    records = work / XNU_RECORDS
    if not records.is_dir():
        raise SystemExit(f'no XNU object tree at {records}; the native build did not run')
    lock = lock_inputs(root)
    stamp_file = work / 'xnu-inputs.json'
    if not stamp_file.is_file():
        raise SystemExit('xnu-inputs.json is missing; the driver did not prepare the patch series')
    xnu_inputs = read_json(stamp_file)
    head = git_head(work / 'tree/.cache/xnu-work')
    if head != lock['xnu_commit']:
        raise SystemExit(f'patched XNU source HEAD {head} differs from upstream.lock.json')
    base = kernel_name(records)
    kernel = records / f'{base}.unstripped.noctf'
    if not kernel.is_file():
        raise SystemExit(f'{kernel} is missing; the native build did not link a kernel')
    audit = audit_link_inputs(work / 'tree', records, base)
    native = work / 'kernel/kernel.macho'
    object_files, object_size = file_totals(list(iter_tree(records)))
    entries = input_manifest(root, 'xnu')
    state = {
        'schema': SCHEMA,
        'key': key,
        'created_at': datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'workflow': {name: os.environ.get(variable, '') for name, variable in (
            ('run_id', 'GITHUB_RUN_ID'), ('run_attempt', 'GITHUB_RUN_ATTEMPT'),
            ('sha', 'GITHUB_SHA'), ('ref', 'GITHUB_REF'), ('job', 'GITHUB_JOB'))},
        'toolchain': toolchain_identity(),
        'inputs': {'manifest_sha256': identity_digest(entries),
                   'stable_sha256': stable_digest(entries),
                   'files': [{'path': name, 'sha256': value} for name, value in entries]},
        'source': {'unixnu_commit': lock['unixnu']['commit'], 'xnu_commit': lock['xnu_commit'],
                   'os_profile': lock['os_profile'], 'xnu_work_head': head},
        'xnu_inputs': xnu_inputs,
        'xnu_inputs_sha256': sha256_file(stamp_file),
        'objects': {'config': XNU_CONFIG, 'files': object_files, 'bytes': object_size,
                    'startup_record': True},
        'kernel': {'path': f'build/xnu/ios-obj/{XNU_CONFIG}/{kernel.name}',
                   'sha256': sha256_file(kernel), 'bytes': kernel.stat().st_size},
        'native': ({'sha256': sha256_file(native), 'bytes': native.stat().st_size}
                   if native.is_file() else None),
        'link_stamps': audit['stamps'],
        'link_stamp_sha256': stamp_payload(records),        'link_audit': {'ok': audit['ok'], 'errors': audit['errors'], 'notes': audit['notes']},
    }
    say(f'== ci_cache: recorded XNU compile cache {key}')
    say(f'   kernel {kernel.name} sha256 {state["kernel"]["sha256"]} ({human(state["kernel"]["bytes"])})')
    if state['native']:
        say(f'   native kernel.macho sha256 {state["native"]["sha256"]}')
    say(f'   objects {object_files} files, {human(object_size)}; '
        f'{len(xnu_inputs.get("patches", []))} patches at XNU {head[:12]}')
    say(f'   archive link inputs: {len(audit["stamps"])} stamps verified against the archives on disk')
    for note in audit['notes']:
        warn(note)
    for problem in audit['errors']:
        error(problem)
    if audit['errors']:
        say('== ci_cache: refusing to record a cache state whose archive invalidation does not hold')
        return 1
    state_dir = work / '.ci-cache'
    state_dir.mkdir(parents=True, exist_ok=True)
    temporary = state_dir / 'xnu-state.json.tmp'
    temporary.write_text(json.dumps(state, indent=2) + '\n')
    os.replace(temporary, state_dir / 'xnu-state.json')
    say(f'   state written to {state_dir / "xnu-state.json"}')
    write_outputs(github_output, {'xnu-state': 'recorded'})
    return 0


def selftest(root=None):
    """Synthetic regression test: key stability, sensitivity and restore rejection."""
    scratch = Path(tempfile.mkdtemp(prefix='ci-cache-selftest-'))
    fake = scratch / 'repo'
    profile = 'macos-27.3-26C5035r'
    work = fake / '.work/UniXNU/out/tb710fu' / profile / 'tb710fu_macos'

    def put(relative, text):
        path = fake / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    put('upstream.lock.json', json.dumps({'unixnu': {'commit': '0c1bac3d' + '0' * 32},
                                         'os_profile': profile, 'xnu_commit': 'ac9718fb' + '0' * 32,
                                         'kernelcache_sha256': 'd1' * 32}, indent=2))
    put('board.toml', f'schema_version = 2\n[board]\nos_profile = "{profile}"\n'
                      f'[kernel]\ndefconfig = "tb710fu_macos_defconfig"\n')
    put('feeds.toml', '[[source]]\nname = "xnu"\nkind = "refer"\n')
    put('unixnu.lock', '[source."xnu"]\ncommit = "ac9718fb"\n')
    put('configs/tb710fu_macos_defconfig', 'CONFIG_XNU_ARCH="arm64e"\n')
    put('patches/xnu/series', '0001-first.patch\n')
    put('patches/xnu/0001-first.patch', 'diff --git a/x b/x\n')
    put('patches/pre-os/series', 'sheng.patch\n')
    put('patches/pre-os/sheng.patch', 'diff --git a/y b/y\n')
    put('package/Tb710fuUsbCdc/CdcXnu.c', 'int main(void) { return 0; }\n')
    put(f'.work/UniXNU/profiles/os/{profile}/profile.toml',
        f'["kernel"."prelink"]\n"kernel" = "build/xnu/ios-obj/{XNU_CONFIG}/mach.development.bcm2837.unstripped.noctf"\n')

    def run(*arguments):
        result = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--root', str(fake),
                                 *arguments], text=True, capture_output=True)
        if os.environ.get('CI_CACHE_SELFTEST_TRACE'):
            print(f'--- {" ".join(arguments)} ---\n{result.stdout}{result.stderr}', flush=True)
        if result.returncode:
            raise SystemExit(f'selftest step {" ".join(arguments)} failed:\n{result.stdout}\n{result.stderr}')
        return result.stdout

    output = scratch / 'out.txt'
    run('key', '--github-output', str(output))
    keys = dict(line.split('=', 1) for line in output.read_text().splitlines() if '=' in line)
    assert keys['xnu-key'].startswith(KEY_PREFIX), keys

    # Same inputs, same key.
    output.write_text('')
    run('key', '--github-output', str(output))
    again = dict(line.split('=', 1) for line in output.read_text().splitlines() if '=' in line)
    assert again['xnu-key'] == keys['xnu-key'], 'key is not stable'

    # A package or recipe edit must not move the XNU object key.
    (fake / 'package/Tb710fuUsbCdc/CdcXnu.c').write_text('int main(void) { return 1; }\n')
    put('scripts/build-tb710fu-cdc-log.py', 'print(1)\n')
    output.write_text('')
    run('key', '--github-output', str(output))
    package = dict(line.split('=', 1) for line in output.read_text().splitlines() if '=' in line)
    assert package['xnu-key'] == keys['xnu-key'], 'a package/ or recipe edit invalidated the XNU objects'

    # A patch edit must move it.
    put('patches/xnu/0001-first.patch', 'diff --git a/x b/x\n+more\n')
    output.write_text('')
    run('key', '--github-output', str(output))
    patched = dict(line.split('=', 1) for line in output.read_text().splitlines() if '=' in line)
    assert patched['xnu-key'] != keys['xnu-key'], 'a patch edit left the XNU object key unchanged'
    assert patched['feeds-key'] == keys['feeds-key'], 'a patch edit moved the pinned-source key'

    # Build a fake successful state, record it, then adopt it with the current key.
    work.mkdir(parents=True, exist_ok=True)
    (fake / '.work/resolved-work.txt').write_text(str(work) + '\n')
    records = work / XNU_RECORDS
    (records / 'osfmk/DEVELOPMENT').mkdir(parents=True, exist_ok=True)
    (records / 'osfmk/DEVELOPMENT/startup.o.json').write_text('{}\n')
    (records / 'bsd/kern').mkdir(parents=True, exist_ok=True)
    (records / 'bsd/kern/bsd_init.o').write_bytes(b'\x00' * 64)
    archive_dir = work / 'tree/build/tb710fu-cdc-log'
    archive_dir.mkdir(parents=True, exist_ok=True)
    archive = archive_dir / 'libTb710fuCdcLog.a'
    archive.write_bytes(b'archive-bytes')
    (archive_dir / 'build-report.json').write_text(json.dumps(
        {'archive': str(archive), 'archive_sha256': sha256_file(archive)}, indent=2) + '\n')
    (records / '.tb710fu-cdc-link-input.json').write_text(json.dumps({
        'archive': 'build/tb710fu-cdc-log/libTb710fuCdcLog.a', 'archive_sha256': sha256_file(archive),
        'link_target': str(records / 'mach.development.bcm2837.unstripped.noctf')}, indent=2) + '\n')
    (records / 'mach.development.bcm2837.unstripped.noctf').write_bytes(b'\x00' * 32)
    (work / 'tree/.cache/xnu-work/.git').mkdir(parents=True, exist_ok=True)
    (work / 'tree/.cache/xnu-work/.git/HEAD').write_text('ac9718fb' + '0' * 32 + '\n')
    (work / 'tree/.cache/xnu-work/bsd/kern').mkdir(parents=True, exist_ok=True)
    (work / 'tree/.cache/xnu-work/bsd/kern/bsd_init.c').write_text('/* patched */\n')
    (work / 'tree/.cache/fakeroot/usr/local/include/sys').mkdir(parents=True, exist_ok=True)
    (work / 'tree/.cache/fakeroot/usr/local/include/sys/a.h').write_text('#pragma once\n')
    (work / 'xnu-inputs.json').write_text(json.dumps(
        {'source': str(fake / '.work/UniXNU/out/feeds/xnu-x'), 'commit': 'ac9718fb' + '0' * 32,
         'patches': [{'path': str(fake / 'patches/xnu/0001-first.patch'),
                      'sha256': sha256_file(fake / 'patches/xnu/0001-first.patch')}]}, indent=2) + '\n')
    (work / 'kernel').mkdir(parents=True, exist_ok=True)
    (work / 'kernel/kernel.macho').write_bytes(b'native')

    # An archive that changed under its stamp must stop the record.
    kept = archive.read_bytes()
    archive.write_bytes(b'changed-behind-the-stamp')
    refused = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--root', str(fake),
                              'record', '--key', patched['xnu-key']], text=True, capture_output=True)
    assert refused.returncode != 0, 'the audit accepted an archive that changed under its stamp'
    assert 'differs from the archive on disk' in refused.stdout, refused.stdout
    archive.write_bytes(kept)

    run('record', '--key', patched['xnu-key'])
    state = read_json(work / '.ci-cache/xnu-state.json')
    assert state['key'] == patched['xnu-key']
    assert state['kernel']['bytes'] == 32

    # `unixnu fetch` rewrites unixnu.lock inside the run being cached, so the bytes at
    # record time are not the bytes the next checkout carries; that must not reject it.
    lock_text = (fake / 'unixnu.lock').read_text()
    (fake / 'unixnu.lock').write_text(lock_text + '# rewritten by uniXNU fetch\n')
    run('record', '--key', patched['xnu-key'])
    (fake / 'unixnu.lock').write_text(lock_text)

    # Adoption pins mtimes, drops the stale link and keeps the objects.
    output = run('adopt', '--key', patched['xnu-key'])
    assert 'adopted XNU compile cache' in output, output
    assert not (records / 'mach.development.bcm2837.unstripped.noctf').exists(), 'link output was reused'
    assert (records / 'bsd/kern/bsd_init.o').is_file(), 'objects were dropped'
    stamp = (records / 'bsd/kern/bsd_init.o').stat().st_mtime
    source = (work / 'tree/.cache/xnu-work/bsd/kern/bsd_init.c').stat().st_mtime
    assert stamp == FIXED_SOURCE_MTIME + OBJECT_MTIME_OFFSET, stamp
    assert source == FIXED_SOURCE_MTIME, source
    assert stamp > source, 'objects are not newer than sources'

    # A restored stamp carries the commit label and the absolute link flags of the run
    # that recorded it, so it always differs from the next run's stamp.  That must not
    # invalidate the objects - adopt drops the stale stamp and lets the builder rewrite it.
    (records / '.tb710fu-cdc-link-input.json').write_text(json.dumps({
        'archive': 'build/tb710fu-cdc-log/libTb710fuCdcLog.a', 'archive_sha256': sha256_file(archive),
        'device_commit': 'f' * 40, 'display_build_label': 'src-ffffffff-next',
        'link_target': str(records / 'mach.development.bcm2837.unstripped.noctf')}, indent=2) + '\n')
    (records / 'mach.development.bcm2837.unstripped.noctf').write_bytes(b'\x00' * 32)
    output = run('adopt', '--key', patched['xnu-key'])
    assert 'adopted XNU compile cache' in output, output
    assert not (records / '.tb710fu-cdc-link-input.json').exists(), 'a per-commit stamp survived adoption'

    # A key that does not match throws the state away instead of using it.
    run('adopt', '--key', 'tb710fu-xnu-obj-v1-darwin-arm64-0000000000000000')
    assert not (work / 'tree/.cache/xnu-work').exists(), 'a mismatched key kept the patched sources'
    assert not (work / '.ci-cache').exists(), 'a mismatched key kept the state record'
    shutil.rmtree(scratch, ignore_errors=True)
    say('== ci_cache selftest: ok (key stability, package immunity, patch invalidation, adopt, audit)')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT, help='project checkout to identify')
    modes = parser.add_subparsers(dest='mode', required=True)
    key_parser = modes.add_parser('key', help='compute the cache keys and print the input manifest')
    key_parser.add_argument('--github-output')
    adopt_parser = modes.add_parser('adopt', help='validate a restored XNU compile state')
    adopt_parser.add_argument('--key', required=True)
    adopt_parser.add_argument('--github-output')
    record_parser = modes.add_parser('record', help='record the identity of a successful build')
    record_parser.add_argument('--key', required=True)
    record_parser.add_argument('--github-output')
    modes.add_parser('selftest', help='synthetic regression test of key/adopt/record')
    args = parser.parse_args()
    if args.mode == 'key':
        root = args.root.resolve()
        keys = cache_keys(root)
        say(f'== ci_cache: XNU object cache inputs ({len(keys["xnu-manifest"])} entries)')
        for name, value in keys['xnu-manifest']:
            say(f'   {name}  {value}')
        say(f'== ci_cache: xnu-object  {keys["xnu"]}')
        say(f'== ci_cache: feeds        {keys["feeds"]}')
        say(f'== ci_cache: kernelcache  {keys["ipsw"]}')
        write_outputs(args.github_output, {'xnu-key': keys['xnu'], 'feeds-key': keys['feeds'],
                                           'ipsw-key': keys['ipsw'], 'xnu-digest': keys['xnu-digest']})
        return 0
    if args.mode == 'adopt':
        return adopt(args.root.resolve(), args.key, args.github_output)
    if args.mode == 'record':
        return record(args.root.resolve(), args.key, args.github_output)
    return selftest(args.root)


if __name__ == '__main__':
    raise SystemExit(main())
