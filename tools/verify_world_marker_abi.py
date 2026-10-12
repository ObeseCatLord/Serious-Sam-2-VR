#!/usr/bin/env python3
"""Check compiled x86 marker caller shape. Reads owned mod code; executes none.

This deliberately bounded checker does not prove the contents of reference
buffers or that ECX derives from the incoming player. Preserve an independent
manual provenance audit for each accepted artifact hash (Astra source review).
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
artifact = ROOT / 'build-game' / parser.parse_args().artifact
nm = subprocess.check_output(['i686-w64-mingw32-nm', str(artifact)], text=True)
symbols = [row.split() for row in nm.splitlines() if len(row.split()) == 3]

def symbol(needle, kind):
    matches = [row for row in symbols if needle in row[2] and row[1] == kind and '.cold' not in row[2]]
    require(len(matches) == 1, (needle, matches))
    return int(matches[0][0], 16)

start = symbol('__ZN5ss2vr4gameL20nativeMarkerPostludeEPv', 't')
end = min(int(row[0], 16) for row in symbols if row[1] in ('t', 'T') and int(row[0], 16) > start)
assembly = subprocess.check_output(['i686-w64-mingw32-objdump', '-d',
    f'--start-address={start}', f'--stop-address={end}', str(artifact)], text=True)
instructions = [line.split('\t')[-1].strip() for line in assembly.splitlines()
                if re.match(r'^[0-9a-f]+:', line.strip()) and
                re.match(r'^[a-z]+(?:\s|$)', line.split('\t')[-1].strip())]
require(instructions[0] == 'lea    0x4(%esp),%ecx', instructions[:5])
source = (ROOT / 'src/game/engine.cpp').read_text()
require('using MarkerDraw = void(__thiscall *)(void *, const Matrix34 &, const Matrix44 &,' in source, "Required boundary check failed: 'using MarkerDraw = void(__thiscall *)(void *, const Matrix34 &, const Matrix44 &,' in source")
require('const WorldMarkerDimensions &, float);' in source, "Required boundary check failed: 'const WorldMarkerDimensions &, float);' in source")
require('using MarkerFade = float(__thiscall *)(void *);' in source, "Required boundary check failed: 'using MarkerFade = float(__thiscall *)(void *);' in source")
entries = {}
references = []
receiver_loads = []
for name in ['nativeNavigation', 'nativeObjectives']:
    target = symbol(name + 'E', 'b')
    calls = [i for i, ins in enumerate(instructions) if ins == f'call   *0x{target:x}']
    require(len(calls) == 1, (name, calls))
    at = calls[0]
    before = instructions[max(0, at - 20):at]
    # Four arguments occupy exactly sixteen outgoing stack bytes. The compiler
    # restores that space after each native callee's audited ret16.
    require('sub    $0x10,%esp' in instructions[at+1:at+4], name)
    refs = []
    for destination in ['(%esp)', '0x4(%esp)', '0x8(%esp)']:
        store = 'mov    %eax,' + destination
        position = max(i for i, ins in enumerate(before) if ins == store)
        require(position > 0, name)
        producer = position-1
        while producer >= 0 and before[producer] == 'fstps  0xc(%esp)':
            producer -= 1
        require(producer >= 0, name)
        pointer = re.fullmatch(r'lea    (-0x[0-9a-f]+)\(%ebp\),%eax', before[producer])
        require(pointer, (name, before[producer]))
        refs.append(int(pointer[1], 16))
    require(refs[1] - refs[0] == 48, (name, refs)) # Matrix34 then full Matrix44 value copy.
    require('fstps  0xc(%esp)' in before, (name, before))
    receiver = re.fullmatch(r'mov    \(%([a-z]+)\),%ecx', before[-1])
    require(receiver, (name, before[-1]))
    references.append(refs)
    receiver_loads.append(receiver[1])
    entries[name] = {'ecx_receiver': True, 'stack_argument_bytes': 16,
                     'matrix34_matrix44_dimensions_by_reference': True,
                     'fade_float_stack_slot': 12, 'native_callee_cleanup_bytes': 16}
require(references[0] == references[1], references)
require(receiver_loads[0] == receiver_loads[1], receiver_loads)
fade_calls = [i for i, ins in enumerate(instructions) if ins == 'call   *0x60c(%eax)']
require(len(fade_calls) == 1, fade_calls)
at = fade_calls[0]
require(instructions[at-1] == 'mov    (%ecx),%eax', instructions[at-3:at+4])
require(re.fullmatch(r'mov    \(%[a-z]+\),%ecx', instructions[at-2]), instructions[at-2])
# The float result is consumed straight from x87 st0: duplicated (fld st0) or
# stored to a frame slot (fsts/fstps); the register choice is compiler-owned.
consume = instructions[at+1]
require(consume == 'fld    %st(0)' or re.fullmatch(r'fstp?s\s+-0x[0-9a-f]+\(%ebp\)', consume), instructions[at+1:at+5])
require(any(re.match(r'fstp?s\s+-0x', ins) for ins in instructions[at+1:at+15]), "Required boundary check failed: the fade result is never stored to a frame slot")
entries['nativeFade'] = {'virtual_byte_slot': '0x60c', 'ecx_receiver': True,
                         'return_consumed_from_x87_st0': True}
print(json.dumps({'runtime_executed': False, 'artifact': artifact.name,
    'artifact_sha256': hashlib.sha256(artifact.read_bytes()).hexdigest(),
    'compiled_x86_marker_abi': entries,
    'method': 'defined-symbol bounded owned-code objdump; native callee ABI independently audited'}, indent=2))
