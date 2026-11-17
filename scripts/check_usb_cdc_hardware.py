#!/usr/bin/env python3
"""Exercise the read-only SM8650 clock, PHY and identity-DMA predicates."""
from pathlib import Path
import subprocess
import tempfile

from check_usb_cdc_protocol import compiler_command


def main():
    root = Path(__file__).resolve().parents[1]
    source = root / "package/Tb710fuUsbCdc/Test/cdc_hardware_test.c"
    with tempfile.TemporaryDirectory(prefix="tb710fu-cdc-hardware-") as temporary:
        binary = Path(temporary) / "cdc-hardware-test"
        subprocess.run(compiler_command() + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-fsanitize=address,undefined", str(source), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
