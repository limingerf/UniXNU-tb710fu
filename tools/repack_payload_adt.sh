#!/bin/bash
# Rebuild a payload FAT image with only the ADT sidecar replaced.
#
# Packaging copies the ADT forward verbatim, so a sidecar that identifies as the
# hardware DTS rather than as a Mac is carried into every later delivery
# unnoticed - which is exactly what happened before board.toml grew
# [payload.adt]. This script swaps the sidecar in place and proves every other
# file and the FAT geometry are unchanged.
#
# usage: repack_payload_adt.sh <source-payload.img> <new-adt> <output.img>
#
# Run under WSL with mtools. Exits non-zero on any unexpected difference.
set -euo pipefail

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <source-payload.img> <new-adt> <output.img>" >&2
  exit 2
fi
SRC=$1
NEW_ADT=$2
OUT=$3

for f in "$SRC" "$NEW_ADT"; do
  [ -f "$f" ] || { echo "missing input: $f" >&2; exit 1; }
done
if [ "$OUT" = "$SRC" ]; then
  echo "refusing to overwrite the source payload" >&2
  exit 1
fi

rm -f "$OUT"
cp "$SRC" "$OUT"

echo "=== before ==="
mdir -i "$OUT" ::/EFI/UniXNU/Payloads | sed -n '1,12p'

echo ""
echo "=== replacing only EFI/UniXNU/Payloads/kernel.macho.adt ==="
MTOOLS_SKIP_CHECK=1 mcopy -o -i "$OUT" "$NEW_ADT" ::/EFI/UniXNU/Payloads/kernel.macho.adt

echo ""
echo "=== readback verification ==="
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
sha() { sha256sum "$1" | cut -d' ' -f1; }

K_REF=$(sha "$SRC.orig-kernel" 2>/dev/null || true)
# hash the original files straight from the source image
MTOOLS_SKIP_CHECK=1 mcopy -n -i "$SRC" ::/EFI/UniXNU/Payloads/kernel.macho "$T/kern.ref"
MTOOLS_SKIP_CHECK=1 mcopy -n -i "$SRC" ::/EFI/UniXNU/unixnu.toml            "$T/toml.ref"
MTOOLS_SKIP_CHECK=1 mcopy -n -i "$OUT" ::/EFI/UniXNU/Payloads/kernel.macho  "$T/kern.bin"
MTOOLS_SKIP_CHECK=1 mcopy -n -i "$OUT" ::/EFI/UniXNU/Payloads/kernel.macho.adt "$T/adt.bin"
MTOOLS_SKIP_CHECK=1 mcopy -n -i "$OUT" ::/EFI/UniXNU/unixnu.toml            "$T/toml.bin"

K_REF=$(sha "$T/kern.ref"); T_REF=$(sha "$T/toml.ref")
K_NEW=$(sha "$T/kern.bin"); T_NEW=$(sha "$T/toml.bin")
A_NEW=$(sha "$T/adt.bin");  A_REF=$(sha "$NEW_ADT")

echo "kernel.macho  before=$K_REF"
echo "kernel.macho  after =$K_NEW"
[ "$K_REF" = "$K_NEW" ] && echo "  kernel  MATCH (unchanged)" || { echo "  kernel  MISMATCH"; exit 1; }
echo "unixnu.toml   before=$T_REF"
echo "unixnu.toml   after =$T_NEW"
[ "$T_REF" = "$T_NEW" ] && echo "  toml    MATCH (unchanged)" || { echo "  toml    MISMATCH"; exit 1; }
echo "kernel.macho.adt wanted=$A_REF"
echo "kernel.macho.adt read  =$A_NEW"
[ "$A_REF" = "$A_NEW" ] && echo "  adt     MATCH (replaced as intended)" || { echo "  adt     MISMATCH"; exit 1; }

echo ""
echo "=== geometry unchanged? ==="
python3 - "$SRC" "$OUT" <<'PY'
import struct, sys, hashlib
def geom(path):
    d=open(path,'rb').read()
    bps=struct.unpack_from('<H',d,11)[0]; spc=d[13]
    res=struct.unpack_from('<H',d,14)[0]; nf=d[16]
    tot=struct.unpack_from('<I',d,32)[0]; fat=struct.unpack_from('<I',d,36)[0]
    return dict(size=len(d), bps=bps, spc=spc, reserved=res, nfats=nf,
                total=tot, fat_sz=fat, clusters=(tot-(res+nf*fat))//spc,
                sha=hashlib.sha256(d).hexdigest())
a,b=geom(sys.argv[1]),geom(sys.argv[2])
for k in a:
    same = a[k]==b[k]
    print(f"  {k:9s} {a[k]!s:>12} -> {b[k]!s:>12}  {'same' if same else 'CHANGED'}")
assert a['clusters']==b['clusters'] and a['size']==b['size']
print(f"\n  clusters={b['clusters']} (>=65525: {b['clusters']>=65525})")
PY
echo ""
sha256sum "$OUT"
