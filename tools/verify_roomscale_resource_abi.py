#!/usr/bin/env python3
"""Static owned-Engine and compile-only optional-query cancellation checks.

This does not activate the gates, execute native code, certify complete callback
closure, or establish a body-movement/collision result.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import capstone
import pefile

ROOT=Path(__file__).resolve().parents[1]
ENGINE_SHA='da6efc9f72637eb3b6f48eadca2107be89b09c00618b6e72d5d3632938a7d851'
RESOURCE_SITES={
    'resourceWorld':(0x29114,0x29146,24),
    'resourceModel':(0xda419,0xda439,24),
    'resourceConfiguration':(0xe17a1,0xe17c2,0),
    'resourceSkeleton':(0xe1806,0xe1827,0),
    'resourceMesh':(0xe1a47,0xe1a68,4),
    'resourceOverride':(0xe1d5e,0xe1d7f,0),
    'resourceChildOne':(0xe1f56,0xe1f77,4),
    'resourceChildTwo':(0xe1ff9,0xe201a,4),
    'resourceMaterialInherited':(0xe6932,0xe6972,4),
    'resourceMaterialDirect':(0xe6951,0xe6972,4),
    'resourcePrimitiveMaterial':(0x528e4,0x52905,4),
    'resourceCollisionConfigOne':(0xcb407,0xcb428,0),
    'resourceCollisionConfigTwo':(0xcb438,0xcb459,0),
    'resourceCollisionConfigThree':(0xcb46c,0xcb48d,0),
    'resourceCollisionMeshOne':(0xcb49b,0xcb4bb,24),
    'resourceCollisionConfigFour':(0xcb4c8,0xcb4e9,0),
    'resourceCollisionMeshTwo':(0xcb4f4,0xcb515,0),
    'resourceCollisionVertexResource':(0xcba64,0xcba84,24),
    'resourceCollisionMaterial':(0xcbef3,0xcbf14,0),

}
RETURN_SITES={
    'resourceMaterialReturn':0xe1cc9,
    'resourceChildOneReturn':0xe1f9e,
    'resourceChildTwoReturn':0xe203e,
    'resourcePrepareReturn':0xe2472,
    'resourceQueryReturn':0xda4e1,
}


def require(condition,message):
    if not condition: raise ValueError(message)

def check_patch_incoming_edges(instructions,base,patches):
    """Direct edges within a pinned decoded function, not cross-module coverage."""
    for ins in instructions:
        jump=ins.group(capstone.CS_GRP_JUMP)
        if not jump and not ins.group(capstone.CS_GRP_CALL):continue
        if not ins.operands or ins.operands[0].type!=capstone.x86.X86_OP_IMM:
            require(not jump,'Unclassified indirect jump in model preparation')
            continue # Existing virtual calls are not direct branch receipts.
        target=ins.operands[0].imm-base
        for name,start in patches:
            require(not start<target<start+5,
                    'Native edge enters patched jump interior: '+name+' at '+hex(ins.address-base))


def functions(assembly):
    parts=re.split(r'^[0-9a-f]+ <([^>]+)>:\n',assembly,flags=re.M)
    return dict(zip(parts[1::2],parts[2::2]))


def code(body):
    out=[]
    for line in body.splitlines():
        cols=line.split('\t')
        if len(cols)>=3 and re.fullmatch(r'\s*[0-9a-f]+:',cols[0]):
            out.append(' '.join(cols[-1].split()))
    return out


def verify(game):
    native=(game/'Bin/Engine.dll').read_bytes()
    require(hashlib.sha256(native).hexdigest()==ENGINE_SHA,'Engine fingerprint differs')
    pe=pefile.PE(data=native);base=pe.OPTIONAL_HEADER.ImageBase
    require(base==0x10000000 and pe.FILE_HEADER.Machine==0x14c,'Unexpected pinned image ABI')
    md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32);md.detail=True
    source=(ROOT/'src/game/roomscale_resource_gate.cpp').read_text()
    pattern=r'BIND\((\w+),(0x[0-9a-f]+),(0x[0-9a-f]+|0),BYTES\(([^)]+)\),(\d+),'
    bindings=re.findall(pattern,source)
    require(len(bindings)==26,'Binding inventory changed')
    found=set()
    for name,rva,clear,raw,size in bindings:
        address=int(rva,16);expected=bytes(int(x,16) for x in raw.split(','))
        require(len(expected)==int(size) and pe.get_data(address,len(expected))==expected,'Native prefix differs: '+name)
        found.add(name)
        if name=='resourceCollisionBuffer':
            require(address==0xcba9f and clear=='0','Vertex-buffer admission mapping differs')
            require('RESOURCE_GATE(resourceCollisionBuffer,12,3)' in source,'Native argument capture differs')
            require([(i.mnemonic,i.op_str) for i in md.disasm(expected,base+address)]==
                    [('call','0x1008f8e0')],'Vertex-buffer call target differs')
        elif name=='resourceHullDispatch':
            require(address==0x2f9d1 and clear=='0','Hull dispatch mapping differs')
            require('RESOURCE_GATE(resourceHullDispatch,20,2)' in source,'Hull table register differs')
            require(pe.get_data(0x2f9c6,11)==bytes.fromhex('8b168bce c7466c01000000'),
                    'Hull table and visited registration changed')
            require(pe.get_data(0x2f9d6,2)==bytes.fromhex('7406'),'Native no-hit continuation changed')
        elif name in RESOURCE_SITES:
            start,target,slot=RESOURCE_SITES[name]
            require((address,int(clear,16))==(start,target),'Resource branch mapping differs')
            ins=list(md.disasm(expected,base+address))
            require(len(ins)==2 and ins[0].mnemonic=='test' and ins[1].mnemonic=='je','Not the exact resource branch')
            require(ins[0].operands[0].mem.disp==4 and ins[0].operands[1].imm==1 and
                    ins[1].operands[0].imm==base+target,'Unexpected resource flag/destination')
            reg={0:'edi',4:'esi',24:'ecx'}[slot]
            require(md.reg_name(ins[0].operands[0].mem.base)==reg,'Wrong captured resource register')
            require(f'RESOURCE_GATE({name},{slot},0)' in source,'Entry receiver differs')
        else:
            require(address==RETURN_SITES[name] and clear=='0','Post-call mapping differs')
            require(f'RESOURCE_GATE({name},0,1)' in source,'Post-call kind differs')
    require(found==set(RESOURCE_SITES)|set(RETURN_SITES)|{'resourceHullDispatch','resourceCollisionBuffer'},'Missing gate')
    # MinHook overwrites five bytes. A native branch into the second instruction
    # of a six-byte displaced prefix bypasses the detour and executes displacement
    # bytes. Check the complete pinned E1720 preparation body, including BOTH
    # no-child paths. The common joins retain the native argument cleanup.
    preparation_start=0xe1720;preparation_end=0xe2056
    preparation_bytes=pe.get_data(preparation_start,preparation_end-preparation_start)
    preparation=list(md.disasm(preparation_bytes,base+preparation_start))
    require(preparation and preparation[-1].address+preparation[-1].size==base+preparation_end and
            preparation[-1].mnemonic=='ret','Incomplete model preparation decode')
    preparation_patches=[(name,int(rva,16)) for name,rva,*_ in bindings
                         if preparation_start<=int(rva,16)<preparation_end]
    require(len(preparation_patches)==9,'Model preparation gate inventory differs')
    check_patch_incoming_edges(preparation,base,preparation_patches)
    for name,old,branch in [('resourceChildOneReturn',0xe1f9b,0xe1f44),
                            ('resourceChildTwoReturn',0xe203b,0xe1fe7)]:
        old_patches=[(n,old if n==name else address) for n,address in preparation_patches]
        try:check_patch_incoming_edges(preparation,base,old_patches)
        except ValueError:pass
        else:raise ValueError('Former interior-branch hook was accepted: '+name)
        # A future verifier regression must reject an actual encoded branch
        # shifted one byte into the new detour, even though other edges are valid.
        instruction=next(i for i in preparation if i.address==base+branch)
        require(instruction.imm_size==1,'Negative-control branch encoding differs')
        changed=bytearray(preparation_bytes)
        offset=branch-preparation_start+instruction.imm_offset
        changed[offset]+=1
        mutated=list(md.disasm(bytes(changed),base+preparation_start))
        try:check_patch_incoming_edges(mutated,base,preparation_patches)
        except ValueError:pass
        else:raise ValueError('Encoded interior-branch mutation was accepted: '+name)
    for table,target in [(0x209268,0x525b0),(0x2094b8,0x51430),
                         (0x209308,0x4fa20),(0x2093b8,0x111f80)]:
        require(struct.unpack('<I',pe.get_data(table+0x40,4))[0]==base+target,
                'Admitted hull dispatch target changed')
    # The admitted system-memory lock/unlock route contains no provider call.
    lock_instructions={i.address-base:(i.mnemonic,i.op_str)
                       for start,length in [(0x8f8e0,0x12e),(0x8fa20,0x64),(0xcba84,0x30)]
                       for i in md.disasm(pe.get_data(start,length),base+start)}
    for rva,instruction in {
        0x8f8f4:('cmp','eax, dword ptr [0x102e68e8]'),
        0x8f900:('mov','ecx, dword ptr [0x102e68e4]'),
        0x8f90a:('lea','esi, [ecx + eax*4 - 0xc]'),
        0x8f923:('movzx','edx, byte ptr [esi + 0xa]'),
        0x8f929:('and','ecx, 0xffffff3f'),
        0x8f94d:('cmp','word ptr [esi + 8], bx'),
        0x8f96e:('cmp','dword ptr [ebp + 8], 0x1f'),
        0x8f972:('jne','0x1008f9f3'),
        0x8f992:('call','dword ptr [0x102e6324]'),
        0x8f9f3:('mov','eax, dword ptr [esi]'),
        0x8f9f5:('mov','edx, dword ptr [ebp + 0x14]'),
        0x8f9f9:('add','eax, edx'),
        0x8f9fb:('inc','word ptr [esi + 8]'),
        0x8fa52:('and','dl, 0x3f'),
        0x8fa55:('cmp','dl, 0x1f'),
        0x8fa58:('jne','0x1008fa72'),
        0x8fa69:('call','dword ptr [0x102e6328]'),
        0x8fa72:('dec','word ptr [esi + 8]'),
        0xcba99:('push','0x93'),0xcba9e:('push','edi'),
    }.items():require(lock_instructions.get(rva)==instruction,'Vertex-buffer route changed: '+hex(rva))
    # Native save/cleanup contracts; no recovery from arbitrary allocator faults.
    suffixes={
        0x52918:'5f5e33c05b8be55dc3',
        0xe1720:'558bec83ec4c5356',
        0xe18a1:'5f5e83c8ff5b8be55dc3',
        0xe264d:'5e',
        0xe6974:'5f5e5dc3',
        0xda476:'5f5e33c05b8b4dfc',
    }
    # Check equivalent zeroing encodings from the pinned image independently.
    for address,hexbytes in list(suffixes.items()):
        expected=bytes.fromhex(hexbytes)
        if address==0xda476:
            ins=list(md.disasm(pe.get_data(address,10),base+address))
            require([(x.mnemonic,x.op_str) for x in ins[:4]]==
                    [('pop','edi'),('pop','esi'),('xor','eax, eax'),('pop','ebx')],'Early model return differs')
        else: require(pe.get_data(address,len(expected))==expected,'Cleanup contract differs '+hex(address))
    collision_prefix=list(md.disasm(pe.get_data(0xcb3b0,0x2b),base+0xcb3b0))
    require(any(i.mnemonic=='sub' and i.op_str=='esp, 0xa8' for i in collision_prefix),
            'Collision query local frame changed')
    require([i.op_str for i in collision_prefix if i.mnemonic=='push' and i.op_str in ('ebx','esi','edi')]==
            ['ebx','esi','edi'],'Collision query saved-register frame changed')
    collision_exit=list(md.disasm(pe.get_data(0xcbf5f,0x2e),base+0xcbf5f))
    require(collision_exit[-1].mnemonic=='ret' and collision_exit[-1].op_str=='8',
            'Collision query must retain thiscall ret8')
    require(any(i.mnemonic=='xor' and i.op_str=='eax, eax' for i in collision_exit),
            'Collision cancellation must return false')
    require([(i.mnemonic,i.op_str) for i in collision_exit if i.mnemonic=='pop']==
            [('pop','edi'),('pop','esi'),('pop','ebx'),('pop','ebp')],
            'Collision cancellation must retain native register cleanup')
    reset=list(md.disasm(pe.get_data(0xdad90,0x49),base+0xdad90))
    require(reset[-1].mnemonic=='ret' and all(i.mnemonic in ('xor','mov','ret') for i in reset),
            'Scratch reset must remain a scalar callback-free routine')
    require(all(not (i.operands and i.operands[0].type==capstone.x86.X86_OP_REG and
                    md.reg_name(i.operands[0].reg) in ('ebx','esi','edi','ebp')) for i in reset),
            'Scratch reset changed callee-saved state')
    with tempfile.TemporaryDirectory(prefix='ss2-resource-abi-') as folder:
        obj=Path(folder)/'gate.o'
        subprocess.run(['i686-w64-mingw32-g++','-std=c++20','-O2','-Wall','-Wextra','-Werror',
                        '-I'+str(ROOT/'src'),'-c',str(ROOT/'src/game/roomscale_resource_gate.cpp'),'-o',str(obj)],check=True)
        asm=subprocess.check_output(['i686-w64-mingw32-objdump','-dr','-Mintel',str(obj)],text=True)
        bodies=functions(asm)
        for name in sorted(found):
            instructions=code(bodies['_'+name])
            require(instructions[:9]==['pushf','pusha','cld','mov ebp,esp','and esp,0xfffffff0',
                    'sub esp,0x200','fxsave [esp]','fninit','fldcw WORD PTR [esp]'],'Save frame differs: '+name)
            require(sum(x.startswith('call ') for x in instructions)==1,'Extra gate call: '+name)
            require(instructions.count('fxrstor [esp]')==3 and instructions.count('popa')==3 and
                    instructions.count('popf')==3,'Every continuation must restore native state')
            require(not any(x.startswith('ret') for x in instructions),'Gate must not synthesize a native return')
            require(sum(x.startswith('jmp DWORD PTR') for x in instructions)==3,'Wrong continuation count')
        require('lea esp,[ebp-0xc0]' in code(bodies['_resourceCancelCollisionQuery']),'Wrong collision query stack restoration')
        require('lea esp,[ebp-0x58]' in code(bodies['_resourceCancelModelPreparation']),'Wrong model stack restoration')
        require(code(bodies['_resourceCancelMaterialGetter'])[:5]==
                ['xor eax,eax','pop edi','pop esi','pop ebp','ret'],'Wrong material getter return')
        require(code(bodies['_resourceCancelOuterPreparation'])[0]=='add esp,0x14','Wrong outer argument retirement')
        query=code(bodies['_resourceCancelQueryTail'])
        require(query[:2]==['add esp,0xc','xor esi,esi'] and sum(x.startswith('call ') for x in query)==1,
                'Query cleanup must retire three arguments and return false after native scratch reset')
        decision=bodies['_ss2vrResourceDecision']
        require('__tls_index' in decision and '.tls$' in decision,'Native TLS required')
        require(not any(x.startswith('call ') for x in code(decision)),'Decision must be a call-free leaf')
        object_hash=hashlib.sha256(obj.read_bytes()).hexdigest()
    return {'runtime_executed':False,'hooks_activated':False,'native_resource_branches':len(RESOURCE_SITES),
            'hull_dispatch_gates':1,'vertex_buffer_admission_gates':1,'post_call_cancellation_sites':len(RETURN_SITES),'engine_sha256':ENGINE_SHA,'compiled_object_sha256':object_hash,
            'source_sha256':hashlib.sha256(source.encode()).hexdigest(),
            'model_preparation_direct_edge_checks':{'gates':9,'old_sites_rejected':2,
                                                   'encoded_branch_mutations_rejected':2},
            'query_ownership_complete':False,'body_movement_implemented':False}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--game',type=Path,required=True)
    print(json.dumps(verify(p.parse_args().game),indent=2))
