#!/usr/bin/env python3
"""Run the real C live-log sender against the unchanged pinned OneSend receiver."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'package/Tb710fuLiveLog'
RECEIVER_REPOSITORY = 'makerjackie/onesend'
RECEIVER_COMMIT = '39e95ce59df2f2dc38d2556dd4239cec6a0ca35f'
RECEIVER_PATH = 'website/app/web-transfer-protocol.mjs'
RECEIVER_SHA256 = 'fd94d7f24ba27892d07c2d18cf67986dd793501d2b6298332cbb21863d7ccae3'


def pinned_receiver(path, manifest):
    expected = {'repository': RECEIVER_REPOSITORY, 'commit': RECEIVER_COMMIT,
                'files': [{'path': RECEIVER_PATH, 'sha256': RECEIVER_SHA256}]}
    if manifest != expected:
        raise SystemExit('OneSend manifest differs from the audited receiver pin')
    receiver = path.resolve()
    url = f'https://raw.githubusercontent.com/{RECEIVER_REPOSITORY}/{RECEIVER_COMMIT}/{RECEIVER_PATH}'
    fetched = False
    if not receiver.exists():
        with urllib.request.urlopen(url, timeout=60) as response:
            data = response.read(1024 * 1024 + 1)
        if hashlib.sha256(data).hexdigest() != RECEIVER_SHA256:
            raise SystemExit('Fetched receiver source hash differs; receiver was not saved or executed')
        receiver.parent.mkdir(parents=True, exist_ok=True)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=receiver.parent, prefix=receiver.name + '.',
                                             delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(data)
            os.replace(temporary, receiver)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
        fetched = True
    if not receiver.is_file() or hashlib.sha256(receiver.read_bytes()).hexdigest() != RECEIVER_SHA256:
        raise SystemExit('Pinned receiver source hash differs; receiver was not executed')
    return receiver, {'path': str(receiver), 'sha256': RECEIVER_SHA256,
                      'pinned_url': url, 'fetched_to_cache': fetched}


def compiler_command():
    requested = shlex.split(os.environ.get('CC', 'cc'))
    if requested and shutil.which(requested[0]):
        return requested
    for fallback in ('cc', 'clang', 'gcc'):
        if shutil.which(fallback):
            return [fallback]
    raise SystemExit('No host C compiler found; set CC to clang or cc')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--receiver', type=Path,
                        default=ROOT / '.local/early-fault-qr/onesend-receiver.mjs')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--node', default='node', help='Node executable; an explicit Windows node.exe also works from WSL')
    args = parser.parse_args()
    manifest_path = ROOT / 'diagnostics/early-report/receiver-sources.json'
    manifest = json.loads(manifest_path.read_bytes())
    receiver, receiver_copy = pinned_receiver(args.receiver, manifest)
    node = shutil.which(args.node)
    if node is None:
        raise SystemExit('Node.js is required for real receiver interoperability')
    with tempfile.TemporaryDirectory(prefix='tb710fu-live-log-') as temporary:
        output = args.output_dir.resolve() if args.output_dir else Path(temporary)
        output.mkdir(parents=True, exist_ok=True)
        binary = output / ('live-protocol-test.exe' if os.name == 'nt' else 'live-protocol-test')
        command = compiler_command() + [
            '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            '-I', str(SOURCE), str(SOURCE / 'Test/live_protocol_test.c'),
            str(SOURCE / 'LiveProtocol.c'), '-o', str(binary),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(binary), str(output)], check=True)
        def node_path(path):
            if os.name != 'nt' and node.lower().endswith('.exe'):
                return subprocess.check_output(['wslpath', '-w', str(path)], text=True).strip()
            return str(path)
        receiver_command = [node, node_path(SOURCE / 'Test/check_receiver.mjs'),
                            '--receiver', node_path(receiver), '--fixtures', node_path(output),
                            '--output', node_path(output / 'receiver-check.json')]
        subprocess.run(receiver_command, check=True)
        source_paths = ['LiveProtocol.c', 'LiveProtocol.h', 'Test/live_protocol_test.c', 'Test/check_receiver.mjs']
        result = {'result': 'PASS', 'compile_command': command, 'receiver_command': receiver_command,
                  'sources_sha256': {str((SOURCE / name).relative_to(ROOT)).replace('\\', '/'):
                                     hashlib.sha256((SOURCE / name).read_bytes()).hexdigest()
                                     for name in source_paths},
                  'receiver_source': manifest,
                  'receiver_copy': receiver_copy,
                  'verification_inputs_sha256': {
                      str(Path(__file__).relative_to(ROOT)).replace('\\', '/'):
                          hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                      str(manifest_path.relative_to(ROOT)).replace('\\', '/'):
                          hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
                  },
                  'sanitizers': 'AddressSanitizer/UndefinedBehaviorSanitizer',
                  'capture_checks': json.loads((output / 'capture-check.json').read_bytes()),
                  'interop_result': json.loads((output / 'receiver-check.json').read_bytes()),
                  'device_operations': False, 'hardware_verified': False}
        (output / 'protocol-check.json').write_text(json.dumps(result, indent=2) + '\n')
        print('Live log protocol C/ASan/UBSan and pinned OneSend receiver checks PASS')


if __name__ == '__main__':
    main()
