#!/usr/bin/env python3
"""Build and run host tests for the hardware-independent CDC layer."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "package/Tb710fuUsbCdc"


def compiler_command():
    requested = shlex.split(os.environ.get("CC", "cc"))
    if requested and shutil.which(requested[0]):
        return requested
    for fallback in ("cc", "gcc"):
        if shutil.which(fallback):
            return [fallback]
    raise SystemExit("No host C compiler found; set CC to clang or cc")


def main():
    test = ROOT / "tests/usb_cdc_protocol_test.c"
    with tempfile.TemporaryDirectory(prefix="tb710fu-cdc-") as temporary:
        binary = Path(temporary) / "usb-cdc-test"
        command = compiler_command() + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-fsanitize=address,undefined", "-I", str(SOURCE),
            str(test), str(SOURCE / "CdcAcmProtocol.c"),
            str(SOURCE / "CdcLogRing.c"), "-o", str(binary),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
    print("CDC descriptor, request and log-ring tests passed")


if __name__ == "__main__":
    main()
