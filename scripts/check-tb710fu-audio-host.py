#!/usr/bin/env python3
"""Host check for the TB710FU audio stack (no IOKit, no hardware).

Compiles the portable C modules of package/Tb710fuAudio together with tests/tb710fu_audio_test.c under
AddressSanitizer + UndefinedBehaviorSanitizer, runs the binary, and writes
build/tb710fu-audio-host/report.json. When TB710FU_ACF_FILE points at a real aw88461 ACF container the
run also parses it.

Exits non-zero when the compile or any check fails.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'package/Tb710fuAudio'
TEST = ROOT / 'tests/tb710fu_audio_test.c'
OUT = ROOT / 'build/tb710fu-audio-host'

MODULES = ('Tb710fuAcf.c', 'Tb710fuGeniI2c.c', 'Tb710fuAw88461.c', 'Tb710fuGpr.c', 'Tb710fuAudioReach.c',
           'Tb710fuAudioGain.c', 'Tb710fuAudioEngine.c')
HEADERS = ('Tb710fuAcf.h', 'Tb710fuGeniI2c.h', 'Tb710fuAw88461.h', 'Tb710fuGpr.h', 'Tb710fuAudioReach.h',
           'Tb710fuAudioAbi.h', 'Tb710fuAudioEngine.h')
CPP_SOURCES = ('Tb710fuAudio.cpp', 'Tb710fuAudioService.cpp')
CPP_HEADER = 'Tb710fuAudioService.h'

# Members of the two service classes, and what the audit requires of the C++ sources it cannot
# compile here (no macOS SDK on this host): that the declared members are the defined ones, that no
# value is stored through an IOMemoryMap, that no volatile lvalue is incremented, that the platform
# header is present where getPlatform() is used, and that no IOAudioFamily identifier is named.
MEMBER_CLASSES = ('Tb710fuAudioDevice', 'Tb710fuAudioUserClient', 'Tb710fuAudioGeniChannel')
FORBIDDEN_IDENTIFIERS = ('IOAudioDevice', 'IOAudioEngine', 'IOAudioStream', 'IOAudioControl',
                         'IOAudioPort', 'IOAudioSelectorControl', 'IOAudioLevelControl')


def strip_code(text):
    """Source with comments and string/char literals blanked out, keeping line structure."""
    out, i, n = [], 0, len(text)
    while i < n:
        ch = text[i]
        if ch == '/' and i + 1 < n and text[i + 1] == '/':
            while i < n and text[i] != '\n':
                i += 1
        elif ch == '/' and i + 1 < n and text[i + 1] == '*':
            i += 2
            while i + 1 < n and not (text[i] == '*' and text[i + 1] == '/'):
                if text[i] == '\n':
                    out.append('\n')
                i += 1
            i = min(i + 2, n)
        elif ch in '"\'':
            quote = ch
            i += 1
            while i < n and text[i] != quote:
                if text[i] == '\\':
                    i += 1
                i += 1
            i += 1
            out.append('""' if quote == '"' else "''")
        else:
            out.append(ch)
            i += 1
    return ''.join(out)


def balanced(code, path):
    pairs = {')': '(', ']': '[', '}': '{'}
    openers = set('([{')
    stack = []
    for index, ch in enumerate(code):
        if ch in openers:
            stack.append((ch, index))
        elif ch in pairs:
            if not stack or stack[-1][0] != pairs[ch]:
                line = code.count('\n', 0, index) + 1
                raise SystemExit(f'{path}:{line}: unmatched {ch}')
            stack.pop()
    if stack:
        line = code.count('\n', 0, stack[-1][1]) + 1
        raise SystemExit(f'{path}:{line}: unclosed {stack[-1][0]}')


def declared_members(header_text):
    """Return {(class, member)} for the declarations inside each class body."""
    code = strip_code(header_text)
    found = set()
    for klass in MEMBER_CLASSES:
        # The definition opens its body; a forward declaration ("class X;") ends at the semicolon.
        match = re.search(r'class\s+' + klass + r'\s*(?::[^;{}]*)?\{', code)
        if not match:
            raise SystemExit(f'{CPP_HEADER}: class {klass} is not declared')
        brace = match.end() - 1
        depth, index = 0, brace
        while index < len(code):
            if code[index] == '{':
                depth += 1
            elif code[index] == '}':
                depth -= 1
                if depth == 0:
                    break
            index += 1
        body = code[brace:index]
        for match in re.finditer(r'(?<![\w:>])([A-Za-z_]\w*)\s*\(', body):
            name = match.group(1)
            if name in ('if', 'for', 'while', 'switch', 'return', 'sizeof', 'static_cast', 'reinterpret_cast',
                        'decltype', 'alignof', 'OSDeclareDefaultStructors'):
                continue
            found.add((klass, name))
    return found


def defined_members(sources):
    found = set()
    for path, text in sources:
        code = strip_code(text)
        for match in re.finditer(r'\b(' + '|'.join(MEMBER_CLASSES) + r')::([A-Za-z_]\w*)\s*\(', code):
            found.add((match.group(1), match.group(2)))
    return found


def audit_cxx(root, package):
    sources = [(name, (package / name).read_text()) for name in CPP_SOURCES]
    header = (package / CPP_HEADER).read_text()
    findings = []

    for name, text in sources:
        code = strip_code(text)
        balanced(code, name)
        for member in FORBIDDEN_IDENTIFIERS:
            if re.search(r'\b' + member + r'\b', code):
                findings.append(f'{name}: names the closed family identifier {member}')
        if 'getPlatform(' in code and '#include <IOKit/IOPlatformExpert.h>' not in text:
            findings.append(f'{name}: getPlatform() without <IOKit/IOPlatformExpert.h>')
        if '#include <IOKit/audio/' in text:
            findings.append(f'{name}: includes an IOAudioFamily header')
        # A store through a mapping or a device register must never be a memset/memcpy/bzero.
        for match in re.finditer(r'\b(memcpy|memset|bzero|bcopy)\s*\(', code):
            tail = code[match.end():match.end() + 120]
            if 'getVirtualAddress' in tail or re.search(r'\b(mmio|regs|reg_base|bar\d)\b', tail):
                line = code.count('\n', 0, match.start()) + 1
                findings.append(f'{name}:{line}: bulk memory call on mapped device memory')
        # ++/-- must not be applied to a volatile lvalue (clang -Werror,-Wdeprecated-volatile).
        volatile = set(re.findall(r'volatile\s+[\w:<>]+\s+([A-Za-z_]\w*)', code))
        for match in re.finditer(r'(\+\+|--)\s*([A-Za-z_]\w*)|\b([A-Za-z_]\w*)\s*(\+\+|--)', code):
            name_token = match.group(2) or match.group(3)
            if name_token in volatile:
                line = code.count('\n', 0, match.start()) + 1
                findings.append(f'{name}:{line}: volatile {name_token} is incremented')

    declared = declared_members(header)
    defined = defined_members(sources)
    missing = sorted(m for m in declared if m not in defined)
    extra = sorted(m for m in defined if m not in declared)
    for klass, member in missing:
        findings.append(f'{klass}::{member} declared but not defined')
    for klass, member in extra:
        findings.append(f'{klass}::{member} defined but not declared')
    if '#include "Tb710fuAudioAbi.h"' not in header:
        findings.append(f'{CPP_HEADER}: does not publish Tb710fuAudioAbi.h')
    if findings:
        raise SystemExit('C++ audit failed:\n' + '\n'.join('  ' + item for item in findings))
    return {'cpp_files': list(CPP_SOURCES), 'members_checked': len(declared), 'findings': []}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compiler():
    for name in ('gcc', 'clang', 'cc'):
        found = shutil.which(name)
        if found:
            return found
    raise SystemExit('no host C compiler (gcc, clang or cc)')


def build(cc, sources, binary, sanitizers):
    command = [cc, '-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(PACKAGE)]
    if sanitizers:
        command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    # _DEFAULT_SOURCE is what exposes the POSIX bits the test uses; the driver code itself stays C11.
    command += ['-D_DEFAULT_SOURCE', *map(str, sources), '-o', str(binary)]
    return command, subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    os.chdir(ROOT)
    audit = audit_cxx(ROOT, PACKAGE)
    print(f"ok   c++ audit: {audit['members_checked']} members across {', '.join(audit['cpp_files'])}")
    cc = compiler()
    binary = OUT / 'tb710fu_audio_test'
    sources = [TEST] + [PACKAGE / name for name in MODULES]
    sanitizers = True
    command, compile_result = build(cc, sources, binary, sanitizers)
    log = compile_result.stdout
    if compile_result.returncode and any(word in log for word in ('libclang_rt', 'libasan', 'cannot find -lasan')):
        # The host toolchain has no sanitizer runtime; the checks themselves do not depend on it.
        sanitizers = False
        command, compile_result = build(cc, sources, binary, sanitizers)
        log = 'sanitizer runtime missing, retried without sanitizers\n' + compile_result.stdout
    (OUT / 'compile.log').write_text(log)
    if compile_result.returncode:
        print(log[-6000:])
        print('compile failed')
        return 1
    acf = os.environ.get('TB710FU_ACF_FILE', '')
    child_env = dict(os.environ)
    if acf:
        child_env['TB710FU_ACF_FILE'] = acf
    else:
        child_env.pop('TB710FU_ACF_FILE', None)
    run = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         env=child_env)
    (OUT / 'run.log').write_text(run.stdout)
    print(run.stdout.strip())
    report = {
        'binary': str(binary.relative_to(ROOT)),
        'binary_sha256': sha(binary),
        'compiler': cc,
        'compile_command': ' '.join(command),
        'compile_ok': compile_result.returncode == 0,
        'sanitizers': 'address,undefined' if sanitizers else 'none',
        'run_exit': run.returncode,
        'run_ok': run.returncode == 0,
        'acf_file': acf,
        'cpp_audit': audit,
        'sources': {str(p.relative_to(ROOT)): sha(p) for p in sources + [PACKAGE / name for name in HEADERS] +
                    [PACKAGE / name for name in CPP_SOURCES] + [PACKAGE / CPP_HEADER]},
    }
    (OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if run.returncode:
        print('host checks failed')
        return 1
    print(f'host checks passed: {binary}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
