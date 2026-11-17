#!/usr/bin/env python3
"""Validate the actual native QR adapter and unchanged pinned Mu encoder.

The host C stage runs ASan/UBSan, repeated encoding and allocator boundaries,
then exports fixed512 K=1 and live4096 K=6 frames plus actual BGRA bitmaps.
--decode-only uses an independent ZXing decoder for exact frame and complete
file recovery, and can run in a different Python environment from compilation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "package/Tb710fuLiveLog"
QR_PIN = "b8d46c71610318e36a661819e1e3c0b8bdda413a"
SHIMS = ("Uefi.h", "Protocol/GraphicsOutput.h", "Library/BaseMemoryLib.h",
         "Library/DebugLib.h", "Library/MemoryAllocationLib.h", "Library/UefiBootServicesTableLib.h")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def decode(output, dependency_dir):
    if dependency_dir:
        import sys
        sys.path.insert(0, str(dependency_dir.resolve()))
    import zxingcpp
    from PIL import Image
    results = []
    for stem, count in (("fixed512", 2), ("live4096", 12)):
        blocks, sessions, checksum, envelope_size, block_count = {}, set(), None, None, None
        for index in range(count):
            name = f"{stem}-{index:02}"
            expected = (output / (name + ".bin")).read_bytes()
            bgra = (output / (name + ".bgra")).read_bytes()
            image = Image.frombytes("RGBA", (121, 121), bgra, "raw", "BGRA").convert("RGB")
            result = zxingcpp.read_barcode(image)
            if result is None or result.bytes != expected:
                raise SystemExit("Actual Mu QR did not decode to the exact source frame: " + name)
            if len(expected) != 752 or expected[:4] != b"OS\x02\x00":
                raise SystemExit("Unexpected OneSend frame header: " + name)
            if zlib.crc32(expected[:748]) != struct.unpack_from("<I", expected, 748)[0]:
                raise SystemExit("Frame checksum differs: " + name)
            session, sequence, block_count, block_bytes, envelope_size, checksum = struct.unpack_from("<6I", expected, 4)
            if block_bytes != 720 or sequence % 5 == 4:
                raise SystemExit("Unexpected systematic frame mapping: " + name)
            block = ((sequence // 5) * 4 + sequence % 5) % block_count
            payload = expected[28:748]
            if block in blocks and blocks[block] != payload:
                raise SystemExit("A transfer mutated a retained source block: " + name)
            blocks[block] = payload
            sessions.add(session)
        if len(sessions) != 1 or len(blocks) != block_count:
            raise SystemExit("Incomplete or mixed immutable transfer: " + stem)
        envelope = b"".join(blocks[n] for n in range(block_count))[:envelope_size]
        if zlib.crc32(envelope) != checksum or envelope[:6] != b"LMS1\x02\x00":
            raise SystemExit("Envelope checksum/header differs: " + stem)
        name_len, mime_len, length, crc, original_length, original_crc = struct.unpack_from("<HH4I", envelope, 6)
        body = envelope[26 + name_len + mime_len:]
        source = (output / (stem + ".log")).read_bytes()
        if body != source or length != len(body) or original_length != len(body) or crc != zlib.crc32(body) or original_crc != crc:
            raise SystemExit("Recovered log bytes differ: " + stem)
        filename = envelope[26:26 + name_len].decode("ascii")
        if (stem == "fixed512") != (block_count == 1) or stem == "fixed512" and "fixed" not in filename:
            raise SystemExit("Fixed snapshot is not a distinct K=1 transfer")
        image.save(output / (stem + "-last.png"))
        results.append({"fixture": stem, "QR_frames_decoded": count, "block_count": block_count,
                        "file_bytes": len(body), "filename": filename, "file_sha256": hashlib.sha256(body).hexdigest(),
                        "all_decoded_frame_bytes_exact": True, "recovered_file_bytes_exact": True})
    report = {"result": "PASS", "decoder": "independent ZXing-C++", "cases": results,
              "hardware_verified": False}
    (output / "qr-independent-decode.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qr-source", type=Path, default=ROOT / ".local/early-fault-qr/upstream")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--decode-only", action="store_true")
    parser.add_argument("--decode", action="store_true")
    parser.add_argument("--dependency-dir", type=Path)
    args = parser.parse_args()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if args.decode_only:
        decode(output, args.dependency_dir)
        return
    manifest = json.loads((ROOT / "diagnostics/early-report/qr-sources.json").read_text())
    if manifest.get("repository") != "microsoft/mu_plus" or manifest.get("commit") != QR_PIN:
        raise SystemExit("QR manifest differs from the audited Mu pin")
    for item in manifest["files"]:
        if digest(args.qr_source / item["path"]) != item["sha256"]:
            raise SystemExit("Pinned QR source hash differs: " + item["path"])
    include = output / "include"
    for relative in SHIMS:
        path = include / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("/* Real types/services supplied by forced LiveQrCompat.h. */\n", encoding="utf-8")
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler or not shutil.which(compiler[0]):
        raise SystemExit("No host C compiler; set CC to gcc or clang")
    sources = [SOURCE / "LiveQr.c", args.qr_source / "MsGraphicsPkg/Library/QrEncoderLib/QrEncoderLib.c",
               args.qr_source / "MsGraphicsPkg/Library/QrEncoderLib/QrEncoderTables.c"]
    common = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-fno-builtin",
              "-fno-omit-frame-pointer", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(SOURCE),
              "-I", str(include), "-I", str(args.qr_source / "MsGraphicsPkg/Include")]
    commands, objects, symbols = [], [], {}
    for source in sources:
        obj = output / (source.stem + ".o")
        command = compiler + common + ["-include", str(SOURCE / "LiveQrCompat.h")]
        if source.name.startswith("QrEncoder"):
            # The pinned 2016 source uses (-1 << Count) to construct an all-one
            # mask. Give its signed arithmetic explicit wrapping semantics;
            # keep every source byte and all sanitizer checks unchanged.
            command += ["-fwrapv", "-Wno-sign-compare", "-Wno-unused-parameter",
                        "-Wno-shift-negative-value", "-Wno-unused-but-set-variable"]
        command += ["-c", str(source), "-o", str(obj)]
        subprocess.run(command, check=True)
        symbol_text = subprocess.check_output(["nm", "-g", str(obj)], text=True)
        for line in symbol_text.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1].upper() != "U" and not parts[2].startswith("tb_live_"):
                # ASan's generated registration symbols are compiler runtime,
                # not QR/UEFI globals. All production definitions stay private.
                if not parts[2].startswith(("__odr_asan.", "___asan")):
                    raise SystemExit("Unprefixed QR external definition: " + parts[2])
        symbols[source.name] = symbol_text
        commands.append(command)
        objects.append(obj)
    binary = output / "live-qr-test"
    command = compiler + common + [str(SOURCE / "Test/live_qr_test.c"), str(SOURCE / "LiveProtocol.c"),
                                   *map(str, objects), "-o", str(binary)]
    commands.append(command)
    subprocess.run(command, check=True)
    subprocess.run([str(binary), str(output)], check=True)
    local = [SOURCE / name for name in ("LiveQr.c", "LiveQr.h", "LiveQrCompat.h", "LiveProtocol.c",
                                       "LiveProtocol.h", "Test/live_qr_test.c")]
    report = {"result": "PASS", "stage": "real-live-QR-and-Mu-host-test",
              "sanitizers": ["address", "undefined"], "QR_source": manifest,
              "source_hashes": {str(path.relative_to(ROOT)): digest(path) for path in local},
              "test_script_sha256": digest(Path(__file__)), "compile_commands": commands,
              "QR_symbols": symbols, "repeated_encodes": 64,
              "Mu_compile_semantics": "-fwrapv for its signed all-one mask; fixed Mu bytes unchanged",
              "fixtures": {"fixed512": {"bytes": 512, "blocks": 1, "frames": 2},
                           "live4096": {"bytes": 4096, "blocks": 6, "frames": 12}},
              "independent_decode_pending": not args.decode,
              "hardware_verified": False}
    (output / "qr-host-check.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.decode:
        decode(output, args.dependency_dir)
    print("Real LiveQr + unchanged pinned Mu: host ASan/UBSan and namespace checks PASS")


if __name__ == "__main__":
    main()
