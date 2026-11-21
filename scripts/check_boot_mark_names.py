#!/usr/bin/env python3
"""Every bottom-band tag must fit the 8 characters left after the lane prefix.

live_display_boot_mark() copies the 4-byte prefix "BSD " or "CDC " into a
12-column buffer and then copies the stage name until the buffer ends, so a
stage longer than 8 characters is silently truncated. Two different stages that
share their first 8 characters become indistinguishable on screen, which is
exactly how pseudo000..pseudo004 all rendered as pseudo00.
"""
from pathlib import Path
import re
import sys

LANE_CHARACTERS = 12
PREFIX_CHARACTERS = 4
LIMIT = LANE_CHARACTERS - PREFIX_CHARACTERS  # 8

ROOT = Path(__file__).resolve().parents[1]


def literals():
    """Every string literal passed to a boot/scheduler mark macro."""
    pattern = re.compile(
        r'TB710FU_(?:BSD|DLIL)_(?:MARK|SCHED_MARK)\s*\(\s*"([^"]*)"')
    ternary = re.compile(
        r'TB710FU_(?:BSD|DLIL)_(?:MARK|SCHED_MARK)\s*\([^;]*?\)\s*;')
    inner = re.compile(r'"([a-z][a-z0-9_]*)"')
    for path in sorted((ROOT / "patches/xnu").glob("*.patch")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in pattern.finditer(text):
            yield path.name, match.group(1)
        for match in ternary.finditer(text):
            body = match.group(0)
            if body.count('"') < 2:
                continue
            for name in inner.findall(body):
                yield path.name, name


def main():
    seen = {}
    failures = []
    for patch, tag in literals():
        if len(tag) > LIMIT:
            failures.append((patch, tag))
        seen.setdefault(tag, set()).add(patch)
    # Two stages sharing their first LIMIT characters are indistinguishable.
    clipped = {}
    for tag in seen:
        clipped.setdefault(tag[:LIMIT], []).append(tag)
    for prefix, tags in sorted(clipped.items()):
        if len(tags) > 1:
            failures.append(("collision", f"{prefix!r} <- {sorted(tags)}"))
    if failures:
        for patch, tag in failures:
            print(f"FAIL {patch}: {tag}", file=sys.stderr)
        raise SystemExit(f"{len(failures)} bottom-band tag problem(s)")
    print(f"boot marks: {len(seen)} distinct names, all within "
          f"{LIMIT} characters and pairwise distinct after clipping")


if __name__ == "__main__":
    main()
