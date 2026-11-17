"""Install three minimal standard color probes in a COPY of a native fileset.

Uses the exact pinned official parser/site locator and assembler unchanged.
No historical QR reporter, call wrappers, new segments, waits or reset probes.
The independent verifier reconstructs all allowed bytes before output is saved.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

from verify_standard_bands import (OFFICIAL_PROBE_SHA256, CAVE_BYTES, ROOT,
                                   source_hashes, sha, verify_standard_bands)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--framework", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--expect-kernel-sha256")
    args = parser.parse_args()
    original = args.kernel.read_bytes()
    if args.expect_kernel_sha256 and sha(original) != args.expect_kernel_sha256:
        raise SystemExit("Original kernel does not match the explicitly requested SHA-256")
    lock = json.loads((ROOT / "upstream.lock.json").read_text())
    relative = "scripts/patch-xnu-entry-probe.py"
    path = args.framework / relative
    pinned = subprocess.check_output(["git", "-C", str(args.framework), "show",
                                      lock["unixnu"]["commit"] + ":" + relative])
    if pinned != path.read_bytes() or sha(pinned) != OFFICIAL_PROBE_SHA256:
        raise SystemExit("Official probe/parser is not the unchanged pinned source")
    before_sources = source_hashes()
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("official_standard_probe", path)
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)
    # LLVM's fileset output has three-field "Symbols ..." headings. The pinned
    # parser assumes every three-field row begins with a hex address. Keep its
    # source unchanged and adapt only that documented text-input boundary, as
    # the existing early-report builder does; the verifier reads nlists itself.
    def numeric_symbols(kernel):
        found = {}
        for line in subprocess.check_output(["llvm-nm", "-n", str(kernel)], text=True).splitlines():
            fields = line.split()
            if len(fields) == 3 and re.fullmatch("[0-9a-fA-F]+", fields[0]):
                found.setdefault(int(fields[0], 16), fields[2])
        return found
    probe.kernel_symbols = numeric_symbols
    data = bytearray(original)
    probe.load_segments(data)
    probe.rebase(args.kernel, data)
    output = args.output_dir.resolve()
    candidate = output / "kernel-standard-bands.macho"
    report_path = output / "build-report.json"
    if candidate.exists() or report_path.exists():
        raise SystemExit("Output exists; preserve previous evidence")
    output.mkdir(parents=True, exist_ok=True)
    checkpoints, spans = [], []
    for stage, site, target, cave in (
            (1, probe.ENTRY_VA, probe.START_FIRST_CPU, probe.ENTRY_VA + 0x80),
            (2, probe.MMU_ENABLE_VA, probe.MMU_ENABLE_VA + 4, probe.ENTRY_VA + 0x200),
            (3, probe.AFTER_MMU_VA, probe.AFTER_MMU_VA + 4, probe.ENTRY_VA + 0x380)):
        at = probe.file_offset(cave)
        if any(original[at:at + CAVE_BYTES]) or len(original[at:at + CAVE_BYTES]) != CAVE_BYTES:
            raise SystemExit("Standard probe cave is not entirely zero")
        expected = {1: probe.branch(site, target), 2: 0xd5181000, 3: 0xd2a00600}[stage]
        site_offset = probe.file_offset(site)
        if struct.unpack_from("<I", original, site_offset)[0] != expected:
            raise SystemExit("Standard probe site does not contain the original instruction")
        source = (ROOT / "diagnostics/early-report" / ("StandardCompact" + str(stage) + ".S")).read_text()
        code = probe.assemble(source, cave, {"target": target}, output)
        if not code or len(code) > CAVE_BYTES:
            raise SystemExit("Standard probe does not fit the guarded cave")
        checkpoints.append({"stage": stage, "site": hex(site), "target": hex(target),
                            "cave": hex(cave), "code_bytes": len(code), "code_sha256": sha(code)})
        for kind, offset, replacement in (
                ("compact_code", at, code),
                ("compact_branch", site_offset, struct.pack("<I", probe.branch(site, cave)))):
            spans.append({"kind": kind, "offset": offset,
                          "before_hex": original[offset:offset + len(replacement)].hex(),
                          "after_hex": replacement.hex()})
            data[offset:offset + len(replacement)] = replacement
    if before_sources != source_hashes():
        raise SystemExit("Sources changed during transform; output rejected")
    report = {"schema_version": 1, "stage": "standard-early-bands-derivative",
              "original_sha256": sha(original), "kernel_sha256": sha(data), "kernel_bytes": len(data),
              "source_hashes": before_sources,
              "symbol_text_adapter": "numeric-address llvm-nm rows only; fileset headings ignored",
              "official_probe": {"repository": lock["unixnu"]["repository"],
                                 "commit": lock["unixnu"]["commit"], "path": relative,
                                 "sha256": sha(pinned)},
              "compact_checkpoints": checkpoints, "patch_spans": spans,
              "new_segments": False, "QR_reporter": False, "intentional_waits": 0,
              "hardware_verified": False,
              "limits": "Three early completed-stage markers only; no proof of later execution or hardware boot."}
    verification = verify_standard_bands(original, bytes(data), report)
    candidate.write_bytes(data)
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (output / "verification.json").write_text(json.dumps(verification, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(verification, indent=2))


if __name__ == "__main__":
    main()
