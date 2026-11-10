#!/usr/bin/env python3
"""Statically verify the pinned x86 sniper-predicate hook windows.

This reads the installed PE image but never loads it, starts a process, or
executes Windows code.  It deliberately emits hashes and bounded addresses,
never native bytes or decoded instruction text.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from contextlib import contextmanager

try:
    import capstone
    import pefile
except ModuleNotFoundError as error:
    raise SystemExit(
        "missing offline dependency: install tools/requirements.txt and capstone "
        "without running the game") from error


ROOT = Path(__file__).resolve().parents[1]
BOUNDARIES_PATH = ROOT / "tools/data/sniper-predicate-boundaries.json"
CHECKS_PATH = ROOT / "tools/data/sniper-predicate-site-checks.json"
MINHOOK_FILES = {
    "trampoline_c_sha256": "src/trampoline.c",
    "trampoline_h_sha256": "src/trampoline.h",
    "hde32_c_sha256": "src/hde/hde32.c",
    "hde32_h_sha256": "src/hde/hde32.h",
    "hde32_pstdint_sha256": "src/hde/pstdint.h",
    "hde32_table_sha256": "src/hde/table32.h",
}


def minhook_paths(source=None, root=ROOT):
    root = Path(root)
    if source is None:
        cache = root / "build-game/CMakeCache.txt"
        values = {}
        if cache.is_file():
            for line in cache.read_text().splitlines():
                if ":" in line and "=" in line and not line.startswith(("#", "//")):
                    key, value = line.split("=", 1)
                    values[key.split(":", 1)[0]] = value
        configured = values.get("minhook_SOURCE_DIR") or values.get("MINHOOK_SOURCE")
        source = Path(configured) if configured else root / "deps/vendor/minhook"
        if configured and not source.is_absolute():
            source = root / source
    source = Path(source).resolve(strict=True)
    return {name: source / relative for name, relative in MINHOOK_FILES.items()}


HDE32_HOST_SCRATCH = Path("/tmp/ss2-vr-equivalence")
IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_REL_BASED_HIGHLOW = 3
MINHOOK_X86_JMP_REL_SIZE = 5
MINHOOK_X86_TRAMPOLINE_MAX_SIZE = 32
MINHOOK_MAX_INSTRUCTION_BOUNDARIES = 8
HDE32_F_ERROR = 0x00001000
HDE32S_EXPECTED_SIZE = 28


class Hde32HostResult(ctypes.Structure):
    """Bounded facts exported by the private host-only HDE bridge."""
    _fields_ = [("structure_size", ctypes.c_uint32),
                ("length", ctypes.c_uint32),
                ("flags", ctypes.c_uint32),
                ("opcode", ctypes.c_uint8),
                ("opcode2", ctypes.c_uint8)]


def sha256_file(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rva_text(value):
    return f"0x{value:x}"


def parse_rva(value):
    return int(value, 0) if isinstance(value, str) else value


def fail(message):
    raise ValueError(message)


def instruction_text_digest(instructions):
    """Hash exact Capstone instruction syntax without retaining it in output."""
    canonical = "\n".join(
        f"{instruction.size}:{instruction.mnemonic} {instruction.op_str}".rstrip()
        for instruction in instructions
    )
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def hde32_fact_digest(facts):
    """Pin HDE length/error/opcode facts without recording native bytes."""
    canonical = "\n".join(
        f"{fact['length']}:{fact['flags']:08x}:{fact['opcode']:02x}:{fact['opcode2']:02x}"
        for fact in facts
    )
    return hashlib.sha256(canonical.encode("ascii")).hexdigest()


def create_private_hde32_scratch():
    HDE32_HOST_SCRATCH.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(HDE32_HOST_SCRATCH, 0o700)
    if os.stat(HDE32_HOST_SCRATCH).st_mode & 0o077:
        fail("private HDE32 scratch directory is accessible by other users")


@contextmanager
def built_hde32_host_decoder(paths):
    """Build unchanged pinned hde32.c for the POSIX host, never game code."""
    compiler = shutil.which("cc")
    if compiler is None:
        fail("portable HDE32 host decoder requires a C compiler named cc")
    create_private_hde32_scratch()
    with tempfile.TemporaryDirectory(prefix="hde32-", dir=HDE32_HOST_SCRATCH) as temporary:
        directory = Path(temporary)
        shim_dir = directory / "windows-shim"
        shim_dir.mkdir()
        # pstdint.h includes <windows.h>.  This private shim deliberately
        # supplies only the fixed-width INT/UINT names it consumes.
        (shim_dir / "windows.h").write_text(
            "#ifndef SS2_VR_HDE32_WINDOWS_SHIM_H\n"
            "#define SS2_VR_HDE32_WINDOWS_SHIM_H\n"
            "typedef signed char INT8;\n"
            "typedef short INT16;\n"
            "typedef int INT32;\n"
            "typedef long long INT64;\n"
            "typedef unsigned char UINT8;\n"
            "typedef unsigned short UINT16;\n"
            "typedef unsigned int UINT32;\n"
            "typedef unsigned long long UINT64;\n"
            "#endif\n",
            encoding="ascii")
        bridge = directory / "hde32_host_bridge.c"
        bridge.write_text(
            "#include <stddef.h>\n"
            "#include \"hde32.h\"\n"
            "typedef struct {\n"
            "    uint32_t structure_size;\n"
            "    uint32_t length;\n"
            "    uint32_t flags;\n"
            "    uint8_t opcode;\n"
            "    uint8_t opcode2;\n"
            "} hde32_host_result;\n"
            "int hde32_host_decode(const uint8_t *code, hde32_host_result *result) {\n"
            "    hde32s decoded;\n"
            "    if (code == NULL || result == NULL) return 0;\n"
            "    result->length = hde32_disasm(code, &decoded);\n"
            "    result->structure_size = (uint32_t)sizeof(decoded);\n"
            "    result->flags = decoded.flags;\n"
            "    result->opcode = decoded.opcode;\n"
            "    result->opcode2 = decoded.opcode2;\n"
            "    return 1;\n"
            "}\n",
            encoding="ascii")
        library = directory / "libss2_hde32_host.so"
        command = [compiler, "-shared", "-fPIC", "-std=c99", "-D_M_IX86=1",
                   "-I", str(shim_dir), "-I", str(paths["hde32_c_sha256"].parent),
                   str(paths["hde32_c_sha256"]), str(bridge),
                   "-o", str(library)]
        completed = subprocess.run(command, capture_output=True, text=True, check=False)
        if completed.returncode:
            fail("pinned HDE32 host decoder compilation failed")
        host_library = ctypes.CDLL(str(library))
        decode = host_library.hde32_host_decode
        decode.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.POINTER(Hde32HostResult)]
        decode.restype = ctypes.c_int

        def decode_bytes(data):
            if len(data) < 16:
                fail("HDE32 host decoder requires a 16-byte read-only window")
            buffer = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
            result = Hde32HostResult()
            if decode(buffer, ctypes.byref(result)) != 1:
                fail("HDE32 host bridge rejected its decoder invocation")
            return {"structure_size": result.structure_size, "length": result.length,
                    "flags": result.flags, "opcode": result.opcode, "opcode2": result.opcode2}

        rejection = decode_bytes(b"\x66" * 16 + b"\x90")
        if not rejection["flags"] & HDE32_F_ERROR:
            fail("HDE32 host decoder did not reject the safe overlength probe")
        yield decode_bytes


def hde32_matches_capstone(fact, instruction, role):
    raw = bytes(instruction.bytes)
    prefix_bytes = {0xf0, 0xf2, 0xf3, 0x26, 0x2e, 0x36, 0x3e,
                    0x64, 0x65, 0x66, 0x67}
    opcode_index = 0
    while opcode_index < len(raw) and raw[opcode_index] in prefix_bytes:
        opcode_index += 1
    if opcode_index >= len(raw):
        fail(f"{role}: Capstone instruction has no opcode byte")
    if fact["structure_size"] != HDE32S_EXPECTED_SIZE:
        fail(f"{role}: HDE32 structure size is not the pinned x86 layout")
    if fact["flags"] & HDE32_F_ERROR:
        fail(f"{role}: pinned HDE32 rejects instruction")
    if fact["length"] != instruction.size:
        fail(f"{role}: HDE32 instruction length disagrees with Capstone")
    if fact["opcode"] != raw[opcode_index]:
        fail(f"{role}: HDE32 opcode disagrees with Capstone bytes")
    expected_opcode2 = raw[opcode_index + 1] if raw[opcode_index] == 0x0f else 0
    if fact["opcode2"] != expected_opcode2:
        fail(f"{role}: HDE32 second opcode disagrees with Capstone bytes")


def first_instruction(decoder, pe, rva, role):
    decoded = list(decoder.disasm(pe.get_data(rva, 15), rva, count=1))
    if len(decoded) != 1 or decoded[0].address != rva or decoded[0].size == 0:
        fail(f"{role}: undecodable instruction at declared boundary")
    return decoded[0]


def hde32_fact_for_instruction(decode_hde32, pe, instruction, role):
    window = pe.get_data(instruction.address, 16)
    if len(window) != 16:
        fail(f"{role}: cannot provide HDE32's required 16-byte decode window")
    fact = decode_hde32(window)
    hde32_matches_capstone(fact, instruction, role)
    return fact


def is_direct_relative_call_or_jump(instruction):
    if not instruction.operands or instruction.operands[0].type != capstone.x86.X86_OP_IMM:
        return False
    return (instruction.group(capstone.CS_GRP_CALL) or
            instruction.group(capstone.CS_GRP_JUMP))


def is_jcc_or_loop(raw):
    if not raw:
        return False
    opcode = raw[0]
    return ((opcode & 0xF0) == 0x70 or (opcode & 0xFC) == 0xE0 or
            (opcode == 0x0F and len(raw) > 1 and (raw[1] & 0xF0) == 0x80))


def minhook_x86_plan(instructions, site_rva, role):
    """Model CreateTrampolineFunction's x86 stealing/relocation branches.

    The source hashes are checked separately.  This mirrors its fixed five-byte
    patch requirement, relative branch expansion, old/new IP bookkeeping, and
    the continuation jump.  It does not allocate a trampoline or execute it.
    """
    by_offset = {instruction.address - site_rva: instruction for instruction in instructions}
    old_pos = 0
    new_pos = 0
    jump_destination = 0
    old_offsets = []
    new_offsets = []
    decisions = []
    finished = False

    while not finished:
        if old_pos >= MINHOOK_X86_JMP_REL_SIZE:
            copy_size = MINHOOK_X86_JMP_REL_SIZE
            old_offsets.append(old_pos)
            new_offsets.append(new_pos)
            decisions.append("append_continuation_jmp_rel32")
            new_pos += copy_size
            finished = True
            continue

        instruction = by_offset.get(old_pos)
        if instruction is None:
            fail(f"{role}: MinHook would read beyond decoded stolen instructions")
        raw = bytes(instruction.bytes)
        original_size = instruction.size
        copy_size = original_size
        decision = "copy"
        old_instruction_rva = site_rva + old_pos

        if raw[0] == 0xE8:
            if not is_direct_relative_call_or_jump(instruction):
                fail(f"{role}: HDE direct CALL classification disagrees with decoder")
            copy_size = MINHOOK_X86_JMP_REL_SIZE
            decision = "relocate_call_rel32"
        elif (raw[0] & 0xFD) == 0xE9:
            if not is_direct_relative_call_or_jump(instruction):
                fail(f"{role}: HDE direct JMP classification disagrees with decoder")
            destination = instruction.operands[0].imm
            if site_rva <= destination < site_rva + MINHOOK_X86_JMP_REL_SIZE:
                jump_destination = max(jump_destination, destination)
                decision = "copy_internal_jmp"
            else:
                copy_size = MINHOOK_X86_JMP_REL_SIZE
                decision = "relocate_jmp_rel32"
                finished = old_instruction_rva >= jump_destination
        elif is_jcc_or_loop(raw):
            if not is_direct_relative_call_or_jump(instruction):
                fail(f"{role}: HDE direct Jcc classification disagrees with decoder")
            destination = instruction.operands[0].imm
            if site_rva <= destination < site_rva + MINHOOK_X86_JMP_REL_SIZE:
                jump_destination = max(jump_destination, destination)
                decision = "copy_internal_conditional_branch"
            elif (raw[0] & 0xFC) == 0xE0:
                fail(f"{role}: MinHook rejects external LOOP/JECXZ")
            else:
                copy_size = 6  # x86 JCC_REL in pinned trampoline.h.
                decision = "relocate_jcc_rel32"
        elif (raw[0] & 0xFE) == 0xC2:
            decision = "copy_return"
            finished = old_instruction_rva >= jump_destination

        if old_instruction_rva < jump_destination and copy_size != original_size:
            fail(f"{role}: MinHook cannot resize an instruction inside an internal branch")
        if new_pos + copy_size > MINHOOK_X86_TRAMPOLINE_MAX_SIZE:
            fail(f"{role}: MinHook x86 trampoline slot would overflow")
        if len(old_offsets) >= MINHOOK_MAX_INSTRUCTION_BOUNDARIES:
            fail(f"{role}: MinHook instruction-boundary table would overflow")

        old_offsets.append(old_pos)
        new_offsets.append(new_pos)
        decisions.append(decision)
        new_pos += copy_size
        old_pos += original_size

    return {
        "stolen_bytes": old_pos,
        "continuation_rva": rva_text(site_rva + old_pos),
        "old_instruction_offsets": old_offsets,
        "new_instruction_offsets": new_offsets,
        "relocation_decisions": decisions,
        "trampoline_bytes": new_pos,
        "patch_above_required": False,
    }


def executable_sections(pe):
    sections = [section for section in pe.sections
                if section.Characteristics & IMAGE_SCN_MEM_EXECUTE]
    if not sections:
        fail("PE has no executable sections")
    return sections


def rva_is_executable(rva, sections):
    return any(section.VirtualAddress <= rva <
               section.VirtualAddress + max(section.Misc_VirtualSize, section.SizeOfRawData)
               for section in sections)


def scan_direct_interior_references(pe, decoder, sites, sections):
    """Find static direct edges and absolute relocation operands into interiors.

    Indirect control transfers and relocatable data pointers can be changed or
    interpreted at runtime, so this provides no proof that all incoming paths
    are absent.  The caller reports that coverage explicitly as UNKNOWN.
    """
    findings = {site["role"]: {"direct_branch_sources": [],
                                "highlow_relocation_operands": []}
                for site in sites}
    direct_branch_count = 0
    indirect_branch_count = 0

    for section in sections:
        for instruction in decoder.disasm(section.get_data(), section.VirtualAddress):
            # skipdata preserves byte coverage through executable sections, but
            # its synthetic data records have no control-flow groups.
            if instruction.id == 0:
                continue
            if not (instruction.group(capstone.CS_GRP_JUMP) or
                    instruction.group(capstone.CS_GRP_CALL)):
                continue
            if is_direct_relative_call_or_jump(instruction):
                direct_branch_count += 1
                target = instruction.operands[0].imm
                for site in sites:
                    if site["site_rva"] < target < site["continuation_rva"]:
                        findings[site["role"]]["direct_branch_sources"].append(
                            rva_text(instruction.address))
            else:
                indirect_branch_count += 1

    pe.parse_data_directories(
        directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])
    relocation_count = 0
    image_base = pe.OPTIONAL_HEADER.ImageBase
    for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
        for entry in block.entries:
            if entry.type != IMAGE_REL_BASED_HIGHLOW or not rva_is_executable(entry.rva, sections):
                continue
            relocation_count += 1
            value = pe.get_dword_at_rva(entry.rva)
            target = value - image_base
            for site in sites:
                if site["site_rva"] < target < site["continuation_rva"]:
                    findings[site["role"]]["highlow_relocation_operands"].append(
                        rva_text(entry.rva))

    return findings, {"executable_section_count": len(sections),
                      "direct_branch_instruction_count": direct_branch_count,
                      "indirect_branch_instruction_count": indirect_branch_count,
                      "executable_highlow_relocation_count": relocation_count}


def verify(game, checks_path=CHECKS_PATH, boundaries_path=BOUNDARIES_PATH, minhook_source=None):
    checks = json.loads(checks_path.read_text())
    boundaries = json.loads(boundaries_path.read_text())
    native = game / "Bin/Sam2Game.dll"
    if not native.is_file():
        fail("installed Bin/Sam2Game.dll is missing")
    native_sha256 = sha256_file(native)
    if native_sha256 != checks["native_sha256"]:
        fail("unsupported native module fingerprint")
    if boundaries["native_sha256"] != native_sha256:
        fail("boundary record fingerprint differs from site-check fingerprint")
    if len(checks["sites"]) != 8 or len(boundaries["candidates"]) != 8:
        fail("expected exactly eight sniper predicate sites")

    paths = minhook_paths(minhook_source)
    minhook_hashes = {name: sha256_file(path) for name, path in paths.items()}
    if minhook_hashes != checks["pinned_minhook"]["source_sha256"]:
        fail("pinned MinHook/HDE source fingerprint changed")
    hde32_host = checks["pinned_hde32_host"]
    if hde32_host["hde32s_size"] != HDE32S_EXPECTED_SIZE:
        fail("pinned HDE32 structure size is not the expected x86 layout")
    if hde32_host["site_count"] != 8:
        fail("pinned HDE32 acceptance record does not cover eight sites")

    pe = pefile.PE(str(native), fast_load=True)
    if pe.FILE_HEADER.Machine != 0x14C:
        fail("native module is not PE32/i386")
    if pe.OPTIONAL_HEADER.ImageBase != checks["image_base"]:
        fail("native module image base changed")
    sections = executable_sections(pe)
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    decoder.skipdata = True

    boundary_by_role = {candidate["role"]: candidate for candidate in boundaries["candidates"]}
    resolved_sites = []
    aggregate_stolen_instruction_count = 0
    aggregate_continuation_instruction_count = 0
    with built_hde32_host_decoder(paths) as decode_hde32:
        for expected in checks["sites"]:
            role = expected["role"]
            boundary = boundary_by_role.get(role)
            if boundary is None:
                fail(f"{role}: absent from boundary record")
            site_rva = parse_rva(expected["site_rva"])
            if parse_rva(boundary["site_rva"]) != site_rva:
                fail(f"{role}: site RVA differs from boundary record")

            instructions = []
            hde32_stolen_facts = []
            cursor = site_rva
            while cursor - site_rva < MINHOOK_X86_JMP_REL_SIZE:
                instruction = first_instruction(decoder, pe, cursor, role)
                instructions.append(instruction)
                hde32_stolen_facts.append(
                    hde32_fact_for_instruction(decode_hde32, pe, instruction, role))
                cursor += instruction.size

            # CreateTrampolineFunction calls HDE_DISASM before it notices that
            # oldPos already reaches the five-byte patch size and appends JMP.
            continuation_instruction = first_instruction(decoder, pe, cursor, role)
            hde32_continuation_fact = hde32_fact_for_instruction(
                decode_hde32, pe, continuation_instruction, role)
            hde32_actual = {
                "hde32s_size": hde32_stolen_facts[0]["structure_size"],
                "stolen_instruction_count": len(hde32_stolen_facts),
                "stolen_facts_sha256": hde32_fact_digest(hde32_stolen_facts),
                "continuation_instruction_count": 1,
                "continuation_facts_sha256": hde32_fact_digest([hde32_continuation_fact]),
            }
            if hde32_actual != expected["hde32_host"]:
                fail(f"{role}: HDE32 host acceptance differs from pinned facts")
            aggregate_stolen_instruction_count += hde32_actual["stolen_instruction_count"]
            aggregate_continuation_instruction_count += 1
            actual = {
                "role": role,
                "site_rva": rva_text(site_rva),
                "stolen_sha256": hashlib.sha256(pe.get_data(site_rva, cursor - site_rva)).hexdigest(),
                "instruction_sha256": instruction_text_digest(instructions),
                "instruction_lengths": [instruction.size for instruction in instructions],
            }
            actual.update(minhook_x86_plan(instructions, site_rva, role))
            for key in ("site_rva", "stolen_sha256", "instruction_sha256", "instruction_lengths",
                        "stolen_bytes", "continuation_rva", "old_instruction_offsets",
                        "new_instruction_offsets", "relocation_decisions", "trampoline_bytes",
                        "patch_above_required"):
                if actual[key] != expected[key]:
                    fail(f"{role}: {key} differs from pinned static facts")
            if boundary["stolen_sha256"] != actual["stolen_sha256"]:
                fail(f"{role}: stolen-window hash differs from boundary record")
            if boundary["stolen_instruction_bytes"] != actual["instruction_lengths"]:
                fail(f"{role}: instruction lengths differ from boundary record")
            if parse_rva(boundary["continuation_rva"]) != cursor:
                fail(f"{role}: continuation differs from boundary record")
            resolved_sites.append({"role": role, "site_rva": site_rva,
                                   "continuation_rva": cursor, "actual": actual,
                                   "hde32_host": hde32_actual})
    if aggregate_stolen_instruction_count != hde32_host["stolen_instruction_count"]:
        fail("pinned HDE32 stolen-instruction total differs")
    if aggregate_continuation_instruction_count != hde32_host["continuation_instruction_count"]:
        fail("pinned HDE32 continuation-instruction total differs")

    references, scan = scan_direct_interior_references(pe, decoder, resolved_sites, sections)
    if scan != checks["scan"]:
        fail("executable-section scan coverage differs from pinned static facts")
    report_sites = []
    for resolved in resolved_sites:
        role = resolved["role"]
        evidence = references[role]
        expected_evidence = next(site for site in checks["sites"] if site["role"] == role)["interior_references"]
        if evidence != expected_evidence:
            fail(f"{role}: direct or relocation reference enters overwritten interior")
        report_sites.append({**resolved["actual"], "hde32_host": resolved["hde32_host"],
                             "interior_direct_references": evidence,
                             "indirect_incoming_coverage": "UNKNOWN"})

    return {"schema": checks["schema"], "native_sha256": native_sha256,
            "runtime_executed": False, "windows_code_executed": False,
            "hooks_installed": False, "minhook_relocation_executed": False,
            "method": checks["method"], "pinned_minhook": checks["pinned_minhook"],
            "pinned_hde32_host": hde32_host,
            "scan": scan, "sites": report_sites,
            "limits": {"direct_interior_evidence": "NO_DIRECT_INTERIOR_REFERENCE_FOUND",
                       "direct_scan_completeness": "UNKNOWN_LINEAR_SWEEP_ONLY",
                       "indirect_incoming_coverage": "UNKNOWN",
                       "hde_continuation_acceptance": "PINNED_HDE32_HOST_ACCEPTED",
                       "object_lifetime_or_hook_safety": "UNKNOWN"}}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, default=ROOT.parent,
                        help="Serious Sam 2 install root (read-only)")
    parser.add_argument("--checks", type=Path, default=CHECKS_PATH,
                        help="pinned non-proprietary static facts JSON")
    parser.add_argument("--minhook-source", type=Path,
                        help="Pinned checkout; default uses the configured build dependency")
    arguments = parser.parse_args()
    try:
        print(json.dumps(verify(arguments.game.resolve(), arguments.checks.resolve(), minhook_source=arguments.minhook_source), indent=2))
    except (OSError, ValueError, json.JSONDecodeError) as error:
        raise SystemExit(f"FAIL CLOSED: {error}") from error
