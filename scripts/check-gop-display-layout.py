#!/usr/bin/env python3
"""Host checks for plane-base bounds and generated scanout use."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEST = ROOT / 'scripts/tests/gop_display_layout_test.c'
HEADER = ROOT / 'package/GopDisplayLayout.h'
UPSTREAM = ROOT / '.work/UniXNU/drivers/GopFramebuffer/NativeLegacyDisplay.cpp'
GENERATOR = ROOT / 'scripts/build-tb710fu-gop-display.py'


def load_generator():
    spec = importlib.util.spec_from_file_location('tb710fu_gop_display_build', GENERATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_generated_display():
    if not UPSTREAM.is_file():
        raise SystemExit(f'pinned display source is unavailable: {UPSTREAM}')
    transformed = load_generator().transform_display(UPSTREAM)
    required = (
        '#include "GopDisplayLayout.h"',
        'native_legacy_surface_base_offset(surface)',
        'sourceAddress+sourceOffset',
        'sourceBytes,sourceStride,width,height',
        'flip(planeIova,uint32_t(sourceStride))',
        'sourceOffset+uint64_t(y)*shownStride',
    )
    missing = [marker for marker in required if marker not in transformed]
    if missing:
        raise SystemExit('generated scanout omits plane-base handling: ' + ', '.join(missing))
    return len(required)


def main():
    compiler = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    if not compiler:
        raise SystemExit('no C compiler found (cc/gcc/clang)')
    with tempfile.TemporaryDirectory(prefix='tb710fu-gop-layout-') as temporary:
        binary = Path(temporary) / 'gop-layout-test'
        built = subprocess.run(
            [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
             '-I', str(HEADER.parent), str(TEST), '-o', str(binary)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if built.returncode:
            raise SystemExit(built.stdout)
        tested = subprocess.run([str(binary)], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(tested.stdout, end='')
        if tested.returncode:
            raise SystemExit(f'plane layout checks failed ({tested.returncode})')
    generated = check_generated_display()
    print(f'PASS generated display checks={generated}')


if __name__ == '__main__':
    main()
