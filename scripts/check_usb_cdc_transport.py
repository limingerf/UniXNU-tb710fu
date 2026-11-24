#!/usr/bin/env python3
"""Exercise the production DWC3 CDC core using separate CPU/device DMA views."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'package/Tb710fuUsbCdc'


def main():
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    if not compiler or not shutil.which(compiler[0]):
        raise SystemExit('No host C compiler; set CC to gcc or clang')
    with tempfile.TemporaryDirectory(prefix='tb710fu-dwc3-') as temporary:
        output = Path(temporary) / 'transport-test'
        command = compiler + ['-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                              '-I', str(SOURCE)]
        if os.environ.get('CDC_NO_SANITIZERS') != '1':
            command += ['-fsanitize=address,undefined']
        command += [str(SOURCE / 'Test/cdc_dwc3_test.c'), str(SOURCE / 'CdcDwc3.c'),
                    str(SOURCE / 'CdcAcmProtocol.c'), str(SOURCE / 'CdcLogRing.c'), '-o', str(output)]
        subprocess.run(command, check=True)
        subprocess.run([str(output)], check=True)


if __name__ == '__main__':
    main()
