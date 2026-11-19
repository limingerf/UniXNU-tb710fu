#!/usr/bin/env python3
"""Exercise the portable framebuffer renderer; no hardware or QR encoder.

Its host fixture checks all changed pixels, guard regions, padded scanlines,
the exact upstream byte-band formula, invalid input rejection and status/QR
updates preserving the three bands. Checkpoint mode also keeps a bottom-right QR
clear of a bounded scrolling log pane. Host execution is not panel validation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "package/Tb710fuLiveLog"
FONT = ROOT / "diagnostics/early-report/Font.h"
FONT_SHA256 = "e3174a605a6cfb351e5fd613f5c0d29c509a873fc3218b79fc4e7115fe405071"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler or not shutil.which(compiler[0]):
        raise SystemExit("No host C compiler; set CC to gcc or clang")
    font_sha = hashlib.sha256(FONT.read_bytes()).hexdigest()
    if font_sha != FONT_SHA256:
        raise SystemExit("Licensed EDK II font dependency differs from its recorded SHA-256")
    with tempfile.TemporaryDirectory(prefix="tb710fu-live-display-") as directory:
        binary = Path(directory) / "live-display-test"
        command = compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                              "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
                              "-I", str(SOURCE), "-I", str(FONT.parent), str(SOURCE / "Test/live_display_test.c"),
                              str(SOURCE / "LiveDisplay.c"), "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
    sources = [SOURCE / "LiveDisplay.c", SOURCE / "LiveDisplay.h",
               SOURCE / "Test/live_display_test.c", Path(__file__).resolve(), FONT]
    report = {
        "stage": "portable-live-log-display-host-validation",
        "result": "PASS",
        "sanitizers": ["address", "undefined"],
        "standard_bands_source": {
            "repository": "UniXNU/UniXNU",
            "commit": "0c1bac3d51a3009dca1708b15a5198f4b43d057a",
            "paths": ["scripts/patch-xnu-entry-probe.py", "docs/zh/guide/debugging.md"],
            "formula": "band_bytes=((stride_bytes*height)>>4)&~7; offsets=0,band_bytes,2*band_bytes",
            "bgra32": ["0xff00ff00", "0xff0000ff", "0xffff00ff"],
        },
        "font": {"path": str(FONT.relative_to(ROOT)), "sha256": font_sha,
                 "source": "EDK II MdeModulePkg/Universal/Console/GraphicsConsoleDxe/LaffStd.c",
                 "copyright": "Copyright (c) 2006 - 2008, Intel Corporation. All rights reserved.",
                 "license": "BSD-2-Clause-Patent", "existing_dependency_unchanged": True},
        "sources": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in sources},
        "geometry": [[3200, 2000, 12800], [640, 480, 2560], [641, 481, 2576],
                     [240, 480, 960], [153, 240, 616], [289, 480, 1160],
                     [290, 480, 1160], [1600, 900, 6412]],
        "dual_QR": "independent fixed-left and live-right rectangles; status row is below both codes; small surfaces share one caller-alternated slot",
        "checkpoint_display": {"qr_anchor": "bottom-right", "log_region": "left-of-QR",
                               "scroll_max_rows": 64, "scroll_max_columns": 256,
                               "offset_model": "absolute-range; append-unseen-suffix; mark-overwrite-gap",
                               "source": "captured XNU ring tail", "console_writer_draws": False},
        "hardware_verified": False,
        "limits": "Host memory rendering only; no actual panel, cache, timing, mapping or concurrency proof.",
    }
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"result": "PASS", "sources": report["sources"], "hardware_verified": False}))


if __name__ == "__main__":
    main()
