#!/usr/bin/env python3
"""Stage only mod-owned files; no game assets and no installed-game mutations."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import zipfile
from build_contract import validate_products, development_version
ROOT = Path(__file__).resolve().parents[1]

def machine(path):
    with path.open('rb') as f:
        if f.read(2) != b'MZ': raise ValueError('Not a PE image: ' + path.name)
        f.seek(0x3c); offset = struct.unpack('<I', f.read(4))[0]
        f.seek(offset)
        if f.read(4) != b'PE\0\0': raise ValueError('Invalid PE header: ' + path.name)
        return struct.unpack('<H', f.read(2))[0]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def package(output=None):
    payload = {
        'Bin/d3d9.dll': ROOT/'build-game/d3d9.dll',
        'Bin/SS2VRServer.dll': ROOT/'build-game/SS2VRServer.dll',
        'Bin/SS2VR/ss2vr_host.exe': ROOT/'build-host/ss2vr_host.exe',
        'Bin/SS2VR/openxr_loader.dll': ROOT/'build-host/openxr/src/loader/libopenxr_loader.dll',
    }
    contract = validate_products(ROOT, {
        'game': payload['Bin/d3d9.dll'], 'server': payload['Bin/SS2VRServer.dll'],
        'host': payload['Bin/SS2VR/ss2vr_host.exe'],
    })
    version = development_version(contract)
    output = Path(output).resolve() if output is not None else ROOT/'dist'/('ss2vr-' + version)
    if any(output.is_relative_to(ROOT/directory) for directory in
           ['src','tests','cmake','tools','docs','licenses','config']):
        raise ValueError('Package output must be outside copied source directories')
    # Spatial audio: OpenAL Soft built by tools/build_openal.sh (32-bit, loaded by the game).
    payload['Bin/SS2VR/OpenAL32.dll'] = ROOT/'build-openal/OpenAL32.dll'
    for relative, path in payload.items():
        expected = 0x14c if relative in ('Bin/d3d9.dll', 'Bin/SS2VRServer.dll', 'Bin/SS2VR/OpenAL32.dll') else 0x8664
        if machine(path) != expected: raise ValueError('Architecture mismatch: ' + relative)
    payload['Bin/SS2VR/LICENSE.txt'] = ROOT/'LICENSE'
    payload['Bin/SS2VR/SS2VR.ini'] = ROOT/'config/SS2VR.ini'
    payload['Bin/SS2VR/alsoft.ini'] = ROOT/'config/alsoft.ini'
    payload['Content/SS2VR.mod'] = ROOT/'config/SS2VR.mod'
    payload['Bin/SS2VR/THIRD_PARTY.md'] = ROOT/'THIRD_PARTY.md'
    for path in (ROOT/'licenses').iterdir():
        if path.is_file(): payload['Bin/SS2VR/licenses/'+path.name] = path
    if output.exists() or Path(str(output)+'.zip').exists():
        raise FileExistsError('Choose a fresh output directory; existing packages are never overwritten')
    for relative, source in payload.items():
        target = output/relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    for name in ['README.md','AGENTS.md','CMakeLists.txt','LICENSE','THIRD_PARTY.md','MODDING_PLAN.md','MODLOG.md','.clang-format','.gitignore']:
        shutil.copy2(ROOT/name,output/name)
    for directory in ['src','tests','cmake','tools','docs','licenses','config']:
        for source in (ROOT/directory).rglob('*'):
            if source.is_file() and '__pycache__' not in source.parts and source.suffix not in ('.pyc','.log','.dmp'):
                target=output/source.relative_to(ROOT);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
    shutil.copy2(ROOT/'tools/install.py', output/'install.py')
    # Validate the staged bytes too: a source edit during copying must not turn
    # a matching build into an archive containing a different source snapshot.
    validate_products(output, {'game':output/'Bin/d3d9.dll',
        'server':output/'Bin/SS2VRServer.dll','host':output/'Bin/SS2VR/ss2vr_host.exe'})
    files = {relative: digest(output/relative) for relative in payload}
    manifest = {'version': version, 'files': files,
                'game_fingerprints': json.loads((ROOT/'docs/installed-build.json').read_text()),
                'ipc_abi': contract['ipc_abi'], 'multiplayer_wire_version': contract['multiplayer_wire_version'],
                'build_contract': contract,
                'scope': 'incomplete-development-checkpoint',
                'runtime_verified': False}
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    with zipfile.ZipFile(Path(str(output)+'.zip'), 'x', compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(output.rglob('*')):
            if path.is_file(): archive.write(path, path.relative_to(output))
    print('Staged package:', output)
    print('Archive:', Path(str(output)+'.zip'))

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='Fresh directory; default includes the source fingerprint')
    package(parser.parse_args().output)
