#!/usr/bin/env python3
"""Run a verified direct-to-scene SS2 test in an existing private Proton lab.

Configuration, owned game assets, prefixes, logs and images stay outside source.
This launcher never installs, changes global runtimes, joins multiplayer or clicks menus.
"""
from assess_runtime import (complete_pairs_for_pose,dual_topologies,successful_dual_fire,
    dual_identity,DualPhaseEvidence,require_dual_capture,dual_weapon_events)

import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import pty
import re
import signal
import subprocess
import sys
import time
import zipfile
import uuid

ROOT = Path(__file__).resolve().parents[1]

def native_grip_resource_receipts(log):
    """Successful native opens only; never a rendered-instance/grip certificate."""
    names = ('ZapGunWeapon.ep', 'ZapGun_FP.mdl', 'Zapgun.bmf', 'Zapgun.skl', 'R_Hand.bmf')
    receipts = {}
    pattern = r'Lab native grip resource key=(\d+) bytes=(\d+) positionRestored=(\d+) sha256=([a-f0-9]{64})'
    for line in log.splitlines():
        if 'Lab native grip resource ' not in line:
            continue
        match = re.fullmatch(pattern, line.strip())
        if not match:
            raise ValueError('Incomplete, saturated or malformed native grip-resource observation')
        key, size, position = map(int, match.group(1, 2, 3))
        if key not in range(1, 6) or not 0 < size <= 32*1024*1024 or not 0 <= position <= size:
            raise ValueError('Native grip-resource receipt outside the observer contract')
        value = {'resource': names[key-1], 'bytes': size, 'restored_positions': [position],
                 'sha256': match.group(4), 'open_count': 1}
        if key in receipts:
            previous = receipts[key]
            if (previous['bytes'], previous['sha256']) != (size, value['sha256']):
                raise ValueError('Conflicting native opens do not identify one resource version')
            value['open_count'] = previous['open_count'] + 1
            value['restored_positions'] = previous['restored_positions'] + [position]
            if value['open_count'] > 2:
                raise ValueError('Native grip-resource observation exceeded its attempt limit')
        receipts[key] = value
    if set(receipts) != set(range(1, 6)):
        raise ValueError('All five exact native grip-resource opens were not observed')
    return [dict(key=key, **receipts[key]) for key in sorted(receipts)]

def digest(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def checked_file(path, expected):
    path = Path(path).resolve(strict=True)
    if not path.is_file() or digest(path) != expected:
        raise ValueError('Configured file fingerprint differs: ' + path.name)
    return path

def validate_display_probe(cfg, private):
    selected=cfg.get('display_timing_probe')
    mode=cfg.get('display_prerequisite_only',False)
    if type(mode) is not bool:raise ValueError('Display prerequisite-only selection must be boolean')
    if mode and selected is None:raise ValueError('Prerequisite-only mode requires its pinned native probe')
    eager=cfg.get('monado_environment',{}).get('XRT_COMPOSITOR_DISABLE_DEFERRED')
    order=cfg.get('monado_window_order_probe',False)
    if type(order) is not bool:raise ValueError('Monado window-order selector must be boolean')
    helper=cfg.get('private_x11_windows')
    if eager is not None:
        if eager!='1' or cfg.get('renderer_mode','vr')!='vr' or not isinstance(helper,dict) or set(helper)!={'path','sha256'}:
            raise ValueError('Eager graphical Monado requires its sealed read-only window helper')
        path=checked_file(helper['path'],helper['sha256'])
        if not path.is_relative_to(private) or path.read_bytes()[:4]!=b'\x7fELF':
            raise ValueError('Private X11 receipt helper must be a private native executable')
    elif helper is not None or order:
        raise ValueError('Window receipts/order diagnostic require eager graphical Monado')
    if order and (not mode or cfg.get('display_focus_comparison') is not None):
        raise ValueError('Window ordering is a separate nongame prerequisite extent')
    variants=cfg.get('display_focus_comparison')
    if variants is not None:
        if not mode or not isinstance(variants,list) or len(variants)!=2:
            raise ValueError('Foreground comparison requires two sealed nongame observers')
        for variant,name,subsystem in zip(variants,('console','gui'),(3,2)):
            if not isinstance(variant,dict) or set(variant)!={'name','path','sha256'} or variant['name']!=name:
                raise ValueError('Foreground observer identity differs')
            path=checked_file(variant['path'],variant['sha256'])
            if not path.is_relative_to(private):raise ValueError('Foreground observer must be private')
            data=path.read_bytes();offset=int.from_bytes(data[60:64],'little')
            if data[:2]!=b'MZ' or data[offset:offset+6]!=b'PE\0\0\x4c\x01' or \
               data[offset+24:offset+26]!=b'\x0b\x01' or \
               int.from_bytes(data[offset+92:offset+94],'little')!=subsystem:
                raise ValueError('Foreground observer PE32/subsystem differs')
    if selected is None:return
    if not isinstance(selected,dict) or set(selected)!={'path','sha256'}:
        raise ValueError('Display prerequisite requires its exact executable identity')
    path=checked_file(selected['path'],selected['sha256'])
    if not path.is_relative_to(private):raise ValueError('Display prerequisite must stay private')
    # Admit only a compiled x86 PE; never treat a source file as a runnable probe.
    import struct
    with path.open('rb') as stream:
        header=stream.read(64)
        if len(header)!=64 or header[:2]!=b'MZ':raise ValueError('Display prerequisite is not a PE')
        stream.seek(struct.unpack_from('<I',header,60)[0]);pe=stream.read(6)
    if pe!=b'PE\0\0L\x01':raise ValueError('Display prerequisite must use the game x86 architecture')
    if variants is not None or order:
        data=path.read_bytes();offset=int.from_bytes(data[60:64],'little')
        if data[offset+24:offset+26]!=b'\x0b\x01' or int.from_bytes(data[offset+92:offset+94],'little')!=2:
            raise ValueError('Foreground comparison requires a GUI owner probe')

def require_display_timing(report):
    if not isinstance(report,dict) or type(report.get('schema')) is not int or report['schema']!=1 or report.get('stage')!='swapchain-query' or \
       report.get('operation')!='get-raster-status' or report.get('raster_called') is not True or \
       report.get('cleanup_completed') is not True or type(report.get('hresult')) is not int or report['hresult']!=0:
        raise RuntimeError('Private D3D9 display prerequisite did not complete its raster query')
    for key in ('desktop_hz','adapter_hz','swapchain_hz','width','height'):
        value=report.get(key)
        if type(value) is not int or not 1<value<=16384:
            raise RuntimeError('Private D3D9 display prerequisite returned an invalid current mode')

def owner_foreground_samples(path):
    if path.is_symlink() or not path.is_file() or path.stat().st_size>131072:
        raise RuntimeError('Owner foreground samples are not a bounded native file')
    rows=[]
    for line in path.read_bytes().splitlines(keepends=True):
        if not line.endswith(b'\n'):continue # Native writer may still be flushing this row.
        row=json.loads(line)
        if not isinstance(row,dict) or type(row.get('tick_ms')) is not int or not 0<=row['tick_ms']<=0xffffffffffffffff or \
           any(type(row.get(k)) is not int or not 0<=row[k]<=0xffffffff for k in ('foreground_hwnd','foreground_owner','foreground_thread')) or \
           type(row.get('probe_title_matches')) is not bool:
            raise RuntimeError('Owner foreground sample differs')
        if rows and row['tick_ms']<rows[-1]['tick_ms']:raise RuntimeError('Owner foreground sample clock reversed')
        rows.append(row)
    if not rows:raise RuntimeError('Owner foreground sample is absent')
    return rows

def foreground_interval(rows, entry, observed):
    """Only fresh native owner samples bracketing the child's actual interval."""
    prior=[row for row in rows if row['tick_ms']<=entry]
    later=[row for row in rows if row['tick_ms']>observed]
    during=[row for row in rows if entry<row['tick_ms']<=observed]
    before=prior[-1] if prior else None;after=later[0] if later else None
    covered=False
    if before and after and during and entry<=observed:
        sequence=[before,*during,after]
        # Owner samples every20ms; gaps over100ms make this diagnostic inconclusive.
        covered=(entry-before['tick_ms']<=100 and after['tick_ms']-observed<=100 and
                 all(0<=b['tick_ms']-a['tick_ms']<=100 for a,b in zip(sequence,sequence[1:])))
    return before,during,after,covered

def private_window_snapshot(cfg,env,deadline,record):
    helper=cfg['private_x11_windows'];checked_file(helper['path'],helper['sha256'])
    result=subprocess.run([helper['path']],env=env,capture_output=True,timeout=remaining(deadline,2))
    if result.returncode or len(result.stdout)>131072:raise RuntimeError('Private X11 window query failed')
    value=json.loads(result.stdout)
    if not isinstance(value,dict) or type(value.get('schema')) is not int or value['schema']!=1 or value.get('complete') is not True or \
       type(value.get('active_xid')) is not int or not 0<=value['active_xid']<=0xffffffff or \
       not isinstance(value.get('windows'),list) or len(value['windows'])>4096:
        raise RuntimeError('Private X11 window receipt differs')
    for row in value['windows']:
        if not isinstance(row,dict) or type(row.get('xid')) is not int or not 0<row['xid']<=0xffffffff or \
           type(row.get('pid')) is not int or not -1<=row['pid']<=0x7fffffff or \
           type(row.get('role')) is not int or row['role'] not in (1,2) or type(row.get('viewable')) is not bool or \
           type(row.get('input_output')) is not bool or any(type(row.get(k)) is not int or not 0<=row[k]<=65535 for k in ('width','height')):
            raise RuntimeError('Private X11 window identity differs')
    record.write_text(json.dumps(value,indent=2)+'\n')
    return value

def require_owned_monado_window(snapshot,owner,xid=None):
    if not still_owned(owner):raise RuntimeError('Private Monado incarnation changed')
    matches=[row for row in snapshot['windows'] if row['role']==1 and row['pid']==owner['pid'] and row['viewable'] and
             row['input_output'] and row['width']>0 and row['height']>0]
    if len(matches)!=1 or (xid is not None and matches[0]['xid']!=xid):
        raise RuntimeError('Exact private Monado mapped window is absent or ambiguous')
    return matches[0]['xid']

def display_timing_probe(cfg,env,deadline,run_dir,window_observation=None):
    selected=cfg.get('display_timing_probe')
    if selected is None:return None
    if env.get('SS2VR_LAB_PRIVATE_DISPLAY')!='1' or not env.get('DISPLAY') or env.get('WAYLAND_DISPLAY'):
        raise RuntimeError('Display prerequisite requires the separately owned X11 display')
    path=checked_file(selected['path'],selected['sha256'])
    # The outer private-display owner also tracks Wine descendants. This function
    # retires the exact probe executable and Popen launcher on every exit.
    if private_processes(path):raise RuntimeError('Display prerequisite is already running')
    process=None;output=run_dir/'display-timing-probe.log';code=None
    probe_env=env.copy()
    # Installed Proton redirects child stdout/stderr to its own log when
    # PROTON_LOG=1. Keep probe console diagnostics on their explicit handle;
    # the native receipt is independent. Retain normal game/session logging.
    probe_env['PROTON_LOG']='0'
    receipt=run_dir/'display-timing-native.json'
    if receipt.exists() or receipt.is_symlink():raise RuntimeError('Native display receipt already exists')
    probe_env['SS2VR_DISPLAY_TIMING_OUTPUT']='Z:'+str(receipt)
    comparisons=cfg.get('display_focus_comparison')
    if comparisons or window_observation:probe_env['SS2VR_LAB_FOCUS_COMPARE']='1'
    try:
        with output.open('xb') as log:
            # Use the existing observer/tool route. Proton "run" starts through
            # steam.exe; runinprefix directly invokes the diagnostic executable.
            process=subprocess.Popen([cfg['proton'],'runinprefix',str(path)],env=probe_env,stdout=log,stderr=subprocess.STDOUT)
            observations=[]
            if comparisons or window_observation:
                ready=Path(str(receipt)+'.window-ready.json');ready_deadline=min(deadline,time.monotonic()+10)
                target=None
                while time.monotonic()<ready_deadline:
                    if process.poll() is not None:raise RuntimeError('Foreground probe retired before its observation opportunity')
                    if ready.is_symlink():raise RuntimeError('Foreground ready receipt is a symlink')
                    if ready.exists():
                        if not ready.is_file() or ready.stat().st_size>4096:
                            raise RuntimeError('Foreground ready receipt is not a bounded file')
                        try:target=json.loads(ready.read_text())
                        except json.JSONDecodeError:pass # Exclusive native write may still be in progress.
                        else:break
                    time.sleep(.02)
                if not isinstance(target,dict) or type(target.get('schema')) is not int or target['schema']!=1 or \
                   type(target.get('baseline_foreground')) is not bool or \
                   any(type(target.get(k)) is not int or not 0<target[k]<=0xffffffff for k in ('probe_pid','target_hwnd','target_thread')):
                    raise RuntimeError('Foreground probe has no valid ready receipt')
                owner_samples=Path(str(receipt)+'.owner-foreground.jsonl')
                if window_observation:window_observation(target,owner_foreground_samples(owner_samples))
                for variant in comparisons or []:
                    observed=run_dir/('foreground-'+variant['name']+'.json')
                    if process.poll() is not None:raise RuntimeError('Foreground probe retired before observer execution')
                    checked_file(variant['path'],variant['sha256'])
                    before_launch=owner_foreground_samples(owner_samples)[-1]
                    selected_cfg=dict(cfg,observer=variant['path'])
                    observer(selected_cfg,probe_env,'private-foreground-status','Z:'+str(observed),
                             timeout=remaining(deadline,3),deadline=deadline)
                    if process.poll() is not None:raise RuntimeError('Foreground probe retired during observer execution')
                    if observed.is_symlink() or not observed.is_file() or not 1<=observed.stat().st_size<=4096:
                        raise RuntimeError('Foreground observer has no bounded native receipt')
                    sample=json.loads(observed.read_text())
                    if not isinstance(sample,dict) or type(sample.get('schema')) is not int or sample['schema']!=1 or \
                       sample.get('operation')!='private-foreground-status' or \
                       type(sample.get('tick_ms')) is not int or not 0<=sample['tick_ms']<=0xffffffffffffffff or \
                       type(sample.get('entry_tick_ms')) is not int or not 0<=sample['entry_tick_ms']<=sample['tick_ms'] or \
                       any(type(sample.get(k)) is not int or not 0<sample[k]<=0xffffffff for k in ('observer_pid','observer_thread')) or \
                       any(type(sample.get(k)) is not int or not 0<=sample[k]<=0xffffffff for k in ('console_hwnd','foreground_hwnd','foreground_owner','foreground_thread')) or \
                       any(type(sample.get(k)) is not bool for k in ('probe_title_matches','foreground_visible')):
                        raise RuntimeError('Foreground observer receipt differs')
                    after_deadline=min(deadline,time.monotonic()+.5)
                    while True:
                        rows=owner_foreground_samples(owner_samples);after=rows[-1]
                        if after['tick_ms']>sample['tick_ms'] or time.monotonic()>=after_deadline:break
                        if process.poll() is not None:raise RuntimeError('Foreground probe retired before owner post-observation')
                        time.sleep(.02)
                    before,during,after,covered=foreground_interval(rows,sample['entry_tick_ms'],sample['tick_ms'])
                    baseline=bool(before and target['baseline_foreground'] and before['foreground_hwnd']==target['target_hwnd'] and
                              before['foreground_owner']==target['probe_pid'] and before['probe_title_matches'])
                    observations.append({'variant':variant['name'],'sha256':variant['sha256'],
                        'target':target,'sample':sample,'owner_before_launch':before_launch,'owner_before':before,
                        'owner_during':during,'owner_after':after,'interval_covered':covered,
                        'baseline_present':baseline,'conclusive':bool(baseline and covered)})
            code=process.wait(timeout=remaining(deadline,30))
        # Native file output is independent of Proton/Wine console routing.
        # Launcher exit alone never certifies the native program's result.
        if code!=0 or receipt.is_symlink() or not receipt.is_file() or not 1<=receipt.stat().st_size<=4096:
            raise RuntimeError('Private D3D9 display prerequisite has no bounded native receipt; game was not launched')
        report=json.loads(receipt.read_text())
        require_display_timing(report)
        if comparisons:
            report['foreground_comparison']=observations
            report['foreground_comparison_conclusive']=all(row['conclusive'] for row in observations)
        (run_dir/'display-timing.json').write_text(json.dumps(report,indent=2)+'\n')
        return report
    finally:
        cleanup_deadline=time.monotonic()+8;errors=[];requests=[]
        def deferred(signum,frame):requests.append(signum)
        handlers={sig:signal.signal(sig,deferred) for sig in (signal.SIGTERM,signal.SIGINT)}
        def safe(action):
            try:action()
            except Exception as error:errors.append(str(error))
        try:
            if process and process.poll() is None:
                safe(process.terminate)
                try:process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    safe(process.kill);safe(lambda:process.wait(timeout=2))
            for item in private_processes(path):safe(lambda item=item:stop_exact(item,cleanup_deadline))
            # Reporting failure must not skip retirement. Hash identity was
            # already validated before launch; don't reopen the executable here.
            safe(lambda:(run_dir/'display-timing-process.json').write_text(json.dumps({'schema':1,'returncode':code,
                'probe_sha256':selected['sha256'],'proton_log':False,'game_launched':False},indent=2)+'\n'))
        finally:
            for sig,handler in handlers.items():signal.signal(sig,handler)
        if errors or requests:raise RuntimeError('Display prerequisite cleanup interrupted or failed')

def identity(pid):
    p = Path('/proc') / str(pid)
    try:
        stat = (p / 'stat').read_text().rsplit(')', 1)[1].split()
        return {'pid': int(pid), 'start': stat[19], 'parent': int(stat[1]),
                'name': (p / 'comm').read_text().strip(),
                'argv': (p / 'cmdline').read_bytes().split(b'\0')}
    except (OSError, IndexError):
        return None

def running(name):
    return [i for p in Path('/proc').iterdir() if p.name.isdigit()
            if (i := identity(p.name)) and i['name'] == name]

def still_owned(item):
    now = identity(item['pid'])
    return now is not None and now['start'] == item['start'] and now['argv'] == item['argv']

def stop_exact(item, budget=None):
    if not still_owned(item):
        return
    try: os.kill(item['pid'], signal.SIGTERM)
    except ProcessLookupError: return
    deadline = min(time.monotonic()+5,budget) if budget is not None else time.monotonic()+5
    while still_owned(item) and time.monotonic() < deadline:
        time.sleep(.1)
    if still_owned(item):
        try: os.kill(item['pid'], signal.SIGKILL)
        except ProcessLookupError: return
        deadline=min(time.monotonic()+2,budget) if budget is not None else time.monotonic()+2
        while still_owned(item) and time.monotonic()<deadline: time.sleep(.05)
        if still_owned(item): raise RuntimeError('Exact owned process did not terminate')

def remaining(deadline, limit=20):
    seconds=deadline-time.monotonic()
    if seconds<=0: raise TimeoutError('Absolute runtime deadline reached')
    return min(seconds,limit)

def private_processes(executable, token=None):
    expected=str(Path(executable).resolve()).replace('\\','/').casefold()
    found=[]
    for p in Path('/proc').iterdir():
        if not p.name.isdigit(): continue
        item=identity(p.name)
        if not item: continue
        original=[v.decode(errors='replace') for v in item['argv'][:16]]
        args=[v.replace('\\','/').casefold() for v in original]
        if any(v.removeprefix('z:')==expected for v in args) and (token is None or token in original):
            found.append(item)
    return found

def game_owned(item, lab):
    argv = b' '.join(item['argv']).decode(errors='replace').replace('\\', '/').casefold()
    return str(lab / 'Bin/Sam2.exe').casefold() in argv

def window_for(pid):
    x = ctypes.CDLL('libX11.so.6'); D = ctypes.c_void_p; U = ctypes.c_ulong
    x.XOpenDisplay.restype = D
    x.XDefaultRootWindow.argtypes = [D]; x.XDefaultRootWindow.restype = U
    x.XQueryTree.argtypes = [D,U,ctypes.POINTER(U),ctypes.POINTER(U),ctypes.POINTER(ctypes.POINTER(U)),ctypes.POINTER(ctypes.c_uint)]
    x.XFetchName.argtypes = [D,U,ctypes.POINTER(ctypes.c_char_p)]
    x.XFree.argtypes = [D]; x.XCloseDisplay.argtypes = [D]
    x.XInternAtom.argtypes = [D,ctypes.c_char_p,ctypes.c_int]; x.XInternAtom.restype = U
    x.XGetWindowProperty.argtypes = [D,U,U,ctypes.c_long,ctypes.c_long,ctypes.c_int,U,ctypes.POINTER(U),ctypes.POINTER(ctypes.c_int),ctypes.POINTER(U),ctypes.POINTER(U),ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte))]
    x.XSetInputFocus.argtypes = [D,U,ctypes.c_int,U]; x.XRaiseWindow.argtypes = [D,U]
    x.XSync.argtypes = [D,ctypes.c_int]
    class Attributes(ctypes.Structure):
        _fields_=[(name,ctypes.c_int) for name in ('x','y','width','height','border_width','depth')]+[
            ('visual',D),('root',U),('c_class',ctypes.c_int),('bit_gravity',ctypes.c_int),
            ('win_gravity',ctypes.c_int),('backing_store',ctypes.c_int),('backing_planes',U),
            ('backing_pixel',U),('save_under',ctypes.c_int),('colormap',U),('map_installed',ctypes.c_int),
            ('map_state',ctypes.c_int),('all_event_masks',ctypes.c_long),('your_event_mask',ctypes.c_long),
            ('do_not_propagate_mask',ctypes.c_long),('override_redirect',ctypes.c_int),('screen',D)]
    class XError(ctypes.Structure):
        _fields_=[('type',ctypes.c_int),('display',D),('resourceid',U),('serial',U),
                  ('error_code',ctypes.c_ubyte),('request_code',ctypes.c_ubyte),('minor_code',ctypes.c_ubyte)]
    x.XGetWindowAttributes.argtypes=[D,U,ctypes.POINTER(Attributes)]
    x.XSetErrorHandler.argtypes=[D];x.XSetErrorHandler.restype=D
    errors=[]
    @ctypes.CFUNCTYPE(ctypes.c_int,D,ctypes.POINTER(XError))
    def on_error(display,error):
        if len(errors)<16:errors.append((error.contents.error_code,error.contents.request_code))
        return 0
    class ClientMessage(ctypes.Structure):
        _fields_=[('type',ctypes.c_int),('serial',U),('send_event',ctypes.c_int),('display',D),
                  ('window',U),('message_type',U),('format',ctypes.c_int),('data',ctypes.c_long*5)]
    class Event(ctypes.Union):
        _fields_=[('client',ClientMessage),('pad',ctypes.c_long*24)]
    x.XSendEvent.argtypes=[D,U,ctypes.c_int,ctypes.c_long,ctypes.POINTER(Event)]
    d = x.XOpenDisplay(None)
    if not d: raise RuntimeError('No native desktop display')
    previous=x.XSetErrorHandler(ctypes.cast(on_error,D))
    try:
        root = x.XDefaultRootWindow(d); atom = x.XInternAtom(d,b'_NET_WM_PID',0)
        def walk(w):
            a=U(); b=U(); children=ctypes.POINTER(U)(); n=ctypes.c_uint()
            if not x.XQueryTree(d,w,ctypes.byref(a),ctypes.byref(b),ctypes.byref(children),ctypes.byref(n)): return
            nodes=[children[i] for i in range(n.value)]
            if children: x.XFree(children)
            for c in nodes:
                yield c
                yield from walk(c)
        found=[]
        for w in walk(root):
            a=U(); fmt=ctypes.c_int(); n=U(); after=U(); p=ctypes.POINTER(ctypes.c_ubyte)()
            x.XGetWindowProperty(d,w,atom,0,1,0,6,ctypes.byref(a),ctypes.byref(fmt),ctypes.byref(n),ctypes.byref(after),ctypes.byref(p))
            owner=ctypes.cast(p,ctypes.POINTER(U))[0] if p and n.value and fmt.value==32 else 0
            if p: x.XFree(p)
            if owner!=pid: continue
            title=ctypes.c_char_p()
            if x.XFetchName(d,w,ctypes.byref(title)) and title.value:
                attrs=Attributes()
                if (title.value==b'Serious Sam 2' and x.XGetWindowAttributes(d,w,ctypes.byref(attrs)) and
                    attrs.map_state==2 and attrs.c_class==1 and attrs.width>0 and attrs.height>0):found.append(w)
                x.XFree(title)
        if len(found)!=1: return None
        x.XSync(d,0);errors.clear()
        attrs=Attributes()
        if not x.XGetWindowAttributes(d,found[0],ctypes.byref(attrs)) or attrs.map_state!=2:return None
        event=Event();event.client.type=33;event.client.display=d;event.client.window=found[0]
        event.client.message_type=x.XInternAtom(d,b'_NET_ACTIVE_WINDOW',0)
        event.client.format=32;event.client.data[0]=2
        x.XSendEvent(d,root,0,(1<<20)|(1<<19),ctypes.byref(event))
        x.XRaiseWindow(d,found[0]);x.XSetInputFocus(d,found[0],2,0);x.XSync(d,0)
        # Mapping can change between inspection and focus. Reject that attempt;
        # the existing bounded launcher waits, without mapping arbitrary windows.
        if errors:
            if any(code not in (3,8) for code,request in errors):raise RuntimeError('Owned-window X operation failed')
            return None
        return found[0]
    finally:
        x.XSync(d,0);x.XSetErrorHandler(previous);x.XCloseDisplay(d)

class ObserverError(RuntimeError):
    def __init__(self, command, code):
        self.code=code
        super().__init__('IPC observer '+command+' returned '+str(code))

def observer(cfg, env, command, token, output=None, timeout=20, deadline=None):
    args=[cfg['proton'], 'runinprefix', cfg['observer'], command, token]
    status_file=None
    if command in ('status','stock-status'):
        status_file=Path(cfg['observer_output_dir'])/('status-'+uuid.uuid4().hex+'.json')
        args.append('Z:'+str(status_file))
        if command=='status':args.append('Z:'+str(Path(cfg['game_lab'])/'Bin/Sam2.exe'))
    if output is not None:
        args.extend(map(str,output))
    before={i['pid'] for i in private_processes(cfg['observer'],token)}
    if deadline is None: deadline=time.monotonic()+timeout+4
    timeout=remaining(deadline,timeout)
    # Proton opens the same app-id log for every invocation. Status observers
    # must not truncate the owned game's startup trace or mutate its environment.
    observer_env=env.copy();observer_env['PROTON_LOG']='0'
    process=subprocess.Popen(args,env=observer_env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True)
    try:
        stdout,stderr=process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            if process.poll() is None: process.terminate()
            try: process.communicate(timeout=remaining(deadline,1))
            except (subprocess.TimeoutExpired,TimeoutError):
                if process.poll() is None: process.kill()
                try: process.communicate(timeout=remaining(deadline,1))
                except (subprocess.TimeoutExpired,TimeoutError): pass
        finally:
            for item in private_processes(cfg['observer'],token):
                if item['pid'] not in before: stop_exact(item,deadline)
        raise
    if process.returncode:
        raise ObserverError(command,process.returncode)
    if command in ('status','stock-status'):
        if not status_file.is_file():
            (Path(cfg['observer_output_dir'])/'observer-output-diagnostic.txt').write_bytes(stdout+b'\n'+stderr)
            raise RuntimeError('Observer returned success without its private status record')
        return json.loads(status_file.read_text())
    return None

def recorded_native_focus(cfg, env, token, creation, receipt, deadline, timeout=3):
    """Record a bounded existing activation attempt, including rejected guards."""
    if receipt.exists() or receipt.is_symlink():raise RuntimeError('Activation receipt already exists')
    record={'receipt_path':str(receipt),'observer_returncode':0,'receipt_present':False}
    try:
        observer(cfg,env,'stock-focus',token,(creation,'Z:'+str(receipt)),
                 timeout=remaining(deadline,timeout),deadline=deadline)
    except ObserverError as error:
        record['observer_returncode']=error.code
        if error.code!=8:raise
    if receipt.is_symlink():raise RuntimeError('Activation receipt is a symlink')
    if receipt.exists():
        if not receipt.is_file() or not 1<=receipt.stat().st_size<=4096:
            raise RuntimeError('Activation receipt is not a bounded native file')
        record['native_receipt']=json.loads(receipt.read_text())
        record['receipt_present']=True
    # Absence is recorded explicitly, never interpreted as activation success.
    # Keep existing request bounds and loading gates, including rejected code8.
    return record

def idle_probe_weapon(cfg):
    enabled=cfg.get('idle_weapon_probe',False)
    if type(enabled) is not bool:raise ValueError('Idle collection selection must be explicit boolean')
    if not enabled:
        if 'idle_native_id' in cfg:raise ValueError('Idle weapon ID requires enabled neutral collection')
        return None
    selected=cfg.get('idle_native_id',1)
    if type(selected) is not int or selected not in (1,2,13):
        raise ValueError('Idle collection supports only exact native IDs1,2 and13')
    return selected

def preparation_weapon(cfg):
    legacy=cfg.get('prepare_sniper_fixture',False)
    idle=cfg.get('prepare_idle_fixture',False)
    if type(legacy) is not bool or type(idle) is not bool:
        raise ValueError('Fixture preparation must be an explicit boolean')
    if legacy and idle:raise ValueError('Conflicting preparation opt-ins')
    selected=idle_probe_weapon(cfg)
    if legacy and selected!=13:raise ValueError('Sniper preparation requires the exact neutral ID13 collector')
    if idle and selected not in (2,13):raise ValueError('Idle preparation requires exact neutral ID2 or ID13')
    return selected if legacy or idle else None

PREPARATION_TARGETS={2:('autosg-id2.sav','Lab autosg','idle_fixture_preparation'),
                     13:('sniper-id13.sav','Lab sniper','sniper_preparation')}

def preparation_target(native_id):
    if type(native_id) is not int or native_id not in PREPARATION_TARGETS:
        raise ValueError('Unsupported preparation target')
    return PREPARATION_TARGETS[native_id]

def validate_idle_probe(cfg):
    neutral_runtime=cfg.get('neutral_runtime_probe',False)
    if type(neutral_runtime) is not bool:
        raise ValueError('Neutral runtime selection must be an explicit boolean')
    if neutral_runtime and (idle_probe_weapon(cfg) is None or
                           cfg.get('display_prerequisite_only') or cfg.get('monado_window_order_probe')):
        raise ValueError('Neutral runtime check requires the prepared stationary game extent')
    alternate=cfg.get('idle_alternate_world_eyes',False)
    if type(alternate) is not bool:raise ValueError('Idle eye order must be an explicit boolean')
    if alternate and idle_probe_weapon(cfg)!=13:
        raise ValueError('Alternate world eyes requires the exact neutral ID13 collector')
    preparation_weapon(cfg)
    if idle_probe_weapon(cfg) is None:return
    if cfg.get('renderer_mode','vr')!='vr' or not isinstance(cfg.get('expected_product_source'),str) or \
       not re.fullmatch('[a-f0-9]{64}',cfg['expected_product_source']):
        raise ValueError('Idle collection requires pinned VR products')
    if any(cfg.get(k) for k in ('native_dual_probe','background_controls_probe','native_input_probe','grip_resource_probe')):
        raise ValueError('Idle collection cannot mix firing, movement or other diagnostic modes')
    if cfg.get('pose_steps')!=[{'name':'baseline','head':cfg.get('baseline_head')}]:
        raise ValueError('Idle collection requires only the stationary baseline pose')

IDLE_TOOLS=('runtime_lab.py','collect_idle_evidence.py','assess_idle_weapon.py','idle_stream_evidence.py','idle_submission_evidence.py','idle_projection_evidence.py','idle_native_reference.py','replay_idle_geometry.py',
            'measure_idle_reference.py','match_idle_geometry.py',
            'private_display_lab.py','display_timing_probe.cpp','lab_window_focus.hpp','private_x11_windows.cpp',
            'build_contract.py','runtime_observer.cpp')
IDLE_INVENTORIES=('game/Content/SeriousSam2/Config','game/Content/PlayerProfiles',
                  'prefix/pfx/drive_c/users')
IDLE_FIXED_FILES=('game/Content/SeriousSam2/Sam2.ini','game/Bin/SS2VR/SS2VR.ini',
                  'prefix/pfx/system.reg','prefix/pfx/user.reg','prefix/pfx/userdef.reg',
                  'prefix/config_info','prefix/version','prefix/tracked_files')

def idle_configuration_digest(cfg):
    # validate() adds two derived records. Neither may alter the launch selection.
    data={k:v for k,v in cfg.items() if k not in
          ('idle_preparation','compiled_product_contract','verified_scene_providers')}
    return hashlib.sha256(json.dumps(data,sort_keys=True,separators=(',',':'),allow_nan=False).encode()).hexdigest()

def validate_idle_preparation(cfg,private,lab,prefix):
    """Check one prepared private collection before any runtime process starts."""
    if not cfg.get('idle_weapon_probe') and 'idle_preparation' not in cfg:return
    if cfg.get('idle_weapon_probe') is not True:
        raise ValueError('Prepared idle collection cannot disable its neutral selector')
    selected=cfg.get('idle_preparation')
    if not isinstance(selected,dict) or set(selected)!={'path','sha256'}:
        raise ValueError('Idle collection requires its exact private preparation receipt')
    receipt=checked_file(selected['path'],selected['sha256'])
    root=receipt.parent
    if not root.is_relative_to(private) or lab!=root/'game' or prefix!=root/'prefix' or receipt.stat().st_size>1024*1024:
        raise ValueError('Idle preparation must own separate game/prefix trees')
    record=json.loads(receipt.read_text())
    if record.get('schema')!=1 or record.get('game_lab')!=str(lab) or record.get('prefix')!=str(prefix) or \
       record.get('source_fingerprint')!=cfg['expected_product_source'] or \
       record.get('configuration_sha256')!=idle_configuration_digest(cfg):
        raise ValueError('Idle preparation/configuration identity changed')
    files=record.get('files')
    if not isinstance(files,dict) or not 1<=len(files)<=4096:raise ValueError('Invalid idle preparation file inventory')
    required=set(IDLE_FIXED_FILES);links={}
    for relative in IDLE_INVENTORIES:
        base=root/relative
        if base.is_symlink():raise ValueError('Idle settings/profile root is redirected')
        if not base.exists():continue
        for path in base.rglob('*'):
            if path.is_symlink():
                if relative!='prefix/pfx/drive_c/users' or not path.resolve(strict=True).is_relative_to(base):
                    raise ValueError('Idle user-folder redirection escapes private prefix')
                links[str(path.relative_to(root))]=os.readlink(path)
            elif path.is_file():required.add(str(path.relative_to(root)))
    if len(links)>4096 or record.get('user_links')!=links:
        raise ValueError('Idle user-folder link inventory changed')
    if set(files)!=required:raise ValueError('Idle settings/profile inventory changed')
    for relative,expected in files.items():
        path=root/relative
        if path.is_symlink() or not path.resolve(strict=True).is_relative_to(root):
            raise ValueError('Idle preparation file escapes its private tree')
        checked_file(path,expected)
    mappings=prefix/'pfx/dosdevices'
    if mappings.is_symlink() or {p.name for p in mappings.iterdir()}!={'c:','z:'} or \
       not (mappings/'c:').is_symlink() or (mappings/'c:').resolve()!=prefix/'pfx/drive_c' or \
       not (mappings/'z:').is_symlink() or (mappings/'z:').resolve()!=Path('/'):
        raise ValueError('Idle private DOS mappings changed')
    if set(record.get('source_tools',{}))!=set(IDLE_TOOLS):raise ValueError('Idle tool inventory differs')
    for name,expected in record['source_tools'].items():checked_file(ROOT/'tools'/name,expected)

def validate_fixture_destination(lab,native_id):
    basename,_,_=preparation_target(native_id)
    destination=lab/'Temp/SS2VR'
    if destination.is_symlink() or not destination.is_dir() or not destination.resolve(strict=True).is_relative_to(lab.resolve()):
        raise ValueError('Fixture preparation requires its owned private save directory')
    if any((destination/name).exists() or (destination/name).is_symlink() for name in
           (basename,basename+'.preload')):
        raise ValueError('Fixture preparation cannot overwrite an existing fixture')

def fixture_preparation_receipt(log,lab,native_id):
    basename,prefix,_=preparation_target(native_id)
    if prefix+' preparation failed' in log:
        raise ValueError('Native fixture preparation rejected its fixture')
    events=list(re.finditer(re.escape(prefix)+r' preparation (?:issued stage=(grant|select|save) owner=(\d+)|complete owner=(\d+) nativeId='+str(native_id)+r' path=([^\r\n]+))',log))
    stages=[m[1] or 'complete' for m in events]
    owners=[int(m[2] or m[3]) for m in events]
    if stages!=['grant','select','save','complete'] or len(set(owners))!=1 or not 0<owners[0]<=0xffffffff:
        raise ValueError('Native fixture preparation lacks one coherent completion sequence')
    expected='Z:'+str(lab.resolve()).replace('/','\\')+'\\Temp\\SS2VR\\'+basename
    if events[-1][4].casefold()!=expected.casefold():
        raise ValueError('Native save destination differs from the owned lab')
    files={}
    for name in (basename,basename+'.preload'):
        path=lab/'Temp/SS2VR'/name
        if name.endswith('.preload') and not path.exists() and not path.is_symlink():continue
        if path.is_symlink() or not path.is_file() or not path.resolve(strict=True).is_relative_to(lab.resolve()):
            raise ValueError('Native fixture save is missing or redirected')
        if not 0<path.stat().st_size<=32*1024*1024:raise ValueError('Native fixture save exceeds its evidence budget')
        files[str(path.relative_to(lab))]={'bytes':path.stat().st_size,'sha256':digest(path)}
    return {'schema':1,'native_id':native_id,'owner':owners[0],'native_stages':stages,'files':files,
            'private_cheated_fixture':True,'save_reload_verified':False,'alignment_accepted':False}

def validate_sniper_destination(lab):
    return validate_fixture_destination(lab,13)

def sniper_preparation_receipt(log,lab):
    return fixture_preparation_receipt(log,lab,13)

def same_owned_native_game(expected_pid,state,owned):
    # Observer/IPC identifiers are Windows PIDs. /proc ownership is a separate
    # Linux incarnation check; Wine does not make those numeric domains equal.
    actual=state.get('game_pid')
    return type(expected_pid) is int and 0<expected_pid<=0xffffffff and \
        type(actual) is int and actual==expected_pid and still_owned(owned)

def validate(cfg):
    validate_idle_probe(cfg)
    private=Path(cfg['private_root']).resolve(strict=True)
    if private.is_relative_to(ROOT) or ROOT.is_relative_to(private):
        raise ValueError('Runtime data must be outside the source tree')
    lab=Path(cfg['game_lab']).resolve(strict=True)
    prefix=Path(cfg['prefix']).resolve(strict=True)
    for p in (lab,prefix):
        if not p.is_relative_to(private): raise ValueError('Lab and prefix must be private owned paths')
    validate_idle_preparation(cfg,private,lab,prefix)
    validate_display_probe(cfg,private)
    mode=cfg.get('renderer_mode','vr')
    if mode not in ('vr','stock'):raise ValueError('Unknown renderer comparison mode')
    from build_contract import read_contract, validate_observer_layout
    contracts={role:read_contract(lab/relative,role) for role,relative in
        {'game':'Bin/d3d9.dll','server':'Bin/SS2VRServer.dll','host':'Bin/SS2VR/ss2vr_host.exe'}.items()}
    reference={k:v for k,v in contracts['game'].items() if k!='component'}
    if any({k:v for k,v in value.items() if k!='component'}!=reference for value in contracts.values()):
        raise ValueError('Installed product contracts disagree')
    cfg['compiled_product_contract']=reference
    expected=cfg.get('expected_product_source')
    if mode=='stock' or expected is not None:
        if not isinstance(expected,str) or not re.fullmatch(r'[a-f0-9]{64}',expected):
            raise ValueError('Comparator requires an exact compiled source identity')
        if reference['source_fingerprint']!=expected:
            raise ValueError('Installed products do not match the pinned source')
    marker=json.loads((lab/'.ss2vr-runtime-lab.json').read_text())
    checked_file(lab/'Bin/Sam2.exe',marker['game_sha256'])
    receipt=json.loads((lab/'Bin/SS2VR/install-receipt.json').read_text())
    for name, expected in receipt['files'].items(): checked_file(lab/name,expected)
    startup=cfg['startup']
    expected_arguments=['+mod','SeriousSam2','+sam_bBootSequence','0','+sam_bSkipMovies','1','+level',cfg['scene']['entry']]
    if preparation_weapon(cfg) is not None:
        expected_arguments[6:6]=['+sam_iEnableCheats','1']
        validate_fixture_destination(lab,preparation_weapon(cfg))
    zero_mouse_arguments=expected_arguments[:6]+['+inp_fMouseSensitivity','0']+expected_arguments[6:]
    if startup['arguments'] not in (expected_arguments,zero_mouse_arguments):
        raise ValueError('Only the verified stock local +level startup is admitted')
    if not startup['evidence'] or not startup['arguments']:
        raise ValueError('Verified direct-start evidence and arguments are required')
    for item in startup['evidence']:
        path=checked_file(lab/item['relative'],item['sha256'])
        if not path.is_relative_to(lab): raise ValueError('Startup evidence escapes lab')
    scene=cfg['scene']; archive=checked_file(lab/scene['archive'],scene['archive_sha256'])
    if not archive.is_relative_to(lab): raise ValueError('Scene archive escapes lab')
    with zipfile.ZipFile(archive) as z:
        if hashlib.sha256(z.read(scene['entry'])).hexdigest()!=scene['entry_sha256']:
            raise ValueError('Owned scene bytes differ')
    # Record providers without guessing mount precedence. The successful native
    # stream receipt must later certify the configured exact scene bytes.
    providers=[]
    for candidate in lab.rglob('*'):
        if not candidate.is_file() or candidate.suffix.casefold()!='.gro': continue
        if not candidate.resolve().is_relative_to(lab): raise ValueError('External archive provider')
        with zipfile.ZipFile(candidate) as z:
            for name in z.namelist():
                if name.replace('\\','/').casefold()==scene['entry'].casefold():
                    data=z.read(name)
                    providers.append({'archive':str(candidate.relative_to(lab)),
                        'entry_sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data)})
    if not providers: raise ValueError('No owned scene provider')
    if any(str(p.relative_to(lab)).casefold()==scene['entry'].casefold() for p in lab.rglob('*') if p.is_file()):
        raise ValueError('Loose scene override is not admitted')
    cfg['verified_scene_providers']=providers
    for name in ('proton','monado_service','native_manifest','observer','pose_driver'):
        checked_file(cfg[name],cfg['fingerprints'][name])
    validate_observer_layout(cfg['observer'], reference)
    if not 1 <= cfg['remote_port'] <= 65535 or cfg['monado_config'] != {'active':'remote','remote':{'version':0,'port':cfg['remote_port'],'view_count':2}}:
        raise ValueError('Exact private remote configuration required')
    allowed_environment={'P_OVERRIDE_ACTIVE_CONFIG','SDL_VIDEODRIVER','XRT_COMPOSITOR_FORCE_XCB',
        'XRT_COMPOSITOR_COMPUTE','U_PACING_APP_USE_MIN_FRAME_PERIOD','XRT_DEBUG_GUI','XRT_CURATED_GUI',
        'XRT_COMPOSITOR_DISABLE_DEFERRED'}
    if set(cfg['monado_environment'])-allowed_environment or cfg['monado_environment'].get('P_OVERRIDE_ACTIVE_CONFIG')!='remote':
        raise ValueError('Only process-local simulation settings are admitted')
    if not 0<cfg.get('timeout',60)<=180: raise ValueError('Runtime budget must be between zero and 180 seconds')
    if cfg.get('native_input_probe') not in (None,'read-only-three-samples','owned-activation-three-samples'):
        raise ValueError('Unknown bounded native input probe')
    if cfg.get('native_input_probe') and cfg.get('renderer_mode','vr')!='vr':
        raise ValueError('Native input diagnostic requires the actual VR channel')
    if cfg.get('proton_environment',{}) not in ({},{'PROTON_NO_NTSYNC':'1'}):
        raise ValueError('Only the verified process-local Proton sync comparison is admitted')
    if cfg.get('native_dual_probe') not in (None,'zap-initial-inventory'):
        raise ValueError('Only the verified native ID1 charge/release fixture is admitted')
    if cfg.get('background_controls_probe') not in (None,'inactive-native-route','joystick-native-movement'):
        raise ValueError('Unknown background controls diagnostic')
    if cfg.get('background_controls_probe') and (cfg.get('native_dual_probe') or
        cfg.get('renderer_mode','vr')!='vr' or cfg.get('native_input_probe')):
        raise ValueError('Background controls diagnostic requires a separate non-firing VR run')
    if type(cfg.get('capture_desktop',True)) is not bool:
        raise ValueError('Desktop capture selection must be explicit boolean')
    if type(cfg.get('grip_resource_probe',False)) is not bool:
        raise ValueError('Grip resource probe selection must be explicit boolean')
    if not cfg['pose_steps'] or cfg['pose_steps'][0]['name']!='baseline' or cfg['pose_steps'][0]['head']!=cfg['baseline_head']:
        raise ValueError('First held-pose case must certify the commanded baseline')
    for head in [cfg['baseline_head'],*[step['head'] for step in cfg['pose_steps']]]:
        if len(head)!=6 or not all(isinstance(v,(int,float)) and math.isfinite(v) and abs(v)<=10 for v in head):
            raise ValueError('Invalid held-pose specification')
    expected=cfg['expected_baseline_head']
    if expected!={'p':[0,0,0],'q':[0,0,0,1]} or cfg['baseline_head']!=[0,1.6,0,0,0,0]:
        raise ValueError('Initial remote STAGE head and native LOCAL reference must match the verified baseline')
    return private,lab,prefix

def background_controls_probe(cfg,env,token,deadline,run_dir,state):
    """Bounded neutral or walking-only input; both triggers stay neutral."""
    lab=Path(cfg['game_lab']);stock_token='Z:'+str(lab/'Bin/Sam2.exe')
    record={'movement_injected':False,'fire_injected':False,'samples':[], 'inactive_route_observed':False}
    incarnation=observer(cfg,env,'stock-status',stock_token,timeout=remaining(deadline,3),deadline=deadline)
    if incarnation['game_pid']!=state['game_pid']:
        raise RuntimeError('Background probe process differs from channel')
    if state.get('trigger')!=[0,0] or not state.get('gameplay') or state.get('menu'):
        raise RuntimeError('Background probe requires neutral gameplay')
    try:
        receipt=run_dir/'background-window-request.json'
        observer(cfg,env,'stock-background',stock_token,(incarnation['process_creation'],'Z:'+str(receipt)),
                 timeout=remaining(deadline,3),deadline=deadline)
        stop=min(deadline-4,time.monotonic()+5);previous=state['input_sequence']
        while time.monotonic()<stop:
            sample=observer(cfg,env,'status',token,timeout=remaining(stop,2),deadline=stop)
            if sample.get('trigger')!=[0,0]:raise RuntimeError('Non-neutral trigger in background probe')
            if (sample['input_sequence']>previous and sample.get('native_activation_repeated') and
                sample.get('native_core_foreground')==0 and sample.get('native_input_exclusive')==0 and
                sample.get('native_host_iconic') and sample.get('focused') and sample.get('head_valid') and
                sample.get('gameplay') and not sample.get('menu') and sample['input_age_ms']<150):
                record['samples'].append(sample);previous=sample['input_sequence']
            log_path=lab/'Bin/SS2VR.log'
            rows=re.findall(r'Lab controls observation route=inactive ([^\n]+)',log_path.read_text(errors='replace'))
            matching=[r for r in rows if 'binding=1 ' in r and 'fire=0 ' in r and 'move=0,0,0' in r and
                      int(re.search(r'tick=(\d+)',r)[1])>=state['input_tick_ms']]
            if len(matching)>=3 and len(record['samples'])>=3:
                record['inactive_route_observed']=True;record['native_receipt_count']=len(matching);break
            if len(record['samples'])>3:record['samples']=record['samples'][-3:]
            time.sleep(.05)
        if not record['inactive_route_observed']:
            raise RuntimeError('No current owned inactive-controls receipt under fresh background XR input')
        if cfg['background_controls_probe']=='joystick-native-movement':
            record['movement']=background_joystick_probe(cfg,env,token,deadline,run_dir,record['samples'][-1])
            record['movement_injected']=True
        return record
    finally:
        # Normal owned-window restoration only, including failed diagnostics.
        restore=run_dir/'background-restore-request.json'
        try:
            observer(cfg,env,'stock-focus',stock_token,(incarnation['process_creation'],'Z:'+str(restore)),
                     timeout=remaining(deadline,3),deadline=deadline)
        except ObserverError as error:
            if error.code!=8:raise
            record['restore_immediate_foreground_observed']=False
        finally:
            (run_dir/'background-controls.json').write_text(json.dumps(record,indent=2))

def background_joystick_probe(cfg,env,token,deadline,run_dir,state):
    """One small walking demand/release through actual native ClientAction."""
    report={'phases':[],'body_movement_observed':False,'origin_settlement_acceptance':False,
            'fire_injected':False}
    identity=(state['session'],state['reference'],state['tracking_generation'],state['game_pid'])
    if deadline-time.monotonic()<15:raise TimeoutError('Insufficient full movement/neutral/restoration budget')
    boundary=state['input_tick_ms'];baseline=cfg['expected_baseline_head']
    def admitted(s):
        return (tuple(s[k] for k in ('session','reference','tracking_generation','game_pid'))==identity and
            s['focused'] and s['head_valid'] and s['gameplay'] and not s['menu'] and s['input_age_ms']<150 and
            s.get('native_activation_repeated') and s.get('native_input_exclusive')==0 and
            s.get('native_core_foreground')==0 and s.get('health',0)>0 and s.get('wheel_open')==[0,0] and
            s.get('trigger')==[0,0] and s.get('fire_sequence')==[0,0] and
            max(abs(a-b) for a,b in zip(s['head']['p'],baseline['p']))<.001 and
            abs(sum(a*b for a,b in zip(s['head']['q'],baseline['q'])))>.99999)
    try:
        for name,y,duration in [('walk',.3,.6),('release',0,1.2)]:
            subprocess.run([cfg['pose_driver'],str(cfg['remote_port']),*map(str,cfg['baseline_head']),
                '--move','0',str(y),'0','0'],check=True,timeout=remaining(deadline,3))
            observations=[];previous=state['input_sequence'];stop=None;confirm=time.monotonic()+3
            while stop is None or time.monotonic()<stop:
                if stop is None and time.monotonic()>confirm:raise TimeoutError('Movement axis not confirmed')
                s=observer(cfg,env,'status',token,timeout=remaining(deadline,2),deadline=deadline)
                if not admitted(s):raise RuntimeError('Non-firing background movement lost fixture admission')
                if s['input_sequence']>previous:
                    previous=s['input_sequence']
                    axes=s.get('axes');expected_axes=[0,y,0,0]
                    if axes and max(abs(v-e) for v,e in zip(axes[0]+axes[1],expected_axes))<.001:
                        if stop is None:stop=time.monotonic()+duration
                        observations.append(s)
                    elif stop is not None:raise RuntimeError('Held movement axes changed after confirmation')
                time.sleep(.02)
            endpoint=observer(cfg,env,'status',token,timeout=remaining(deadline,2),deadline=deadline)
            if not admitted(endpoint) or not endpoint.get('axes') or max(abs(v-e) for v,e in
                    zip(endpoint['axes'][0]+endpoint['axes'][1],[0,y,0,0]))>=.001:
                raise RuntimeError('Movement endpoint lost matching admitted axes')
            observations.append(endpoint)
            if len(observations)<3:raise RuntimeError('Movement phase lacks fresh matching axis observations: '+name)
            state=observations[-1];report['phases'].append({'name':name,'observations':observations})
        endpoint_tick=report['phases'][-1]['observations'][-1]['input_tick_ms']+report['phases'][-1]['observations'][-1]['input_age_ms']
        log_path=Path(cfg['game_lab'])/'Bin/SS2VR.log';receipt_deadline=min(deadline-4,time.monotonic()+2)
        while True:
            native=log_path.read_text(errors='replace')
            if 'Lab controls observation saturated route=inactive' in native:
                raise RuntimeError('Native movement trace budget exhausted')
            rows=re.findall(r'Lab controls observation route=inactive ([^\n]+)',native)
            if rows and int(re.search(r'tick=(\d+)',rows[-1])[1])>=endpoint_tick:break
            if time.monotonic()>=receipt_deadline:raise TimeoutError('Native receipts do not reach release endpoint')
            time.sleep(.02)
        rows=[r for r in rows if int(re.search(r'tick=(\d+)',r)[1])>=boundary]
        if len(rows)<6 or any('binding=1 ' not in r or 'fire=0 ' not in r or 'bodyValid=1 ' not in r or
                             'completed=1' not in r for r in rows):
            raise RuntimeError('Movement lacks owned zero-fire actual native body receipts')
        def field(r,name):return re.search(r'(?:^| )'+name+r'=([^ ]+)',r)[1]
        if any(tuple(int(field(r,k)) for k in ('session','reference','generation'))!=identity[:3] for r in rows):
            raise RuntimeError('Native movement receipt differs from admitted XR identity')
        owners={(field(r,'brain'),field(r,'player'),field(r,'mechanism'),field(r,'root'),field(r,'session'),field(r,'reference'),
                 field(r,'generation'),field(r,'rig'),field(r,'origin'),field(r,'turn')) for r in rows}
        if len(owners)!=1:raise RuntimeError('Movement changed owner/origin/basis during probe')
        bodies=[list(map(float,field(r,'body').split(','))) for r in rows]
        if not all(math.isfinite(v) for b in bodies for v in b):raise RuntimeError('Invalid native body receipt')
        walk_start=report['phases'][0]['observations'][0]['input_sequence']
        walk_end=report['phases'][0]['observations'][-1]['input_sequence']
        moving=[r for r in rows if walk_start<=int(field(r,'sequence'))<=walk_end]
        if len(moving)<3:raise RuntimeError('Stick demand did not reach inactive native movement')
        for r in moving:
            turn=float(field(r,'turn'));actual=list(map(float,field(r,'move').split(',')))
            if len(actual)!=3 or not math.isfinite(turn) or not all(math.isfinite(v) for v in actual):
                raise RuntimeError('Nonfinite or malformed native movement receipt')
            expected=[-.3*math.sin(turn),0,-.3*math.cos(turn)]
            if max(abs(v-e) for v,e in zip(actual,expected))>.001:
                raise RuntimeError('Native movement direction/magnitude differs from commanded stick')
        release_sequence=report['phases'][-1]['observations'][0]['input_sequence']
        released=[r for r in rows if int(field(r,'sequence'))>=release_sequence]
        if len(released)<3 or any(any(float(v)!=0 for v in field(r,'move').split(',')) for r in released):
            raise RuntimeError('Neutral did not clear native movement demand')
        distance=math.hypot(bodies[-1][0]-bodies[0][0],bodies[-1][2]-bodies[0][2])
        before=[r for r in released if int(field(r,'tick'))<=endpoint_tick-250]
        after=[r for r in released if int(field(r,'tick'))>=endpoint_tick]
        if not before or not after:raise RuntimeError('Cessation receipts do not bracket full final250ms')
        start=int(field(before[-1],'tick'));end_tick=int(field(after[0],'tick'))
        settled=[r for r in released if start<=int(field(r,'tick'))<=end_tick]
        ticks=[int(field(r,'tick')) for r in settled]
        if (len(settled)<3 or ticks[0]<endpoint_tick-350 or ticks[-1]>endpoint_tick+100 or
                any(b-a>100 or b<=a for a,b in zip(ticks,ticks[1:]))):
            raise RuntimeError('Cessation receipt coverage has a gap or misses endpoint')
        end=list(map(float,field(settled[-1],'body').split(',')))
        drift=max(math.hypot(float(field(r,'body').split(',')[0])-end[0],
                             float(field(r,'body').split(',')[2])-end[2]) for r in settled)
        if distance<.02 or drift>.02:raise RuntimeError('Actual native body movement/cessation not established')
        report.update(body_movement_observed=True,horizontal_displacement_m=distance,
                      last_250ms_horizontal_drift_m=drift,release_endpoint_tick=endpoint_tick,
                      cessation_receipt_ticks=ticks,native_receipts=len(rows),native_owner=list(owners)[0])
        return report
    finally:
        try:
            subprocess.run([cfg['pose_driver'],str(cfg['remote_port']),*map(str,cfg['baseline_head']),
                '--move','0','0','0','0'],check=True,timeout=3)
        except Exception as error:
            report['neutral_release_error']=str(error)
            raise RuntimeError('Movement probe final neutral release failed') from error
        finally:
            (run_dir/'background-movement.json').write_text(json.dumps(report,indent=2))

def native_dual_probe(cfg,env,token,deadline,run_dir,state):
    """Bounded unchanged-inventory Zap probe via ordinary simulated input.

    Native receipts and visible impacts need subsequent review. This function
    never grants weapons, changes native eligibility or edits command history.
    """
    steps=[('neutral',0,0,2),('right',0,1,2),('neutral-after-right',0,0,3),
        ('left',1,0,2),('neutral-after-left',0,0,3),('both-first',1,1,2),
        ('right-retained',0,1,2),('neutral-after-both',0,0,3),('both-second',1,1,2),
        ('left-retained',1,0,2),('final-neutral',0,0,3)]
    identity_key=dual_identity(state)
    def ready(value):
        return (value['focused'] and value['head_valid'] and value['gameplay'] and not value['menu'] and
            value.get('health',0)>0 and value.get('current_weapon')==[1,1] and
            value.get('native_activation_repeated') and value.get('native_input_enabled') and
            value.get('native_input_exclusive') and value.get('native_core_foreground') and value.get('native_host_foreground') and
            value.get('hand_valid')==[1,1] and value.get('primary_active_mask')==3 and
            value.get('wheel_open')==[0,0] and value['input_age_ms']<=150 and
            value['input_tick_ms']+value['input_age_ms']>=value.get('ui_tick_ms',0) and
            value['input_tick_ms']+value['input_age_ms']-value.get('ui_tick_ms',0)<=250 and
            value.get('ui_tracking_generation')==value.get('tracking_generation') and
            dual_identity(value)==identity_key)
    report={'result':'incomplete','steps':[],'native_compatibility_acceptance':False}
    prior=[0,0];cycles={};last_release_tick=0
    def send(left,right):
        subprocess.run([cfg['pose_driver'],str(cfg['remote_port']),*map(str,cfg['baseline_head']),
            str(left),str(right),'-0.2','0.2'],check=True,timeout=remaining(deadline,3))
    def native_trace():
        text=(Path(cfg['game_lab'])/'Bin/SS2VR.log').read_text(errors='replace')
        if 'Lab native weapon trace saturated' in text:
            raise RuntimeError('Native dual observation trace saturated; evidence incomplete')
        return text
    try:
        if not ready(state):raise RuntimeError('Native dual Zap fixture lacks fresh owned two-hand/native foreground readiness')
        for name,left,right,duration in steps:
            boundary=state['input_sequence'];send(left,right)
            stop=min(deadline,time.monotonic()+duration);phase=DualPhaseEvidence(boundary,[left,right])
            while time.monotonic()<stop:
                state=observer(cfg,env,'status',token,timeout=remaining(deadline,2),deadline=deadline)
                if not ready(state):raise RuntimeError('Native dual probe lost its fixture/identity/readiness')
                phase.observe(state)
                time.sleep(.02)
            phase.require_progress();observations=phase.observations
            for hand,high in enumerate((left,right)):
                if high and not prior[hand]:cycles[hand]=phase.confirmed_boundary()
            report['steps'].append({'name':name,'input_boundary':boundary,
                'end_sequence':observations[-1]['input_sequence'],'expected_trigger':[left,right],'observations':observations})
            (run_dir/'native-dual-probe.json').write_text(json.dumps(report,indent=2))
            if left or right:
                native=native_trace()
                topologies=dual_topologies(native,identity_key,boundary,observations[-1]['input_sequence'])
                if not topologies:raise RuntimeError('Native independent two-hand topology was not compatible')
            report['steps'][-1]['successful_fire_hands_in_phase']=sorted(successful_dual_fire(
                native_trace(),identity_key,boundary,state['input_sequence'],dual_topologies(
                    native_trace(),identity_key,boundary,state['input_sequence'])))
            if left or right or name=='final-neutral':
                q=cfg['expected_baseline_head']['q'];p=cfg['expected_baseline_head']['p']
                capture=run_dir/('dual-'+name)
                observer(cfg,env,'capture',token,(capture,observations[0]['input_sequence'],*p,*q),timeout=remaining(deadline),deadline=deadline)
                metadata=json.loads(Path(str(capture)+'-metadata.json').read_text())
                state=observer(cfg,env,'status',token,timeout=remaining(deadline,2),deadline=deadline)
                if not ready(state):raise RuntimeError('Native dual capture lost fixture/identity/readiness')
                phase.observe(state)
                require_dual_capture(metadata,phase,identity_key,state)
                report['steps'][-1]['capture']=metadata
                report['steps'][-1]['end_sequence']=state['input_sequence']
                native_trace()
            closed={hand for hand,high in enumerate((left,right)) if prior[hand] and not high}
            for hand in closed:
                native=native_trace();start=cycles.pop(hand);end=state['input_sequence']
                topologies=dual_topologies(native,identity_key,start,end)
                bindings={(v['owner'],tuple(v['weapon']),tuple(v['receiver'])) for v in topologies}
                if len(bindings)!=1:raise RuntimeError('Zap cycle lacks one stable compatible native topology')
                releases=[v for v in dual_weapon_events(native,identity_key,boundary,end,topologies,'release')
                    if v['hand']==hand and v['trigger'][hand]<.01]
                if not releases or hand not in successful_dual_fire(native,identity_key,start,end,topologies):
                    raise RuntimeError('Zap press/release cycle lacks successful native fire/release receipts: '+name)
                last_release_tick=max(last_release_tick,max(v['completion_tick'] for v in releases))
                report['steps'][-1].setdefault('closed_cycles',[]).append({'hand':hand,'start_input':start,
                    'end_input':end,'normal_release_receipts':releases})
            if not left and not right and name!='neutral':phase.require_quiet_after(last_release_tick)
            prior=[left,right]
            native_trace()
        if cycles:raise RuntimeError('Native Zap probe ended with an unclosed ordinary input cycle')
        native_trace()
        report['result']='ordinary_input_sequence_observed_native_receipts_unreviewed'
    finally:
        # Release ordinary synthetic controls even if the runtime deadline expired.
        # This bounded release is not a native history/gesture manipulation.
        try:
            subprocess.run([cfg['pose_driver'],str(cfg['remote_port']),*map(str,cfg['baseline_head']),
                '0','0','-0.2','0.2'],check=True,timeout=3)
        except Exception as error:
            report['release_error']=str(error);report['result']='incomplete'
        (run_dir/'native-dual-probe.json').write_text(json.dumps(report,indent=2))
    return report

def finish_idle_capture(cfg,lab,run_dir,manifest):
    """Select postprocessing only; the caller retains the existing run/finally."""
    if cfg.get('neutral_runtime_probe') is True:
        manifest['result']='native_neutral_captured'
        return True
    if cfg.get('idle_weapon_probe'):
        from assess_idle_weapon import assess
        log=(lab/'Bin/SS2VR.log').read_text(errors='strict')
        if len(log.encode())>16*1024*1024:raise ValueError('Idle log exceeds the bounded reader budget')
        evidence=assess(log,cfg['compiled_product_contract']['source_fingerprint'])
        path=run_dir/'idle-event-pose-geometry.json';path.write_text(json.dumps(evidence,indent=2)+'\n')
        manifest['idle_collection']={'schema':evidence['schema'],'evidence_file':path.name,
            'complete_event_pose_observations':len(evidence['copied_event_pose_observations']),
            'rejected_observations':len(evidence['rejected_or_missing_observations']),
            'copied_geometry_draws':sum(o['draws'] for o in evidence['copied_event_pose_observations']),
            'positive_grasp_verified':False,'alignment_accepted':False}
        manifest['result']='idle_collection_copied_unreviewed'
        return True
    return False

def run(cfg):
    private,lab,prefix=validate(cfg)
    stock=cfg.get('renderer_mode','vr')=='stock'
    # A copied directory does not isolate native Steam remote mutations. Keep
    # actual game launch closed until the reviewed early process-local adapter
    # is compiled and explicitly selected in this private configuration.
    if cfg.get('online_isolation')!='native-init-barrier-verified':
        raise RuntimeError('Native save/profile isolation is not verified; no game launched')
    if running('Sam2.exe') or running('ss2vr_host.exe'):
        raise RuntimeError('An existing game or host is running; no duplicate test will start')
    observer_exclusions={(i['pid'],i['start']) for i in private_processes(cfg['observer'])}
    run_dir=private/'runs'/time.strftime('%Y%m%dT%H%M%S')
    run_dir.mkdir(parents=True,exist_ok=False)
    cfg['observer_output_dir']=str(run_dir/'observations')
    Path(cfg['observer_output_dir']).mkdir(mode=0o700)
    runtime=run_dir/'runtime';runtime.mkdir(mode=0o700)
    config_dir=run_dir/'config';config_dir.mkdir(mode=0o700)
    manifest={'source_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
              'source_diff_sha256':hashlib.sha256(subprocess.check_output(['git','diff','HEAD'],cwd=ROOT)).hexdigest(),
              'products':json.loads((lab/'Bin/SS2VR/install-receipt.json').read_text()),
              'harness_sha256':digest(Path(__file__)), 'runtime_helpers':cfg['fingerprints'],
              'scene':cfg['scene'], 'startup':cfg['startup'],
              'validated_config':cfg,
              'harness_sources':{name:digest(ROOT/'tools'/name) for name in
                  ('runtime_lab.py','runtime_observer.cpp','monado_pose_driver.cpp','assess_runtime.py')},
              'simulation':not stock,'renderer_mode':'stock' if stock else 'vr','hardware_acceptance':False,
              'game_launch_requested':False,'monado_launch_requested':False,'result':'incomplete'}
    last_focus=0.0;last_state=None;loading_continue_sent=False;stock_creation=None
    loading_receipt=None;last_native_focus=0;native_focus_requests=0
    stock_token='Z:'+str(lab/'Bin/Sam2.exe')
    master,slave=pty.openpty(); service=None;launch=None;owned_game=None;owned_host=None;token=None;owned_window=None
    env=os.environ.copy()
    env.update({'STEAM_COMPAT_DATA_PATH':str(prefix),'STEAM_COMPAT_CLIENT_INSTALL_PATH':cfg['steam'],
        'SteamAppId':'204340','SteamGameId':'204340','XDG_RUNTIME_DIR':str(runtime),
        'XDG_CONFIG_HOME':str(config_dir),'XR_RUNTIME_JSON':cfg['native_manifest'],
        'WINEXR_RUNTIME_JSON':r'C:\openxr\wineopenxr64.json','VR_OVERRIDE':cfg['xrizer'],
        'LD_LIBRARY_PATH':cfg['runtime_libraries'],'WINEDLLOVERRIDES':'d3d9=n,b;d3d11,dxgi=n',
        'PROTON_LOG':'1','PROTON_LOG_DIR':str(run_dir),'SS2VR_LAB_TRACE':'1','SS2VR_LAB_ISOLATE_ONLINE':'1','SS2VR_LAB_SCENE':cfg['scene']['entry']})
    env.update(cfg['monado_environment'])
    env.update(cfg.get('proton_environment',{}))
    env.pop('SS2VR_LAB_STOCK_RENDER',None)
    env.pop('SS2VR_LAB_BACKGROUND_MOVE',None)
    if cfg.get('background_controls_probe')=='joystick-native-movement':env['SS2VR_LAB_BACKGROUND_MOVE']='1'
    env.pop('SS2VR_LAB_IDLE_WEAPON',None)
    selected_idle_weapon=idle_probe_weapon(cfg)
    if selected_idle_weapon is not None:env['SS2VR_LAB_IDLE_WEAPON']=str(selected_idle_weapon)
    env.pop('SS2VR_LAB_ALTERNATE_EYES',None)
    if cfg.get('idle_alternate_world_eyes'):env['SS2VR_LAB_ALTERNATE_EYES']='1'
    env.pop('SS2VR_LAB_PREPARE_IDLE',None)
    env.pop('SS2VR_LAB_PREPARE_SNIPER',None)
    if cfg.get('prepare_sniper_fixture'):env['SS2VR_LAB_PREPARE_SNIPER']='1'
    elif cfg.get('prepare_idle_fixture'):env['SS2VR_LAB_PREPARE_IDLE']=str(preparation_weapon(cfg))
    env.pop('SS2VR_LAB_GRIP_RESOURCES',None)
    if cfg.get('grip_resource_probe'):env['SS2VR_LAB_GRIP_RESOURCES']='1'
    if stock:env['SS2VR_LAB_STOCK_RENDER']='1'
    files=[]; cleanup_errors=[]
    deadline=time.monotonic()+cfg.get('timeout',60)
    try:
        # Proton consumes OpenVR paths before processing VR_OVERRIDE. Preserve
        # its bootstrap inside this run's private config, not global defaults.
        openvr=config_dir/'openvr';openvr.mkdir()
        (openvr/'openvrpaths.vrpath').write_text(json.dumps({
            'version':1,'jsonid':'vrpathreg','runtime':[cfg['xrizer']],
            'config':[str(openvr)],'log':[str(run_dir)]}))
        # One validated run extent: prefix bootstrap may mutate this private
        # prefix, so do not run a probe externally and reseal settings afterward.
        manifest['display_timing']=display_timing_probe(cfg,env,deadline,run_dir)
        if cfg.get('display_prerequisite_only') is True and not cfg.get('monado_window_order_probe'):
            # A sealed diagnostic extent ends here even on successful readiness.
            # Existing finally still owns retirement and result preservation.
            manifest.update(result='display_prerequisite_only',simulation=False,
                            game_launch_requested=False,monado_launch_requested=False)
            return run_dir
        config=(config_dir/'monado');config.mkdir()
        (config/'config_v0.json').write_text(json.dumps(cfg['monado_config']))
        if not stock:
            f=(run_dir/'monado.log').open('xb');files.append(f)
            manifest['monado_launch_requested']=True
            service=subprocess.Popen([cfg['monado_service']],stdin=slave,stdout=f,stderr=subprocess.STDOUT,env=env,start_new_session=True)
            os.close(slave);slave=-1
            while not (runtime/'monado_comp_ipc').exists():
                if service.poll() is not None:raise RuntimeError('Private Monado startup failed')
                if time.monotonic()>deadline:raise TimeoutError('Private Monado socket not ready')
                time.sleep(.1)
            # The native upstream listener is patched to loopback only. Certify the
            # actual listening inode belongs to this exact private service before control.
            while time.monotonic()<deadline:
                sockets=set()
                for p in Path('/proc',str(service.pid),'fd').iterdir():
                    try:sockets.add(p.readlink().name)
                    except OSError:pass
                listeners=[]
                for line in Path('/proc/net/tcp').read_text().splitlines()[1:]:
                    fields=line.split();address,port=fields[1].split(':')
                    if int(port,16)==cfg['remote_port'] and fields[3]=='0A':
                        listeners.append((address,fields[9]))
                if listeners:
                    if len(listeners)!=1 or listeners[0][0]!='0100007F' or ('socket:['+listeners[0][1]+']') not in sockets:
                        raise RuntimeError('Remote listener is not private-service loopback owned')
                    break
                if service.poll() is not None:raise RuntimeError('Private runtime stopped')
                time.sleep(.1)
            else:raise TimeoutError('Private exact-pose listener not ready')
            subprocess.run([cfg['pose_driver'],str(cfg['remote_port']),*map(str,cfg['baseline_head'])],check=True,timeout=remaining(deadline,5))
            if cfg['monado_environment'].get('XRT_COMPOSITOR_DISABLE_DEFERRED')=='1':
                service_owner=identity(service.pid)
                if not service_owner:raise RuntimeError('Private Monado ownership unavailable')
                manifest['monado_owner']={key:service_owner[key] for key in ('pid','start')}
                window_deadline=min(deadline,time.monotonic()+15)
                while True:
                    if not still_owned(service_owner):raise RuntimeError('Private Monado incarnation changed before query')
                    snapshot=private_window_snapshot(cfg,env,window_deadline,run_dir/'monado-before-game.json')
                    try:monado_xid=require_owned_monado_window(snapshot,service_owner)
                    except RuntimeError:
                        if service.poll() is not None or time.monotonic()>=window_deadline:raise
                        time.sleep(.05)
                    else:break
                manifest['monado_graphical_window_ready']=True
                if cfg.get('monado_window_order_probe'):
                    manifest.update(game_launch_requested=False,monado_launch_requested=True)
                    if snapshot['active_xid']!=monado_xid:raise RuntimeError('Window-order diagnostic lacks initial Monado foreground')
                    order_dir=run_dir/'window-order';order_dir.mkdir()
                    def observe_order(target,rows):
                        if not still_owned(service_owner):raise RuntimeError('Private Monado incarnation changed before query')
                        owners=private_processes(Path(cfg['display_timing_probe']['path']))
                        snapshot=private_window_snapshot(cfg,env,deadline,order_dir/'during-probe.json')
                        require_owned_monado_window(snapshot,service_owner,monado_xid)
                        # Proton's launcher may also carry the exact executable
                        # argv. Select only the actual resource-owning process;
                        # do not equate its Linux PID with the native Windows PID.
                        candidates=[row for row in snapshot['windows'] if row['role']==2 and row['pid'] in {owner['pid'] for owner in owners} and row['viewable'] and
                                    row['input_output'] and row['width']>0 and row['height']>0]
                        if len(candidates)!=1:raise RuntimeError('Ambiguous native display-probe window owner')
                        owner=next(owner for owner in owners if owner['pid']==candidates[0]['pid'])
                        if not still_owned(owner):raise RuntimeError('Native display-probe incarnation changed during query')
                        if len(candidates)!=1 or snapshot['active_xid']!=candidates[0]['xid'] or \
                           not target['baseline_foreground'] or not rows[-1]['probe_title_matches'] or \
                           rows[-1]['foreground_hwnd']!=target['target_hwnd'] or rows[-1]['foreground_owner']!=target['probe_pid']:
                            raise RuntimeError('Later native probe did not acquire private foreground')
                        manifest['monado_window_order']={'monado_xid':monado_xid,'probe_xid':candidates[0]['xid'],
                            'probe_owner':{key:owner[key] for key in ('pid','start')},'native_target':target}
                    manifest['ordered_display_timing']=display_timing_probe(cfg,env,deadline,order_dir,observe_order)
                    if not still_owned(service_owner):raise RuntimeError('Private Monado incarnation changed before query')
                    after=private_window_snapshot(cfg,env,deadline,order_dir/'after-probe.json')
                    require_owned_monado_window(after,service_owner,monado_xid)
                    manifest.update(result='monado_window_order_prerequisite_only',simulation=True)
                    return run_dir
        # Existing runtime logs are preserved before each new process opens them.
        for path in (lab/'Bin/SS2VR.log',lab/'Bin/SS2VR/ss2vr_host.log',lab/'Sam2.log'):
            if path.exists():
                # Reversible move gives the fresh launch a new log boundary.
                path.rename(run_dir/('previous-'+path.name))
        f=(run_dir/'launch.log').open('xb');files.append(f)
        manifest['game_launch_requested']=True
        launch=subprocess.Popen([cfg['proton'],'run',str(lab/'Bin/Sam2.exe'),*cfg['startup']['arguments']],
            cwd=lab,env=env,stdout=f,stderr=subprocess.STDOUT)
        while time.monotonic()<deadline:
            games=[i for i in running('Sam2.exe') if game_owned(i,lab)]
            if len(games)>1:raise RuntimeError('Ambiguous game process ownership')
            if games:
                owned_game=games[0]
                manifest['owned_game']={key:owned_game[key] for key in ('pid','start')}
                if not cfg.get('native_input_probe') and (owned_window is None or ((not token or (last_state and last_state.get('menu',stock))) and time.monotonic()-last_focus>1)):
                    # X11 calls can block; keep them in a deadline-bounded child.
                    focus=subprocess.run([sys.executable,str(Path(__file__)),
                        '--focus-owned',str(owned_game['pid']),'--start',owned_game['start'],'--lab',str(lab)],
                        capture_output=True,text=True,timeout=remaining(deadline,2))
                    if focus.returncode:
                        # Preserve the concrete child failure in private evidence;
                        # a generic focus error cannot diagnose native startup.
                        manifest['focus_failure']={'returncode':focus.returncode,
                            'stdout':focus.stdout[-4096:],'stderr':focus.stderr[-4096:]}
                        raise RuntimeError('Owned-game focus helper failed')
                    owned_window=json.loads(focus.stdout);last_focus=time.monotonic()
            hosts=running('ss2vr_host.exe')
            for h in hosts:
                normalized=b' '.join(h['argv']).decode(errors='replace').replace('\\','/').casefold()
                if str(lab/'Bin/SS2VR/ss2vr_host.exe').casefold() not in normalized:continue
                args=[v.decode(errors='replace') for v in h['argv']]
                if '--channel' in args:
                    token=args[args.index('--channel')+1];owned_host=h
            # Native background/minimized loading can precede the presentation
            # that starts the host. Restore the verified game window without
            # requiring that later IPC channel; confirmation still needs it.
            if (not stock and not token and owned_game and not cfg.get('native_input_probe') and
                native_focus_requests<4 and time.monotonic()-last_native_focus>=2):
                incarnation=observer(cfg,env,'stock-status',stock_token,timeout=remaining(deadline,3),deadline=deadline)
                if incarnation.get('loading_native_ready')==1:
                    receipt=run_dir/('early-activation-'+str(native_focus_requests)+'.json')
                    attempt=recorded_native_focus(cfg,env,stock_token,incarnation['process_creation'],receipt,deadline)
                    native_focus_requests+=1
                    manifest['native_focus_requests']=native_focus_requests
                    manifest.setdefault('native_activation_requests',[]).append(attempt)
                    if receipt.exists():manifest.setdefault('early_activation_requests',[]).append(json.loads(receipt.read_text()))
                last_native_focus=time.monotonic()
            if stock and owned_game and owned_window:
                if owned_host:raise RuntimeError('Stock comparator unexpectedly started a VR host')
                token=stock_token
                try:state=observer(cfg,env,'stock-status',token,timeout=remaining(deadline),deadline=deadline)
                except ObserverError as error:
                    if error.code==4:
                        time.sleep(.05);continue
                    raise
                if stock_creation is None:stock_creation=state['process_creation']
                if state['process_creation']!=stock_creation:raise RuntimeError('Stock process incarnation changed')
                last_state=state
                native_log=(lab/'Bin/SS2VR.log').read_text(errors='replace')
                if 'Native VR hooks attached' in native_log:raise RuntimeError('Stock reference has native VR hooks')
                receipts=re.findall(r'Lab native scene stream bytes=(\d+) positionRestored=(\d+) sha256=([a-f0-9]{64})',native_log)
                isolated=('Lab native online initializer suppressed; interface=0;' in native_log and
                    'Lab stock renderer selected before VR worker creation; graphics hooks disabled' in native_log)
                scene=(lab/'Sam2.log').read_text(errors='replace') if (lab/'Sam2.log').exists() else ''
                if state['loading_ready'] and not loading_continue_sent:
                    if not isolated or not receipts or any(h!=cfg['scene']['entry_sha256'] for size,pos,h in receipts):
                        raise RuntimeError('Stock continuation lacks consistent scene/isolation evidence')
                    try:observer(cfg,env,'stock-continue-loading',token,(stock_creation,),timeout=remaining(deadline),deadline=deadline)
                    except ObserverError as error:
                        if error.code==8:continue # Rejected before any input was posted.
                        raise
                    loading_continue_sent=True
                    manifest['loading_continue_messages_posted']=True
                if (isolated and state['local_transport'] and state['online_interface_null'] and receipts and
                    state['menu_clear'] and state['camera_repeated_equal'] and state['view_pose_match'] and state['perspective_projection'] and
                    ('+inp_fMouseSensitivity' not in cfg['startup']['arguments'] or state['mouse_zero']) and
                    all(h==cfg['scene']['entry_sha256'] for size,pos,h in receipts) and
                    re.search(r"Started simulation on '"+re.escape(cfg['scene']['entry'])+r"'",scene)):
                    (run_dir/'ready.json').write_text(json.dumps(state,indent=2));break
            elif token:
                try:state=observer(cfg,env,'status',token,timeout=remaining(deadline),deadline=deadline)
                except ObserverError as error:
                    if error.code==4:
                        time.sleep(.05);continue
                    raise
                last_state=state
                if cfg.get('native_input_probe'):
                    if state.get('native_state_repeated') and state.get('loading_native_ready')==1:
                        samples=manifest.setdefault('native_input_samples',[]);samples.append(state)
                        if len(samples)==1 and cfg['native_input_probe']=='owned-activation-three-samples':
                            if not (state.get('native_activation_repeated') and state.get('native_input_enabled') and
                                state.get('native_running') and state.get('native_simulation_present') and state.get('native_exclusive_block')==0):
                                raise RuntimeError('Native activation probe lacks verified eligible owned state')
                            incarnation=observer(cfg,env,'stock-status',stock_token,timeout=remaining(deadline),deadline=deadline)
                            if incarnation['game_pid']!=state['game_pid']:raise RuntimeError('Activation owner differs from channel')
                            receipt=run_dir/'activation-request.json'
                            try:observer(cfg,env,'stock-focus',stock_token,(incarnation['process_creation'],'Z:'+str(receipt)),timeout=remaining(deadline),deadline=deadline)
                            except ObserverError as error:
                                if error.code!=8:raise
                            if receipt.exists():manifest['activation_request']=json.loads(receipt.read_text())
                            manifest['activation_attempted_once']=True
                        if len(samples)==3:
                            manifest['game_pid']=owned_game['pid'];manifest['host_pid']=owned_host['pid'] if owned_host else None
                            manifest['result']='native_input_state_sampled_no_controls_injected'
                            return run_dir
                    time.sleep(.2);continue
                if (state.get('loading_native_ready')==1 and not state.get('loading_ready') and
                    native_focus_requests<4 and time.monotonic()-last_native_focus>=2):
                    incarnation=observer(cfg,env,'stock-status',stock_token,timeout=remaining(deadline),deadline=deadline)
                    if incarnation['game_pid']!=state['game_pid']:raise RuntimeError('Native focus owner differs from the channel')
                    receipt=run_dir/('activation-'+str(native_focus_requests)+'.json')
                    attempt=recorded_native_focus(cfg,env,stock_token,incarnation['process_creation'],receipt,deadline,timeout=20)
                    native_focus_requests+=1;last_native_focus=time.monotonic()
                    manifest['native_focus_requests']=native_focus_requests
                    manifest.setdefault('native_activation_requests',[]).append(attempt)
                if state.get('loading_ready') and not loading_continue_sent:
                    native_log=(lab/'Bin/SS2VR.log').read_text(errors='replace')
                    native_receipts=re.findall(r'Lab native scene stream bytes=(\d+) positionRestored=(\d+) sha256=([a-f0-9]{64})',native_log)
                    if ('Lab native online initializer suppressed; interface=0;' not in native_log or
                        not native_receipts or any(h!=cfg['scene']['entry_sha256'] for size,pos,h in native_receipts)):
                        raise RuntimeError('Loading continuation lacks consistent native scene/isolation evidence')
                    receipt=run_dir/'loading-post.json'
                    try:observer(cfg,env,'continue-loading',token,('Z:'+str(lab/'Bin/Sam2.exe'),'Z:'+str(receipt)),timeout=remaining(deadline),deadline=deadline)
                    except ObserverError as error:
                        if error.code==8:continue # Rejected before any input was posted.
                        raise
                    loading_continue_sent=True
                    manifest['loading_continue_messages_posted']=True
                    loading_receipt=json.loads(receipt.read_text())
                    manifest['loading_post_receipt']=loading_receipt
                if (loading_receipt and state.get('native_state_repeated') and
                    state.get('native_simulation')==loading_receipt['native_simulation'] and
                    state.get('native_world_start_blocked')==0 and state.get('native_current_menu')==0):
                    manifest['loading_native_transition_observed']=True
                if state['renderer'] and state['gameplay'] and not state['menu'] and state['focused'] and state['head_valid']:
                    (run_dir/'ready.json').write_text(json.dumps(state,indent=2));break
            if launch.poll() is not None and not games:raise RuntimeError('Game exited before world readiness')
            time.sleep(.2)
        else:raise TimeoutError('Verified scene did not reach gameplay readiness')
        manifest['game_pid']=owned_game['pid'];manifest['host_pid']=owned_host['pid'] if owned_host else None
        scene_log=(lab/'Sam2.log').read_text(errors='replace')
        if not re.search(r"Started simulation on '"+re.escape(cfg['scene']['entry'])+r"'",scene_log):
            raise RuntimeError('Fresh native log does not certify requested simulation')
        manifest['loaded_scene_confirmed']=True
        isolation_log=(lab/'Bin/SS2VR.log').read_text(errors='replace')
        if 'Lab native online initializer suppressed; interface=0;' not in isolation_log:
            raise RuntimeError('Fresh native log does not certify early online isolation')
        if (not stock and 'Lab local gameplay observed with online interface=0' not in isolation_log) or \
            (stock and not (state['local_transport'] and state['online_interface_null'])):
            raise RuntimeError('Fresh native local gameplay/isolation invariant not observed')
        receipts=re.findall(r'Lab native scene stream bytes=(\d+) positionRestored=(\d+) sha256=([a-f0-9]{64})',isolation_log)
        if not receipts: raise RuntimeError('No successful native scene-stream receipt')
        if any(h!=cfg['scene']['entry_sha256'] for size,pos,h in receipts):
            manifest['native_scene_receipts']=receipts
            raise RuntimeError('Actual native scene differs from the configured exact scene')
        manifest['native_scene_receipts']=receipts
        if cfg.get('grip_resource_probe'):
            manifest['native_grip_resource_receipts']=native_grip_resource_receipts(isolation_log)
            manifest['rendered_grip_alignment_verified']=False
        if 'Steam initialize (AppID' in scene_log or 'from Steam cloud:' in scene_log:
            raise RuntimeError('Native Steam interface unexpectedly initialized')
        if stock:
            (run_dir/'stock-camera-before.json').write_text(json.dumps(state,indent=2))
            desktop=run_dir/'stock-desktop.png'
            subprocess.run([sys.executable,str(Path(__file__)),
                '--focus-owned',str(owned_game['pid']),'--start',owned_game['start'],
                '--lab',str(lab),'--capture-owned',str(desktop),'--private-root',str(private)],
                capture_output=True,text=True,check=True,timeout=remaining(deadline,5))
            after=observer(cfg,env,'stock-status',stock_token,timeout=remaining(deadline),deadline=deadline)
            (run_dir/'stock-camera-after.json').write_text(json.dumps(after,indent=2))
            if after['process_creation']!=stock_creation:raise RuntimeError('Stock capture process incarnation changed')
            manifest['result']='stock_image_captured_camera_unverified'
            manifest['camera_comparison_acceptance']=False
            return run_dir
        preparation_id=preparation_weapon(cfg)
        if preparation_id is not None:
            _,preparation_prefix,preparation_key=preparation_target(preparation_id)
            preparation_native_pid=state['game_pid']
            preparation_deadline=min(deadline,time.monotonic()+35)
            while True:
                native_path=lab/'Bin/SS2VR.log'
                if native_path.stat().st_size>16*1024*1024:raise ValueError('Preparation log exceeds its evidence budget')
                native=native_path.read_text(errors='strict')
                if preparation_prefix+' preparation failed' in native:raise RuntimeError('Native private fixture preparation failed; preserve logs')
                if preparation_prefix+' preparation complete' in native:
                    manifest[preparation_key]=fixture_preparation_receipt(native,lab,preparation_id)
                    state=observer(cfg,env,'status',token,timeout=remaining(deadline),deadline=deadline)
                    if not same_owned_native_game(preparation_native_pid,state,owned_game):
                        raise RuntimeError('Prepared fixture process changed')
                    if state.get('current_weapon',[None,None])[1]==preparation_id:break
                if time.monotonic()>=preparation_deadline:raise TimeoutError('Native private fixture preparation did not complete')
                if not still_owned(owned_game):raise RuntimeError('Owned game exited during private fixture preparation')
                time.sleep(.1)
        baseline=cfg['expected_baseline_head'];baseline_packet=cfg['baseline_head']
        for step in cfg['pose_steps']:
            if not step['name'].replace('-','').isalnum():raise ValueError('Unsafe pose step name')
            boundary=state['input_sequence']
            head=step['head'];subprocess.run([cfg['pose_driver'],str(cfg['remote_port']),*map(str,head)],check=True,timeout=remaining(deadline,5))
            def mul(a,b):
                x,y,z,w=a;X,Y,Z,W=b
                return [w*X+x*W+y*Z-z*Y,w*Y-x*Z+y*W+z*X,w*Z+x*Y-y*X+z*W,w*W-x*X-y*Y-z*Z]
            yaw,pitch,roll=head[3:]
            q=mul(mul([0,math.sin(yaw/2),0,math.cos(yaw/2)],
                      [math.sin(pitch/2),0,0,math.cos(pitch/2)]),[0,0,math.sin(roll/2),math.cos(roll/2)])
            expected=[baseline['p'][i]+head[i]-baseline_packet[i] for i in range(3)]
            # Private neutral collection launches one owned observer per sample.
            # Its startup overhead can exceed ten seconds for thirty samples;
            # retain the same observations and clip to the global deadline.
            pose_seconds=20 if cfg.get('idle_weapon_probe') else 10
            matches=0;previous=boundary;pose_deadline=min(deadline,time.monotonic()+pose_seconds)
            while matches<30 and time.monotonic()<pose_deadline:
                try:state=observer(cfg,env,'status',token,timeout=remaining(pose_deadline),deadline=pose_deadline)
                except ObserverError as error:
                    if error.code==4:
                        time.sleep(.01);continue
                    raise
                values=state['head']['p']+state['head']['q']
                valid=(all(math.isfinite(v) for v in values) and abs(sum(v*v for v in state['head']['q'])-1)<.001 and
                    state['input_age_ms']<=150 and state['input_sequence']>boundary and
                    state['focused'] and state['head_valid'] and state['gameplay'] and not state['menu'] and
                    max(abs(state['head']['p'][i]-expected[i]) for i in range(3))<.001 and
                    abs(sum(a*b for a,b in zip(state['head']['q'],q)))>.99999)
                if state['input_sequence']>previous:
                    matches=matches+1 if valid else 0;previous=state['input_sequence']
                time.sleep(.01)
            if matches<30:raise TimeoutError('Held pose did not produce 30 matching OpenXR observations: '+step['name'])
            if step['name']=='baseline':
                sustained_deadline=min(deadline,time.monotonic()+60)
                count=0
                while time.monotonic()<sustained_deadline:
                    native_path=lab/'Bin/SS2VR.log';host_path=lab/'Bin/SS2VR/ss2vr_host.log'
                    native=native_path.read_text(errors='replace') if native_path.exists() else ''
                    host=host_path.read_text(errors='replace') if host_path.exists() else ''
                    count=len(complete_pairs_for_pose(native,host,{'p':expected,'q':q}))
                    if count>=30:break
                    if not still_owned(owned_game):raise RuntimeError('Owned game exited during sustained neutral rendering')
                    time.sleep(.1)
                manifest['distinct_neutral_complete_pairs']=count
                if count<30:raise TimeoutError('Neutral pose lacks 30 distinct complete native/UI/projection requests')
            (run_dir/(step['name']+'-input.json')).write_text(json.dumps(state,indent=2))
            desktop=run_dir/(step['name']+'-desktop.png')
            observer(cfg,env,'capture',token,(run_dir/step['name'],state['input_sequence'],*expected,*q),timeout=remaining(deadline),deadline=deadline)
            if cfg.get('capture_desktop',True):
                captured=subprocess.run([sys.executable,str(Path(__file__)),
                    '--focus-owned',str(owned_game['pid']),'--start',owned_game['start'],
                    '--lab',str(lab),'--capture-owned',str(desktop),'--private-root',str(private)],
                    capture_output=True,text=True,timeout=remaining(deadline,5))
                if captured.returncode:
                    manifest.setdefault('desktop_capture_errors',[]).append({'pose':step['name'],
                        'returncode':captured.returncode,'stderr':captured.stderr[-4096:]})
        if finish_idle_capture(cfg,lab,run_dir,manifest):
            return run_dir
        if cfg.get('background_controls_probe'):
            manifest['background_controls_probe']=background_controls_probe(cfg,env,token,deadline,run_dir,state)
        if cfg.get('native_dual_probe')=='zap-initial-inventory':
            # Retain the same normal window activation policy for actual input
            # probes; native eye capture does not establish exclusive input.
            if not state.get('native_host_foreground') or not state.get('native_input_exclusive'):
                incarnation=observer(cfg,env,'stock-status',stock_token,timeout=remaining(deadline,3),deadline=deadline)
                receipt=run_dir/'dual-activation.json'
                try:observer(cfg,env,'stock-focus',stock_token,(incarnation['process_creation'],'Z:'+str(receipt)),timeout=remaining(deadline,3),deadline=deadline)
                except ObserverError as error:
                    if error.code!=8:raise
                stop=min(deadline,time.monotonic()+3)
                while time.monotonic()<stop:
                    state=observer(cfg,env,'status',token,timeout=remaining(stop,2),deadline=stop)
                    if state.get('native_host_foreground') and state.get('native_input_exclusive'):break
                    time.sleep(.05)
                else:raise RuntimeError('Native dual fixture did not regain normal exclusive input')
                if receipt.exists():manifest['dual_activation_request']=json.loads(receipt.read_text())
            manifest['native_dual_probe']=native_dual_probe(cfg,env,token,deadline,run_dir,state)
        manifest['result']='world_images_captured_unreviewed'
    except Exception as error:
        manifest['error']=str(error)
        raise
    finally:
        cleanup_deadline=time.monotonic()+20
        termination_requests=[]
        def defer_termination(signum,frame): termination_requests.append(signum)
        previous_handlers={sig:signal.signal(sig,defer_termination) for sig in (signal.SIGTERM,signal.SIGINT)}
        def cleanup(action):
            try:action()
            except Exception as error:cleanup_errors.append(str(error))
        def graceful_close():
            if token and owned_game and still_owned(owned_game):
                try:
                    if stock and stock_creation:
                        observer(cfg,env,'stock-close',stock_token,(stock_creation,),timeout=3,deadline=cleanup_deadline)
                    elif not stock:observer(cfg,env,'close',token,timeout=3,deadline=cleanup_deadline)
                except (RuntimeError,subprocess.TimeoutExpired,TimeoutError):pass
                end=min(cleanup_deadline,time.monotonic()+3)
                while still_owned(owned_game) and time.monotonic()<end:time.sleep(.1)
        cleanup(graceful_close)
        # Rediscover late descendants before and after stopping the launcher;
        # exact executable path/start identity prevents unrelated process cleanup.
        def retire_children():
            for executable in (lab/'Bin/Sam2.exe',lab/'Bin/SS2VR/ss2vr_host.exe',Path(cfg['observer'])):
                if executable==Path(cfg['observer']) and not token:continue
                for item in private_processes(executable,token if executable==Path(cfg['observer']) else None):
                    if executable==Path(cfg['observer']) and (item['pid'],item['start']) in observer_exclusions:continue
                    cleanup(lambda item=item:stop_exact(item,cleanup_deadline))
        cleanup(retire_children)
        def retire_launcher(process):
            if not process or process.poll() is not None: return
            cleanup(process.terminate)
            try: process.wait(timeout=remaining(cleanup_deadline,3))
            except (subprocess.TimeoutExpired,TimeoutError):
                cleanup(process.kill)
                cleanup(lambda:process.wait(timeout=remaining(cleanup_deadline,2)))
        cleanup(lambda:retire_launcher(launch))
        cleanup(retire_children)
        cleanup(lambda:retire_launcher(service))
        for f in files:f.close()
        os.close(master)
        if slave>=0:os.close(slave)
        import shutil
        for path in (lab/'Bin/SS2VR.log',lab/'Bin/SS2VR/ss2vr_host.log',lab/'Sam2.log'):
            if path.exists():shutil.copy2(path,run_dir/path.name)
        manifest['shutdown_observed']='Lab native online shutdown wrapper entered; interface=0;' in (run_dir/'SS2VR.log').read_text(errors='replace') if (run_dir/'SS2VR.log').exists() else False
        manifest['termination_requests']=termination_requests
        manifest['cleanup_errors']=cleanup_errors
        (run_dir/'result.json').write_text(json.dumps(manifest,indent=2))
        print('Private runtime record:',run_dir)
        for sig,handler in previous_handlers.items():signal.signal(sig,handler)
        if termination_requests and not cleanup_errors: cleanup_errors.append('Termination requested during cleanup')
        if cleanup_errors:raise RuntimeError('Private runtime cleanup failed; inspect preserved result')
    return run_dir

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config',type=Path)
    parser.add_argument('--check',action='store_true')
    parser.add_argument('--focus-owned',type=int,help=argparse.SUPPRESS)
    parser.add_argument('--start',help=argparse.SUPPRESS)
    parser.add_argument('--capture-owned',type=Path,help=argparse.SUPPRESS)
    parser.add_argument('--private-root',type=Path,help=argparse.SUPPRESS)
    parser.add_argument('--lab',type=Path,help=argparse.SUPPRESS)
    args=parser.parse_args()
    if args.focus_owned is not None:
        item=identity(args.focus_owned)
        if not item or item['start']!=args.start or item['name']!='Sam2.exe' or not args.lab or not game_owned(item,args.lab.resolve()):
            raise SystemExit('Focus helper ownership mismatch')
        window=window_for(item['pid'])
        if args.capture_owned:
            capture_deadline=time.monotonic()+2
            while not window and time.monotonic()<capture_deadline:
                if not still_owned(item):raise SystemExit('Capture owner retired while waiting for its mapped window')
                time.sleep(.1);window=window_for(item['pid'])
        if args.capture_owned:
            if not window or not args.private_root:raise SystemExit('Owned capture unavailable')
            destination=args.capture_owned.resolve()
            private=args.private_root.resolve(strict=True)
            if private.is_relative_to(ROOT) or not destination.is_relative_to(private) or destination.exists():
                raise SystemExit('Unsafe capture destination')
            subprocess.run(['import','-window',str(window),str(destination)],check=True,timeout=3)
        print(json.dumps(window))
        raise SystemExit(0)
    if not args.config:parser.error('--config is required')
    def interrupted(signum,frame):raise RuntimeError('Runtime lab received termination signal')
    signal.signal(signal.SIGTERM,interrupted)
    configuration=json.loads(args.config.read_text())
    if args.check:
        validate(configuration);print('Private lab, installed products, startup evidence and scene bytes verified')
    else:run(configuration)
