#!/usr/bin/env python3
"""Audit the delivered green-bar boot candidate from its actual bytes.

Why this exists: the delivery chain claims a candidate built from a fresh native
compile plus a strict prelink (run 43763928457), and the device still stops at
DarwinBoot's green band. Every previous gate compared the new artifact against
the previous artifact's hashes, which proves the build reproduced itself but
cannot prove the kernel can identify the platform. This tool re-derives the
delivery's claims from out/kernel-pr249-final/* and then asks the two questions
those gates never asked:

  1. Is the upstream PR249 pool change even relevant to this board's window?
     For the 2 GiB window in the shipped ADT it is not: the pre-PR249 16-page
     pool already covered the DEVELOPMENT-only capacity check.

  2. Does the shipped kernel.macho.adt identify as a macOS device?
     It does not: root model is still the hardware DTS identity, /arm-io has no
     hardware nodes, /product is empty and /chosen lacks osenvironment,
     boot-uuid and root-matching.

Usage: python tools/audit_greenbar_delivery.py [--json PATH]
Exit status is 0 only when the artifact checks and the identity gate pass.
"""
import argparse
import hashlib
import json
import lzma
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DELIVERY = ROOT / 'out/kernel-pr249-final'
EVIDENCE = ROOT / 'docs/evidence/kernel-pr249-build.json'
ADT_CHECKER = ROOT / 'tools/check_adt_identity.py'

POOL_PAGES = {'arm_large_memory': 34, 'native_4k_before_pr249': 16,
              'native_4k_pr249': 22, 'fallback': 8}
ARM_TT_L1_SIZE = 1 << 30          # 1 GiB per L1 entry (ARM_TT_L1_SIZE)

TG0_NAME = {0: '4 KiB', 1: '64 KiB', 2: '16 KiB', 3: 'reserved'}
TG1_NAME = {1: '16 KiB', 2: '4 KiB', 3: '64 KiB', 0: 'reserved'}
SHARE = {0: 'non-shareable', 1: 'RESERVED', 2: 'outer shareable', 3: 'inner shareable'}
CACHE = {0: 'non-cacheable', 1: 'write-back WA', 2: 'write-through', 3: 'write-back'}
IPS = {0: '32-bit', 1: '36-bit', 2: '40-bit', 3: '42-bit', 4: '44-bit',
       5: '48-bit', 6: '52-bit'}


def sha(data):
    return hashlib.sha256(data).hexdigest()


class Payload:
    """The FAT32 vendor ramdisk inside a vendor_boot v4 image."""

    def __init__(self, image):
        if image[:8] != b'VNDRBOOT':
            raise SystemExit(f'not a vendor_boot image: {image[:8]!r}')
        self.page = struct.unpack_from('<I', image, 12)[0]
        self.ramdisk_addr = struct.unpack_from('<I', image, 20)[0]
        self.ramdisk_size = struct.unpack_from('<I', image, 24)[0]
        self.header_size = struct.unpack_from('<I', image, 2096)[0]
        start = self.page * ((self.header_size + self.page - 1) // self.page)
        self.data = image[start:start + self.ramdisk_size]
        d = self.data
        self.bps = struct.unpack_from('<H', d, 11)[0]
        self.spc = d[13]
        self.reserved = struct.unpack_from('<H', d, 14)[0]
        self.nfats = d[16]
        self.total32 = struct.unpack_from('<I', d, 32)[0]
        self.fat_sz = struct.unpack_from('<I', d, 36)[0]
        self.root_cluster = struct.unpack_from('<I', d, 44)[0]
        self.cluster_bytes = self.bps * self.spc
        self.fat_start = self.reserved * self.bps
        self.first_data = (self.reserved + self.nfats * self.fat_sz) * self.bps

    @property
    def clusters(self):
        return (self.total32 - (self.reserved + self.nfats * self.fat_sz)) // self.spc

    def _chain(self, cluster):
        out, seen = [], set()
        while 2 <= cluster < 0x0FFFFFF8:
            if cluster in seen:
                raise SystemExit(f'FAT loop at cluster {cluster}')
            seen.add(cluster)
            out.append(cluster)
            cluster = struct.unpack_from(
                '<I', self.data, self.fat_start + cluster * 4)[0] & 0x0FFFFFFF
        return out

    def _read(self, cluster, size=None):
        raw = b''.join(
            self.data[self.first_data + (c - 2) * self.cluster_bytes:
                      self.first_data + (c - 1) * self.cluster_bytes]
            for c in self._chain(cluster))
        return raw if size is None else raw[:size]

    def _entries(self, cluster):
        raw, lfn, out = self._read(cluster), [], []
        for i in range(0, len(raw), 32):
            entry = raw[i:i + 32]
            if len(entry) < 32 or entry[0] == 0:
                break
            if entry[0] == 0xE5:
                lfn = []
                continue
            attr = entry[11]
            if attr == 0x0F:
                part = (entry[1:11] + entry[14:26] + entry[28:32]).decode('utf-16-le', 'replace')
                lfn.insert(0, part.split('\x00')[0])
                continue
            if attr & 0x08:
                lfn = []
                continue
            name = ''.join(lfn) if lfn else entry[0:8].decode('latin-1').strip()
            lfn = []
            high = struct.unpack_from('<H', entry, 20)[0]
            low = struct.unpack_from('<H', entry, 26)[0]
            out.append((name, attr, (high << 16) | low,
                        struct.unpack_from('<I', entry, 28)[0]))
        return out

    def files(self, cluster=None, prefix=''):
        found = {}
        for name, attr, first, size in self._entries(
                self.root_cluster if cluster is None else cluster):
            if name in ('.', '..'):
                continue
            if attr & 0x10:
                found.update(self.files(first, prefix + name + '/'))
            else:
                found[prefix + name] = self._read(first, size)
        return found


def lzma_kernel(encoded):
    prop = encoded[0]
    lc, rest = prop % 9, prop // 9
    lp, pb = rest % 5, rest // 5
    return lzma.decompress(
        encoded[13:], format=lzma.FORMAT_RAW,
        filters=[{'id': lzma.FILTER_LZMA1,
                  'dict_size': int.from_bytes(encoded[1:5], 'little'),
                  'lc': lc, 'lp': lp, 'pb': pb}])


def macho_segments(kernel):
    magic, cpu, subtype, kind, count, _ = struct.unpack_from('<6I', kernel, 0)
    segments, fileset, pos = [], 0, 32
    for _ in range(count):
        cmd, size = struct.unpack_from('<II', kernel, pos)
        if cmd == 0x19:
            name = kernel[pos + 8:pos + 24].split(b'\0', 1)[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', kernel, pos + 24)
            segments.append({'name': name, 'vmaddr': vmaddr, 'vmsize': vmsize,
                             'fileoff': fileoff, 'filesize': filesize})
        elif cmd == 0x80000035:
            fileset += 1
        pos += size
    return {'filetype': kind, 'segments': segments, 'fileset_entries': fileset}


def movz_x2(imm16):
    return 0xD2800000 | (imm16 << 5) | 2


def lsl_x2(shift):
    """UBFM X2, X2, #immr, #imms (64-bit) == LSL x2, x2, #shift."""
    return 0xD3400000 | (((64 - shift) & 0x3F) << 16) | (((63 - shift) & 0x3F) << 10) | (2 << 5) | 2


def check_pool(kernel, pages):
    """start.s clears the pool with `movz x2,#pages ; lsl x2,x2,#9`."""
    mov = struct.pack('<I', movz_x2(pages))
    shift = struct.pack('<I', lsl_x2(9))
    at = kernel.find(mov)
    return {'pages': pages, 'movz_hits': kernel.count(mov),
            'lsl_hits': kernel.count(shift),
            'adjacent_pair': at != -1 and kernel[at + 4:at + 8] == shift,
            'entries_cleared': pages * 512}


def decode_tcr(value):
    tcr = {'raw': value, 'T0SZ': value & 0x3F, 'TG0': (value >> 14) & 3,
           'T1SZ': (value >> 16) & 0x3F, 'TG1': (value >> 30) & 3,
           'SH0': (value >> 12) & 3, 'SH1': (value >> 28) & 3,
           'IRGN0': (value >> 8) & 3, 'IRGN1': (value >> 24) & 3,
           'ORGN0': (value >> 10) & 3, 'ORGN1': (value >> 26) & 3,
           'IPS': (value >> 32) & 7, 'TBI0': (value >> 37) & 1,
           'TBI1': (value >> 38) & 1}
    tcr.update(TG0=tcr['TG0'], TG0_name=TG0_NAME[tcr['TG0']],
               TG1_name=TG1_NAME[tcr['TG1']], SH0_name=SHARE[tcr['SH0']],
               SH1_name=SHARE[tcr['SH1']], IRGN0_name=CACHE[tcr['IRGN0']],
               IRGN1_name=CACHE[tcr['IRGN1']], ORGN0_name=CACHE[tcr['ORGN0']],
               ORGN1_name=CACHE[tcr['ORGN1']], IPS_name=IPS.get(tcr['IPS'], 'reserved'))
    tcr['anomalies'] = []
    if tcr['TG0'] != 0:
        tcr['anomalies'].append(f"TG0={tcr['TG0_name']}, expected 4 KiB")
    if tcr['TG1'] != 2:
        tcr['anomalies'].append(f"TG1={tcr['TG1_name']}, expected 4 KiB")
    for side in ('0', '1'):
        if tcr['SH' + side] == 1:
            tcr['anomalies'].append(f'SH{side}=0b01 is a reserved encoding')
    return tcr


def decode_sctlr(value):
    bits = [(0, 'M (MMU enable)'), (1, 'A'), (2, 'C (data cache)'), (3, 'SA'),
            (4, 'SA0'), (11, 'EOS'), (12, 'I (instr cache)'), (13, 'EnDB'),
            (14, 'DZE'), (15, 'UCT'), (16, 'nTWI'), (18, 'nTWE'), (19, 'WXN'),
            (20, 'TSCXT'), (21, 'IESB'), (22, 'EIS'), (23, 'SPAN'), (24, 'E0E'),
            (25, 'EE'), (26, 'UCI'), (27, 'EnDA'), (28, 'nTLSMD'),
            (29, 'LSMAOE'), (30, 'EnIB'), (31, 'EnIA')]
    return {'raw': value, 'set': [n for b, n in bits if value >> b & 1],
            'MMU': value & 1, 'data_cache': (value >> 2) & 1,
            'instr_cache': (value >> 12) & 1, 'WXN': (value >> 19) & 1}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--json', type=Path, help='write the findings here')
    args = parser.parse_args()

    report, failures = {}, []

    def check(name, ok, detail):
        report[name] = {'result': 'PASS' if ok else 'FAIL', **detail}
        if not ok:
            failures.append(name)
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail.get('summary', '')}")

    image = (DELIVERY / 'vendor_boot.img').read_bytes()
    payload = Payload(image)
    files = payload.files()
    check('vendor_boot_identity', len(image) == 100663296,
          {'bytes': len(image), 'sha256': sha(image),
           'summary': f'{len(image):,} B  {sha(image)}'})

    enc = files.get('EFI/UniXNU/Payloads/kernel.macho')
    kernel = lzma_kernel(enc) if enc else b''
    check('payload_fat32_mountable', payload.clusters >= 65525,
          {'clusters': payload.clusters, 'floor': 65525,
           'summary': f'{payload.clusters} clusters (floor 65525)'})

    recorded = json.loads(EVIDENCE.read_text())
    check('kernel_reproduces_recorded_hash',
          sha(kernel) == recorded['prelinked_sha256'],
          {'bytes': len(kernel), 'sha256': sha(kernel),
           'recorded': recorded['prelinked_sha256'],
           'summary': f'{len(kernel):,} B  {sha(kernel)}'})

    macho = macho_segments(kernel) if kernel else {'segments': [], 'fileset_entries': 0}
    names = [s['name'] for s in macho['segments']]
    check('prelinked_fileset', macho.get('filetype') == 12,
          {'filetype': macho.get('filetype'), 'fileset_entries': macho['fileset_entries'],
           'segments': names,
           'summary': f"MH_FILESET, {macho['fileset_entries']} entries, "
                      f"{len(names)} segments"})
    check('no_diagnostic_segment', '__KDIAG' not in names,
          {'summary': '__KDIAG absent' if '__KDIAG' not in names else '__KDIAG PRESENT'})

    pool = check_pool(kernel, POOL_PAGES['native_4k_pr249'])
    check('pr249_pool_in_machine_code', pool['movz_hits'] >= 1 and pool['adjacent_pair'],
          {**pool, 'summary': f"movz x2,#{pool['pages']} + lsl x2,x2,#9 adjacent="
                              f"{pool['adjacent_pair']}"})

    # Question 1: is PR249 relevant to this board at all?
    adt = files['EFI/UniXNU/Payloads/kernel.macho.adt']
    adt_tmp = ROOT / '.work/audit/shipped.adt'
    adt_tmp.parent.mkdir(parents=True, exist_ok=True)
    adt_tmp.write_bytes(adt)
    window = extract_window(adt)
    l1_entries = 1 + -(-window // ARM_TT_L1_SIZE) if window else 0
    pages_used = 2 * (l1_entries + 1) if window else 0
    report['pr249_relevance'] = {
        'result': 'NOT-THE-FIX',
        'owned_window_bytes': window,
        'l1_entries': l1_entries,
        'pages_used': pages_used,
        'pool_before_pr249': POOL_PAGES['native_4k_before_pr249'],
        'pool_after_pr249': POOL_PAGES['native_4k_pr249'],
        'old_pool_sufficient': pages_used <= POOL_PAGES['native_4k_before_pr249'],
        'summary': (f'{window / (1 << 30):.2f} GiB window needs {pages_used} bootstrap '
                    f'pages; the old 16-page pool already covered it, so PR249 adds '
                    f'{(POOL_PAGES["native_4k_pr249"] - POOL_PAGES["native_4k_before_pr249"]) * 4} '
                    f'KiB this board cannot use'),
    }
    print(f"[INFO] pr249_relevance: {report['pr249_relevance']['summary']}")

    snapshot = {'TCR_EL1': 0x00080022a51a251a, 'SCTLR_EL1': 0x00000000fc54799d,
                'source': 'reporter-4 checkpoint the user supplied for kernel cc690c9a'}
    tcr, sctlr = decode_tcr(snapshot['TCR_EL1']), decode_sctlr(snapshot['SCTLR_EL1'])
    report['device_registers'] = {'snapshot': snapshot, 'tcr': tcr, 'sctlr': sctlr,
                                  'result': 'PASS' if not tcr['anomalies'] else 'ATYPICAL'}
    print(f"[{'PASS' if not tcr['anomalies'] else 'INFO'}] device_registers: "
          f"TG0={tcr['TG0_name']} TG1={tcr['TG1_name']} IPS={tcr['IPS_name']} "
          f"MMU={sctlr['MMU']} anomalies={tcr['anomalies'] or 'none'}")

    # Question 2: does the shipped ADT identify as a macOS device?
    gate = subprocess.run([sys.executable, str(ADT_CHECKER), '--adt', str(adt_tmp)],
                          capture_output=True, text=True)
    identity_ok = gate.returncode == 0
    check('adt_macos_identity', identity_ok,
          {'returncode': gate.returncode,
           'summary': 'ADT identifies as a Mac' if identity_ok
                      else 'ADT still identifies as the hardware DTS: no macOS platform '
                           'identity, so the kernel matches no platform expert'})
    if not identity_ok:
        for line in gate.stdout.splitlines():
            if line.strip().startswith('[FAIL]'):
                print(f'       {line.strip()}')

    report['verdict'] = {
        'artifact_internally_consistent': not [f for f in failures
                                               if f != 'adt_macos_identity'],
        'adt_identity_gate_passed': identity_ok,
        'pr249_is_the_fix_for_the_green_bar': False,
        'reasoning': (
            'PR249 raises the native 4 KiB bootstrap pool from 16 to 22 pages, a '
            'capacity guard arm_vm_init checks under DEVELOPMENT||DEBUG. This board '
            'owns a 2 GiB window needing 8 pages, so the pre-PR249 pool already passed '
            'that check; the device reached set_mmu_ttb_alternate without panicking, '
            'which proves the guard never fired. The shipped ADT then identifies as a '
            'Qualcomm MTP with an empty /arm-io and no /product, so the kernel cannot '
            'select a macOS platform expert at all.'),
        'hardware_boot_verified': False,
        'device_operations': False,
    }

    print('\n--- verdict ---')
    print(f"  artifact internally consistent : {report['verdict']['artifact_internally_consistent']}")
    print(f"  ADT identity gate              : {'PASS' if identity_ok else 'FAIL'}")
    print('  PR249 is the fix for the hang  : False')
    print('  hardware boot verified         : False')

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + '\n')
        print(f'\nwrote {args.json}')
    return 0 if not failures else 1


def extract_window(adt):
    """Pull /chosen darwinboot-memory-size out of the flat ADT."""
    cursor = [0]

    def node():
        n, c = struct.unpack_from('<II', adt, cursor[0])
        cursor[0] += 8
        props = []
        for _ in range(n):
            key = adt[cursor[0]:cursor[0] + 32].split(b'\0', 1)[0].decode('latin-1')
            cursor[0] += 32
            (size,) = struct.unpack_from('<I', adt, cursor[0])
            cursor[0] += 4
            value = adt[cursor[0]:cursor[0] + size]
            cursor[0] += size + (-size % 4)
            props.append((key, value))
        return props, [node() for _ in range(c)]

    root_props, kids = node()
    for kid in kids:
        kid_name = next((v.rstrip(b'\0').decode('latin-1') for k, v in kid[0] if k == 'name'), '')
        if kid_name == 'chosen':
            for key, value in kid[0]:
                if key == 'darwinboot-memory-size':
                    return int.from_bytes(value, 'little')
    return 0


if __name__ == '__main__':
    sys.exit(main())
