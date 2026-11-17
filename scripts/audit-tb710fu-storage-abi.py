#!/usr/bin/env python3
"""Compare a retained macOS family vtable with one compiled NativeUfs object.

This is a static ABI check. It does not load a family, register a metaclass,
issue I/O, publish media, authenticate a root handoff, or mount APFS.
Only output files explicitly named by --output are written.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tomllib
import zipfile

sys.dont_write_bytecode = True


def sha(data):
    return hashlib.sha256(data).hexdigest()


def file_record(path):
    path = Path(path).resolve()
    data = path.read_bytes()
    return {"path": str(path), "bytes": len(data), "sha256": sha(data)}


class MachO:
    def __init__(self, data, at=0):
        self.data, self.at = data, at
        magic, cpu, subtype, self.kind, count, commands_size = struct.unpack_from("<6I", data, at)
        assert magic == 0xfeedfacf and cpu == 0x100000c
        self.subtype = subtype
        self.segments, self.sections, self.commands, self.entries = [], [], [], {}
        pos = at + 32
        for _ in range(count):
            cmd, size = struct.unpack_from("<II", data, pos)
            assert size >= 8 and pos + size <= at + 32 + commands_size
            self.commands.append((cmd, pos, size))
            if cmd == 0x19:
                name, va, vmsize, fileoff, filesize = struct.unpack_from("<16s4Q", data, pos + 8)
                self.segments.append(dict(name=name.rstrip(b"\0").decode(), va=va,
                                          size=vmsize, fileoff=fileoff, filesize=filesize))
                ns = struct.unpack_from("<I", data, pos + 64)[0]
                for index in range(ns):
                    item = pos + 72 + 80 * index
                    name, seg, va, section_size, fileoff, align, reloff, nreloc, flags = struct.unpack_from("<16s16sQQIIIII", data, item)
                    self.sections.append(dict(name=name.rstrip(b"\0").decode(), segment=seg.rstrip(b"\0").decode(),
                                              va=va, size=section_size, fileoff=fileoff, reloff=reloff, nreloc=nreloc, flags=flags))
            elif cmd == 0x80000035:
                va, fileoff, str_at = struct.unpack_from("<QQI", data, pos + 8)
                end = data.index(b"\0", pos + str_at)
                self.entries[data[pos + str_at:end].decode()] = (va, fileoff)
            pos += size
        assert pos == at + 32 + commands_size
        self.base = (next(s["va"] for s in self.segments if s["fileoff"] == 0 and s["filesize"])
                     if at == 0 and self.kind != 1 else self.segments[0]["va"])

    def offset(self, va, length=1):
        for s in self.segments:
            if s["va"] <= va and va + length <= s["va"] + s["filesize"]:
                return s["fileoff"] + va - s["va"]
        raise ValueError(f"Unbacked VA {va:#x}+{length:#x}")

    def read(self, va, length):
        pos = self.offset(va, length)
        return self.data[pos:pos + length]

    def nlists(self):
        found = next((p for c, p, _ in self.commands if c == 2), None)
        if found is None:
            return []
        symoff, count, stroff, strsize = struct.unpack_from("<4I", self.data, found + 8)
        result = []
        for i in range(count):
            strx, kind, section, desc, value = struct.unpack_from("<IBBHQ", self.data, symoff + 16 * i)
            assert strx < strsize
            end = self.data.index(b"\0", stroff + strx)
            result.append(dict(name=self.data[stroff + strx:end].decode(), kind=kind,
                               section=section, value=value))
        return result

    def symbols(self):
        result = {}
        for symbol in self.nlists():
            if not symbol["kind"] & 0xe0 and symbol["kind"] & 0x0e == 0x0e:
                result.setdefault(symbol["value"], []).append(symbol["name"])
        return result

    def fixups(self):
        found = next((p for c, p, _ in self.commands if c == 0x80000034), None)
        assert found is not None
        dataoff, datasize = struct.unpack_from("<II", self.data, found + 8)
        _, startsoff, _, _, imports, _, _ = struct.unpack_from("<7I", self.data, dataoff)
        assert not imports
        table = dataoff + startsoff
        count = struct.unpack_from("<I", self.data, table)[0]
        result = {}
        for i in range(count):
            relative = struct.unpack_from("<I", self.data, table + 4 + 4 * i)[0]
            if not relative:
                continue
            pos = table + relative
            _, page_size, fmt, segment_offset, _, pages = struct.unpack_from("<IHHQIH", self.data, pos)
            assert fmt in (7, 8)
            for page, start in enumerate(struct.unpack_from(f"<{pages}H", self.data, pos + 22)):
                if start == 0xffff:
                    continue
                assert not start & 0x8000
                site = self.base + segment_offset + page * page_size + start
                while True:
                    raw = struct.unpack("<Q", self.read(site, 8))[0]
                    auth = raw >> 63
                    if fmt == 8:
                        assert not (raw >> 30) & 3
                        target, step = self.base + (raw & 0x3fffffff), (raw >> 51) & 0xfff
                    else:
                        assert not (raw >> 62) & 1
                        offset = raw & 0xffffffff if auth else (raw & 0x7ffffffffff) | ((raw >> 43) & 255) << 56
                        target, step = self.base + offset, (raw >> 51) & 0x7ff
                    result[site] = (target, auth, (raw >> 49) & 3 if auth else None,
                                    (raw >> 32) & 65535 if auth else None, (raw >> 48) & 1 if auth else None)
                    if not step:
                        break
                    site += step * 4
        return result


def ar_members(data):
    assert data[:8] == b"!<arch>\n"
    pos = 8
    while pos < len(data):
        header = data[pos:pos + 60]
        assert header[58:60] == b"`\n"
        size, name = int(header[48:58]), header[:16].decode().strip()
        member = data[pos + 60:pos + 60 + size]
        pos += (60 + size + 1) & ~1
        if name.startswith("#1/"):
            length = int(name[3:])
            name, member = member[:length].rstrip(b"\0").decode(), member[length:]
        yield name, member


def address(symbols, name):
    return next(a for a, names in symbols.items() if name in names)


def target_vtable(image, root_fixups, name, all_symbols):
    base = address(image.symbols(), name)
    slots = []
    for index in range(1024):
        pointer = root_fixups.get(base + 16 + index * 8)
        if not pointer:
            break
        assert pointer[1:3] == (1, 0)
        slots.append(dict(index=index, byte_offset=index * 8, site=hex(base + 16 + index * 8),
                          target=hex(pointer[0]), target_symbols=all_symbols.get(pointer[0], []),
                          pac=dict(auth=pointer[1], key=pointer[2], diversity=pointer[3], address_diversity=pointer[4])))
    assert slots
    return dict(symbol=name, address=hex(base), slots=slots, method_slots=len(slots))


def object_vtable(image, name):
    symbol = next(s for s in image.nlists() if s["name"] == name)
    section = image.sections[symbol["section"] - 1]
    base = symbol["value"] - section["va"]
    symbols, relocs = image.nlists(), {}
    for i in range(section["nreloc"]):
        pos, word = struct.unpack_from("<iI", image.data, section["reloff"] + 8 * i)
        external, reloc_type, symbol_index = (word >> 27) & 1, word >> 28, word & 0xffffff
        if pos >= base + 16 and external:
            raw = struct.unpack_from("<Q", image.data, section["fileoff"] + pos)[0]
            relocs[pos] = dict(symbol=symbols[symbol_index]["name"], type=reloc_type, raw=hex(raw),
                               pac=dict(auth=raw >> 63, key=(raw >> 49) & 3,
                                        diversity=(raw >> 32) & 65535, address_diversity=(raw >> 48) & 1))
    slots = []
    for index in range(1024):
        reloc = relocs.get(base + 16 + index * 8)
        if not reloc:
            break
        assert reloc["type"] == 11  # ARM64_RELOC_AUTHENTICATED_POINTER
        slots.append(dict(index=index, byte_offset=index * 8, **reloc))
    return dict(symbol=name, section=f'{section["segment"]},{section["name"]}',
                section_offset=hex(base), slots=slots, method_slots=len(slots))


API = ["doEjectMedia", "doFormatMedia", "doGetFormatCapacities", "doLockUnlockMedia", "doSynchronizeCache",
       "getVendorString", "getProductString", "getRevisionString", "getAdditionalDeviceInfoString", "reportBlockSize",
       "reportEjectability", "reportLockability", "reportMaxValidBlock", "reportMediaState", "reportPollRequirements",
       "reportRemovability", "reportWriteProtection", "getWriteCacheState", "setWriteCacheState", "doAsyncReadWrite",
       "requestIdle", "doDiscard", "doUnmap", "doSetPriority", "doSynchronize", "doGetProvisionStatus"]


def canonical_method(name):
    return re.sub(r"^__ZN(K?)\d+(?:IOBlockStorageDevice|NativeUfsBlock)", r"__ZN\1CLASS", name)


def wsl_path(path):
    path = str(Path(path).resolve())
    if re.match(r"^[A-Za-z]:", path):
        return "/mnt/" + path[0].lower() + path[2:].replace("\\", "/")
    return path


def tool_output(command, args, stdin=None):
    result = subprocess.run(command + args, input=stdin, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(result.stderr)
    return result.stdout


def disassemble(command, code):
    return tool_output(command, ["--disassemble", "--triple=arm64e-apple-darwin", "--mattr=+pauth"],
                       "\n".join(" ".join(f"0x{b:02x}" for b in code[i:i + 4]) for i in range(0, len(code), 4)))


def consumer_dispatch(image, name, mc_command, expected, table):
    symbols = image.symbols()
    start = address(symbols, name)
    end = min(a for a in symbols if a > start)
    code = image.read(start, end - start)
    assembly = disassemble(mc_command, code)
    # Follow the address and function operands through both clang dispatch forms:
    # mov-offset/add/ldr and immediate-add/indexed-ldr. This is a small straight-
    # line provenance tracker, not execution or a general AArch64 interpreter.
    registers, pairs = {}, []
    instruction_index = 0
    for line in assembly.splitlines():
        line = line.split(";", 1)[0].strip()
        if not line or line.startswith("."):
            continue
        pc = start + instruction_index * 4
        instruction_index += 1
        branch = re.fullmatch(r"(blraa|braa)\s+(x\d+), (x\d+)", line)
        if branch:
            function = registers.get(branch.group(2))
            modifier = registers.get(branch.group(3))
            if function and modifier and function[0] == "function" and modifier[0] == "modifier":
                offset, diversity = modifier[1:]
                if function[1] == offset and offset in expected:
                    slot = table["slots"][offset // 8]
                    pairs.append(dict(byte_offset=offset, index=offset // 8, diversity=diversity,
                                      branch=branch.group(1), callsite=hex(pc),
                                      function_operand=branch.group(2), modifier_operand=branch.group(3),
                                      target_slot_diversity=slot["pac"]["diversity"],
                                      matches_expected=diversity == expected[offset] == slot["pac"]["diversity"]
                                      and slot["pac"]["auth"] == 1 and slot["pac"]["key"] == 0
                                      and slot["pac"]["address_diversity"] == 1))
            registers.clear()  # A call ends this dispatch sequence.
            continue
        match = re.fullmatch(r"mov\s+(x\d+), (x\d+|#\d+)", line)
        if match:
            value = match.group(2)
            registers[match.group(1)] = ("constant", int(value[1:])) if value.startswith("#") else registers.get(value)
            continue
        match = re.fullmatch(r"autda\s+(x\d+), x\d+", line)
        if match:
            registers[match.group(1)] = ("authenticated-vtable",)
            continue
        match = re.fullmatch(r"add\s+(x\d+), (x\d+), (x\d+|#\d+)", line)
        if match:
            destination, base, increment = match.groups()
            value = ("constant", int(increment[1:])) if increment.startswith("#") else registers.get(increment)
            registers[destination] = (("slot-address", value[1]) if registers.get(base) == ("authenticated-vtable",)
                                      and value and value[0] == "constant" else None)
            continue
        match = re.fullmatch(r"ldr\s+(x\d+), \[(x\d+)(?:, #(\d+))?\]", line)
        if match:
            destination, base, increment = match.groups()
            value = registers.get(base)
            offset = int(increment or 0)
            registers[destination] = (("function", value[1] + offset) if value and value[0] == "slot-address"
                                      else ("function", offset) if value == ("authenticated-vtable",) else None)
            continue
        match = re.fullmatch(r"movk\s+(x\d+), #(\d+), lsl #48", line)
        if match:
            destination, immediate = match.groups()
            value = registers.get(destination)
            registers[destination] = (("modifier", value[1], int(immediate)) if value and value[0] == "slot-address" else None)
            continue
        # Invalidate other explicit register destinations, while stores, compares
        # and control flow do not write their first operand.
        match = re.match(r"(\w+)\s+([xw]\d+)", line)
        if match and match.group(1) not in {"stp", "str", "cmp", "tbz", "tbnz", "cbz", "cbnz", "brk"}:
            registers.pop("x" + match.group(2)[1:], None)
            if match.group(1) == "ldp":
                second = re.match(r"ldp\s+[xw]\d+, ([xw]\d+)", line)
                if second:
                    registers.pop("x" + second.group(1)[1:], None)
    passed = ({p["byte_offset"] for p in pairs} == set(expected) and all(p["matches_expected"] for p in pairs))
    return dict(symbol=name, address=hex(start), bytes=len(code), code_sha256=sha(code), code_hex=code.hex(),
                disassembly=assembly, observed_storage_dispatch=pairs, expected_dispatch=expected,
                static_dispatch_matches=passed, executed=False)


def zip_record(path, expected, member, member_hash):
    record = file_record(path)
    assert record["sha256"] == expected["digest"].removeprefix("sha256:")
    assert record["bytes"] == expected["size_in_bytes"]
    with zipfile.ZipFile(path) as archive:
        with archive.open(member) as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        assert digest == member_hash
        record["member"] = dict(name=member, bytes=archive.getinfo(member).file_size, sha256=digest)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--provenance", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--kernel-report", type=Path, required=True)
    parser.add_argument("--kernel-zip", type=Path, required=True)
    parser.add_argument("--native-archive", type=Path, required=True)
    parser.add_argument("--compile-report", type=Path, required=True)
    parser.add_argument("--native-zip", type=Path, required=True)
    parser.add_argument("--native-zip-member", default="libTb710fuNativeUfs.a")
    parser.add_argument("--object-member", default="NativeUfs.o")
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    provenance = json.loads(args.provenance.read_text())
    kernel_report = json.loads(args.kernel_report.read_text())
    compile_report = json.loads(args.compile_report.read_text())
    profile = tomllib.loads(args.profile.read_text())
    kernel_data, native_data = args.kernel.read_bytes(), args.native_archive.read_bytes()
    assert sha(kernel_data) == kernel_report["candidate_sha256"] == provenance["kernel"]["sha256"]
    assert sha(native_data) == compile_report["archive_sha256"]
    assert kernel_report["inputs"]["kernelcache_sha256"] == profile["kernel"]["prelink"]["kernelcache_sha256"]
    assert kernel_report["mapping_complete"] and not kernel_report["missing"] and not kernel_report["stubbed"]
    kernel_zip = zip_record(args.kernel_zip, provenance["kernel"]["artifact"], "kernel/kernel.macho", sha(kernel_data))
    native_zip = zip_record(args.native_zip, provenance["native"]["artifact"], args.native_zip_member, sha(native_data))
    root = MachO(kernel_data)
    assert root.kind == 12
    targets = {name: MachO(kernel_data, root.entries[name][1]) for name in
               ("com.apple.kernel", "com.apple.iokit.IOStorageFamily", "com.apple.filesystems.apfs")}
    all_symbols = {}
    for image in targets.values():
        all_symbols.update(image.symbols())
    fixups = root.fixups()
    family = targets["com.apple.iokit.IOStorageFamily"]
    vtables = {name: target_vtable(family, fixups, name, all_symbols) for name in
               ("__ZTV20IOBlockStorageDevice", "__ZTV20IOBlockStorageDriver", "__ZTV9IOStorage", "__ZTV7IOMedia")}
    member_name, member_data = next((name, data) for name, data in ar_members(native_data) if name == args.object_member)
    source = object_vtable(MachO(member_data), "__ZTV14NativeUfsBlock")
    target = vtables["__ZTV20IOBlockStorageDevice"]
    mismatches = [dict(index=i, target=t["pac"], source=s["pac"]) for i, (t, s) in
                  enumerate(zip(target["slots"], source["slots"])) if t["pac"] != s["pac"]]
    api = []
    for index, method in enumerate(API, 272):
        t, s = target["slots"][index], source["slots"][index]
        source_name_ok = f"{len(method)}{method}" in s["symbol"]
        pure = t["target_symbols"] == ["___cxa_pure_virtual"]
        target_name_ok = pure or any(canonical_method(n) == canonical_method(s["symbol"]) for n in t["target_symbols"])
        api.append(dict(index=index, byte_offset=index * 8, method=method, source_symbol=s["symbol"],
                        target_symbols=t["target_symbols"], target_abstract_pure_slot=pure,
                        method_name_match=source_name_ok and target_name_ok, pac_match=t["pac"] == s["pac"], pac=t["pac"]))
    dwarf_cmd = provenance["tools"]["dwarfdump"]
    dwarf_path = wsl_path(args.native_archive) if dwarf_cmd[0].lower().endswith("wsl.exe") else str(args.native_archive.resolve())
    dwarf = tool_output(dwarf_cmd, ["--name=IOBlockStorageDevice", dwarf_path])
    source_size = int(re.search(r"DW_AT_byte_size\s+\((0x[0-9a-f]+)\)", dwarf).group(1), 16)
    sy = family.symbols()
    ctor = address(sy, "__ZN20IOBlockStorageDevice9MetaClassC1Ev")
    next_symbol = min(a for a in sy if a > ctor)
    code = family.read(ctor, next_symbol - ctor)
    sizes = [(ctor + off, ((word >> 5) & 65535) << (((word >> 21) & 3) * 16)) for off in range(0, len(code), 4)
             if ((word := struct.unpack_from("<I", code, off)[0]) & 0xff80001f) == 0x52800003]
    assert len(sizes) == 1
    mc_cmd = provenance["tools"]["llvm_mc"]
    disasm = disassemble(mc_cmd, code)
    class_size = dict(source_dwarf_bytes=source_size, target_metaclass_bytes=sizes[0][1], equal=source_size == sizes[0][1],
                      target_constructor=hex(ctor), size_instruction=hex(sizes[0][0]), target_constructor_hex=code.hex(),
                      target_disassembly=disasm, source_dwarf=dwarf,
                      limitation="Supplementary object-size evidence; it does not establish virtual dispatch compatibility.")
    consumers = [consumer_dispatch(family, name, mc_cmd, expected, target) for name, expected in [
        ("__ZN20IOBlockStorageDriver21recordMediaParametersEv", {0x8c8: 40251, 0x8e0: 32176, 0x900: 57027}),
        ("__ZN20IOBlockStorageDriver14executeRequestEyP18IOMemoryDescriptorP19IOStorageAttributesP19IOStorageCompletionPNS_7ContextE", {0x918: 58235})]]
    passed = (target["method_slots"] == source["method_slots"] == 326 and not mismatches and
              all(a["method_name_match"] and a["pac_match"] for a in api) and class_size["equal"])
    passed = passed and all(c["static_dispatch_matches"] for c in consumers)
    entry_inventory = {name: dict(header_va=hex(root.entries[name][0]), header_fileoff=hex(root.entries[name][1]),
                                  defined_symbol_addresses=len(image.symbols()),
                                  vtable_symbols={n: hex(a) for a, names in image.symbols().items() for n in names if n.startswith("__ZTV")})
                       for name, image in targets.items() if name != "com.apple.kernel"}
    report = dict(schema_version=1, result="static-block-device-layout-matched" if passed else "mismatch",
                  runtime_qualified=False, hardware_boot_verified=False, root_mount_verified=False,
                  audit_script=file_record(__file__), provenance_file=file_record(args.provenance),
                  provenance=provenance, kernel=file_record(args.kernel), native_archive=file_record(args.native_archive),
                  kernel_zip=kernel_zip, native_zip=native_zip, kernel_report=file_record(args.kernel_report),
                  compile_report=file_record(args.compile_report), profile=file_record(args.profile),
                  exact_kernelcache_input_sha256=kernel_report["inputs"]["kernelcache_sha256"],
                  original_kernelcache_independently_rehashed=False,
                  original_kernelcache_note="Retained prelink provenance identifies the profile-pinned Apple cache. The original unmodified cache was not present in the inspected cache directories.",
                  source_revision=compile_report["upstream_revision"], source_sha256=compile_report["upstream_source_sha256"],
                  source_build_evidence={k: compile_report[k] for k in
                                         ("board_patches", "board_patch", "board_patch_sha256", "object_sha256", "xnu_compile_record_sha256", "target")
                                         if k in compile_report},
                  source_header_inputs={k: v for k, v in compile_report["included_inputs_sha256"].items()
                                        if any(k.endswith("/" + n) for n in ("IOBlockStorageDevice.h", "IOStorage.h", "IOMedia.h"))},
                  archive_member=dict(name=member_name, sha256=sha(member_data), bytes=len(member_data)),
                  fileset_entries=len(root.entries), chained_fixup_sites=len(fixups), entries=entry_inventory,
                  full_pac_comparison=dict(target_method_slots=target["method_slots"], source_method_slots=source["method_slots"],
                                           mismatches=mismatches, compared_fields=["auth", "key", "diversity", "address_diversity"]),
                  named_api_comparison=api, class_size=class_size, binary_consumer_dispatch=consumers,
                  target_vtables=vtables, source_vtable=source,
                  limitations=["Slots 0..271: inherited PAC layout only; not an implementation-equivalence proof.",
                               "Slots 272..297: source method name/order and target named-or-pure slot plus complete PAC metadata.",
                               "Slots 298..325: reserved4..31 remain opaque; slot/PAC equality does not qualify reserved semantics.",
                               "IOStorage/IOMedia target vtables and APFS symbols are inventoried; there is no corresponding compiled source model comparison for these classes in this check.",
                               "No SourceStorage startup, metaclass registration, binary IOStorageFamily virtual call, DMA read, media UUID/parent-chain or APFS root mount was executed."])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(dict(result=report["result"], output=str(args.output.resolve()), slots=target["method_slots"],
                          pac_mismatches=len(mismatches), named_methods=len(api), class_size_equal=class_size["equal"],
                          runtime_qualified=False), indent=2))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
