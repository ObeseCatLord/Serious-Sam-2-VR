#!/usr/bin/env python3
"""Check head-volume integration boundaries in source and the x86 object."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

def require(ok,message):
    if not ok:raise ValueError(message)
def verify(obj,root):
    assembly=subprocess.check_output(['objdump','-drC','-Mintel',str(obj)],text=True)
    require('file format pe-i386' in assembly,'Expected x86 engine object')
    split=re.split(r'^[0-9a-f]+ <(.+)>:\n',assembly,flags=re.M)
    bodies=dict(zip(split[1::2],split[2::2]))
    def one(fragment,suffix=None):
        found=[v for k,v in bodies.items() if fragment in k and 'clone' not in k and
               (k.endswith(suffix) if suffix else k.startswith('ss2vr::game::'+fragment))]
        require(len(found)==1,'Missing/ambiguous head boundary: '+fragment)
        return found[0]
    def code(text):
        out=[]
        for line in text.splitlines():
            c=line.split('\t')
            if len(c)>=3 and re.fullmatch(r'\s*[0-9a-f]+:',c[0]):
                value=' '.join(c[-1].split()).split(' <')[0]
                if re.match('[a-z]',value):out.append(value)
        return out
    query=one('queryHeadVolume(void*)');instructions=code(query)
    require('mov DWORD PTR [esp+0x1c],0x1' in instructions,'Head queries must reject initial contact')
    require('mov DWORD PTR [esp+0x8],0x0' in instructions,'Head query must not grant a penetration budget')
    require(sum(i.startswith('call ') for i in instructions)==1 and 'runOwnedSphereQueries' in query,
            'Head query must use the owned native sphere driver')
    cleanup=one('runPostSimulationHeadVisibility(', '::Context::finish(void*, int)')
    require('_tls_index' in cleanup and '.tls$' in cleanup,'Head owner cleanup lost explicit native TLS')
    require(not any(i.startswith('call ') or re.match(r'f[a-z]',i) or
                    re.search(r'\b(?:xmm|ymm|zmm)[0-9]',i) for i in code(cleanup)),
            'Head cleanup must stay scalar metadata-only')
    body=one('runPostSimulationHeadVisibility(', '::Context::run(void*)')
    require('runRoomscaleMathFrame' in body,'Head query preparation must enter its FP frame')
    math=one('prepareAndQueryHeadVolume(')
    require('runRoomscaleResourceScope' in math,'Head query lost pending-resource containment')
    simulation=one('simulationStep(void*, void*)','::Context::run(void*)')
    require(simulation.index('runPostSimulationRoomscale')<simulation.index('runPostSimulationHeadVisibility')<
            simulation.index('multiplayer::completeTick'),'Head query must follow body settlement and precede tick retirement')
    engine=(root/'src/game/engine.cpp').read_text();bridge=(root/'src/game/bridge.cpp').read_text()
    host=(root/'src/host/host.cpp').read_text();protocol=(root/'src/common/protocol.hpp').read_text()
    require('headFramePrepared(puppet,slot.request)' in bridge and
            bridge.index('headFramePrepared(puppet,slot.request)')<bridge.index('chosen=i; request=slot.request; slot.state=SlotState::Rendering'),
            'Do not consume a request before its post-simulation query attempt')
    for guard in ['headVolumeVisibilityReason(headVolumeObservation','slot.headClearance=eyeHeadClearance',
                  'headVolumeDrawSafe','HeadClearanceMode::Opaque','headVolumeObservation.numericalGuard']:
        require(guard in engine,'Missing native head receipt guard: '+guard)
    for guard in ['validHeadClearance(slot.headClearance)','cachedClearance = clearance;',
                  'headClearanceAllows(cachedClearance,cachedRequest,input,currentEyes,GetTickCount64())']:
        require(guard in host,'Missing late-frame head receipt check: '+guard)
    require(host.count('admitFinalWorld(layers, world, input, validViews,views)')==3,
            'Recheck head clearance after potentially waiting HUD uploads')
    ipc_abi = int(re.search(r'\bAbi\s*=\s*(\d+)', protocol).group(1))
    require(ipc_abi == 11 and 'static_assert(sizeof(HeadClearance)==40)' in protocol,
            'Versioned fixed-width head receipt ABI missing')
    require('!captureHeadQueryBinding(h,h.binding)' in engine and 'headQueryOwnerCurrent(&h)' in engine,
            'Head filters must use their separate owned binding, not a fabricated foot body')
    require('(!c.snapshot.rider.handheld()&&!c.snapshot.rider.seated())' in engine,
            'Mounted head admission must retain explicit native rider state')
    return {'object_sha256':hashlib.sha256(obj.read_bytes()).hexdigest(),
            'post_simulation_head_query':True,'initial_contact_rejected':True,
            'host_late_pose_and_age_gate':True,'ipc_abi':ipc_abi,'runtime_executed':False,
            'mounted_world_head_query_enabled':True}
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--object',type=Path,required=True)
    p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1])
    a=p.parse_args();print(json.dumps(verify(a.object,a.root),indent=2))
