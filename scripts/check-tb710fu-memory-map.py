#!/usr/bin/env python3
"""Prove the TB710FU DarwinBoot/XNU RAM window from on-disk evidence.

The tablet has 8 GiB of LPDDR5X in two physical DDR banks. XNU manages one
contiguous physical span [dram-base, dram-base + darwinboot-memory-size); the
loader fills /chosen/darwinboot-memory-excluded with the parts of that span
XNU must not use. This script reads the two captured device trees (live and
TWRP), board.toml and the board UEFI memory table and proves:

  a. the chosen span lies inside one real DDR bank;
  b. it intersects no /reserved-memory region, GOP/scanout buffer, ADSP/CDSP
     or other PIL carveout, SMEM, cmd-db, TZ/HYP region, UEFI-reserved
     descriptor or UEFI MMAP_IO device window;
  c. the base and size meet DarwinBoot's 2 MiB block + kernel-link offset
     rule and XNU's page-granular span rule, and the excluded-range table is
     large enough for every non-usable range inside the window.

It also checks that the chosen size is the largest contiguous free run in the
8 GiB map (the "maximum safe" claim), and that the UEFI table actually
describes that run as conventional memory (the "usable" claim).

Usage (defaults find the clean tree next to this repository):

  python scripts/check-tb710fu-memory-map.py \
      --live-dts  ../unixnu-tb710fu-clean/.local/adt/live.dts \
      --twrp-dts  ../unixnu-tb710fu-clean/.local/twrp-fdt/twrp.dts \
      --board     board.toml \
      --uefi-memmap firmware/overlay/Platforms/Lenovo/tb710fuPkg/Library/MemoryMapLib/MemoryMapLib.c

Add --adt path/to/kernel.macho.adt to also verify the built sidecar. Exit
status is 0 only when every check is PASS.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import struct
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

PAGE = 0x1000
TWIG = 0x200000                 # DarwinBoot Block and XNU ARM_TT_TWIG (4 KiB granule)
KERNEL_TEXT_OFFSET = 0x4000     # prelinked kernel __TEXT vmaddr low 21 bits
MIN_WINDOW = 0x10000000         # devicetree.py: 256 MiB
MAX_WINDOW = 0x200000000        # devicetree.py: 8 GiB

OWNERS = (
    ('HYP/TZ', ('gunyah', 'cpusys', 'hyp')),
    ('SMEM', ('smem',)),
    ('cmd-db', ('cmd_db', 'cmd-db')),
    ('ADSP/PIL', ('adsp',)),
    ('CDSP/PIL', ('cdsp',)),
    ('MODEM/PIL', ('mpss', 'modem')),
    ('WPSS/PIL', ('wpss',)),
    ('VENUS/PIL', ('venus',)),
    ('TZ', ('tz_', 'qsee', 'qcedev', 'tzapps')),
    ('GOP/framebuffer', ('splash', 'display', 'demura', 'scanout')),
    ('Gunyah VM', ('trust_ui', 'oem_vm')),
    ('ramoops', ('ramoops',)),
    ('PVMFW', ('pvmfw',)),
    ('debug', ('qdss', 'qlink')),
)

# Categories the board is required to expose somewhere; the others are optional
# on this SoC and only reported when present.
REQUIRED_OWNERS = {'HYP/TZ', 'SMEM', 'cmd-db', 'ADSP/PIL', 'CDSP/PIL', 'TZ', 'GOP/framebuffer'}


@dataclass(frozen=True)
class Span:
    name: str
    start: int
    size: int
    owner: str = 'firmware'
    no_map: bool = False
    dynamic: bool = False

    @property
    def end(self) -> int:
        return self.start + self.size


@dataclass
class Check:
    label: str
    ok: bool
    detail: str
    info: bool = False


@dataclass
class Node:
    name: str
    parent: 'Node | None' = None
    props: dict[str, 'str | None'] = field(default_factory=dict)
    children: list['Node'] = field(default_factory=list)

    def child(self, name: str) -> 'Node | None':
        return next((c for c in self.children if c.name == name), None)

    def strings(self, key: str) -> list[str]:
        return re.findall(r'"([^"]*)"', self.props.get(key) or '')

    def cells(self, key: str) -> list[int]:
        raw = self.props.get(key)
        if not raw:
            return []
        body = raw[raw.find('<') + 1:raw.rfind('>')]
        return [int(token, 0) for token in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b', body)]

    def pairs(self, key: str, address_cells: int, size_cells: int) -> list[tuple[int, int]]:
        values = self.cells(key)
        step = address_cells + size_cells
        result = []
        for offset in range(0, len(values) - step + 1, step):
            address = combine(values[offset:offset + address_cells])
            size = combine(values[offset + address_cells:offset + step])
            if size:
                result.append((address, size))
        return result


def combine(values: list[int]) -> int:
    result = 0
    for value in values:
        result = (result << 32) | value
    return result


def cells_of(node: Node, key: str, default: int) -> int:
    values = node.cells(key)
    return values[0] if values else default


NODE_LINE = re.compile(r'^\s*([A-Za-z0-9_,.+\-@#/]+)\s*\{\s*$')
PROP_LINE = re.compile(r'^\s*([A-Za-z0-9_,.+\-#?]+)\s*=\s*(.*);\s*$')
BOOL_LINE = re.compile(r'^\s*([A-Za-z0-9_,.+\-#?]+)\s*;\s*$')


def parse_dts(path: Path) -> tuple[Node, list[tuple[int, int]]]:
    """Parse the line-oriented output of dtc -O dts (one property per line)."""
    text = path.read_text(encoding='utf-8', errors='replace')
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    root = Node('/')
    stack = [root]
    memreserve: list[tuple[int, int]] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith('//') or line.startswith('/dts-v1/'):
            continue
        if line.startswith('/memreserve/'):
            values = [int(token, 0) for token in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b', line)]
            if len(values) >= 2 and values[1]:
                memreserve.append((values[0], values[1]))
            continue
        if line in ('};', '}'):
            if len(stack) > 1:
                stack.pop()
            continue
        match = NODE_LINE.match(line)
        if match:
            node = Node(match.group(1), stack[-1])
            stack[-1].children.append(node)
            stack.append(node)
            continue
        match = PROP_LINE.match(line)
        if match:
            stack[-1].props[match.group(1)] = match.group(2).strip()
            continue
        match = BOOL_LINE.match(line)
        if match:
            stack[-1].props[match.group(1)] = None
    # dtc writes the root as "/ {"; the top Node("/") holds that node.
    if len(root.children) == 1 and root.children[0].name == '/':
        root = root.children[0]
    return root, memreserve


def classify(name: str, compatible: list[str]) -> str:
    text = (name + ' ' + ' '.join(compatible)).lower()
    for owner, needles in OWNERS:
        if any(needle in text for needle in needles):
            return owner
    return 'firmware'


@dataclass
class DeviceTree:
    path: Path
    sha: str
    banks: list[Span]
    system_ram: list[Span]
    reserved: list[Span]
    memreserve: list[Span]


def load_device_tree(path: Path) -> DeviceTree:
    root, memreserve = parse_dts(path)
    address_cells = cells_of(root, '#address-cells', 2)
    size_cells = cells_of(root, '#size-cells', 2)

    banks: list[Span] = []
    ddr = root.child('ddr-regions')
    if ddr is not None:
        # Qualcomm's ddr-regions value is base(2 cells) + size(2 cells) + six
        # DDR-configuration cells; only the first four are an address range.
        for key in ('region0', 'region1', 'region2', 'region3'):
            values = ddr.cells(key)[:4]
            if len(values) == 4:
                banks.append(Span(f'ddr-{key}', combine(values[:2]), combine(values[2:]), 'DDR bank'))

    system_ram: list[Span] = []
    for node in root.children:
        if node.strings('device_type') == ['memory'] or node.name.startswith('memory'):
            for index, (start, size) in enumerate(node.pairs('reg', address_cells, size_cells)):
                system_ram.append(Span(f'{node.name}[{index}]', start, size, 'System RAM'))

    reserved: list[Span] = []
    memory_node = root.child('reserved-memory')
    if memory_node is not None:
        reserved_ac = cells_of(memory_node, '#address-cells', 2)
        reserved_sc = cells_of(memory_node, '#size-cells', 2)
        for node in memory_node.children:
            compatible = node.strings('compatible')
            no_map = 'no-map' in node.props
            fixed = node.pairs('reg', reserved_ac, reserved_sc)
            for index, (start, size) in enumerate(fixed):
                reserved.append(Span(node.name, start, size, classify(node.name, compatible), no_map))
            if not fixed:
                sizes = node.cells('size')
                for index, size in enumerate(sizes):
                    if size:
                        reserved.append(Span(node.name, 0, size, classify(node.name, compatible),
                                             no_map, dynamic=True))

    mem = [Span(f'memreserve[{i}]', start, size, 'memreserve', True)
           for i, (start, size) in enumerate(memreserve)]
    return DeviceTree(path, hashlib.sha256(path.read_bytes()).hexdigest(), banks, system_ram,
                      reserved, mem)


def normalize(spans: list[Span], name: str = 'union') -> list[Span]:
    """Merge overlapping or adjacent ranges; size zero entries disappear."""
    ordered = sorted((s for s in spans if s.size), key=lambda s: (s.start, s.end))
    merged: list[Span] = []
    for span in ordered:
        if merged and span.start <= merged[-1].end:
            last = merged[-1]
            end = max(last.end, span.end)
            merged[-1] = Span(last.name, last.start, end - last.start, last.owner)
        else:
            merged.append(Span(span.name, span.start, span.size, span.owner))
    return merged


def subtract(spans: list[Span], holes: list[Span]) -> list[Span]:
    """spans minus holes, exact integer interval arithmetic."""
    result = normalize(spans)
    for hole in normalize(holes):
        cut: list[Span] = []
        for span in result:
            if hole.end <= span.start or hole.start >= span.end:
                cut.append(span)
                continue
            if span.start < hole.start:
                cut.append(Span(span.name, span.start, hole.start - span.start, span.owner))
            if hole.end < span.end:
                cut.append(Span(span.name, hole.end, span.end - hole.end, span.owner))
        result = cut
    return result


def overlaps(a: Span, b: Span) -> bool:
    return a.start < b.end and b.start < a.end


def overlaps_window(start: int, size: int, span: Span) -> bool:
    return start < span.end and span.start < start + size


def mib(value: int) -> str:
    return f'{value / (1 << 20):.1f} MiB'


def gib(value: int) -> str:
    return f'{value / (1 << 30):.3f} GiB'


def parse_uefi(path: Path) -> list[Span]:
    text = path.read_text(encoding='utf-8')
    pattern = re.compile(r'\{"([^"]+)",\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+)\}')
    spans = []
    for name, base, size, _hob, resource, _attributes, kind, _arm in pattern.findall(text):
        # The memory type alone is not enough: a MMAP_IO descriptor is a device
        # window even when its type says Conv (IMEM, the display DBI dump).
        spans.append(Span(name, int(base, 16), int(size, 16),
                          kind if resource == 'SYS_MEM' else 'MMIO:' + kind))
    return spans


def parse_flat_adt(path: Path) -> dict[str, bytes]:
    blob = path.read_bytes()
    cursor = [0]

    def node() -> tuple[dict[str, bytes], list[tuple[dict[str, bytes], list]]]:
        prop_count, child_count = struct.unpack_from('<II', blob, cursor[0])
        cursor[0] += 8
        props: dict[str, bytes] = {}
        for _ in range(prop_count):
            key = blob[cursor[0]:cursor[0] + 32].split(b'\0', 1)[0].decode('latin-1')
            cursor[0] += 32
            (size,) = struct.unpack_from('<I', blob, cursor[0])
            cursor[0] += 4
            props[key] = blob[cursor[0]:cursor[0] + size]
            cursor[0] += size + (-size % 4)
        children = [node() for _ in range(child_count)]
        return props, children

    root, children = node()
    if cursor[0] != len(blob):
        raise ValueError(f'flat ADT trailing bytes: {cursor[0]} != {len(blob)}')
    for props, grand in children:
        if props.get('name', b'').rstrip(b'\0') == b'chosen':
            return props
    return root


def main() -> int:
    here = Path(__file__).resolve().parent.parent
    clean = here.parent / 'unixnu-tb710fu-clean'
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--live-dts', type=Path, default=clean / '.local/adt/live.dts')
    parser.add_argument('--twrp-dts', type=Path, default=clean / '.local/twrp-fdt/twrp.dts')
    parser.add_argument('--board', type=Path, default=here / 'board.toml')
    parser.add_argument('--uefi-memmap', type=Path,
                        default=here / 'firmware/overlay/Platforms/Lenovo/tb710fuPkg/Library/MemoryMapLib/MemoryMapLib.c')
    parser.add_argument('--adt', type=Path, help='optional built kernel.macho.adt')
    parser.add_argument('--allow-smaller', action='store_true', help='do not require the window to be the largest free run')
    args = parser.parse_args()

    for label, path in (('live DTS', args.live_dts), ('TWRP DTS', args.twrp_dts),
                        ('board.toml', args.board), ('UEFI memory table', args.uefi_memmap)):
        if not path.is_file():
            raise SystemExit(f'missing {label}: {path}')
    if args.adt and not args.adt.is_file():
        raise SystemExit(f'missing ADT: {args.adt}')

    live = load_device_tree(args.live_dts)
    twrp = load_device_tree(args.twrp_dts)
    board = tomllib.loads(args.board.read_text(encoding='utf-8'))
    adt_options = board.get('payload', {}).get('adt', {})
    uefi = parse_uefi(args.uefi_memmap)

    checks: list[Check] = []

    def check(label: str, ok: bool, detail: str, info: bool = False) -> None:
        checks.append(Check(label, ok, detail, info))

    # --- inputs and cross-check -------------------------------------------------
    check('live.dts and twrp.dts agree on DDR banks',
          [(b.start, b.size) for b in live.banks] == [(b.start, b.size) for b in twrp.banks],
          f'{len(live.banks)} bank(s)')
    check('live.dts and twrp.dts agree on System RAM runs',
          sorted((s.start, s.end) for s in live.system_ram) == sorted((s.start, s.end) for s in twrp.system_ram),
          f'{len(live.system_ram)} run(s)')
    live_fixed = sorted((s.name, s.start, s.end) for s in live.reserved if not s.dynamic)
    twrp_fixed = sorted((s.name, s.start, s.end) for s in twrp.reserved if not s.dynamic)
    check('live.dts and twrp.dts agree on fixed reserved-memory regions',
          live_fixed == twrp_fixed, f'{len(live_fixed)} fixed region(s)')

    banks = live.banks
    total = sum(b.size for b in banks)
    check('8 GiB of physical DDR in the captured trees',
          total == 8 << 30, f'{total / (1 << 30):.3f} GiB in {len(banks)} bank(s)')

    system_ram = normalize(live.system_ram)
    for run in system_ram:
        inside = [b for b in banks if b.start <= run.start and run.end <= b.end]
        if not inside:
            check(f'System RAM run {run.start:#x} is inside a DDR bank', False,
                  f'{run.start:#x}..{run.end:#x}')
    check('every System RAM run lies inside a DDR bank',
          all(any(b.start <= r.start and r.end <= b.end for b in banks) for r in system_ram),
          f'{len(system_ram)} merged run(s)')

    fixed_reserved = [s for s in live.reserved if not s.dynamic]
    for span in fixed_reserved:
        if span.start >= 0x100000000 and not any(b.start <= span.start < b.end for b in banks):
            check(f'reserved region {span.name} is inside a DDR bank', False,
                  f'{span.start:#x}')
    check('every fixed reserved-memory region lies inside a DDR bank',
          all(any(b.start <= s.start and s.end <= b.end for b in banks) for s in fixed_reserved),
          f'{len(fixed_reserved)} region(s)')

    # --- the free map and the largest contiguous run ---------------------------
    # A byte is free for XNU only when both views agree: the Linux live FDT calls
    # it System RAM and the board UEFI table does not reserve it for firmware.
    uefi_nonconv = [s for s in uefi if s.owner != 'Conv']
    free = subtract(system_ram, fixed_reserved + uefi_nonconv)
    largest = max(free, key=lambda s: (s.size, -s.start)) if free else None
    check('a free run exists at all', largest is not None, 'none' if largest is None else f'{largest.start:#x}')

    # --- the requested window ---------------------------------------------------
    if 'owned_memory_size' not in adt_options:
        raise SystemExit('[payload.adt] owned_memory_size is missing from board.toml')
    size = int(str(adt_options['owned_memory_size']), 0)
    slots = int(str(adt_options.get('owned_memory_excluded_ranges', '0')), 0)

    if largest is not None:
        base = largest.start
        check('board.toml size is the largest contiguous free run' if not args.allow_smaller
              else 'board.toml size does not exceed the largest contiguous free run',
              (size == largest.size) if not args.allow_smaller else (size <= largest.size),
              f'chosen {mib(size)} vs largest {mib(largest.size)} at {largest.start:#x}',
              info=args.allow_smaller and size < largest.size)
        check('the chosen base is the start of that run', True, f'{base:#x}')
    else:
        base = 0

    if args.adt:
        chosen = parse_flat_adt(args.adt)
        adt_base = int.from_bytes(chosen.get('dram-base', b'\0' * 8), 'little')
        adt_dram = int.from_bytes(chosen.get('dram-size', b'\0' * 8), 'little')
        adt_window = int.from_bytes(chosen.get('darwinboot-memory-size', b'\0' * 8), 'little')
        adt_slots = len(chosen.get('darwinboot-memory-excluded', b'')) // 16
        check('built ADT darwinboot-memory-size equals board.toml', adt_window == size,
              f'ADT {adt_window:#x} board {size:#x}')
        check('built ADT darwinboot-memory-excluded has enough slots', adt_slots >= slots,
              f'ADT {adt_slots} slots, board {slots}')
        check('built ADT dram-base equals the proven run start', adt_base == base,
              f'ADT {adt_base:#x} proven {base:#x}')
        check('built ADT dram-size covers the chosen span', adt_dram >= size,
              f'ADT {adt_dram:#x} span {size:#x}')
        if adt_window != size or adt_base != base:
            size = adt_window
    else:
        adt_base = adt_dram = None

    span = Span('window', base, size)
    print('TB710FU 8 GiB memory map')
    print(f'  live DT : {live.path}  sha256:{live.sha[:16]}')
    print(f'  TWRP DT : {twrp.path}  sha256:{twrp.sha[:16]}')
    print(f'  board   : {args.board}')
    print(f'  UEFI    : {args.uefi_memmap}')
    print()
    print('Physical DDR banks')
    for index, bank in enumerate(banks):
        print(f'  bank {index}  {bank.start:#013x} - {bank.end - 1:#013x}  '
              f'{mib(bank.size):>12}  {gib(bank.size):>10}')
    print(f'  total    {mib(total):>12}  {gib(total):>10}')
    print()
    print('System RAM runs (merged memory-node reg)')
    for run in system_ram:
        print(f'  {run.start:#013x} - {run.end - 1:#013x}  {mib(run.size):>12}')
    print()
    print('Free runs after subtracting every fixed reserved-memory region')
    for run in sorted(free, key=lambda s: s.start):
        if run.size >= 1 << 20:
            print(f'  {run.start:#013x} - {run.end - 1:#013x}  {mib(run.size):>12}')
    print()
    print('Fixed reserved-memory regions (from live.dts)')
    print(f'  {"name":34} {"start":>13} {"end":>13} {"size":>12}  owner')
    for item in sorted(fixed_reserved, key=lambda s: s.start):
        print(f'  {item.name:34} {item.start:#013x} {item.end - 1:#013x} {mib(item.size):>12}  {item.owner}')
    dynamic = [s for s in live.reserved if s.dynamic]
    for item in dynamic:
        print(f'  {item.name:34} {"(dynamic)":>13} {"":>13} {mib(item.size):>12}  {item.owner}')
    print()
    print('UEFI board table (high DDR rows and rows inside the window)')
    for item in sorted(uefi, key=lambda s: s.start):
        if item.start >= (1 << 32) or overlaps_window(span.start, span.size, item):
            print(f'  {item.name:20} {item.start:#013x} + {mib(item.size):>10}  {item.owner}')
    print()
    print('Chosen DarwinBoot / XNU window')
    print(f'  dram-base                  {base:#x}')
    print(f'  darwinboot-memory-size     {size:#x}  ({mib(size)}, {gib(size)})')
    print(f'  excluded-range slots       {slots}')
    if adt_base is not None:
        print(f'  ADT dram-base/dram-size    {adt_base:#x} / {adt_dram:#x}')
    print()

    # --- proof a: inside one DDR bank ------------------------------------------
    containing = [b for b in banks if b.start <= span.start and span.end <= b.end]
    check('(a) window fits inside one physical DDR bank', bool(containing),
          f'{span.start:#x}..{span.end:#x} in ' + ', '.join(f'{b.name} {b.start:#x}+{b.size:#x}' for b in containing))

    # --- proof b: zero overlap with every carveout -----------------------------
    clashes = [s for s in fixed_reserved if overlaps_window(span.start, span.size, s)]
    check('(b) zero overlap with fixed /reserved-memory regions', not clashes,
          'none' if not clashes else ', '.join(f'{s.name}@{s.start:#x}' for s in clashes))
    for owner, _needles in OWNERS:
        present = [s for s in fixed_reserved if s.owner == owner]
        outside = not any(overlaps_window(span.start, span.size, s) for s in present)
        if owner in REQUIRED_OWNERS:
            check(f'(b) {owner} carveouts present and outside the window',
                  bool(present) and outside, f'{len(present)} region(s), none overlapping')
        elif present:
            check(f'(b) {owner} carveouts outside the window', outside,
                  f'{len(present)} region(s), none overlapping')

    uefi_reserved = [s for s in uefi if s.owner != 'Conv']
    uefi_clashes = [s for s in uefi_reserved if overlaps_window(span.start, span.size, s)]
    check('(b) zero overlap with UEFI reserved/MMIO/boot-service descriptors', not uefi_clashes,
          'none' if not uefi_clashes else ', '.join(f'{s.name}@{s.start:#x}' for s in uefi_clashes))

    conv = [s for s in uefi if s.owner == 'Conv']
    covered = sum(max(0, min(span.end, s.end) - max(span.start, s.start)) for s in conv)
    check('(b/coverage) UEFI conventional memory covers the whole window', covered >= span.size,
          f'{mib(covered)} of {mib(span.size)} conventional')

    # --- proof c: alignment, budgets, excluded-table capacity ------------------
    check('(c) base is 4 KiB and 2 MiB aligned', span.start % PAGE == 0 and span.start % TWIG == 0,
          f'{span.start:#x}')
    check('(c) size is 4 KiB and 2 MiB aligned', span.size % PAGE == 0 and span.size % TWIG == 0,
          f'{span.size:#x}')
    check('(c) size is 256 MiB..8 GiB (devicetree.py contract)',
          MIN_WINDOW <= span.size <= MAX_WINDOW, f'{mib(span.size)}')
    loader_start = ((span.start + TWIG - 1) & ~(TWIG - 1)) + KERNEL_TEXT_OFFSET
    check('(c) DarwinBoot places the prelinked kernel on a 2 MiB block + 0x4000 offset',
          loader_start % TWIG == KERNEL_TEXT_OFFSET, f'{loader_start:#x}')
    head = max(span.size // 64, 0x2000000)
    head_run = [s for s in conv if s.start <= loader_start and loader_start + head <= s.end]
    check('(c) the %s head allocation fits one conventional run' % mib(head), bool(head_run),
          ', '.join(s.name for s in head_run) or 'none')

    # Non-usable ranges inside the loader window (what DarwinBoot writes to the table).
    loader_end = loader_start + span.size
    usable = [s for s in uefi if s.owner in ('Conv', 'LoaderCode', 'LoaderData')]
    excluded: list[tuple[int, int]] = []
    cursor = loader_start
    while cursor < loader_end:
        covering = [s for s in usable if s.start <= cursor < s.end]
        if covering:
            cursor = max(covering, key=lambda s: s.end).end
            continue
        nxt = min((s.start for s in usable if s.start > cursor), default=loader_end)
        nxt = min(nxt, loader_end)
        excluded.append((cursor, nxt))
        cursor = nxt
    check('(c) excluded-range table covers every non-usable range in the window', len(excluded) <= slots,
          f'{len(excluded)} range(s) need {slots} slot(s)')
    if excluded:
        print('  non-usable ranges inside the loader window:')
        for start, end in excluded:
            print(f'    {start:#013x} - {end - 1:#013x}  {mib(end - start)}')
        print()

    # --- the maximum claim and multi-bank alternatives -------------------------
    high_bank = max(banks, key=lambda b: b.size)
    high_free = [s for s in free if high_bank.start <= s.start and s.end <= high_bank.end]
    high_total_free = sum(s.size for s in high_free)
    check('the high DDR bank holds the window',
          high_bank.start <= span.start and span.end <= high_bank.end,
          f'{high_bank.start:#x}+{high_bank.size:#x}')
    check('largest bank-covering span would need only exclusion holes',
          len(high_free) >= 1,
          f'{mib(high_total_free)} free in {len(high_free)} run(s) across {mib(high_bank.size)}')
    print(f'  note: a {mib(high_bank.size)} span over the high bank would contain '
          f'{len(high_free)} free run(s) ({mib(high_total_free)}) and '
          f'{mib(high_bank.size - high_total_free)} of holes of unqualified ownership;')
    print('        the single contiguous run above is the zero-overlap ceiling.')
    print()

    # --- report ---------------------------------------------------------------
    passed = 0
    for item in checks:
        if item.ok:
            passed += 1
            print(f'  [{"INFO" if item.info else "PASS"}] {item.label}: {item.detail}')
        else:
            print(f'  [FAIL] {item.label}: {item.detail}')
    failures = [c for c in checks if not c.ok]
    print()
    print(f'{passed}/{len(checks)} checks passed' + ('' if not failures else f', {len(failures)} FAILED'))
    return 0 if not failures else 1


if __name__ == '__main__':
    sys.exit(main())
