"""Compiler cache identity with verified compatibility across diagnostic relinks."""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

LOSS_NAME = 'tu_darwin_record_device_loss'
DIAGNOSTIC_FILES = {'src/vulkan/runtime/vk_device.c', 'src/vulkan/runtime/vk_queue.c',
                    'src/freedreno/vulkan/tu_knl_darwin.cc'}
PROFILE_FIELDS = ('mesa_revision', 'target', 'compiler', 'options')


def canonical_source(name, data):
    if name not in DIAGNOSTIC_FILES:
        return data
    text = data.decode('utf-8')
    if name.endswith('.c'):
        def keep(block):
            return '' if LOSS_NAME in block.group(1) else block.group(0)
        text = re.sub(r'\n#ifdef __APPLE__\n(.*?)\n#endif', keep, text, flags=re.S)
    else:
        declaration = 'extern "C" __attribute__((visibility("default"))) void ' + LOSS_NAME + '(const char *, int, const char *);\n'
        text = text.replace(declaration, '')
        signature = 'extern "C" void\n' + LOSS_NAME + '('
        if signature in text:
            start = text.index(signature)
            end = text.index('{', start) + 1
            depth = 1
            while depth and end < len(text):
                depth += (text[end] == '{') - (text[end] == '}')
                end += 1
            if depth:
                raise ValueError('Unterminated loss diagnostic function')
            if text[end:end+1] == '\n':
                end += 1
            text = text[:start] + text[end:]
    if name == 'src/vulkan/runtime/vk_queue.c':
        text = text.replace('#include "vk_device.h"\n\n#include "vk_fence.h"',
                            '#include "vk_device.h"\n#include "vk_fence.h"')
    return re.sub(r"\n{3,}", "\n\n", text).encode()


def digest(value):
    return hashlib.sha256(value).hexdigest()


def compiler_source(name, data):
    data = canonical_source(name, data)
    if name == 'src/util/os_misc.c':
        text, count = re.subn(r'\nbool\nos_get_total_physical_memory\([^\n]*\)\n\{.*?\n\}',
                             '\n/* host physical memory query */', data.decode(), count=1, flags=re.S)
        if count != 1:
            raise ValueError('Missing host physical memory query')
        return text.encode()
    if name != 'src/freedreno/vulkan/tu_knl_darwin.cc':
        return data
    text = data.decode()
    # These host ownership routines do not generate shaders or define cached
    # pipeline layouts. Keep capability, synchronization and compiler changes
    # in the fingerprint; exclude only BO retirement and submission assembly.
    for name in ('darwin_bo_finish', 'darwin_submit_create',
                 'darwin_submit_finish', 'darwin_submit_add_entries'):
        pattern = r'\nstatic [^\n]+\n' + name + r'\([^\n]*(?:\n[^\{]*)?\n\{.*?\n\}'
        text, count = re.subn(pattern, '\n/* host BO ownership: ' + name + ' */', text, count=1, flags=re.S)
        if count != 1:
            raise ValueError('Missing backend ownership routine: ' + name)
    text, count = re.subn(r'\nstruct tu_darwin_queue_submit \{.*?\n\};',
                         '\n/* host submission ownership */', text, count=1, flags=re.S)
    if count != 1:
        raise ValueError('Missing backend submission ownership structure')
    return text.encode()


def source_fingerprint(source, profile, compiler_only=False):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(source), *args])
    names = set(git('diff', '--name-only', '-z', 'HEAD').split(b'\0'))
    names.update(git('ls-files', '--others', '--exclude-standard', '-z').split(b'\0'))
    entries = {}
    for raw in sorted(names):
        if not raw:
            continue
        name = raw.decode();path = Path(source)/name
        if not path.is_file():
            entries[name] = {'deleted': True}
            continue
        normalize = compiler_source if compiler_only else canonical_source
        data = normalize(name, path.read_bytes())
        if name in DIAGNOSTIC_FILES or (compiler_only and name == 'src/util/os_misc.c'):
            baseline = subprocess.run(['git','-C',str(source),'show','HEAD:'+name],capture_output=True)
            if baseline.returncode == 0 and data == normalize(name, baseline.stdout):
                continue
        entries[name] = {'sha256': digest(data), 'executable': bool(path.stat().st_mode & 0o111)}
    value = {key: profile[key] for key in PROFILE_FIELDS}
    value['sources'] = entries
    return digest(json.dumps(value,sort_keys=True,separators=(',',':')).encode())


def image_uuid(binary):
    if not binary.is_file():
        return ''
    with binary.open('rb') as f:
        header=f.read(32)
        if len(header)!=32 or struct.unpack_from('<I',header)[0]!=0xfeedfacf:
            return ''
        ncmds,size=struct.unpack_from('<II',header,16);commands=f.read(size)
    at=0
    for _ in range(ncmds):
        command,length=struct.unpack_from('<II',commands,at)
        if length<8 or at+length>len(commands):raise ValueError('Invalid Mach-O commands')
        if command==0x1b:return commands[at+8:at+24].hex()
        at+=length
    return ''


def legacy_id(profile):
    value={key:profile[key] for key in ('mesa_revision','source_diff_sha256','extra_sources')}
    return digest(json.dumps(value,sort_keys=True).encode())[:40]


def select_identity(fingerprint, profile, previous, state, binary_bytes, uuid,
                    compiler_fingerprint=None):
    legacy=[];proof='new compiler inputs';cache_id=fingerprint[:40]
    if (state and state.get('fingerprint')==fingerprint and
        state.get('binary_sha256')==digest(binary_bytes) and
        re.fullmatch('[0-9a-f]{40}',state.get('cache_id',''))):
        cache_id=state['cache_id'];legacy=list(state.get('legacy_image_uuids',[]));proof='verified compiler fingerprint and binary'
    elif (compiler_fingerprint and state and
          state.get('compiler_fingerprint') == compiler_fingerprint and
          state.get('binary_sha256') == digest(binary_bytes) and
          re.fullmatch('[0-9a-f]{40}', state.get('cache_id', ''))):
        cache_id=state['cache_id'];legacy=list(state.get('legacy_image_uuids', []))
        proof='verified unchanged compiler inputs across host BO ownership repair'
    elif previous and all(previous.get(k)==profile.get(k) for k in (*PROFILE_FIELDS,'source_diff_sha256','extra_sources')):
        old=legacy_id(previous)
        if old.encode()+b'\0' in binary_bytes:
            cache_id=old;proof='verified legacy source profile and embedded compiler ID'
    if proof!='new compiler inputs' and uuid:
        legacy.append(uuid)
    return {'version':1,'fingerprint':fingerprint,'compiler_fingerprint':compiler_fingerprint,'cache_id':cache_id,
            'legacy_image_uuids':list(dict.fromkeys(legacy))[-32:],'proof':proof}


def prepare_identity(source, profile, output):
    binary=output/'src/freedreno/vulkan/libvulkan_freedreno.dylib'
    previous_file=output/'builtin-build-profile.json';state_file=output/'compiler-cache-identity.json'
    previous=json.loads(previous_file.read_text()).get('profile') if previous_file.exists() else None
    state=json.loads(state_file.read_text()) if state_file.exists() else None
    data=binary.read_bytes() if binary.exists() else b''
    return select_identity(source_fingerprint(source,profile),profile,previous,state,data,image_uuid(binary),
                           source_fingerprint(source,profile,compiler_only=True))


def finish_identity(identity, output, binary):
    value=dict(identity);uuids=list(value['legacy_image_uuids']);uuid=image_uuid(binary)
    if uuid:uuids.append(uuid)
    value.update(binary_sha256=digest(binary.read_bytes()),legacy_image_uuids=list(dict.fromkeys(uuids))[-32:])
    (output/'compiler-cache-identity.json').write_text(json.dumps(value,indent=2)+'\n')
