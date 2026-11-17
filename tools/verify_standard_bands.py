"""Independently verify a three-probe standard-band kernel derivative.

Only six original-file spans may differ: three guarded zero caves and three
direct branches. The verifier reconstructs every AArch64 instruction without
running the producer, assembler or compiler. No segments or waits are added.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from verify_storage_integration import load_audit

ROOT = Path(__file__).resolve().parents[1]
OFFICIAL_PROBE_SHA256 = "159eb1ea8d6c1da30f26913a281f46e21d940a1ae7886656529907ec3edf96ad"
SOURCE_PATHS = tuple("diagnostics/early-report/StandardCompact" + str(n) + ".S" for n in (1, 2, 3)) + (
    "tools/patch_kernel_standard_bands.py", "tools/verify_standard_bands.py",
    "tools/verify_storage_integration.py", "scripts/audit-tb710fu-storage-abi.py", "upstream.lock.json")
CAVE_BYTES = 0x180


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(value, message):
    if not value:
        raise ValueError("Standard bands: " + message)


def source_hashes():
    return {name: sha((ROOT / name).read_bytes()) for name in SOURCE_PATHS}


def branch(site, target):
    delta = target - site
    require(not delta % 4 and -(1 << 27) <= delta < (1 << 27), "direct branch out of range")
    return 0x14000000 | ((delta // 4) & 0x3ffffff)


def word(image, address):
    return struct.unpack("<I", image.read(address, 4))[0]


def layout(original):
    """Locate sites from actual fileset commands, native symbols and instructions."""
    audit = load_audit()
    root = audit.MachO(original)
    require(root.kind == 12 and root.subtype & 0xffffff == 2 and
            "com.apple.kernel" in root.entries, "expected an arm64e native fileset")
    require(not any(s["name"] in ("__KDIAG", "__KDATRW") for s in root.segments),
            "input already has the historical QR reporter")
    native = audit.MachO(original, root.entries["com.apple.kernel"][1])
    require(native.kind == 2 and native.subtype & 0xffffff == 2, "invalid native executable")
    definitions = [s for s in native.nlists() if s["name"] == "_start_first_cpu" and
                   not s["kind"] & 0xe0 and s["kind"] & 0x0e == 0x0e]
    require(len(definitions) == 1, "missing or ambiguous start_first_cpu")
    definition = definitions[0]
    require(0 < definition["section"] <= len(native.sections), "invalid start_first_cpu section")
    section = native.sections[definition["section"] - 1]
    start = definition["value"]
    require(section["name"] == "__text" and section["flags"] & 0x80000000 and
            section["va"] <= start < section["va"] + section["size"],
            "start_first_cpu is not executable text")
    entries = [p for c, p, size in root.commands if c == 5 and size >= 288]
    require(len(entries) == 1 and struct.unpack_from("<II", original, entries[0] + 8) == (6, 68),
            "expected one ARM_THREAD_STATE64 entry")
    entry = struct.unpack_from("<Q", original, entries[0] + 16 + 32 * 8)[0]
    require(entry % 4 == 0 and word(root, entry) == branch(entry, start),
            "entry does not branch directly to start_first_cpu")
    matches = []
    for address in range(start, start + 0x1000, 4):
        if (word(root, address) == 0xd5181000 and word(root, address + 4) == 0xd5033fdf and
                word(root, address + 0x20) == 0xd2a00600 and
                word(root, address + 0x24) == 0xd5181040):
            matches.append(address)
    require(len(matches) == 1, "expected one bounded SCTLR/ISB/MOV/CPACR sequence")
    mmu = matches[0]
    result = []
    spans = []
    for stage, site, target, cave in (
            (1, entry, start, entry + 0x80),
            (2, mmu, mmu + 4, entry + 0x200),
            (3, mmu + 0x20, mmu + 0x24, entry + 0x380)):
        offset = root.offset(cave, CAVE_BYTES)
        require(not any(original[offset:offset + CAVE_BYTES]), "probe cave is not entirely zero")
        require(native.offset(cave, CAVE_BYTES) == offset, "native/root cave mappings differ")
        require(native.offset(site, 4) == root.offset(site, 4), "native/root site mappings differ")
        require(section["va"] <= cave and cave + CAVE_BYTES <= section["va"] + section["size"],
                "probe cave is outside native executable text")
        require(not any(cave <= s["value"] < cave + CAVE_BYTES for s in native.nlists()
                        if not s["kind"] & 0xe0 and s["kind"] & 0x0e == 0x0e),
                "a native symbol occupies the probe cave")
        spans.extend(((site, site + 4), (cave, cave + CAVE_BYTES)))
        result.append({"stage": stage, "site": site, "target": target, "cave": cave})
    for index, (low, high) in enumerate(spans):
        require(all(high <= a or b <= low for a, b in spans[:index]), "probe ranges overlap")
    return root, result


def expected_code(stage, cave, target):
    # Literal reviewed instruction encodings, independent of the producer's
    # assembler. PAINT uses exact upstream band bytes; CBZ safely skips an empty
    # framebuffer. Stage 3 only replays MOV and returns to original code.
    words = {
        1: [0xf9401401, 0xf9401c02, 0xf9402403, 0x9b037c42, 0xd344fc42,
            0x927df042, 0xb4000142, 0xd2800024, 0x9b047c45, 0x8b050021,
            0x52801fe6, 0x72bfe006, 0x28811826, 0xf1002042, 0x54ffffc8, 0xd5033f9f],
        2: [0xf9401681, 0xf9401e82, 0xf9402683, 0x9b037c42, 0xd344fc42,
            0x927df042, 0xb4000142, 0xd2800044, 0x9b047c45, 0x8b050021,
            0x52801fe6, 0x72bfffe6, 0x28811826, 0xf1002042, 0x54ffffc8,
            0xd5033f9f, 0xd5181000],
        3: [0xd2a00600],
    }[stage]
    words = words + [branch(cave + len(words) * 4, target)]
    return struct.pack("<" + str(len(words)) + "I", *words)


def verify_standard_bands(original, candidate, report):
    try:
        require(isinstance(report, dict) and report.get("schema_version") == 1 and
                report.get("stage") == "standard-early-bands-derivative", "wrong report schema/stage")
        require(report.get("original_sha256") == sha(original) and
                report.get("kernel_sha256") == sha(candidate) and
                report.get("kernel_bytes") == len(candidate) == len(original), "kernel hash/size differs")
        require(report.get("source_hashes") == source_hashes(), "transform source hashes differ")
        require(report.get("symbol_text_adapter") ==
                "numeric-address llvm-nm rows only; fileset headings ignored", "symbol text adapter differs")
        lock = json.loads((ROOT / "upstream.lock.json").read_text())
        require(report.get("official_probe") == {
            "repository": lock["unixnu"]["repository"], "commit": lock["unixnu"]["commit"],
            "path": "scripts/patch-xnu-entry-probe.py", "sha256": OFFICIAL_PROBE_SHA256},
            "official probe identity differs")
        root, sites = layout(original)
        rebuilt = bytearray(original)
        checkpoints, spans = [], []
        for item in sites:
            stage, site, target, cave = (item[k] for k in ("stage", "site", "target", "cave"))
            code = expected_code(stage, cave, target)
            checkpoints.append({"stage": stage, "site": hex(site), "target": hex(target),
                                "cave": hex(cave), "code_bytes": len(code), "code_sha256": sha(code)})
            for kind, address, replacement in (
                    ("compact_code", cave, code),
                    ("compact_branch", site, struct.pack("<I", branch(site, cave)))):
                at = root.offset(address, len(replacement))
                spans.append({"kind": kind, "offset": at, "before_hex": original[at:at + len(replacement)].hex(),
                              "after_hex": replacement.hex()})
                rebuilt[at:at + len(replacement)] = replacement
        require(report.get("compact_checkpoints") == checkpoints, "reported checkpoint contract differs")
        require(report.get("patch_spans") == spans, "reported six patch spans differ")
        require(bytes(rebuilt) == candidate, "unexpected bytes outside the six reconstructed spans")
        require(report.get("new_segments") is False and report.get("QR_reporter") is False and
                report.get("intentional_waits") == 0 and report.get("hardware_verified") is False,
                "scope claims differ")
        return {"stage": "verified-standard-early-bands-derivative", "result": "PASS",
                "original_prelink_sha256": sha(original), "kernel_sha256": sha(candidate),
                "kernel_bytes": len(candidate), "patch_spans_verified": 6,
                "compact_probes_verified": 3, "other_bytes_unchanged": True,
                "segments_unchanged": True, "no_QR_reporter_or_waits": True,
                "hardware_verified": False}
    except (AssertionError, KeyError, StopIteration, struct.error, UnicodeDecodeError,
            TypeError, IndexError, OverflowError) as error:
        raise ValueError("Standard bands: malformed/incomplete Mach-O or report") from error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = verify_standard_bands(args.original.read_bytes(), args.candidate.read_bytes(),
                                   json.loads(args.report.read_text()))
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


# Match the package verifier's conventional import name.
verify = verify_standard_bands


if __name__ == "__main__":
    main()
