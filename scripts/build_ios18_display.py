#!/usr/bin/env python3
"""Generate the reviewed 22H384 framebuffer ABI and validate compiled PAC slots."""
import json
from pathlib import Path
import struct


def generate(output):
    abi_path = Path(__file__).resolve().parents[1] / 'profiles/legacy-display-22H384.json'
    abi = json.loads(abi_path.read_text())
    slots = [(int(t, 16), int(d, 16)) for t, d in abi['slots']]
    D, R = abi['display'], abi['request']
    ABI_PATH, OUT = abi_path, output
    def require(value, message):
        if not value:
            raise RuntimeError(message)
    PROTOTYPES = {
        'isMainDisplay': 'bool isMainDisplay()', 'getDotPitch': 'IOReturn getDotPitch(uint32_t *)',
        'supportedFrameInfo': 'IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,uint32_t *)=0',
        'getDisplayDimensions': 'void getDisplayDimensions(uint32_t *,uint32_t *)', 'startHardware': 'bool startHardware()',
        'kickSwapQueue': 'void kickSwapQueue()', 'completeCurrentSwap': 'void completeCurrentSwap()',
        'validateSwap': 'bool validateSwap(const OriginalLegacySwapRequest *)',
        'getRefreshAttributes': 'bool getRefreshAttributes(uint64_t *,bool)',
        'collectCompletionReports': 'IOReturn collectCompletionReports(uint32_t,void *,uint32_t *,uint32_t)=0',
        'applyStateRequest': 'int applyStateRequest(OriginalLegacyStateRequest *,bool)',
        # The family invokes timeout cleanup through slot 355.
        'cancelSwapsAfterTimeout': 'bool cancelSwapsAfterTimeout()',
        # Slot 307 initializes optional family state and returns void.
        'create_default_fb_surface': 'void create_default_fb_surface()'}
    by_slot = {slot: name for name, slot in abi['methods'].items()}
    pure = {212,214,247,248,314,315,347,348,355}
    renames = []
    for base, (name, suffix) in abi['names'].items():
        mangled = f'_ZN25IOMobileFramebufferLegacy{len(name)}{name}{suffix}'
        slot = abi['methods'].get(base) or int(base[len('opaque'):])
        renames.append(f'#define {base} {name}\n')
    leaves = ' '.join(f'LEAF({i})' for i in abi['leaves'])
    header = f'''// SPDX-License-Identifier: BSD-2-Clause-Patent
    // Opaque ABI storage for the original iOS 18 (22H384) class, generated from {ABI_PATH.name}. Each original slot is declared
    // under a name whose pointer-authentication discriminator is the original slot's diversity (the names below), so original
    // callers authenticate the owned subclass's entries. Only qualified base prototypes may be called from source.
    #pragma once
    #include <IOKit/IOService.h>
    #include <libkern/libkern.h>
    struct OriginalLegacySwapRequest;
    struct OriginalLegacyStateRequest {{uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];}};
    static constexpr unsigned kLegacyVsyncClients={D['vsync_clients']};
    static constexpr unsigned kRequestRect0={R['rect0']},kRequestRect1={R['rect1']},kRequestMasks={R['masks']},kRequestOptions={R['options']},kRequestDims={R['dims']},kRequestSurfaces={R['surfaces']};
    #define NATIVE_LEGACY_LEAVES(LEAF) {leaves}
    
    #define NATIVE_LEGACY_CANCEL_SWAPS 1
    
    {''.join(renames)}class IOMobileFramebufferLegacy : public IOService {{
     OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
     unsigned char originalPrivate[{abi['instance_size']}-sizeof(IOService)];
    public:
     void installBootDimensions(uint32_t width,uint32_t height) {{
      memcpy(reinterpret_cast<unsigned char *>(this)+{D['width']},&width,4);memcpy(reinterpret_cast<unsigned char *>(this)+{D['height']},&height,4);
     }}
     const OriginalLegacySwapRequest *pendingSwapRequest() const {{
      const OriginalLegacySwapRequest *request=nullptr;
      memcpy(&request,reinterpret_cast<const unsigned char *>(this)+{D['head']},sizeof(request));return request;
     }}
     // Tail queue: head {D['head']}, tail pointer {D['tail']}, each request's next {R['next']} and previous-link {R['prev']}; the head moves
     // to the current-swap slot {D['current']} for the original completion body.
     bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {{
      auto self=reinterpret_cast<unsigned char *>(this);
      auto bytes=reinterpret_cast<const unsigned char *>(request);
      const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
      memcpy(&current,self+{D['current']},8);memcpy(&head,self+{D['head']},8);memcpy(&tail,self+{D['tail']},8);
      if(!request || current || head!=request)return false;
      memcpy(&next,bytes+{R['next']},8);memcpy(&previous,bytes+{R['prev']},8);
      if(previous!=self+{D['head']})return false;
      if(next) {{
       const void *link=self+{D['head']};
       memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+{R['prev']},&link,8);
      }} else if(tail!=bytes+{R['next']})return false;
      memcpy(self+{D['current']},&request,8);
      if(next) {{memcpy(self+{D['head']},&next,8);}}
      else {{const void *empty=nullptr,*emptyTail=self+{D['head']};memcpy(self+{D['head']},&empty,8);memcpy(self+{D['tail']},&emptyTail,8);}}
      return true;
     }}
     void free() override;
     bool init(OSDictionary *properties=nullptr) override;
     IOReturn setProperties(OSObject *) override;
     void systemWillShutdown(IOOptionBits) override;
     IOService *probe(IOService *,SInt32 *) override;
     bool start(IOService *) override;
     void stop(IOService *) override;
     IOWorkLoop *getWorkLoop() const override;
     IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
     IOReturn setPowerState(unsigned long,IOService *) override;
    '''
    for i in range(168, len(slots)):
        if i in by_slot:
            header += f' virtual {PROTOTYPES[by_slot[i]]};\n'
        elif i in pure:
            header += f' virtual __attribute__((noreturn)) void opaque{i}()=0;\n'
        else:
            header += f' virtual __attribute__((deprecated("Unclassified ABI: never call from source"))) void opaque{i}();\n'
    header += f'}};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)=={abi["instance_size"]},"Actual original instance extent");\n'
    (OUT / 'OriginalLegacyFramebufferABI.h').write_text(header)


def bindings(obj, output):
    abi = json.loads((Path(__file__).resolve().parents[1] / 'profiles/legacy-display-22H384.json').read_text())
    slots = [(int(t, 16), int(d, 16)) for t, d in abi['slots']]
    def require(value, message):
        if not value:
            raise RuntimeError(message)
    def macho(data):
        """Load commands, segments and sections of an arm64e relocatable object."""
        magic, cpu, _, _, count = struct.unpack_from('<5I', data)
        require(magic == 0xfeedfacf and cpu == 0x100000c, 'arm64 Mach-O object')
        commands, segments, offset = [], [], 32
        for _ in range(count):
            command, size = struct.unpack_from('<2I', data, offset)
            commands.append((command, offset, size))
            if command == 0x19:
                nsects = struct.unpack_from('<I', data, offset + 64)[0]
                sections = []
                for i in range(nsects):
                    at = offset + 72 + 80 * i
                    va, length, fileoff = struct.unpack_from('<QQI', data, at + 32)
                    sections.append({'name': data[at:at + 16].rstrip(b'\0').decode(), 'va': va, 'size': length,
                                     'fileoff': fileoff, 'command_offset': at})
                segments.append({'sections': sections})
            offset += size
        return {'commands': commands, 'segments': segments}
    
    
    raw = obj.read_bytes()
    image = macho(raw)
    st = next(p for c, p, _ in image['commands'] if c == 2)
    sy, count, strings, _ = struct.unpack_from('<4I', raw, st + 8)
    symbols = []
    for i in range(count):
        ix, kind, section, _, va = struct.unpack_from('<IBBHQ', raw, sy + i * 16)
        symbols.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), va, kind))
    vtable = next(va for name, va, _ in symbols if name == '__ZTV19NativeLegacyDisplay')
    entries = {}
    for segment in image['segments']:
        for sec in segment['sections']:
            if sec['va'] <= vtable < sec['va'] + sec['size']:
                ro, rc = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
                for i in range(rc):
                    offset, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                    # extern, 8 bytes, ARM64_RELOC_AUTHENTICATED_POINTER
                    if bits >> 27 & 1 and bits >> 25 & 3 == 3 and bits >> 28 == 11:
                        word = struct.unpack_from('<Q', raw, sec['fileoff'] + offset)[0]
                        entries[sec['va'] + offset] = (symbols[bits & 0xffffff][0], word)
    compiled = [entries.get(vtable + 16 + i * 8) for i in range(len(slots))]
    require(all(compiled) and vtable + 16 + len(slots) * 8 not in entries, f'Exact owned {len(slots)}-entry vtable')
    for i, (name, word) in enumerate(compiled):
        require(word >> 63 and word >> 48 & 1 and word >> 49 & 3 == 0 and (word >> 32) & 0xffff == slots[i][1],
                f'Slot {i} ({name}) signed with IA, address diversity and the original diversity {slots[i][1]:#x}')
    result = dict(abi['bindings'])
    for i, (name, word) in enumerate(compiled):
        if '25IOMobileFramebufferLegacy' in name:
            result[name] = slots[i][0]
    undefined = {name for name, _, kind in symbols if kind == 1 and '25IOMobileFramebufferLegacy' in name}
    require(undefined <= set(result) | {'__ZN25IOMobileFramebufferLegacy10gMetaClassE', '__ZTV25IOMobileFramebufferLegacy'},
            'Unbound original display method: ' + str(undefined - set(result)))
    (output / 'display-abi.json').write_text(json.dumps({'slots': len(compiled), 'bindings': result}, indent=2) + '\n')
    return result
