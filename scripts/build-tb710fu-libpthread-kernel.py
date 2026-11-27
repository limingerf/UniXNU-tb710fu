#!/usr/bin/env python3
"""Build the TB710FU libpthread kernel archive with device workq stack patch.

Applies device-owned patches/libpthread/*.patch onto the libpthread work tree
before delegating to the official framework build-libpthread-kernel.py script.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DEVICE = Path(os.environ['UNIXNU_DEVICE']).resolve() if os.environ.get('UNIXNU_DEVICE') else ROOT
SOURCE = ROOT / 'refer/libpthread'
REVISION = os.environ.get('UNIXNU_LIBPTHREAD_COMMIT', 'c0e60fb5ecfc54cf379c261d3f9572cc22ded891')
WORK = ROOT / '.cache/libpthread-work'

if WORK.exists() and subprocess.check_output(['git', '-C', str(WORK), 'rev-parse', 'HEAD'], text=True).strip() != REVISION:
    shutil.rmtree(WORK)
if not WORK.exists():
    subprocess.run(['git', 'clone', '--shared', str(SOURCE), str(WORK)], check=True)

patch_dir = DEVICE / 'patches/libpthread'
if patch_dir.is_dir():
    for patch in sorted(patch_dir.glob('*.patch')):
        if subprocess.run(['git', '-C', str(WORK), 'apply', '--reverse', '--check', str(patch)],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
            subprocess.run(['git', '-C', str(WORK), 'apply', str(patch)], check=True)

subprocess.run([sys.executable, str(ROOT / 'scripts/build-libpthread-kernel.py')], check=True)
