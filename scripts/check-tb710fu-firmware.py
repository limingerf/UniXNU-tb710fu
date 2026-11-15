"""Official firmware preflight: verify the deployed source overlay and board policy."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys

device = Path(os.environ['UNIXNU_DEVICE'])
uefi = Path(os.environ['UNIXNU_UEFI'])
# The opt-in UFS build flips TB710FU_ENABLE_UFS in the deployed DSC. That edit is the
# only permitted difference from the source overlay, and it is reported by the build
# step that made it; every other file must still match byte for byte.
ufs_variant = os.environ.get('UNIXNU_ENABLE_UFS') == '1'
allowed_edit = Path('Platforms/Lenovo/tb710fuPkg/tb710fu.dsc')
overlay = device / 'firmware/overlay'
for source in overlay.rglob('*'):
    if not source.is_file() or '__pycache__' in source.parts:
        continue
    relative = source.relative_to(overlay)
    if ufs_variant and relative == allowed_edit:
        continue
    target = uefi / relative
    if not target.is_file() or hashlib.sha256(source.read_bytes()).digest() != hashlib.sha256(target.read_bytes()).digest():
        raise SystemExit('Deployed UEFI overlay differs: ' + str(relative))
checker = overlay / 'Platforms/Lenovo/tb710fuPkg/Tools/check-board.py'
subprocess.run([sys.executable, str(checker), '--uefi-root', str(uefi)], check=True)
