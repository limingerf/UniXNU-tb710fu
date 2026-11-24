#!/usr/bin/env python3
"""Build the TB710FU macOS 27 GopFramebuffer display archive (pl21).

The macOS 27 (26C5035r) `IOMobileFramebufferLegacy` is a prelinked kext class, so
this board cannot bind it the way the iPadOS recipe does (fixed collection
addresses plus a slide): XnuPrelink re-lays Apple kext text in this fileset, and
none of the 483 vtable targets in the reviewed fixture sit at their fixture
address in our own `kernel.macho` (verified by scripts/check-gop-display-abi.py).
What is stable is the *shape*: 483 authenticated vtable slots, 272 of them
IOService's, their diversities, the 0x23a0 instance extent and the field offsets.

So the generated subclass owns all 211 legacy slots (25 real overrides, 186 guards),
declares every one of them under a name whose pointer-authentication discriminator
is the original slot's diversity, and at boot adopts the live original vtable entry
for every slot the backend does not own. No original code address, symbol or vtable
is referenced at build time; package/GopDisplayStart.cpp resolves the live metaclass
by name and derives the live vtable from it.

Boot-arg `tb710fu-gop=1` gates all of it: without it
`native_tb710fu_gop_display_start` returns before touching the registry, so
tb710fu-live-log keeps its console and nothing else changes.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

FIXTURE = 'legacy-display-26C5035r.json'
DISPLAY_SOURCE = 'NativeLegacyDisplay.cpp'

# Four macOS-gated family slots (setNotifyTagMaskGated 318, supportedFrameInfo 320,
# factoryPortal 361, executeFactoryConfigurations 362) are rebound by XnuPrelink to a
# single native function, so their live entry carries that function's discriminator.
REBOUND_SLOTS = (318, 320, 361, 362)
IOSERVICE_SLOTS = ('start', 'stop', 'free', 'setPowerState', 'newUserClient')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def revision(path):
    top = Path(subprocess.check_output(
        ['git', '-C', str(path), 'rev-parse', '--show-toplevel'], text=True).strip()).resolve()
    if top != path.resolve():
        raise SystemExit(f'Initialize the pinned component before native build: {path}')
    return subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()


def helper(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def defer_constructor(obj, late, init):
    data = bytearray(obj.read_bytes())
    if struct.unpack_from('<I', data)[0] != 0xfeedfacf:
        raise SystemExit(f'{obj}: not a 64-bit Mach-O object')
    count = struct.unpack_from('<I', data, 16)[0]
    symtab, renamed, offset = None, 0, 32
    for _ in range(count):
        command, size = struct.unpack_from('<II', data, offset)
        if command == 0x19:
            sections = struct.unpack_from('<I', data, offset + 64)[0]
            for index in range(sections):
                section = offset + 72 + index * 80
                if data[section:section + 16].rstrip(b'\0') == b'__mod_init_func':
                    data[section:section + 16] = b'__mod_deferred\0\0'
                    flags = struct.unpack_from('<I', data, section + 64)[0]
                    struct.pack_into('<I', data, section + 64, flags & ~0xff)
                    renamed += 1
        elif command == 0x2:
            symtab = struct.unpack_from('<IIII', data, offset + 8)
        offset += size
    if not renamed:
        raise SystemExit(f'{obj}: expected eager metaclass constructor section missing')
    if symtab is None:
        raise SystemExit(f'{obj}: no symbol table')
    symoff, nsyms, stroff, _ = symtab
    external = 0
    for index in range(nsyms):
        entry = symoff + index * 16
        string, n_type = struct.unpack_from('<IB', data, entry)
        name = bytes(data[stroff + string:]).split(b'\0', 1)[0]
        if name == init.encode():
            struct.pack_into('<B', data, entry + 4, n_type | 0x01)
            external += 1
    if not external:
        raise SystemExit(f'{obj}: expected deferred constructor {init} missing from symbols')
    late.write_bytes(bytes(data))


def object_layout(data):
    """Sections and symbols of an arm64e relocatable object."""
    magic, _cpu, _subtype, kind, count = struct.unpack_from('<5I', data)
    if magic != 0xfeedfacf or kind != 1:
        raise SystemExit('not a relocatable 64-bit Mach-O object')
    sections, symbols, offset = [], [], 32
    for _ in range(count):
        command, size = struct.unpack_from('<II', data, offset)
        if command == 0x19:
            segment = data[offset + 8:offset + 24].rstrip(b'\0').decode()
            nsects = struct.unpack_from('<I', data, offset + 64)[0]
            for index in range(nsects):
                section = offset + 72 + index * 80
                name = data[section:section + 16].rstrip(b'\0').decode()
                sva, vsize, fileoff = struct.unpack_from('<QQI', data, section + 32)
                reloff, nreloc = struct.unpack_from('<2I', data, section + 56)
                sections.append({'segment': segment, 'name': name, 'va': sva, 'size': vsize,
                                 'fileoff': fileoff, 'reloff': reloff, 'nreloc': nreloc})
        elif command == 0x2:
            symoff, nsyms, stroff, _ = struct.unpack_from('<4I', data, offset + 8)
            for index in range(nsyms):
                entry = symoff + index * 16
                string, ntype, nsect, _desc, value = struct.unpack_from('<IBBHQ', data, entry)
                name = data[stroff + string:data.index(b'\0', stroff + string)].decode()
                symbols.append({'name': name, 'type': ntype, 'section': nsect, 'value': value})
        offset += size
    return sections, symbols


def object_vtable(data, sections, symbols, vtable_symbol, count):
    """[(target symbol, discriminator)] of the first count entries of the named vtable."""
    va = next(symbol['value'] for symbol in symbols if symbol['name'] == vtable_symbol)
    section = next(s for s in sections if s['va'] <= va < s['va'] + s['size'])
    entries = {}
    for index in range(section['nreloc']):
        offset, bits = struct.unpack_from('<2I', data, section['reloff'] + index * 8)
        if bits >> 28 == 11 and (bits >> 25) & 3 == 3 and (bits >> 27) & 1:
            word = struct.unpack_from('<Q', data, section['fileoff'] + offset)[0]
            entries[section['va'] + offset] = (symbols[bits & 0xffffff]['name'], (word >> 32) & 0xffff)
    return [entries.get(va + 16 + i * 8) for i in range(count)]


def demangled_label(symbol, class_name='NativeLegacyDisplay'):
    """Method label of `_ZN18NativeLegacyDisplay...` or None for another symbol."""
    marker = f'{len(class_name)}{class_name}'
    if not symbol.startswith(f'__ZN{marker}'):
        return None
    rest = symbol[len('__ZN') + len(marker):]
    digits = ''
    for character in rest:
        if not character.isdigit():
            break
        digits += character
    if not digits:
        return None
    length = int(digits)
    return rest[len(digits):len(digits) + length]


def generate_traits(path, abi, owned_slots, ioservice_slots):
    offsets = (('SURFACE_DESCRIPTOR', 0x30, 8), ('SURFACE_WIDTH', 0x58, 8),
               ('SURFACE_HEIGHT', 0x60, 8), ('SURFACE_STRIDE', 0x68, 8),
               ('SURFACE_FORMAT', 0x80, 4), ('SURFACE_PLANES', 0x98, 4))
    lines = [
        '// SPDX-License-Identifier: BSD-2-Clause-Patent',
        f'// Generated from {FIXTURE} by scripts/build-tb710fu-gop-display.py. Do not edit.',
        '#pragma once',
        f'#define TB710FU_GOP_INSTANCE_SIZE 0x{int(abi["instance_size"], 16):x}u',
        f'#define TB710FU_GOP_BASE_SLOTS {abi["base_slots"]}u',
        f'#define TB710FU_GOP_SLOT_COUNT {len(abi["slots"])}u',
        f'#define TB710FU_GOP_METACLASS_VA 0x{int(abi["bindings"]["__ZN25IOMobileFramebufferLegacy10gMetaClassE"], 16):x}ull',
        f'#define TB710FU_GOP_VTABLE_VA 0x{int(abi["vtable"], 16):x}ull',
    ]
    for name, value, width in offsets:
        lines.append(f'#define TB710FU_GOP_{name} 0x{value:x}u')
        lines.append(f'#define TB710FU_GOP_{name}_WIDTH {width}u')
    for name, slot in sorted(ioservice_slots.items()):
        lines.append(f'#define TB710FU_GOP_SLOT_{name.upper()} {slot}u')
    lines += [
        f'#define TB710FU_GOP_OWNED_COUNT {len(owned_slots)}u',
        'extern "C" const unsigned tb710fu_gop_owned_slots[TB710FU_GOP_OWNED_COUNT];',
        'extern "C" const unsigned short tb710fu_gop_slot_diversity[TB710FU_GOP_SLOT_COUNT];',
        '',
    ]
    path.write_text('\n'.join(lines))

    source = [
        '// SPDX-License-Identifier: BSD-2-Clause-Patent',
        f'// Generated from {FIXTURE} by scripts/build-tb710fu-gop-display.py. Do not edit.',
        '#include "GopDisplayTraits.h"',
        f'extern "C" const unsigned tb710fu_gop_owned_slots[{len(owned_slots)}] = {{',
        '    ' + ', '.join(str(slot) for slot in owned_slots),
        '};',
        f'extern "C" const unsigned short tb710fu_gop_slot_diversity[{len(abi["slots"])}] = {{',
    ]
    values = [int(diversity, 16) for _, diversity in abi['slots']]
    for start in range(0, len(values), 16):
        source.append('    ' + ', '.join(f'0x{value:04x}' for value in values[start:start + 16]) + ',')
    source += ['};', '']
    (path.parent / 'GopDisplayTraits.cpp').write_text('\n'.join(source))


def generate_abi_header(path, abi):
    display, request, notify = abi['display'], abi['request'], abi['notify']
    by_slot = {slot: name for name, slot in abi['methods'].items()}
    prototypes = abi['prototypes']
    first, count = abi['base_slots'], len(abi['slots'])
    leaf_slots = [slot for slot in range(first, count) if slot not in by_slot]
    if len(by_slot) + len(leaf_slots) != count - first:
        raise SystemExit('Fixture does not classify every legacy slot')
    renames = []
    for base in abi['names']:
        if base in abi['methods']:
            renames.append((abi['methods'][base], base))
        else:
            renames.append((int(base[len('opaque'):]), base))
    renames.sort()
    header = f'''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opaque ABI storage for the original macOS 27 (26C5035r) IOMobileFramebufferLegacy,
// generated from {FIXTURE}. Every original slot is declared under a name whose
// pointer-authentication discriminator is the original slot's diversity, and every
// slot is pure here: the owned subclass answers all {count - first} legacy slots itself, so no
// original vtable entry or method symbol can be inherited or linked by accident.
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
struct OriginalLegacySwapRequest;
struct OriginalLegacyStateRequest {{uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];}};
static constexpr unsigned kLegacyVsyncClients={display['vsync_clients']};
static constexpr unsigned kRequestRect0={request['rect0']},kRequestRect1={request['rect1']},kRequestMasks={request['masks']},kRequestOptions={request['options']},kRequestDims={request['dims']},kRequestSurfaces={request['surfaces']};
static constexpr unsigned kLegacyCurrentSwap={display['current']};
static constexpr unsigned kRequestDropReasons={request.get('drop_reasons', 0)},kRequestSwapInfoA={request.get('swap_info_a', 0)},kRequestSwapInfoB={request.get('swap_info_b', 0)};
static constexpr unsigned kLegacySwapInfoTime={display.get('swap_info_time', 0)},kLegacySwapInfoHeld={display.get('swap_info_held', 0)};
static constexpr unsigned kNotifyAsyncReference={notify.get('async_reference', 0)},kNotifyMask={notify.get('mask', 0)},kNotifyLastTime={notify.get('last_time', 0)};
static constexpr unsigned kLegacyDefaultSurface={display['default_surface']};
#define NATIVE_LEGACY_MACOS 1
#define NATIVE_LEGACY_DYNAMIC_BINDING 1
#define NATIVE_LEGACY_CANCEL_SWAPS 1
#define NATIVE_LEGACY_DEFAULT_SURFACE 0
#define NATIVE_LEGACY_LEAVES(LEAF) {' '.join(f'LEAF({slot})' for slot in leaf_slots)}
extern "C" void native_tb710fu_gop_adopt(void *);
extern "C" void native_legacy_parent_free(void *);
extern "C" bool native_legacy_parent_start(void *,IOService *);
extern "C" void native_legacy_parent_stop(void *,IOService *);
extern "C" IOReturn native_legacy_parent_set_power_state(void *,unsigned long,IOService *);
extern "C" IOReturn native_legacy_parent_new_user_client(void *,task_t,void *,UInt32,IOUserClient **);
extern "C" bool native_legacy_parent_refresh_attributes(void *,uint64_t *,bool);
extern "C" void native_legacy_parent_complete_current_swap(void *);
extern "C" void native_legacy_parent_display_dimensions(void *,uint32_t *,uint32_t *);
extern "C" int native_legacy_parent_apply_state_request(void *,OriginalLegacyStateRequest *,bool);
extern "C" void native_legacy_parent_create_default_surface(void *);
'''
    for slot, base in renames:
        header += f'#define {base} {abi["names"][base][0]}\n'
    header += f'''class IOMobileFramebufferLegacy : public IOService {{
protected:
 IOMobileFramebufferLegacy(const OSMetaClass *meta) : IOService(meta) {{}}
 ~IOMobileFramebufferLegacy() override = default;
public:
 unsigned char originalPrivate[{int(abi['instance_size'], 16)}-sizeof(IOService)];
 void installBootDimensions(uint32_t width,uint32_t height) {{
  memcpy(reinterpret_cast<unsigned char *>(this)+{display['width']},&width,4);memcpy(reinterpret_cast<unsigned char *>(this)+{display['height']},&height,4);
 }}
 const OriginalLegacySwapRequest *pendingSwapRequest() const {{
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+{display['head']},sizeof(request));return request;
 }}
 // Tail queue: head {display['head']}, tail pointer {display['tail']}, each request's next {request['next']} and previous link {request['prev']};
 // the head moves to the current-swap slot {display['current']} for the original completion body.
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {{
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+{display['current']},8);memcpy(&head,self+{display['head']},8);memcpy(&tail,self+{display['tail']},8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+{request['next']},8);memcpy(&previous,bytes+{request['prev']},8);
  if(previous!=self+{display['head']})return false;
  if(next) {{
   const void *link=self+{display['head']};
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+{request['prev']},&link,8);
  }} else if(tail!=bytes+{request['next']})return false;
  memcpy(self+{display['current']},&request,8);
  if(next) {{memcpy(self+{display['head']},&next,8);}}
  else {{const void *empty=nullptr,*emptyTail=self+{display['head']};memcpy(self+{display['head']},&empty,8);memcpy(self+{display['tail']},&emptyTail,8);}}
  return true;
 }}
'''
    for slot in range(first, count):
        if slot in by_slot:
            prototype = prototypes[by_slot[slot]].replace('=0', '').strip()
            header += f' virtual {prototype}=0;\n'
        else:
            header += f' virtual __attribute__((noreturn)) void opaque{slot}()=0;\n'
    header += f'}};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)=={int(abi["instance_size"], 16)},"Actual original instance extent");\n'
    path.write_text(header)


def transform_display(source):
    text = source.read_text()
    macro = 'OSDefineMetaClassAndStructors(NativeLegacyDisplay,IOMobileFramebufferLegacy)'
    if text.count(macro) != 1:
        raise SystemExit('NativeLegacyDisplay.cpp metaclass macro changed')
    binding = '''// pl21: bind to the live macOS 27 IOMobileFramebufferLegacy metaclass that
// package/GopDisplayStart.cpp resolves by name; no original symbol, address or
// vtable is linked into this object.
NativeLegacyDisplay::MetaClass NativeLegacyDisplay::gMetaClass;
const OSMetaClass * const NativeLegacyDisplay::metaClass = &NativeLegacyDisplay::gMetaClass;
const OSMetaClass * const NativeLegacyDisplay::superClass = &IOService::gMetaClass;
NativeLegacyDisplay::NativeLegacyDisplay(const OSMetaClass *meta) : IOMobileFramebufferLegacy(meta) {}
NativeLegacyDisplay::~NativeLegacyDisplay() {}
const OSMetaClass *NativeLegacyDisplay::getMetaClass() const { return &gMetaClass; }
extern "C" const OSMetaClass *native_tb710fu_legacy_parent(void);
NativeLegacyDisplay::MetaClass::MetaClass()
    : OSMetaClass("NativeLegacyDisplay", native_tb710fu_legacy_parent(), sizeof(NativeLegacyDisplay)) {}
OSDefineBasicStructors(NativeLegacyDisplay, IOMobileFramebufferLegacy)
OSDefineOperatorMethods(NativeLegacyDisplay)'''
    text = text.replace(macro, binding, 1)

    allocate = 'IOService *native_legacy_display_allocate(void) {return new NativeLegacyDisplay;}'
    if text.count(allocate) != 1:
        raise SystemExit('NativeLegacyDisplay.cpp allocation entry changed')
    text = text.replace(
        allocate,
        'IOService *native_legacy_display_allocate(void) {NativeLegacyDisplay *display=new NativeLegacyDisplay;'
        'if(display)native_tb710fu_gop_adopt(display);return display;}', 1)

    # Qualified base calls are the only place the source names the original class;
    # route each to the live adopted slot instead of an unresolvable symbol.
    for pattern, replacement in (
            ('IOMobileFramebufferLegacy::free();', 'native_legacy_parent_free(this);'),
            ('IOMobileFramebufferLegacy::stop(provider);', 'native_legacy_parent_stop(this,provider);'),
            ('IOMobileFramebufferLegacy::start(provider)', 'native_legacy_parent_start(this,provider)'),
            ('IOMobileFramebufferLegacy::setPowerState(ordinal,device)',
             'native_legacy_parent_set_power_state(this,ordinal,device)'),
            ('IOMobileFramebufferLegacy::getRefreshAttributes(words,fixed)',
             'native_legacy_parent_refresh_attributes(this,words,fixed)'),
            ('IOMobileFramebufferLegacy::completeCurrentSwap();',
             'native_legacy_parent_complete_current_swap(this);'),
            ('IOMobileFramebufferLegacy::newUserClient(task,security,type,client)',
             'native_legacy_parent_new_user_client(this,task,security,type,client)')):
        if pattern not in text:
            raise SystemExit(f'NativeLegacyDisplay.cpp lost the original call: {pattern}')
        text = text.replace(pattern, replacement)

    anchor = '#define LEAF(n)'
    if text.count(anchor) != 1:
        raise SystemExit('NativeLegacyDisplay.cpp leaf anchor changed')
    forwarders = ('    // pl21: base bodies this backend does not implement dispatch through the adopted live vtable.\n'
                  '    void getDisplayDimensions(uint32_t *width,uint32_t *height) override {native_legacy_parent_display_dimensions(this,width,height);}\n'
                  '    void completeCurrentSwap() override {native_legacy_parent_complete_current_swap(this);}\n'
                  '    int applyStateRequest(OriginalLegacyStateRequest *request,bool powered) override {return native_legacy_parent_apply_state_request(this,request,powered);}\n'
                  '    void create_default_fb_surface() override {native_legacy_parent_create_default_surface(this);}\n')
    text = text.replace(anchor, forwarders + anchor, 1)
    return text


def main():
    device = Path(os.environ['UNIXNU_DEVICE']).resolve()
    tree = Path.cwd().resolve()
    catalog = tree / 'components.toml'
    if not catalog.is_symlink():
        raise SystemExit('Expected the fixed framework catalog link in the resolved tree')
    framework = catalog.resolve().parent
    lock = json.loads((device / 'upstream.lock.json').read_text())
    if revision(framework) != lock['unixnu']['commit']:
        raise SystemExit('Framework revision differs from upstream.lock.json')

    gop_dir = framework / 'drivers/GopFramebuffer'
    expected_gop = subprocess.check_output(
        ['git', '-C', str(framework), 'rev-parse', lock['unixnu']['commit'] + ':drivers/GopFramebuffer'],
        text=True).strip()
    if revision(gop_dir) != expected_gop:
        raise SystemExit('Initialize the pinned drivers/GopFramebuffer submodule before native build')

    records = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    cpp_record_path = records / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    if not cpp_record_path.is_file():
        raise SystemExit(f'Missing compile record: {cpp_record_path}')
    cpp_record = json.loads(cpp_record_path.read_text().strip().rstrip(','))

    adapter = helper('tb710fu_gop_adapter', device / 'scripts/build-tb710fu-ufs-adapter.py')
    target = helper('tb710fu_gop_target', framework / 'scripts/kernel_target.py')
    abi_check = helper('tb710fu_gop_abi', device / 'scripts/check-gop-display-abi.py')

    fixture_path = device / 'scripts' / FIXTURE
    abi = json.loads(fixture_path.read_text())
    if abi['kernelcache_sha256'] != lock['kernelcache_sha256']:
        raise SystemExit('Fixture is not the pinned 26C5035r kernelcache')
    report = abi_check.check_fixture(abi)

    output = tree / 'build/tb710fu-gop-display'
    output.mkdir(parents=True, exist_ok=True)
    archive = output / 'libTb710fuGopDisplay.a'
    archive.unlink(missing_ok=True)

    generate_abi_header(output / 'OriginalLegacyFramebufferABI.h', abi)
    gen_display = output / DISPLAY_SOURCE
    gen_display.write_text(transform_display(gop_dir / DISPLAY_SOURCE))

    includes = [output, gop_dir, tree / '.cache/xnu-work/osfmk', tree / '.cache/xnu-work/libkern']
    objects = []
    for path, extra_flags in ((gen_display, ['-DNATIVE_LEGACY_MACOS=1']),):
        obj = output / (path.stem + '.o')
        command = adapter.compile_command(cpp_record, path, obj, includes, extra_flags)
        command = ['--target=arm64e-apple-ios27.0' if v.startswith('--target=') else v for v in command]
        result = subprocess.run(command, cwd=cpp_record['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (output / (path.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(f'{path.name}:\n{result.stdout[-8000:]}')
        target.check_object(obj.read_bytes(), path.name)
        if path.name == DISPLAY_SOURCE:
            init = '__GLOBAL__sub_I_' + path.name
            late = obj.with_suffix('.late.o')
            defer_constructor(obj, late, init)
            target.check_object(late.read_bytes(), path.name + ' deferred')
            undef = subprocess.check_output(['xcrun', 'nm', '-u', str(late)], text=True)
            bad = [line for line in undef.splitlines()
                   if 'IOMobileFramebuffer' in line or 'IOAV' in line or 'IOAVElement' in line]
            if bad:
                raise SystemExit('Unexpected undefined IOMobileFramebuffer symbols:\n' + '\n'.join(bad))
            obj = late
        objects.append(obj)

    # Audit the compiled vtable slot by slot before the bootstrap is compiled: the
    # owned set and the IOService slot indices come from what clang actually emitted.
    data = objects[0].read_bytes()
    sections, symbols = object_layout(data)
    count = len(abi['slots'])
    entries = object_vtable(data, sections, symbols, '__ZTV19NativeLegacyDisplay', count)
    if any(entry is None for entry in entries):
        raise SystemExit('Owned vtable is not an exact authenticated-pointer array of 483 entries')
    diversities = [int(diversity, 16) for _, diversity in abi['slots']]
    owned, guards, ioservice, failures = [], [], {}, []
    owned_labels = {abi['names'][name][0] if name in abi['names'] else name for name in abi['methods']}
    for index, (symbol, discriminator) in enumerate(entries):
        label = demangled_label(symbol)
        if index < abi['base_slots']:
            if label in IOSERVICE_SLOTS:
                ioservice[label] = index
            continue
        if discriminator != diversities[index] and index not in REBOUND_SLOTS:
            failures.append(f'slot {index}: diversity {discriminator:#x} != {diversities[index]:#x}')
        if label is not None and label in owned_labels:
            owned.append(index)
        else:
            guards.append(index)
    if failures:
        raise SystemExit('\n'.join(failures[:8]))
    expected_owned = set(abi['methods'].values())
    if set(owned) != expected_owned:
        raise SystemExit(f'Owned slots {sorted(owned)} differ from the classified methods {sorted(expected_owned)}')
    if len(set(owned) | set(guards)) != count - abi['base_slots']:
        raise SystemExit('Legacy slot coverage is not exact')
    for name in IOSERVICE_SLOTS:
        if name not in ioservice:
            raise SystemExit(f'IOService::{name} slot was not located in the owned vtable')

    generate_traits(output / 'GopDisplayTraits.h', abi, sorted(owned), ioservice)
    for source in (device / 'package/GopDisplayStart.cpp', device / 'package/GopDisplaySupport.cpp', output / 'GopDisplayTraits.cpp'):
        obj = output / (source.stem + '.o')
        command = adapter.compile_command(cpp_record, source, obj, includes, [])
        command = ['--target=arm64e-apple-ios27.0' if v.startswith('--target=') else v for v in command]
        result = subprocess.run(command, cwd=cpp_record['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (output / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(f'{source.name}:\n{result.stdout[-8000:]}')
        target.check_object(obj.read_bytes(), source.name)
        objects.append(obj)

    # No definition and no reference to any original class symbol may survive.
    undefined = subprocess.check_output(['xcrun', 'nm', '-gU', *map(str, objects)], text=True)
    for original in ('IOMobileFramebufferLegacy', 'IOMobileFramebuffer', 'IOAVElement',
                     'native_legacy_surface_create', 'native_legacy_surface_allocate'):
        if original in undefined:
            raise SystemExit(f'Archive references an original family symbol: {original}')
    defined = subprocess.check_output(['xcrun', 'nm', '-g', '-U', '-n', *map(str, objects)], text=True)
    for symbol in ('__ZTV19NativeLegacyDisplay', '__ZN19NativeLegacyDisplay10gMetaClassE',
                   'native_tb710fu_legacy_parent', 'native_tb710fu_gop_display_start'):
        if defined.count(symbol) != 1:
            raise SystemExit(f'{symbol} is not defined exactly once in the archive')

    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    digest = sha(archive)
    (output / 'build-report.json').write_text(json.dumps({
        'archive': str(archive),
        'archive_sha256': digest,
        'gop_revision': expected_gop,
        'fixture_sha256': report.get('fixture_sha256', sha(fixture_path)),
        'instance_size': report.get('instance_size', abi['instance_size']),
        'slot_count': report.get('slot_count', len(abi['slots'])),
        'diversity_sha256': report.get('diversity', ''),
        'owned_slots': sorted(owned),
        'guard_slots': len(guards),
        'ioservice_slots': ioservice,
        'rebound_slots': list(REBOUND_SLOTS),
        'surface_accessors': report.get('surface_accessors', {}),
        'objects_sha256': {o.name: sha(o) for o in objects},
    }, indent=2) + '\n')

    link_target = records / 'mach.development.bcm2837.unstripped.noctf'
    stamp = records / '.tb710fu-gop-display-link-input.json'
    stamp_text = json.dumps({'archive_sha256': digest}, indent=2) + '\n'
    prev = stamp.read_text() if stamp.is_file() else ''
    if prev != stamp_text:
        link_target.unlink(missing_ok=True)
        with tempfile.NamedTemporaryFile(mode='w', dir=records, prefix=stamp.name + '.', delete=False) as tf:
            tmp = Path(tf.name)
            tf.write(stamp_text)
        os.replace(tmp, stamp)
        print(f'TB710FU GopFramebuffer archive changed: invalidated {link_target}')
    print(f'TB710FU GopFramebuffer archive passed: {archive} ({digest[:12]})')


if __name__ == '__main__':
    main()
