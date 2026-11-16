#!/usr/bin/env python3
"""Compile the UFS read-probe state machine into a sanitized host harness."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def compiler():
    requested = shlex.split(os.environ.get("CC", "cc"))
    if requested and shutil.which(requested[0]):
        return requested
    for candidate in ("clang", "gcc"):
        if shutil.which(candidate):
            return [candidate]
    raise SystemExit("No C compiler found; set CC")


def main():
    with tempfile.TemporaryDirectory(prefix="tb710fu-ufs-") as temp:
        binary = Path(temp) / "ufs-probe-test"
        command = compiler() + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-fsanitize=address,undefined", "-I", str(ROOT),
            str(ROOT / "Test/ufs_read_probe_test.c"),
            str(ROOT / "UfsReadProbe.c"), "-o", str(binary),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
    print("UFS read-probe state-machine tests passed")


if __name__ == "__main__":
    main()
