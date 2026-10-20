#!/usr/bin/env python3
"""Inspect compiled remote-render unwind boundaries; never execute native code."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from verify_ride_control_abi import bodies as decoded_bodies,decoded_nodes


def require(ok, message):
    if not ok:
        raise ValueError(message)

def verify_ride_frame_source(text):
    """Finite current-source markers; not a general CFG/lifetime proof."""
    def region(a,b):
        start=text.index(a);end=text.index(b,start+len(a))
        return ''.join(re.sub(r'//[^\n]*|/\*.*?\*/','',text[start:end],flags=re.S).split())
    copy=region('static bool copyRidePaletteBookend(', '__attribute__((noinline)) static void observeRidePalette(')
    require('if(name==mainName){++mains;frame.mainBone=uint32_t(i);frame.boneDefinition=uint32_t(definition);}' in copy and
            'if(name==seatName){++seats;frame.seatBone=uint32_t(i);frame.seatDefinition=uint32_t(definition);}' in copy and
            'if(mains!=1||seats!=1||frame.mainBone==frame.seatBone)returnfalse;' in copy,
            'Paired ride frames require unique distinct selected Main/Seat')
    require('frame.matrices+frame.mainBone*48' in copy and 'frame.matrices+frame.seatBone*48' in copy and
            copy.index('if(mains!=1')<copy.index('std::memcpy(frame.seat.data()'),
            'Seat copy must use the admitted global canonical index')
    observe=region('static void observeRidePalette()', '__attribute__((noinline)) static void publishRideObservation(')
    require('before!=after' in observe and 'rideFrames[index]=before.frame;' in observe,
            'Ride frame must retain both raw native bookends')
    publish=region('static void publishRideObservation(', 'enum class IdleNativeDrawPolicy')
    owner=region('static bool rideObservationOwnerCurrent(', '// Copy raw words after native DDE30 production.')
    require('!rideReadPhaseCurrent(frozenPair,phase)' in owner and
            'RideReadPhasephase=RideReadPhase::Drawing' in owner and
            '!frozenPresentationOwnerMatches(' in owner,
            'Ride owner requires exact bank and explicit draw/retained phase')
    require(text.count('RideReadPhase::Retained')==1 and
            'if(!rideObservationOwnerCurrent(owner,RideReadPhase::Retained)' in publish,
            'Only copied-value publication may use retained ride ownership')
    commit=region('bool commitPair(', 'void useFrozenPair(')
    mono=region('void completeMonoPresentation(', 'void retirePresentation(')
    require('committed=commitNativeFrame(' in commit and
            'if(committed)publishRideObservation(owner,true,&request,gripRig);' in commit and
            commit.index('committed=commitNativeFrame(')<commit.index('if(committed)publishRideObservation(') and
            'if(completed)publishRideObservation(owner,false);' in mono,
            'Retained publication must follow successful stereo or normal mono completion')
    # Single-pass stereo observes the ride once: eye 0 stands for eye 1.
    require('!rideFrameSeen[0])return;' in publish and
            'if(stereo&&!rideFrameSeen[1]){' in publish and
            'rideFrames[1]=rideFrames[0];rideFrameSeen[1]=true;' in publish and
            'emit("SeatCanonical",frame.seat);' in publish and
            'resourceClaim=0seatClaim=0graspClaim=0steeringClaim=0' in publish and
            'schema=5source=%.*s' in publish,
            'Ride frame output lacks completion/source/claim limits')
    attachment=region('static bool copyRideAttachmentBookend(', '__attribute__((noinline)) static void observeRidePalette(')
    require('if(copy.parameterFlags&1)returnfalse;' in attachment and
            attachment.index('if(copy.parameterFlags&1)')<attachment.index('out.seatArray.data()') and
            'readableMemory(ride,0x480)' in attachment,
            'Ride mapping must decline pending parameters before reading seat membership')
    require('if(row[4]||row[5])returnfalse;' in attachment and
            'if(selected!=1||copy.parentName!=parentName)returnfalse;' in attachment and
            'attachmentBefore==attachmentAfter' in observe and
            'if(mapped)before.frame.attachment=attachmentBefore.copy;' in observe,
            'Ride mapping must retain flatness, declared bone name and optional raw bookends')
    require('childWorldAvailable=0flatTree=1' in publish and
            'a.childRecordPresent=b.childRecordPresent=0;' in publish and
            'if(a!=b){rideFrames[0].attachment={};rideFrames[1].attachment={};}' in publish,
            'Ride mapping output must deny child world and clear crossed stereo metadata')
    require('stringId(&seatName,"Seat");' in text and
            'seatName==*invalidId||mainName==seatName' in ''.join(text.split()),
            'Seat IDENT initialization must be distinct and valid')
    require('mainDrawCount=%umainDrawOverflow=%u' in publish and
            'buffersClaim=0positionProgramClaim=0graspClaim=0steeringClaim=0originalSucceeded=1cleanupCurrent=1' in publish and
            'emitDraw("modelWorld",d.world);emitDraw("actualPalette",d.actualPalette);' in publish,
            'Ride Main draws require bounded inventories and zero geometry/program/control claims')
    return {'paired_main_seat_source_checked':True,'limit':'Finite lexical form, not native execution or general CFG proof.'}

def verify_ride_grip_submission_source(host,remote):
    """Finite cross-boundary guards, not runtime submission or grasp proof."""
    def compact(text):
        return ''.join(re.sub(r'//[^\n]*|/\*.*?\*/','',text,flags=re.S).split())
    h=compact(host);r=compact(remote)
    receipt='worldSubmission.sample(GetCurrentProcessId(),cachedRequest,endResult==XR_SUCCESS&&worldPresented&&std::find(layers.begin(),layers.end(),world)!=layers.end());'
    require(h.count('worldSubmission.sample(')==1 and receipt in h and
            h.index('constautoendResult=frame.submit(layers);')<h.index(receipt),
            'World receipt must follow successful actual projection submission')
    require('if(api.lossPending){worldSubmission.invalidate();publishWorldSubmission();frame.submit(layers);break;}' in h,
            'Early compositor loss must retire world feedback')
    start=r.index('staticvoidpublishRideObservation(');end=r.index('enumclassIdleNativeDrawPolicy',start)
    publish=r[start:end]
    require('if(!rideObservationOwnerCurrent(owner,RideReadPhase::Retained)' in publish and
            'if(rideGripEnabled&&stereo&&request&&rig)publishRideGripFrame(*request,*rig);' in publish and
            publish.index('rideObservationOwnerCurrent(')<publish.index('publishRideGripFrame(*request,*rig)'),
            'Geometry handoff must stay behind retained-bank stereo admission')
    # The first copied draw with proved positions is used; later duplicates of
    # the same draw (single-pass stereo) are skipped, never mixed in.
    require('if(selected[eye]||!g.copied||!g.handles.copied||!g.handlePositionAgrees)continue;' in r and
            'if(!selected[eye])returnrideGripFrameFail(3);' in r,
            'Production geometry must take one copied draw with proved positions')
    return {'finite_submission_guards_checked':True,'runtime_executed':False}


def verify_declined_ride_entries(assembly):
    """Actual consumer bytes with the source-reviewed owner gate returning false.

    This checks the dispatch boundary, not native thread-query semantics or
    object lifetime. No ordinary bank read precedes admission or follows its
    false result. A foreign callback can overlap initialization only on that
    declined path. Private stack/output initialization is outside this claim.
    """
    table=decoded_bodies(assembly)
    def selected(fragment):
        found=[b for n,b in table.items() if fragment in n and 'clone' not in n and
               'withNativeFinally<' not in n and '{lambda' not in n]
        require(len(found)==1,'Missing unique ride guard consumer: '+fragment)
        return decoded_nodes(found[0])
    gate=selected('::rideObservationOwnerCurrent(unsigned int, ss2vr::RideReadPhase)')[0][0]
    checked=0
    for name in ('::copyRidePaletteBookend(', '::copyRideAttachmentBookend(', '::observeRidePalette()', '::publishRideObservation(',
                 '::copyCurrentRideMainDraw(', '::rideMainDrawCurrent(', '::recordRideMainDraw(', '::claimRideGpuAttempt('):
        nodes=selected(name)
        calls=[i for i,(_,mn,op,_) in enumerate(nodes) if mn=='call' and op==hex(gate)]
        require(len(calls)==1,'Ride consumer lost unique exact owner-gate call')
        call=calls[0]
        require(not nodes[call][3], 'Local owner-gate call gained relocation/addend')
        require(not any(target=='.bss' for _,_,_,rs in nodes[:call] for _,_,target in rs),
                'Ride bank read precedes owner admission')
        prefix_addresses={a for a,_,_,_ in nodes[:call+1]}
        for a,mn,op,_ in nodes[:call]:
            require(mn!='call' and mn!='ret','Ride entry acquired an unapproved call/return before admission')
            if mn.startswith('j'):
                require(re.fullmatch(r'0x[0-9a-f]+',op) and int(op,16) in prefix_addresses,
                        'Ride entry branch bypasses owner admission')
        index=call+1
        if nodes[index][1:3]==('mov','edx, eax'):index+=1
        require(nodes[index][1:3]==('test','al, al') and nodes[index+1][1] in ('je','jne'),
                'Ride owner false result is not the inspected rejection branch')
        # Actual new consumer uses JNE to the admitted body, with false falling
        # through to scalar return. Existing consumers use JE to rejection.
        if nodes[index+1][1]=='je':address=int(nodes[index+1][2],16)
        else:
            require(index+2<len(nodes),'Missing fall-through rejection')
            address=nodes[index+2][0]
        by_address={row[0]:i for i,row in enumerate(nodes)}
        visited=set()
        while True:
            require(address in by_address and address not in visited,'Unbounded ride rejection path')
            visited.add(address);i=by_address[address];a,mn,op,relocations=nodes[i]
            require(not relocations,'Declined ride path touches relocated/global state')
            require(mn in ('mov','xor','lea','add','pop','ret','jmp','nop'),
                    'Declined ride path acquired work beyond scalar return')
            for memory in re.findall(r'\[([^]]+)\]',op):
                require(re.fullmatch(r'(?:esp|ebp)(?: [+-] 0x[0-9a-f]+)?',memory),
                        'Declined ride path reads memory outside its stack')
            if mn=='ret':break
            if mn=='jmp':
                require(re.fullmatch(r'0x[0-9a-f]+',op),'Indirect ride rejection jump')
                address=int(op,16)
            else:
                require(i+1<len(nodes),'Ride rejection falls outside consumer')
                address=nodes[i+1][0]
        checked+=1
    return checked


def verify(obj):
    assembly = subprocess.check_output(['objdump', '-drC', '-Mintel', '--insn-width=16', str(obj)], text=True)
    ride_frame_source=verify_ride_frame_source((Path(__file__).resolve().parents[1]/'src/game/remote_render.cpp').read_text())
    declined_ride_entries=verify_declined_ride_entries(assembly)
    symbols = subprocess.check_output(['objdump', '-tC', str(obj)], text=True)
    require('file format pe-i386' in assembly, 'Expected GNU x86 remote-render object')
    parts = re.split(r'^[0-9a-f]+ <(.+)>:\n', assembly, flags=re.M)
    bodies = dict(zip(parts[1::2], parts[2::2]))

    def one(fragment, suffix):
        found = [body for name, body in bodies.items() if fragment in name and
                 name.endswith(suffix) and 'clone' not in name]
        require(len(found) == 1, 'Missing or ambiguous render boundary: ' + fragment + suffix)
        return found[0]

    def code(body):
        result = []
        for line in body.splitlines():
            columns = line.split('\t')
            if len(columns) >= 3 and re.fullmatch(r'\s*[0-9a-f]+:', columns[0]):
                instruction = ' '.join(columns[-1].split()).split(' <')[0]
                if re.match('[a-z]', instruction):
                    result.append(instruction)
        return result

    def offset(name):
        found = re.findall(r'0x([0-9a-f]+) ss2vr::game::remote_render::\(anonymous namespace\)::' +
                           re.escape(name) + r'$', symbols, re.M)
        require(len(found) == 1, 'Missing native state symbol: ' + name)
        return int(found[0], 16)

    for name in ['palettePass()', 'modelPass()', 'animationEnd(void*)', 'freezePair(unsigned int)',
                 'commitPair(ss2vr::Slot&, ss2vr::Request const&, bool, unsigned int, ss2vr::RideGripRig const*)']:
        entry = one('ss2vr::game::remote_render::', name)
        require(entry.count('DISP32\tss2vrNativeFinally') == 1,
                'Entry must retain one native unwind extent: ' + name)

    for sampler in ('::copyRidePaletteBookend(', '::copyRideAttachmentBookend('):
        ride_copy = one(sampler, ')')
        require(not any(re.match(r'f[a-z]', i) or re.search(r'\b(?:xmm|ymm|zmm)[0-9]', i)
                        for i in code(ride_copy)),
                'Ride raw-word snapshot body gained floating/SIMD instructions')

    cleanup_names = ['palettePass()', 'modelPass()', 'animationEnd(void*)', 'freezePair(unsigned int)', 'commitPair(',
                     'postModelPass()', 'postPalette()', 'observeLocalScope()']
    fault = 'ss2vr::game::nativeUiFault(char const*)'
    # The diagnostic reason changed this ABI after the historical gate was
    # written. Verify its actual implementation, not just a trusted name.
    bridge = obj.with_name('bridge.cpp.obj')
    bridge_assembly = subprocess.check_output(['objdump', '-drC', '-Mintel', str(bridge)], text=True)
    bridge_parts = re.split(r'^[0-9a-f]+ <(.+)>:\n', bridge_assembly, flags=re.M)
    bridge_bodies = dict(zip(bridge_parts[1::2], bridge_parts[2::2]))
    fault_bodies = [body for name, body in bridge_bodies.items()
                    if name == fault or name.startswith(fault + ' [clone ')]
    require(fault in bridge_bodies and fault_bodies,
            'Missing UI-fault cleanup implementation')
    for fault_body in fault_bodies:
        require(not re.findall(r'DISP32\s+', fault_body) and
                not any(re.match(r'call\b|f[a-z]', i) or re.search(r'\b(?:xmm|ymm|zmm)[0-9]', i)
                        for i in code(fault_body)),
                'UI-fault cleanup must remain scalar with no callbacks or allocation')
        addresses = {int(address, 16) for address in
                     re.findall(r'^\s*([0-9a-f]+):\s', fault_body, re.M)}
        for instruction in code(fault_body):
            if re.match(r'(?:j[a-z]+|loop(?:e|ne)?)\b', instruction):
                target = re.fullmatch(r'\S+ ([0-9a-f]+)', instruction)
                require(target and int(target[1], 16) in addresses,
                        'UI-fault cleanup branch must stay inside its inspected body')
    allowed = {fault,
               'ss2vr::game::multiplayer::PresentationReadGuard::release()',
               'ReleaseSRWLockShared@4', 'ReleaseSRWLockExclusive@4',
               'operator delete(void*, unsigned int)'}
    for name in cleanup_names:
        body = one(name, '::Context::finish(void*, int)')
        instructions = code(body)
        calls = set(re.findall(r'DISP32\s+([^\n]+)', body))
        require(calls <= allowed and fault in calls,
                'Cleanup gained a native callback or lost fault retirement: ' + name)
        require(not any(re.match(r'f[a-z]', i) or re.search(r'\b(?:xmm|ymm|zmm)[0-9]', i) or
                        re.match(r'call (?:DWORD PTR|(?:eax|ebx|ecx|edx|esi|edi|ebp|esp)$)', i) for i in instructions),
                'Cleanup must not use FP or indirect native callbacks: ' + name)
        require(any(re.fullmatch(r'lock cmpxchg DWORD PTR ds:'+hex(offset('pairOwner'))+r',[a-z]{3}',i)
                    for i in instructions) and
                f'mov BYTE PTR ds:{hex(offset("pairInvalid"))},0x1' in instructions,
                'Abort lost scalar exact-owner CAS retirement: ' + name)
        if name in ['freezePair(unsigned int)', 'commitPair(', 'postModelPass()', 'postPalette()']:
            unlock = 'ReleaseSRWLockExclusive@4' if name == 'freezePair(unsigned int)' else 'ReleaseSRWLockShared@4'
            require(unlock in calls and
                    'ss2vr::game::multiplayer::PresentationReadGuard::release()' in calls,
                    'Both presentation locks need explicit retirement: ' + name)
            require(any(re.fullmatch(r'mov BYTE PTR \[[a-z]{3}\+' + hex(offset('presentationBusy')) +
                                     r'\],0x0', i) for i in instructions),
                    'Adapter reentry ownership must retire: ' + name)
        if name in ['postModelPass()', 'postPalette()', 'observeLocalScope()']:
            require('operator delete(void*, unsigned int)' in calls,
                    'Invocation-owned scratch must retire across foreign unwind: ' + name)
    for name, state in [('palettePass()', 'paletteReentrant'), ('modelPass()', 'reentrant')]:
        body = one(name, '::Context::finish(void*, int)')
        instructions = code(body)
        restores = [i for i in instructions if re.fullmatch(
            r'mov BYTE PTR \[[a-z]{3}\+' + hex(offset(state)) + r'\],[abcd]l', i)]
        require(len(restores) == 2,
                'Both normal and abnormal paths must restore saved outer TLS: ' + name)
        from verify_saved_tls import verify_saved_tls
        verify_saved_tls(one('ss2vr::game::remote_render::',name),body,offset(state))
    idle_entry=one('ss2vr::game::remote_render::','copyIdleRaster(void*, ss2vr::IdleRasterCopy&)')
    require(idle_entry.count('DISP32\tss2vrNativeFinally')==1,'Idle raster scratch lacks local containment')
    require(sum(i.startswith('call ') for i in code(idle_entry))==9,'Idle entry lost actual finally/deletion calls')
    idle_cleanup=one('copyIdleRaster(', '::Context::finish(void*, int)')
    idle_calls=re.findall(r'DISP32\s+([^\n]+)',idle_cleanup)
    require(idle_calls==['operator delete(void*, unsigned int)']*8 and
            sum(i.startswith('call ') for i in code(idle_cleanup))==8,
            'All eight idle raster scratch vectors must retire; no native callbacks')
    require(not any(re.match(r'f[a-z]',i) or re.search(r'\b(?:xmm|ymm|zmm)[0-9]',i) or
                    re.match(r'call (?:DWORD PTR|(?:eax|ebx|ecx|edx|esi|edi|ebp|esp)$)',i) for i in code(idle_cleanup)),
            'Idle scratch cleanup gained FP or indirect callbacks')
    idle_cold=[b for n,b in bodies.items() if 'copyIdleRaster(' in n and '::Context::run(void*) [clone .cold]' in n]
    require(len(idle_cold)==1 and '__cxa_begin_catch' in idle_cold[0] and '__cxa_end_catch' in idle_cold[0],
            'Idle raster allocation failure must be contained locally')
    palette_entry=one('ss2vr::game::remote_render::','copyIdlePalette(void*, ss2vr::IdlePaletteCopy&, ss2vr::IdlePaletteBoundaryCopy*)')
    require(palette_entry.count('DISP32\tss2vrNativeFinally')==1,
            'Joined palette scratch lacks local containment')
    palette_cleanup=one('copyIdlePalette(', '::Context::finish(void*, int)')
    palette_calls=re.findall(r'DISP32\s+([^\n]+)',palette_cleanup)
    require(palette_calls==['operator delete(void*, unsigned int)']*8 and
            sum(i.startswith('call ') for i in code(palette_cleanup))==8,
            'All eight joined palette scratch vectors must retire; no native callbacks')
    require(not any(re.match(r'f[a-z]',i) or re.search(r'\b(?:xmm|ymm|zmm)[0-9]',i) or
                    re.match(r'call (?:DWORD PTR|(?:eax|ebx|ecx|edx|esi|edi|ebp|esp)$)',i) for i in code(palette_cleanup)),
            'Joined palette scratch cleanup gained FP or indirect callbacks')
    palette_cold=[b for n,b in bodies.items() if 'copyIdlePalette(' in n and '::Context::run(void*) [clone .cold]' in n]
    require(len(palette_cold)==1 and '__cxa_begin_catch' in palette_cold[0] and '__cxa_end_catch' in palette_cold[0],
            'Joined palette allocation failure must be contained locally')
    # These helpers are callable from foreign-unwind cleanup. Their exact
    # ownership semantics are covered by production-policy/source checks; this
    # compiled gate verifies no hidden lock library, lazy TLS or FP/callback.
    for name in ('retirePresentation(unsigned int)', 'suppressNestedPresentation()',
                 'restorePresentationSuppression(bool)', 'presentationSuppressionCurrent()',
                 'invalidatePresentationForReset(bool)'):
        helper=one('ss2vr::game::remote_render::',name)
        require(not re.findall(r'DISP32\s+',helper) and
                not any(re.match(r'call\b|f[a-z]',i) or re.search(r'\b(?:xmm|ymm|zmm)[0-9]',i)
                        for i in code(helper)), 'Owner retirement/suppression is not scalar: '+name)
        addresses={int(a,16) for a in re.findall(r'^\s*([0-9a-f]+):\s',helper,re.M)}
        for instruction in code(helper):
            if re.match(r'(?:j[a-z]+|loop(?:e|ne)?)\b',instruction):
                target=re.fullmatch(r'\S+ ([0-9a-f]+)',instruction)
                require(target and int(target[1],16) in addresses,
                        'Owner helper branch escaped inspected body')
        if name not in ('restorePresentationSuppression(bool)','presentationSuppressionCurrent()'):
            require(any(re.fullmatch(r'lock cmpxchg DWORD PTR ds:'+hex(offset('pairOwner'))+r',[a-z]{3}',i)
                        for i in code(helper)), 'Missing lock-free x86 owner CAS: '+name)
    return {'object_sha256': hashlib.sha256(obj.read_bytes()).hexdigest(),
            'explicit_native_cleanup_boundaries': len(cleanup_names)+2, 'idle_scratch_vectors_retired':8,
            'palette_scratch_vectors_retired':8,
            'scratch_and_lock_retirement': True,
            'ui_fault_object_sha256': hashlib.sha256(bridge.read_bytes()).hexdigest(),
            'ui_fault_cleanup_scalar': True, 'ui_fault_variants_checked': len(fault_bodies),
            'owner_helpers_scalar_no_callbacks':True,
            'ride_snapshot_body_scalar':True,
            'ride_frame_source':ride_frame_source,
            'declined_ride_entries_without_global_reads':declined_ride_entries,
            'owner_cas_width_bits':32,
            'owner_comparison_semantics':'production portable policy plus source review; not a compiled path proof',
            'runtime_executed': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--object', type=Path, required=True)
    print(json.dumps(verify(parser.parse_args().object), indent=2))
