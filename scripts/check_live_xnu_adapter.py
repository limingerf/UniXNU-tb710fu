#!/usr/bin/env python3
"""Test the actual native LiveXnu adapter with isolated host XNU services.

Protocol and display are the production C sources. QR encoding alone is stubbed
here and checked separately using the pinned actual Mu encoder. This fixture
does not emulate the real scheduler, caches, MMU, framebuffer or hardware.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'package/Tb710fuLiveLog'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command):
    result = subprocess.run(command, text=True, capture_output=True)
    if result.stdout:
        print(result.stdout, end='')
    if result.stderr:
        print(result.stderr, end='')
    result.check_returncode()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--output-dir', type=Path,
                        default=ROOT / '.work/live-xnu-adapter-check')
    args = parser.parse_args()
    compiler = shutil.which(args.cc)
    if not compiler:
        raise SystemExit('Missing C compiler: ' + args.cc)
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='tb710fu-live-xnu-') as temporary:
        scratch = Path(temporary)
        run(['git', 'init', '--quiet', str(scratch)])
        # Build the ABI header by applying actual patches, rather than maintaining
        # a second hand-written copy in the host shim.
        for name in ('0004-tb710fu-ramoops-console.patch', '0013-tb710fu-ramoops-snapshot.patch',
                     '0014-tb710fu-native-live-log.patch'):
            run(['git', '-C', str(scratch), 'apply',
                 '--include=osfmk/kern/tb710fu_ramoops.h',
                 '--include=osfmk/kern/tb710fu_live_log.h', str(ROOT / 'patches/xnu' / name)])
        binary = scratch / 'live-xnu-adapter-test'
        command = [compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   '-fno-sanitize-recover=all', '-I', str(ROOT), '-I', str(scratch / 'osfmk'),
                   '-I', str(SOURCE), '-I', str(ROOT / 'diagnostics/early-report'),
                   str(ROOT / 'tests/live_xnu_adapter_test.c'), str(SOURCE / 'LiveProtocol.c'),
                   str(SOURCE / 'LiveDisplay.c'), '-o', str(binary)]
        run(command)
        result = run([str(binary)])
        names = ['tests/live_xnu_adapter_test.c', 'scripts/check_live_xnu_adapter.py',
                 'patches/xnu/0013-tb710fu-ramoops-snapshot.patch',
                 'patches/xnu/0014-tb710fu-native-live-log.patch',
                 'package/Tb710fuLiveLog/LiveXnu.c', 'package/Tb710fuLiveLog/LiveProtocol.c',
                 'package/Tb710fuLiveLog/LiveProtocol.h', 'package/Tb710fuLiveLog/LiveDisplay.c',
                 'package/Tb710fuLiveLog/LiveDisplay.h', 'diagnostics/early-report/Font.h']
        report = {
            'schema_version': 1,
            'stage': 'native-live-xnu-adapter-host-check',
            'result': 'PASS',
            'groups': 10,
            'invalid_video_cases': 15,
            'sanitizers': ['address', 'undefined'],
            'source_sha256': {name: sha(ROOT / name) for name in names},
            'derived_header_sha256': {
                name: sha(scratch / 'osfmk/kern' / name)
                for name in ('tb710fu_ramoops.h', 'tb710fu_live_log.h')
            },
            'compiler': run([compiler, '--version']).stdout.splitlines()[0],
            'output': result.stdout.strip(),
            'qr_encoder': 'Stubbed frame recorder; production Mu QR checked separately.',
            'checks': {
                'independent_xnu_and_ramoops_bytes': True,
                'zero_initialized_capture_survives_display_init': True,
                'every_left_file_retains_early_backup_after_wrap': True,
                'metadata_lengths_and_current_filename_range': True,
                'stream_frames_match_own_immutable_envelopes': True,
                '16_round_replacement_gate_per_stream': True,
                'one_frame_per_global_250ms_tick_including_refresh': True,
                'initial_raw_ramoops_frame_before_freeze': True,
                'worker_not_started_uses_single_frame_latest_tail': True,
                'single_frame_keeps_early_backup_deferred_metadata': True,
                'dual_slots_preserve_other_stream': True,
                'small_screen_alternates_one_bounded_rectangle': True,
                'capture_and_render_contention_skip_without_wait': True,
                'worker_creation_does_not_imply_execution': True,
                'busy_empty_freeze_is_not_retried_as_early': True,
            },
            'limits': 'Host XNU-service simulation; native ABI, real scheduling/MMU/caches and device behavior unverified.',
            'device_operations': False,
        }
        (output / 'adapter-check.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
