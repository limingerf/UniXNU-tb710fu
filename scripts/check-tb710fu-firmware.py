"""Official firmware preflight: verify the deployed source overlay and board policy."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys

device = Path(os.environ['UNIXNU_DEVICE'])
uefi = Path(os.environ['UNIXNU_UEFI'])
overlay = device / 'firmware/overlay'
for source in overlay.rglob('*'):
    if not source.is_file() or '__pycache__' in source.parts:
        continue
    target = uefi / source.relative_to(overlay)
    if not target.is_file() or hashlib.sha256(source.read_bytes()).digest() != hashlib.sha256(target.read_bytes()).digest():
        raise SystemExit('Deployed UEFI overlay differs: ' + str(source.relative_to(overlay)))
checker = overlay / 'Platforms/Lenovo/tb710fuPkg/Tools/check-board.py'
subprocess.run([sys.executable, str(checker), '--uefi-root', str(uefi)], check=True)
