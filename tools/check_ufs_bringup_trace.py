#!/usr/bin/env python3
"""Check that patches 0004 and 0005 add only NativeUfs diagnostic probes.

Baseline is the post-0002 NativeUfs.cpp, rebuilt here from the pinned source so
the check does not depend on a stale local copy.
"""
import argparse
import io
import re
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PIN = "99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0"
PATCH_DIR = ROOT / "patches/ufs"
CHECKPOINT = re.compile(r'^ *ufsCheckpoint \("([a-z]+)"\);\n', re.M)
# RootCommands.h is also included by the host tests, which have no
# ufsCheckpoint in scope. NativeUfs.cpp therefore selects the drawing path with
# a compile-time switch before including it, and every other includer compiles
# the probes away.
CMD_PROVIDER_SWITCH = re.compile(
    r'^// This translation unit owns ufsCheckpoint,.*\n'
    r'^// RootCommands\.h draw the footer here;.*\n'
    r'^#define TB710FU_CMD_CHECKPOINT_PROVIDER 1\n',
    re.M)
# The probe calls themselves, in either spelling.
CMD_PROBE_CALL = re.compile(r'^ *tb710fuCmdCheckpoint \("[a-z]+"\);\n', re.M)
CHECKPOINT_PROVIDER = """// Text checkpoints use the same optional live-log provider as SourceStorage.
extern "C" void tb710fu_live_log_checkpoint_text (const char *stage)
  __attribute__((weak_import));

static void ufsCheckpoint (const char *stage)
{
  if (tb710fu_live_log_checkpoint_text) {
    tb710fu_live_log_checkpoint_text (stage);
  }
}

"""
FOOTER_STAGES = {
    "ufsentry", "ufsgate", "ufsdeny", "ufsroot", "ufshost", "ufsgcc", "ufsgccrd",
    "ufshc", "ufshcrd", "ufsdma", "ufssmmu", "ufsqsnap", "ufsqown", "ufsnop",
    "ufspower", "ufswake", "ufslun", "ufslay", "ufssuper", "ufspub",
}


def strip_iolog(s):
    """Remove the checkpoint statements this patch adds.

    The traces were originally IOLog calls; they were switched to printf because
    IOLog's serial path did not reach the live-log ring from the bsd_init
    context while printf did. Both spellings are stripped so the equivalence
    proof is independent of which console call the patch uses.

    Only calls whose literal starts with "NativeUfs: host/root/start" are
    removed, so the nine pre-existing IOLog calls and the eight pre-existing
    snprintf calls stay in the comparison.
    """
    out = []
    i = 0
    head = re.compile(r'(?:IOLog|printf)\s*\(')
    while i < len(s):
        m = head.search(s, i)
        if not m:
            out.append(s[i:])
            break
        # Only treat it as a trace if the literal begins with our prefixes.
        lit = re.match(r'\s*"NativeUfs: (?:host|root|start)', s[m.end():])
        if not lit:
            out.append(s[i:m.end()])
            i = m.end()
            continue
        out.append(s[i:m.start()])
        j = m.end()
        depth = 1
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
                        default=PATCH_DIR / "0004-bringup-stage-trace.patch")
    args = parser.parse_args()
    ok = True

    def report(name, good, detail=""):
        nonlocal ok
        print(("PASS  " if good else "FAIL  ") + name + (("  " + detail) if detail else ""))
        if not good:
            ok = False

    with tempfile.TemporaryDirectory(prefix="ufs-trace-verify-") as temporary:
        scratch = Path(temporary)
        source = scratch / "NativeUfs.cpp"
        archive = subprocess.check_output(["git", "-C", str(args.source), "archive", PIN])
        with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
            tar.extractall(scratch, filter="data")
        for name in ("0001-sm8650-compatible.patch", "0002-transactional-handoff.patch"):
            subprocess.run(["git", "apply", str(PATCH_DIR / name)], cwd=scratch, check=True)
        baseline = source.read_text(encoding="utf-8")
        subprocess.run(["git", "apply", str(args.patch.resolve())], cwd=scratch, check=True)
        applied = source.read_text(encoding="utf-8")
        subprocess.run(["git", "apply", str(PATCH_DIR / "0005-synchronous-bringup-checkpoints.patch")],
                       cwd=scratch, check=True)
        checkpointed = source.read_text(encoding="utf-8")
        subprocess.run(["git", "apply", "--check", str(PATCH_DIR / "0003-minimal-source-runtime.patch")],
                       cwd=scratch, check=True)
    report("0001 -> 0002 -> 0004 -> 0005 -> 0003 applies", True)
    report("0005 uses the optional, guarded text checkpoint provider",
           checkpointed.count(CHECKPOINT_PROVIDER) == 1)
    without_checkpoints = CHECKPOINT.sub("", checkpointed.replace(CHECKPOINT_PROVIDER, "", 1))
    # 0004 adds the compile-time provider switch in NativeUfs.cpp and the
    # header-side probe calls. Both are scaffolding that 0004 introduces, so strip
    # them from BOTH sides before comparing -- otherwise the 0004 body keeps the
    # switch and the comparison is asymmetric.
    without_checkpoints = CMD_PROVIDER_SWITCH.sub("", without_checkpoints)
    without_checkpoints = CMD_PROBE_CALL.sub("", without_checkpoints)
    applied_without_scaffold = CMD_PROVIDER_SWITCH.sub("", applied)
    applied_without_scaffold = CMD_PROBE_CALL.sub("", applied_without_scaffold)
    report("0004 uses a compile-time switch so host tests keep compiling",
           CMD_PROVIDER_SWITCH.search(checkpointed) is not None)
    report("0005 leaves the post-0004 body intact apart from trace output and checkpoints",
           strip_iolog(without_checkpoints) == strip_iolog(applied_without_scaffold))
    stages = CHECKPOINT.findall(checkpointed)
    report("all footer labels are unique and fit the visible eight characters",
           set(stages) == FOOTER_STAGES and len(stages) == len(FOOTER_STAGES) and
           all(len(stage) <= 8 for stage in stages))
    report("NativeUfs::start publishes its entry before printf or provider access",
           'bool NativeUfs::start (IOService *provider)\n{\n  ufsCheckpoint ("ufsentry");\n'
           in checkpointed)
    queue_begin = '  ufsCheckpoint ("ufsqown");\n  if (!hostStopQueue (why))'
    queue_end = '  ufsCheckpoint ("ufsnop");\n  if (hostNop ()'
    ownership_window = checkpointed.partition(queue_begin)[2].partition(queue_end)
    report("queue ownership change has no synchronous draws during takeover",
           bool(ownership_window[1]) and "ufsCheckpoint" not in ownership_window[0])

    report("post-0002 body matches after removing diagnostic console traces",
           norm(baseline) == norm(applied_without_scaffold))
    print()
    guards = [
        (r'\bread32\s*\(', "MMIO read sites"),
        (r'\bOSDynamicCast\s*\(', "provider cast sites"),
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
        av = sorted(re.findall(pat, checkpointed))
        report("%s unchanged" % label, bv == av,
               "" if bv == av else "%s vs %s" % (bv[:5], av[:5]))

    # Count trace statements regardless of whether they use IOLog or printf.
    # Labels are "host ...", "root ..." or "start-..." (the start probes).
    trace = re.findall(
        r'(?:IOLog|printf) \("NativeUfs: (start-[a-z-]+|(?:host|root) [a-z-]+)', checkpointed)
    trace = [t[0] if isinstance(t, tuple) else t for t in trace]
    report("trace checkpoint count >= 40", len(trace) >= 40, "found %d" % len(trace))
    pairs = {}
    for t in trace:
        for stage in ("begin", "done", "failed"):
            if t.endswith("-" + stage):
                pairs.setdefault(t[: -len(stage) - 1], set()).add(stage)
    orphan = [k for k, v in pairs.items() if "begin" in v and not ({"done", "failed"} & v)]
    report("every begin has done/failed", not orphan, ",".join(orphan[:5]))

    # The seven start() probes must all be present.
    for probe in ("start-enter", "start-args", "start-gate-return", "start-options-ok",
                  "start-diag", "start-dispatch-root", "start-fallthrough"):
        report("start probe present: " + probe, probe in trace)

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
