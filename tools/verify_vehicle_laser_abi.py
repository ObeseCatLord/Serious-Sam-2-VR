#!/usr/bin/env python3
"""Bounded owned x86 caller-shape check; never executes Windows/native code.

This checks argument locations, callee-cleanup accommodation and consumed
returns. It does not replace manual source/binary argument-provenance review
or establish native lifecycle/concurrency. Native ABI is separately audited.
"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

def require(ok, message):
    if not ok:
        raise ValueError(message)

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--artifact', choices=['d3d9.dll', 'SS2VRServer.dll'], default='d3d9.dll')
parser.add_argument('--artifact-path', type=Path, help='Explicit mod-owned PE fixture for negative checks')
arguments = parser.parse_args()
artifact = arguments.artifact_path or ROOT / 'build-game' / arguments.artifact
rows = [r.split() for r in subprocess.check_output(['i686-w64-mingw32-nm', str(artifact)], text=True).splitlines()
        if len(r.split()) == 3]

def symbol(needle, kind):
    found = [int(r[0], 16) for r in rows if needle in r[2] and r[1] == kind and '.cold' not in r[2]]
    require(len(found) == 1, (needle, found))
    return found[0]

def function(needle):
    start = symbol(needle, 't')
    end = min(int(r[0], 16) for r in rows if r[1] in ('t', 'T') and int(r[0], 16) > start)
    assembly = subprocess.check_output(['i686-w64-mingw32-objdump', '-d',
        f'--start-address={start}', f'--stop-address={end}', str(artifact)], text=True)
    return [line.split('\t')[-1].strip() for line in assembly.splitlines()
            if re.match(r'^[0-9a-f]+:', line.strip()) and
            re.match(r'^[a-z]+(?:\s|$)', line.split('\t')[-1].strip())]

# Native calls now live in the observed body, not its native-finally entry or
# generated cleanup callbacks. Bound exactly that body before inspecting ABI.
calls = function('L18trackedRayInitBodyEjb')
entries = {}
for slot, name, size in [('550', 'selectedAttachment', 4), ('ac', 'purpose1Origin', 8),
                          ('5a4', 'shootDirection', 4)]:
    sites = [i for i, ins in enumerate(calls) if re.fullmatch(r'call   \*0x'+slot+r'\(%[a-z]+\)', ins)]
    require(len(sites) == 1, (name, sites))
    at = sites[0]
    before, after = calls[max(0, at-22):at], calls[at+1:at+6]
    require('mov    %eax,(%esp)' in before, (name, before))
    require(any(re.fullmatch(r'mov    %[a-z]+,%ecx', ins) for ins in before), name)
    require(sum(ins.startswith('push   ') for ins in after) * 4 == size, (name, after))
    if slot == 'ac':
        require('movl   $0x1,0x4(%esp)' in before, before)
    else:
        require(any(re.fullmatch(r'lea    -0x[0-9a-f]+\(%ebp\),%eax', ins) for ins in before), name)
    entries[name] = {'virtual_byte_slot': '0x'+slot, 'hidden_output_stack_slot': 0,
                     'native_callee_cleanup_bytes': size, 'caller_accommodates_cleanup': True,
                     'ecx_receiver_load': True}
    if slot == 'ac':
        entries[name]['purpose_stack_slot'] = 4
        entries[name]['purpose_value'] = 1

target = symbol('laserAttachmentE', 'b')
sites = [i for i, ins in enumerate(calls) if ins == f'call   *0x{target:x}']
require(len(sites) == 1, sites)
at = sites[0]
before = calls[at-12:at]
for slot in ['(%esp)', '0x4(%esp)', '0x8(%esp)']:
    require('mov    %eax,'+slot in before, (slot, before))
require(calls[at+1] == 'test   %eax,%eax', calls[at+1:at+4])
require(not any(ins.startswith(('push   ', 'sub    ')) for ins in calls[at+1:at+3]), 'Compiled vehicle laser ABI check failed')
entries['attachmentExists'] = {'cdecl': True, 'arguments_stack_bytes': 12,
                               'integer_return_eax_consumed': True, 'native_callee_cleanup_bytes': 0}

read = function('L22readVehicleLaserSource')
target = symbol('laserModelInstanceE', 'b')
sites = [i for i, ins in enumerate(read) if ins == f'call   *0x{target:x}']
require(len(sites) == 1, sites)
at = sites[0]
# The receiver load may be scheduled a few instructions before the call; every
# instruction in between must leave ECX alone and must not be a call.
load = at-1
while load > at-4 and not read[load].endswith(',%ecx'):
    require(not read[load].startswith('call') and '%ecx' not in read[load] and '%cx' not in read[load],
            read[load-1:at+1])
    load -= 1
receiver = read[load]
if not re.fullmatch(r'mov    0x[0-9a-f]+\(%esp\),%ecx', receiver):
    # GCC 16 keeps the resolved model pointer in callee-saved EBX across the
    # readability check. Prove this specific sequence, not arbitrary ECX data.
    require(receiver == 'mov    %ebx,%ecx', read[at-2:at+3])
    capture = max(i for i in range(at) if read[i] == 'mov    %eax,%ebx')
    resolve = symbol('4gameL7resolveE', 'b')
    require(read[capture-1] == f'call   *0x{resolve:x}', read[capture-2:capture+2])
    span = read[capture+1:at]
    require(len(span) == 10, span)
    require(span[0] == 'test   %eax,%eax' and span[1].startswith('je '), span)
    require(span[2:5] == ['movl   $0x0,0x8(%esp)', 'movl   $0x60,0x4(%esp)',
                         'mov    %eax,(%esp)'], span)
    require(span[5].startswith('call ') and 'readableMemory' in span[5], span)
    require(span[6:8] == ['mov    %eax,%edx', 'test   %al,%al'], span)
    require(span[8].startswith('je ') and span[9:] == ['mov    %ebx,%ecx'], span)
require(not read[at+1].startswith(('push   ', 'sub    ')), read[at+1:at+4])
entries['modelInstance'] = {'thiscall': True, 'ecx_receiver_load': True,
                            'native_return': 'eax pointer', 'native_callee_cleanup_bytes': 0}
print(json.dumps({'runtime_executed': False, 'artifact': artifact.name,
    'artifact_sha256': hashlib.sha256(artifact.read_bytes()).hexdigest(),
    'compiled_x86_vehicle_laser_abi': entries,
    'method': 'defined-symbol bounded owned-code objdump; manual argument provenance required'}, indent=2))
