#!/usr/bin/env python3
"""Read-only installed ABI and compiled PE verification; never runs Windows code."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import pefile
from build_contract import validate_products
ROOT=Path(__file__).resolve().parents[1]
EXPECTED_EXPORTS={'Direct3DCreate9','Direct3DCreate9Ex','D3DPERF_BeginEvent','D3DPERF_EndEvent',
                  'D3DPERF_GetStatus','D3DPERF_QueryRepeatFrame','D3DPERF_SetMarker','D3DPERF_SetOptions','D3DPERF_SetRegion'}

def verify(game):
    report={'runtime_executed':False,'artifacts':{},'native_hooks':0}
    for relative, machine in [('build-game/d3d9.dll',0x14c),('build-host/ss2vr_host.exe',0x8664),
                              ('build-host/openxr/src/loader/libopenxr_loader.dll',0x8664),('build-game/SS2VRServer.dll',0x14c)]:
        path=ROOT/relative;pe=pefile.PE(str(path),max_symbol_exports=65536)
        if pe.FILE_HEADER.Machine!=machine:raise ValueError('Wrong architecture: '+relative)
        imports=sorted(x.dll.decode() for x in pe.DIRECTORY_ENTRY_IMPORT)
        if any(name.lower().startswith('lib') for name in imports):raise ValueError('Unpackaged MinGW runtime dependency: '+relative)
        exports={e.name.decode() for e in getattr(pe,'DIRECTORY_ENTRY_EXPORT',()).symbols if e.name} if hasattr(pe,'DIRECTORY_ENTRY_EXPORT') else set()
        if relative=='build-game/d3d9.dll' and not EXPECTED_EXPORTS<=exports:raise ValueError('Missing proxy exports')
        if relative=='build-game/SS2VRServer.dll' and not {'SS2VRServer_Startup_t','SS2VRServer_Cleanup'}<=exports:raise ValueError('Missing native server module entrypoints')
        if relative.endswith('libopenxr_loader.dll') and 'xrGetInstanceProcAddr' not in exports:raise ValueError('Missing loader entrypoint')
        report['artifacts'][path.name]={'machine':hex(machine),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'imports':imports}
    report['build_contract']=validate_products(ROOT, {'game':ROOT/'build-game/d3d9.dll',
        'server':ROOT/'build-game/SS2VRServer.dll','host':ROOT/'build-host/ss2vr_host.exe'})
    source=(ROOT/'src/game/engine.cpp').read_text()
    mapping={'g':'Sam2Game.dll','e':'Engine.dll','c':'Core.dll'}
    fingerprints=json.loads((ROOT/'docs/installed-build.json').read_text())
    symbols={}
    for name,data in fingerprints.items():
        path=game/'Bin'/name
        if hashlib.sha256(path.read_bytes()).hexdigest()!=data['sha256']:raise ValueError('Fingerprint changed: '+name)
        p=pefile.PE(str(path),max_symbol_exports=65536)
        symbols[name]={e.name.decode() for e in getattr(p,'DIRECTORY_ENTRY_EXPORT',()).symbols if e.name} if hasattr(p,'DIRECTORY_ENTRY_EXPORT') else set()
    for kind,module,literals in re.findall(r'\b([SH])\(\s*([gec]),\s*((?:"[^"]*"\s*)+),',source):
        name=''.join(re.findall(r'"([^"]*)"',literals))
        if name not in symbols[mapping[module]]:raise ValueError('Missing required native symbol: '+name)
        report['native_hooks']+=kind=='H'
    modules={'engine':'Engine.dll','core':'Core.dll','sam':'Sam2Game.dll'}
    for adapter in ['multiplayer.cpp','remote_render.cpp']:
        adapter_source=(ROOT/'src/game'/adapter).read_text()
        for module,literals in re.findall(r'\bS\(\s*(engine|core|sam),\s*((?:"[^"]*"\s*)+),',adapter_source):
            name=''.join(re.findall(r'"([^"]*)"',literals))
            if name not in symbols[modules[module]]:raise ValueError('Missing '+adapter+' symbol: '+name)
    multiplayer=(ROOT/'src/game/multiplayer.cpp').read_text()
    for literals in re.findall(r'\bH\(\s*((?:"[^"]*"\s*)+),',multiplayer):
        name=''.join(re.findall(r'"([^"]*)"',literals))
        if name not in symbols['Engine.dll']:raise ValueError('Missing multiplayer hook: '+name)
        report['native_hooks']+=1
    dedicated=game/'Bin/DedicatedServer.exe'
    expected_server='bfa9d628483bb6e9cb39338e075848a6e02328ccb612f701d87dd909d4f87b57'
    if hashlib.sha256(dedicated.read_bytes()).hexdigest()!=expected_server:raise ValueError('Dedicated server fingerprint changed')
    report['dedicated_server']={'sha256':expected_server,'graphics_module_required':False}
    # Full module hashes gate these internal seams; check the audited entry
    # prologue too, without treating an export count as internal-hook coverage.
    internals=[('Sam2Game.dll',0x192550,bytes.fromhex('558bec83ec0c'),'native menu input poll'),
               ('Engine.dll',0x1b3b60,bytes.fromhex('558bec6aff'),'native entity simulation interval'),
               ('Engine.dll',0xdbc90,bytes.fromhex('558bec81ec10020000'),'native model-record pass'),
               ('Engine.dll',0xdde30,bytes.fromhex('558bec51a178ac2e10'),'native mapped-palette observer / optional head adapter'),
               ('GfxD3D.dll',0x56a0,bytes.fromhex('558bec83ec28'),'native graphics depth range')]
    report['internal_hooks']=[]
    for name,rva,entry,label in internals:
        native=pefile.PE(str(game/'Bin'/name),max_symbol_exports=65536)
        if native.get_data(rva,len(entry))!=entry:raise ValueError('Internal seam changed: '+label)
        report['internal_hooks'].append({'module':name,'rva':hex(rva),'role':label})
    boundaries=json.loads((ROOT/'docs/sniper-predicate-boundaries.json').read_text())
    selected={'activate','deactivate_owner','deactivate_fov','interpolate','sound_start','sound_stop'}
    sniper=pefile.PE(str(game/'Bin/Sam2Game.dll'),max_symbol_exports=65536)
    for candidate in boundaries['candidates']:
        if candidate['role'] not in selected: continue
        rva=int(candidate['site_rva'],0)
        if not re.search(r'internalHook\(g,\s*0x'+format(rva,'x')+r'\s*,',source):
            raise ValueError('Missing installed zoom predicate declaration: '+candidate['role'])
        window=sniper.get_data(rva,candidate['stolen_bytes'])
        if hashlib.sha256(window).hexdigest()!=candidate['stolen_sha256']:
            raise ValueError('Native zoom predicate window changed: '+candidate['role'])
        report['internal_hooks'].append({'module':'Sam2Game.dll','rva':hex(rva),
                                        'role':'native zoom '+candidate['role']})
    primary=json.loads((ROOT/'docs/primary-load-boundaries.json').read_text())
    if hashlib.sha256((game/'Bin'/primary['module']).read_bytes()).hexdigest()!=primary['module_sha256']:
        raise ValueError('Primary consumer fingerprint differs')
    for boundary in primary['boundaries']:
        rva=int(boundary['site_rva'],0)
        expected_window=bytes.fromhex(boundary['stolen_hex'])
        if not re.search(r'internalHook\(g,\s*0x'+format(rva,'x')+r'\s*,',source):
            raise ValueError('Missing primary read boundary: '+boundary['role'])
        if sniper.get_data(rva,len(expected_window))!=expected_window:
            raise ValueError('Primary read window differs: '+boundary['role'])
        report['internal_hooks'].append({'module':primary['module'],'rva':hex(rva),
                                        'role':'native primary '+boundary['role']})
    for consumer in primary['ordinary_consumers']:
        entry=bytes.fromhex(consumer['entry_hex'])
        if sniper.get_data(int(consumer['entry_rva'],0),len(entry))!=entry:
            raise ValueError('Ordinary primary consumer differs: '+consumer['role'])
    plugin=(ROOT/'src/game/server_plugin.cpp').read_text()
    for name in re.findall(r'"(\?[^"\n]+)"',plugin):
        if name not in symbols['Core.dll']:raise ValueError('Missing server startup API: '+name)
    layouts=[]
    expected=(0x32565253,13,272,480,352,576,2000,48,672,1824)
    for prefix,folder in [('i686','build-game'),('x86_64','build-host')]:
        # Concurrent normal/optimized checks must not truncate each other's
        # compiler output between compile and objcopy. Only owned scratch files
        # are retired; existing build products and historical probes stay intact.
        with tempfile.TemporaryDirectory(prefix='ss2vr-ipc-layout-',dir=ROOT/folder) as scratch:
            object_path=Path(scratch)/'ipc-layout.o';binary=Path(scratch)/'ipc-layout.bin'
            subprocess.run([prefix+'-w64-mingw32-g++','-std=c++20','-I'+str(ROOT/'src'),'-c',str(ROOT/'tests/abi_layout.cpp'),'-o',str(object_path)],check=True)
            subprocess.run([prefix+'-w64-mingw32-objcopy','-O','binary','-j','.ss2abi',str(object_path),str(binary)],check=True)
            layout=struct.unpack('<10I',binary.read_bytes()[:40])
        if layout!=expected:raise ValueError('IPC ABI differs from frozen layout: '+prefix)
        layouts.append(layout)
    if layouts[0]!=layouts[1]:raise ValueError('IPC architecture mismatch')
    report['ipc_layout']={'abi':layouts[0][1],'mapping_bytes':expected[6],'pixel_sections':'per canvas extent','architectures_agree':True}
    print(json.dumps(report,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--game',type=Path,required=True)
    verify(p.parse_args().game.resolve())
