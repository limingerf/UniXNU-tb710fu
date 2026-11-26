#!/usr/bin/env python3
"""Compile and run the TB710FU platform-services host tests.

Every binary is built with the same C11 warning set and with
AddressSanitizer/UndefinedBehaviorSanitizer, so a test that passes here has
also survived the memory and integer checks. Set CC to choose the compiler;
the default is gcc.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

SOURCES = [
    ROOT / "Tb710fuSpmiArbiter.c",
    ROOT / "Tb710fuSpmiMmio.c",
    ROOT / "Tb710fuPmk8550Rtc.c",
    ROOT / "Tb710fuRtcTime.c",
    ROOT / "Tb710fuPmk8550Reset.c",
    ROOT / "Tb710fuPlatformStage.c",
    ROOT / "Tb710fuBacklightCore.c",
    ROOT / "Tb710fuThermalCore.c",
    ROOT / "Test/host_spmi_model.c",
]

TESTS = [
    "platform_spmi_test",
    "platform_rtc_test",
    "platform_rtc_time_test",
    "platform_reset_test",
    "platform_drivers_test",
]

FLAGS = [
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-pedantic",
    "-O1",
    "-g",
    "-fno-omit-frame-pointer",
    "-fsanitize=address,undefined",
    "-fno-sanitize-recover=all",
]


def compiler():
    requested = shlex.split(os.environ.get("CC", "gcc"))
    if requested and shutil.which(requested[0]):
        return requested
    for candidate in ("gcc", "clang"):
        if shutil.which(candidate):
            return [candidate]
    raise SystemExit("No C compiler found; set CC")


def main():
    environment = dict(os.environ)
    environment["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"
    environment["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=1"

    with tempfile.TemporaryDirectory(prefix="tb710fu-platform-") as temp:
        directory = Path(temp)
        for test in TESTS:
            binary = directory / test
            command = compiler() + FLAGS + [
                str(ROOT / "Test" / f"{test}.c"),
                *[str(source) for source in SOURCES],
                "-o",
                str(binary),
            ]
            subprocess.run(command, check=True, env=environment)
            subprocess.run([str(binary)], check=True, env=environment)
            print(f"PASS {test}")
    print("platform services host tests passed")


if __name__ == "__main__":
    main()
