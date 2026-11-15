#!/usr/bin/env python3
"""Validate a payload FAT volume against EnhancedFatDxe's FAT-type rules.

Why this exists: a 25 MiB volume built with `mformat -F` came out as FAT32 with
only 50,380 clusters. The firmware's FAT driver refuses a FAT32 BPB below 65,525
clusters, so it never mounted the payload, UniXNU found no unixnu.toml and fell
back to its built-in menu -- which on the device looked like "no xnu entry"
rather than a filesystem error. This check makes that failure loud on the host.

Usage: check_payload_fat.py <image> [--expect-entry NAME ...]
Exits non-zero when the geometry is unmountable or an expected entry is missing.
"""
import argparse
import struct
import sys
from pathlib import Path

FAT32_MIN_CLUSTERS = 65525


def geometry(data: bytes) -> dict:
    if len(data) < 512 or data[510:512] != b"\x55\xaa":
        raise SystemExit("not a FAT boot sector (no 0x55AA signature)")
    bps = struct.unpack_from("<H", data, 11)[0]
    spc = data[13]
    reserved = struct.unpack_from("<H", data, 14)[0]
    fats = data[16]
    root_entries = struct.unpack_from("<H", data, 17)[0]
    total16 = struct.unpack_from("<H", data, 19)[0]
    fat16 = struct.unpack_from("<H", data, 22)[0]
    total32 = struct.unpack_from("<I", data, 32)[0]
    fat32 = struct.unpack_from("<I", data, 36)[0]
    root_cluster = struct.unpack_from("<I", data, 44)[0]
    label = data[82:90].decode(errors="replace").strip("\x00 ")

    total = total32 or total16
    fat = fat32 or fat16
    root_dir_sectors = ((root_entries * 32) + (bps - 1)) // bps
    data_sectors = total - (reserved + fats * fat + root_dir_sectors)
    clusters = data_sectors // spc if spc else 0
    kind = "FAT32" if fat32 else "FAT16"

    if kind == "FAT32":
        mountable = clusters >= FAT32_MIN_CLUSTERS
        reason = (f"FAT32 with {clusters} clusters "
                  + (f">= {FAT32_MIN_CLUSTERS} floor" if mountable
                     else f"< {FAT32_MIN_CLUSTERS} floor; EnhancedFatDxe rejects this volume"))
    else:
        mountable = clusters <= FAT32_MIN_CLUSTERS - 1
        reason = (f"FAT16 with {clusters} clusters "
                  + ("within the FAT16 range" if mountable
                     else "exceeds the FAT16 limit"))

    return {"bytes_per_sector": bps, "sectors_per_cluster": spc,
            "reserved": reserved, "fats": fats, "root_entries": root_entries,
            "total_sectors": total, "fat_size": fat, "root_dir_sectors": root_dir_sectors,
            "data_sectors": data_sectors, "clusters": clusters, "kind": kind,
            "root_cluster": root_cluster, "label": label,
            "mountable": mountable, "reason": reason,
            "cluster_bytes": spc * bps}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", type=Path)
    ap.add_argument("--expect-entry", action="append", default=[],
                    help="8.3 short-name fragment that must appear in the volume")
    args = ap.parse_args()

    data = args.image.read_bytes()
    g = geometry(data)
    print(f"{args.image}: {len(data):,} B  label={g['label']!r}")
    print(f"  {g['kind']}: bps={g['bytes_per_sector']} spc={g['sectors_per_cluster']} "
          f"cluster={g['cluster_bytes']} B")
    print(f"  total_sectors={g['total_sectors']} fat_size={g['fat_size']} "
          f"reserved={g['reserved']} fats={g['fats']}")
    print(f"  clusters={g['clusters']}")
    print(f"  {'PASS' if g['mountable'] else 'FAIL'}  mountable: {g['reason']}")

    ok = g["mountable"]
    for fragment in args.expect_entry:
        present = fragment.encode() in data
        ok &= present
        print(f"  {'PASS' if present else 'FAIL'}  entry {fragment}")

    if not ok:
        print("\nThis volume would not mount in the firmware; UniXNU would fall back "
              "to its built-in menu.")
        return 1
    print("\nAll payload FAT checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
