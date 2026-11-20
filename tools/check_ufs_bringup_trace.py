#!/usr/bin/env python3
"""Prove patches/ufs/0004-bringup-stage-trace.patch only adds IOLog output.

Baseline is the post-0002 NativeUfs.cpp, rebuilt here from the pinned source so
the check does not depend on a stale local copy. Removing every IOLog call from
the applied result must reproduce that baseline after brace/whitespace
normalisation; hardware calls, wait bounds, error strings and panic sites must
be unchanged.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PIN = "99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0"


def strip_iolog(s):
    out, i = [], 0
    while i < len(s):
        m = re.compile(r'\bIOLog\s*\(').match(s, i)
        if not m:
            out.append(s[i])
            i += 1
            continue
        j, depth = m.end(), 1
        while j < len(s) and depth:
            depth += (s[j] == '(') - (s[j] == ')')
            j += 1
        while j < len(s) and s[j] in ' \t':
            j += 1
        if j < len(s) and s[j] == ';':
            j += 1
        i = j
    return "".join(out)


def norm(s):
    s = strip_iolog(s)
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    s = re.sub(r'//[^\n]*', '', s)
    s = re.sub(r'\s+', ' ', s)
    s = s.replace('{', ' ').replace('}', ' ')
    s = re.sub(r'\s+', ' ', s)
    s = re.sub(r' (\W)', r'\1', s)
    s = re.sub(r'(\W) ', r'\1', s)
    return s.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True,
                        help="Pinned ShengUfs checkout (git archive source)")
    parser.add_argument("--patch", type=Path,
                        default=ROOT / "patches/ufs/0004-bringup-stage-trace.patch")
    args = parser.parse_args()
    ok = True

    def report(name, good, detail=""):
        nonlocal ok
        print(("PASS  " if good else "FAIL  ") + name + (("  " + detail) if detail else ""))
        if not good:
            ok = False

    scratch = Path(tempfile.mkdtemp(prefix="ufs-trace-verify-"))
    archive = subprocess.check_output(["git", "-C", str(args.source), "archive", PIN])
    import io
    import tarfile
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(scratch, filter="data")
    subprocess.run(["git", "init", "--quiet"], cwd=scratch, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for name in ("0001-sm8650-compatible.patch", "0002-transactional-handoff.patch"):
        subprocess.run(["git", "apply", str(ROOT / "patches/ufs" / name)],
                       cwd=scratch, check=True)
    baseline = (scratch / "NativeUfs.cpp").read_text(encoding="utf-8")

    (scratch / "p.patch").write_bytes(args.patch.read_bytes())
    r = subprocess.run(["git", "apply", "p.patch"], cwd=scratch,
                       capture_output=True, text=True)
    report("0004 applies to the post-0002 baseline", r.returncode == 0,
           r.stderr.strip()[:200])
    if r.returncode:
        return 1
    applied = (scratch / "NativeUfs.cpp").read_text(encoding="utf-8")

    report("semantic body identical once IOLog is removed",
           norm(baseline) == norm(applied))
    print()
    guards = [
        (r'write32\s*\(', "write32 calls"),
        (r'IOSleep\s*\(\s*(\d+)\s*\)', "IOSleep constants"),
        (r'hostStopQueue\s*\(', "hostStopQueue calls"),
        (r'hostStartQueue\s*\(', "hostStartQueue calls"),
        (r'hostStopTakenQueue\s*\(', "hostStopTakenQueue calls"),
        (r'restoreUfsSmmu\s*\(', "restoreUfsSmmu calls"),
        (r'smmuBypassStream\s*\(', "smmuBypassStream calls"),
        (r'hostNop\s*\(', "hostNop calls"),
        (r'hostQueryRead\s*\(', "hostQueryRead calls"),
        (r'hostScsi\s*\(', "hostScsi calls"),
        (r'native_ufs_root_step\s*=\s*\d', "root step values"),
        (r'\bpanic\s*\(', "panic sites"),
        (r'\*why = "([^"]+)"', "error strings"),
    ]
    for pat, label in guards:
        bv = sorted(re.findall(pat, baseline))
        av = sorted(re.findall(pat, applied))
        report("%s unchanged" % label, bv == av,
               "" if bv == av else "%s vs %s" % (bv[:5], av[:5]))

    trace = re.findall(r'IOLog \("NativeUfs: (?:host|root) ([a-z-]+)', applied)
    report("trace checkpoint count >= 30", len(trace) >= 30, "found %d" % len(trace))
    pairs = {}
    for t in trace:
        for stage in ("begin", "done", "failed"):
            if t.endswith("-" + stage):
                pairs.setdefault(t[: -len(stage) - 1], set()).add(stage)
    orphan = [k for k, v in pairs.items() if "begin" in v and not ({"done", "failed"} & v)]
    report("every begin has done/failed", not orphan, ",".join(orphan[:5]))

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
