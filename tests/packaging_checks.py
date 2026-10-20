"""Synthetic PE/source fixtures; no Windows product or game is executed."""
from pathlib import Path
import struct
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_contract as contract


def must_reject(action):
    try:
        action()
    except ValueError:
        return
    raise AssertionError('Invalid contract accepted')


def encoded(expected, component=1, **changes):
    values = dict(schema=1, component=component, input_bytes=264, request_bytes=432,
                  ui_bytes=352, slot_bytes=33554920, shared_bytes=83887816, **expected)
    values.update(changes)
    return contract.FORMAT.pack(contract.MAGIC, *(values[k] for k in contract.FIELDS),
                                values['source_fingerprint'].encode())


def image(path, data, machine=0x14c, writable=False, export_name=b'ss2vrBuildContract'):
    raw = bytearray(0x600)
    raw[:2] = b'MZ'
    struct.pack_into('<I', raw, 0x3c, 0x80)
    raw[0x80:0x84] = b'PE\0\0'
    size = 224 if machine == 0x14c else 240
    struct.pack_into('<HHIIIHH', raw, 0x84, machine, 1, 0, 0, 0, size, 0x2022)
    optional = 0x98
    struct.pack_into('<H', raw, optional, 0x10b if machine == 0x14c else 0x20b)
    struct.pack_into('<II', raw, optional+32, 0x1000, 0x200)
    struct.pack_into('<II', raw, optional+56, 0x2000, 0x200)
    struct.pack_into('<H', raw, optional+68, 3)
    directory = optional + (96 if machine == 0x14c else 112)
    struct.pack_into('<I', raw, directory-4, 16)
    struct.pack_into('<II', raw, directory, 0x1000, 40)
    section = optional+size
    struct.pack_into('<8sIIIIIIHHI', raw, section, b'.rdata\0\0', 0x400, 0x1000,
                     0x400, 0x200, 0, 0, 0, 0, 0x40000040 | (0x80000000 if writable else 0))
    struct.pack_into('<IIHHIIIIIII', raw, 0x200, 0, 0, 0, 0, 0x1080, 1, 1, 1,
                     0x1040, 0x1044, 0x1048)
    struct.pack_into('<IIH', raw, 0x240, 0x1100, 0x1050, 0)
    raw[0x250:0x250+len(export_name)+1] = export_name+b'\0'
    raw[0x280:0x28c] = b'fixture.dll\0'
    raw[0x300:0x300+len(data)] = data
    path.write_bytes(raw)


with tempfile.TemporaryDirectory(prefix='ss2vr-contract-') as temporary:
    root = Path(temporary)
    (root/'src/common').mkdir(parents=True)
    (root/'cmake').mkdir()
    (root/'CMakeLists.txt').write_text('project(ss2vr VERSION 0.2.11 LANGUAGES C CXX)\n')
    (root/'src/common/protocol.hpp').write_text('constexpr uint32_t Abi = 9;\n')
    (root/'src/common/network.hpp').write_text('constexpr uint8_t WireVersion = 6;\n')
    expected = contract.source_contract(root)
    products = {}
    for name, (component, machine) in contract.COMPONENTS.items():
        path = root/(name+'.exe')
        image(path, encoded(expected, component), machine)
        products[name] = path
    checked = contract.validate_products(root, products)
    assert checked['ipc_abi'] == 9 and checked['shared_bytes'] == 83887816
    assert contract.development_version(checked).startswith('0.2.11-dev.')
    must_reject(lambda: contract.decode_contract(b''))
    must_reject(lambda: contract.decode_contract(encoded(expected, schema=2)))
    must_reject(lambda: contract.decode_contract(encoded(expected, input_bytes=0)))
    must_reject(lambda: contract.decode_contract(encoded(expected, source_fingerprint='X'*64)))
    for change in [dict(ipc_abi=8), dict(multiplayer_wire_version=5), dict(slot_bytes=20),
                   dict(source_fingerprint='0'*64), dict(version_patch=10)]:
        image(products['game'], encoded(expected, **change))
        must_reject(lambda: contract.validate_products(root, products))
    image(products['game'], encoded(expected), writable=True)
    must_reject(lambda: contract.validate_products(root, products))
    image(products['game'], encoded(expected), export_name=b'other')
    must_reject(lambda: contract.validate_products(root, products))
    image(products['game'], encoded(expected, component=2))
    must_reject(lambda: contract.validate_products(root, products))
    image(products['game'], encoded(expected), machine=0x8664)
    must_reject(lambda: contract.validate_products(root, products))
    image(products['game'], encoded(expected))
    for path in [root/'src/new.hpp', root/'cmake/new.cmake']:
        path.write_text('// changed compiler input\n')
        assert contract.source_contract(root)['source_fingerprint'] != expected['source_fingerprint']
        must_reject(lambda: contract.validate_products(root, products))
        path.unlink()
    (root/'src/common/network.hpp').write_text('constexpr uint8_t WireVersion = 7;\n')
    must_reject(lambda: contract.validate_products(root, products))
    (root/'src/common/network.hpp').write_text('constexpr uint8_t WireVersion = 6;\n')
    assert contract.validate_products(root, products) == checked
    must_reject(lambda: contract.validate_products(root, {'game':products['game']}))

    # Run the real packager against synthetic products and owned fixture files.
    import package as packaging
    import json
    import shutil
    import zipfile
    packaging.ROOT = root
    for directory in ['build-game', 'build-host/openxr/src/loader', 'build-openal', 'licenses',
                      'config', 'tests', 'tools', 'docs']:
        (root/directory).mkdir(parents=True, exist_ok=True)
    for name in ['README.md', 'AGENTS.md', 'LICENSE', 'THIRD_PARTY.md', 'MODDING_PLAN.md',
                 'MODLOG.md', '.clang-format', '.gitignore', 'tools/install.py',
                 'licenses/test.txt', 'config/SS2VR.ini', 'config/SS2VR.mod', 'config/alsoft.ini']:
        (root/name).write_text('fixture\n')
    (root/'docs/installed-build.json').write_text('{}')
    for component, relative in [('game','build-game/d3d9.dll'),
                               ('server','build-game/SS2VRServer.dll'),
                               ('host','build-host/ss2vr_host.exe')]:
        shutil.copy2(products[component],root/relative)
    shutil.copy2(products['host'],root/'build-host/openxr/src/loader/libopenxr_loader.dll')
    shutil.copy2(products['game'],root/'build-openal/OpenAL32.dll') # A 32-bit PE stands in for OpenAL Soft.
    must_reject(lambda: packaging.package(root/'src/recursive-output'))
    assert not (root/'src/recursive-output').exists()
    out=root/'staged'
    packaging.package(out)
    manifest=json.loads((out/'manifest.json').read_text())
    assert manifest['ipc_abi']==9 and manifest['build_contract']==checked
    assert not manifest['runtime_verified']
    with zipfile.ZipFile(str(out)+'.zip') as archive:
        assert json.loads(archive.read('manifest.json'))==manifest
    try:
        packaging.package(out)
    except FileExistsError:
        pass
    else:
        raise AssertionError('Existing package overwritten')
    changed=root/'src/common/network.hpp'
    saved=changed.read_bytes()
    changed.write_bytes(saved+b'// changed after build\n')
    must_reject(lambda: packaging.package(root/'stale'))
    assert not (root/'stale').exists()
    changed.write_bytes(saved)
    original_copy=packaging.shutil.copy2
    def mutate_staged(source,target,*args,**kwargs):
        result=original_copy(source,target,*args,**kwargs)
        if Path(source)==changed:
            with Path(target).open('ab') as stream:
                stream.write(b'// changed during staging\n')
        return result
    packaging.shutil.copy2=mutate_staged
    try:
        must_reject(lambda: packaging.package(root/'changed-during-copy'))
    finally:
        packaging.shutil.copy2=original_copy
    assert not (root/'changed-during-copy/manifest.json').exists()
    assert not (root/'changed-during-copy.zip').exists()
print('Synthetic source/PE identity, ABI, mixed-build and read-only contract checks passed.')
