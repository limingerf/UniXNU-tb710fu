#!/usr/bin/env python3
"""Host verification for the TB710FU platform services package.

Two halves, both required to pass:

  1. the C suites in package/Tb710fuPlatformServices/Test/ - SPMI v7 framing,
     the PMK8550 clock against a modelled arbiter, epoch/civil conversion, the
     PSCI/reboot-reason ladder including EDL, and the staged-ladder, SY7758 and
     TSENS arithmetic;
  2. a text-level audit of the three kernel translation units, because a host
     compiler cannot build them (they need the kernel SDK): no MMIO copied with
     memcpy/memset/bzero, no incremented volatile, no integer-to-function
     pointer cast without ptrauth, no SM8550 sensor window carried over from
     another board, and the devicetree addresses this board actually has.

The report is written to build/tb710fu-platform-host/report.json so the build
log can quote the same verdict. Nothing here touches hardware.
"""
from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "package" / "Tb710fuPlatformServices"
REPORT = ROOT / "build" / "tb710fu-platform-host" / "report.json"

KERNEL_SOURCES = [
    PACKAGE / "Tb710fuBacklight.cpp",
    PACKAGE / "Tb710fuThermal.cpp",
    PACKAGE / "Tb710fuPlatformServicesXnu.cpp",
]

CORE_HEADERS = [
    PACKAGE / "Tb710fuThermalCore.h",
    PACKAGE / "Tb710fuBacklightCore.h",
    PACKAGE / "Tb710fuSpmiArbiter.h",
]

# The SM8550 sensor window the sheng board's ThermalProbe reads. A base from
# that board appearing here would mean an address was copied instead of read
# from sm8650.dtsi, which is the failure this check exists for.
FOREIGN_ADDRESSES = [0x0C271000 + index * 0x1000 for index in range(3)]

# Bases this package must name somewhere, each a devicetree node.
DT_EVIDENCE = {
    "spmi core 0x0c400000": "0c400000",
    "spmi cnfg 0x0c42d000": "0c42d000",
    "spmi obsrvr 0x0c440000": "0c440000",
    "spmi chnls 0x0c500000": "0c500000",
    "tsens0 tm 0x0c228000": "0c228000",
    "tsens1 tm 0x0c229000": "0c229000",
    "tsens2 tm 0x0c22a000": "0c22a000",
    "i2c6 se6 0x00a98000": "00a98000",
    "sy7758 at 0x2e": "sy7758_address 0x2eu",
}


def compiler() -> str:
    for candidate in ("gcc", "clang"):
        if shutil.which(candidate):
            return candidate
    raise SystemExit("no C compiler found")


def run_c_suites() -> dict:
    runner = PACKAGE / "Test" / "run_host_tests.py"
    completed = subprocess.run(
        [sys.executable, str(runner)],
        cwd=str(PACKAGE),
        capture_output=True,
        text=True,
    )
    lines = [line for line in completed.stdout.splitlines() if line.strip()]
    return {
        "runner": str(runner.relative_to(ROOT)),
        "returncode": completed.returncode,
        "passed": completed.returncode == 0,
        "suites": [line.split(" ")[1] for line in lines if line.startswith("PASS ")],
        "tail": lines[-10:],
    }


def strip_comments(text: str) -> str:
    """Comments and string literals out, so the audit reads code only."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(?:\\.|[^"\\])*"', ' "" ', text)
    text = re.sub(r"'(?:\\.|[^'\\])*'", " '' ", text)
    return text


def audit_source(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    code = strip_comments(text)
    findings = []

    # 1. Bulk libc on MMIO: the compiler may turn memcpy into an unaligned
    #    store, which aborts with ESR 0x96000061 on this SoC.
    for name in ("memcpy", "memset", "bzero", "memmove"):
        if re.search(rf"\b{name}\s*\(", code):
            findings.append(f"{name} used in a kernel driver")

    # 2. Deprecated volatile arithmetic (clang -Werror,-Wdeprecated-volatile).
    if re.search(r"\(\s*\*\s*[A-Za-z_][A-Za-z0-9_]*\s*\)\s*(\+\+|--)", code):
        findings.append("++/-- through a register pointer")
    if re.search(r"(\+\+|--)\s*\*", code):
        findings.append("++/-- applied to a dereference")

    # 3. A function pointer made from a raw address must be ptrauth-signed.
    for match in re.finditer(r"\(\s*[^()\n]*\(\s*\*\s*\)[^()\n]*\)\s*[^;\n]*", code):
        if re.search(r"0[xX][0-9a-fA-F]+|\b(gPhys|phys|address)\b", match.group(0)) and \
                "ptrauth_sign_unauthenticated" not in code:
            findings.append("integer-to-function-pointer cast without ptrauth")
            break

    # 4. getPlatform() needs the platform expert header in this port.
    if ("getPlatform()" in code or "getResourceService()" in code) and \
            "<IOKit/IOPlatformExpert.h>" not in text:
        findings.append("getPlatform() without <IOKit/IOPlatformExpert.h>")

    # 5. Provenance: no SM8550 window, and every eight-digit literal listed
    #    for review in the report.
    literals = sorted({int(value, 16)
                       for value in re.findall(
                           r"0[xX]([0-9a-fA-F]{7,8})(?![0-9a-fA-FxX])", code)})
    foreign = [value for value in literals if value in FOREIGN_ADDRESSES]
    if foreign:
        findings.append("SM8550 address " +
                        ", ".join(f"0x{value:08x}" for value in foreign))

    # 6. A kernel-only translation unit, gated by its own boot argument: the
    #    ladder itself lives in the XNU glue, so a driver may name the ladder
    #    through the boot-arg token or through Tb710fuPlatformStage.
    if "#ifdef KERNEL" not in text:
        findings.append("missing #ifdef KERNEL")
    if not re.search(r"TB710FU_BOOTARG_(PLATFORM|BACKLIGHT|THERMAL)", text) and \
            not re.search(r"tb710fu_stage_action|TB710FU_STAGE_", text):
        findings.append("no staged boot argument")

    return {
        "path": str(path.relative_to(ROOT)),
        "bytes": path.stat().st_size,
        "long_literals": [f"0x{value:08x}" for value in literals],
        "findings": findings,
        "ok": not findings,
    }


def dts_evidence() -> dict:
    package_text = "\n".join(
        path.read_text(encoding="utf-8").lower()
        for path in KERNEL_SOURCES + CORE_HEADERS if path.exists()
    )
    checks = {name: literal in package_text for name, literal in DT_EVIDENCE.items()}
    return {"checks": checks, "ok": all(checks.values())}


def main() -> int:
    report: dict = {
        "platform": "Lenovo TB710FU (SM8650Q-AB)",
        "package": str(PACKAGE.relative_to(ROOT)),
        "target": "macOS 27.3 (26C5035r, XNU 12377.121.6, arm64e)",
        "compiler": compiler(),
        # Reported so no reader mistakes a host pass for a hardware result.
        "milestones": {
            "host_reasoning_verified": True,
            "native_kernel_build": False,
            "prelinked_kernel": False,
            "hardware_boot": False,
            "device_rtc": False,
            "device_backlight_brightness": False,
            "device_thermal": False,
        },
    }
    suites = run_c_suites()
    report["c_suites"] = suites
    if not suites["passed"]:
        report["ok"] = False
        REPORT.parent.mkdir(parents=True, exist_ok=True)
        REPORT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print("\n".join(suites["tail"]))
        return 1

    audits = [audit_source(path) for path in KERNEL_SOURCES if path.exists()]
    report["kernel_audits"] = audits
    facts = dts_evidence()
    report["devicetree_evidence"] = facts

    for audit in audits:
        print(f"{'ok  ' if audit['ok'] else 'FAIL'} {audit['path']}")
        for finding in audit["findings"]:
            print(f"     - {finding}")
    print(f"{'ok  ' if facts['ok'] else 'FAIL'} devicetree bases named in the package")
    for name, present in facts["checks"].items():
        if not present:
            print(f"     - missing {name}")

    report["ok"] = bool(audits) and all(audit["ok"] for audit in audits) and \
        facts["ok"] and suites["passed"]
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"report {REPORT.relative_to(ROOT)}")
    print("tb710fu platform host checks passed" if report["ok"]
          else "tb710fu platform host checks FAILED")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
