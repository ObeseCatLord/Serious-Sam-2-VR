#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

// Single-pass stereo vertex shader patch (vs_1_1, the only vertex program
// version the engine creates). The patched program reads a per-eye clip-space
// transform from six extra inputs on stream 15 and applies it to the position:
//
//   oPos            = R * p        (R rows: v11..v14, p = the original oPos value)
//   oT<k>.x         = K . p        (eye half test for the pixel shader's texkill)
//   oT<k>.yzw       = v10.yzw      (v10 = 0,1,1,1)
//
// for the lowest texcoord output k the program does not write itself (the
// other unwritten outputs get v10: pixel programs may read sets their vertex
// program leaves unwritten, and must not see the eye test there). The
// original writes to oPos are redirected to a free temporary. Nothing else
// changes: constants (bones index dynamically up to c255) are not touched.
namespace ss2vr::shader_patch {
constexpr uint32_t VsVersion11 = 0xfffe0101u;
constexpr unsigned FirstStereoInput = 10, StereoInputCount = 6; // v10..v15
constexpr unsigned StereoRowsBytes = StereoInputCount * 16;      // One eye's instance record.

struct VertexInfo {
    bool parsed = false;
    unsigned temps = 0;          // Highest temp register used + 1.
    uint32_t inputsDeclared = 0; // Bit per v#.
    uint32_t texcoordsWritten = 0;
    unsigned positionWrites = 0;
    unsigned instructions = 0;
};
// The texcoord output carrying the eye test (8 when the program writes all).
inline unsigned killSet(const VertexInfo &original) {
    unsigned k = 0;
    while (k < 8 && (original.texcoordsWritten & (1u << k))) ++k;
    return k;
}

namespace detail {
inline unsigned regType(uint32_t t) { return ((t >> 28) & 7u) | ((t >> 8) & 0x18u); }
inline unsigned regNum(uint32_t t) { return t & 0x7ffu; }
// Operand counts for SM 1.x vertex opcodes (lengths are not encoded below 2.0).
inline int operands(uint32_t opcode) {
    switch (opcode) {
    case 0x00: return 0;                                     // nop
    case 0x01: case 0x06: case 0x07: case 0x0e: case 0x0f:   // mov rcp rsq exp log
    case 0x10: case 0x13: case 0x23: case 0x24: case 0x2e:   // lit frc abs nrm mova
    case 0x4e: case 0x4f: return 2;                          // expp logp
    case 0x02: case 0x03: case 0x05: case 0x08: case 0x09:   // add sub mul dp3 dp4
    case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x11:   // min max slt sge dst
    case 0x14: case 0x15: case 0x16: case 0x17: case 0x18:   // m4x4 m4x3 m3x4 m3x3 m3x2
    case 0x20: case 0x21: return 3;                          // pow crs
    case 0x04: case 0x12: case 0x22: return 4;               // mad lrp sgn
    case 0x1f: return 2;                                     // dcl: usage token + register
    case 0x51: return 5;                                     // def: register + 4 floats
    default: return -1;
    }
}
inline uint32_t dst(unsigned type, unsigned num, unsigned mask) {
    return 0x80000000u | ((type & 7u) << 28) | ((type & 0x18u) << 8) | (mask << 16) | num;
}
inline uint32_t src(unsigned type, unsigned num, uint32_t swizzle = 0xe4u) {
    return 0x80000000u | ((type & 7u) << 28) | ((type & 0x18u) << 8) | (swizzle << 16) | num;
}
constexpr unsigned Temp = 0, Input = 1, RastOut = 4, TexCrdOut = 6;
} // namespace detail

// Reads the program; false for anything the patcher does not understand.
inline bool inspect(const uint32_t *words, size_t count, VertexInfo &info) {
    using namespace detail;
    info = {};
    if (!words || count < 2 || words[0] != VsVersion11) return false;
    for (size_t i = 1; i < count;) {
        const uint32_t token = words[i];
        if (token == 0x0000ffffu) { info.parsed = true; return true; }
        const uint32_t opcode = token & 0xffffu;
        if (opcode == 0xfffeu) { i += 1 + ((token >> 16) & 0x7fffu); continue; } // comment
        const int n = operands(opcode);
        if (n < 0 || i + 1 + size_t(n) > count) return false;
        const uint32_t *op = words + i + 1;
        if (opcode == 0x1f) {
            if (regType(op[1]) == Input && regNum(op[1]) < 32) info.inputsDeclared |= 1u << regNum(op[1]);
        } else if (opcode != 0x51) {
            for (int k = 0; k < n; ++k) {
                if (k > 0 && (op[k] & 0x2000u)) {} // Relative (a0) constant addressing: untouched.
                const unsigned type = regType(op[k]), num = regNum(op[k]);
                if (type == Temp) info.temps = num + 1 > info.temps ? num + 1 : info.temps;
                if (k == 0 && type == RastOut && num == 0) ++info.positionWrites;
                if (k == 0 && type == TexCrdOut && num < 8) info.texcoordsWritten |= 1u << num;
            }
            ++info.instructions;
        }
        i += 1 + size_t(n);
    }
    return false; // No end token.
}

// Builds the stereo variant. False (out untouched) if the program cannot be patched.
inline bool patchVertexShader(const uint32_t *words, size_t count, std::vector<uint32_t> &out, VertexInfo *infoOut = nullptr) {
    using namespace detail;
    VertexInfo info;
    if (!inspect(words, count, info) || !info.positionWrites) return false;
    constexpr uint32_t stereoInputs = ((1u << StereoInputCount) - 1) << FirstStereoInput;
    if (info.inputsDeclared & stereoInputs) return false;
    if (info.temps > 11) return false; // vs_1_1 has r0..r11; r11 holds the original position.
    const unsigned position = 11;
    std::vector<uint32_t> result;
    result.reserve(count + 64);
    result.push_back(words[0]);
    // Declarations for v10..v15 (TEXCOORD usage, index = register), ahead of everything.
    for (unsigned k = 0; k < StereoInputCount; ++k) {
        const unsigned reg = FirstStereoInput + k;
        result.push_back(0x1fu);                          // dcl
        result.push_back(0x80000000u | 5u | (reg << 16)); // usage TEXCOORD (5), usage index = reg
        result.push_back(dst(Input, reg, 0xf));
    }
    size_t i = 1;
    for (; i < count && words[i] != 0x0000ffffu;) {
        const uint32_t token = words[i];
        const uint32_t opcode = token & 0xffffu;
        if (opcode == 0xfffeu) { // Comments are dropped.
            i += 1 + ((token >> 16) & 0x7fffu);
            continue;
        }
        const int n = operands(opcode);
        result.push_back(token);
        for (int k = 0; k < n; ++k) {
            uint32_t operand = words[i + 1 + k];
            if (k == 0 && opcode != 0x1f && opcode != 0x51 && regType(operand) == RastOut && regNum(operand) == 0)
                operand = (operand & 0x0fff0000u) | dst(Temp, position, 0); // Keep mask/modifiers, write r11.
            result.push_back(operand);
        }
        i += 1 + size_t(n);
    }
    // oPos = (v11 . p, v12 . p, v13 . p, v14 . p)
    for (unsigned row = 0; row < 4; ++row) {
        result.push_back(0x09u); // dp4
        result.push_back(dst(RastOut, 0, 1u << row));
        result.push_back(src(Temp, position));
        result.push_back(src(Input, FirstStereoInput + 1 + row));
    }
    for (unsigned k = 0; k < 8; ++k) {
        if (info.texcoordsWritten & (1u << k)) continue;
        const bool test = k == killSet(info);
        if (test) {
            result.push_back(0x09u); // dp4 oTk.x, p, v15
            result.push_back(dst(TexCrdOut, k, 0x1));
            result.push_back(src(Temp, position));
            result.push_back(src(Input, FirstStereoInput + 5));
        }
        result.push_back(0x01u); // mov oTk.yzw (or .xyzw), v10
        result.push_back(dst(TexCrdOut, k, test ? 0xe : 0xf));
        result.push_back(src(Input, FirstStereoInput));
    }
    result.push_back(0x0000ffffu);
    out = std::move(result);
    if (infoOut) *infoOut = info;
    return true;
}

// Pixel programs: the instanced pass draws both eyes into the double-wide target
// with one draw, so each fragment of the other eye's half is discarded with a
// texkill on texcoord set `k` (the vertex program writes -/+x of the eye's clip
// position there). The set must be one the pixel program does not read.
constexpr uint32_t PsVersion11 = 0xffff0101u, PsVersion12 = 0xffff0102u, PsVersion13 = 0xffff0103u,
                   PsVersion20 = 0xffff0200u;
namespace detail {
inline int pixelOperands(uint32_t opcode) {
    switch (opcode) {
    case 0x00: return 0;
    case 0x01: case 0x06: case 0x07: case 0x13: return 2;            // mov rcp rsq frc
    case 0x02: case 0x03: case 0x05: case 0x08: case 0x09: return 3;  // add sub mul dp3 dp4
    case 0x04: case 0x12: case 0x50: case 0x58: return 4;            // mad lrp cnd cmp
    case 0x40: case 0x41: case 0x42: case 0x57: return 1;            // texcoord texkill tex texdepth
    case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4a:
    case 0x4d: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: return 2;
    case 0x4c: case 0x59: return 3;                                  // texm3x3spec bem
    case 0x51: return 5;                                             // def
    default: return -1;
    }
}
constexpr unsigned Texture = 3;
} // namespace detail
// Texcoord sets a pixel program reads (bit per set); false if not understood.
inline bool pixelTexcoords(const uint32_t *words, size_t count, uint32_t &sets) {
    using namespace detail;
    sets = 0;
    if (!words || count < 2) return false;
    const uint32_t version = words[0];
    const bool sm1 = version == PsVersion11 || version == PsVersion12 || version == PsVersion13;
    if (!sm1 && version != PsVersion20) return false;
    for (size_t i = 1; i < count;) {
        const uint32_t token = words[i];
        if (token == 0x0000ffffu) return true;
        const uint32_t opcode = token & 0xffffu;
        if (opcode == 0xfffeu) { i += 1 + ((token >> 16) & 0x7fffu); continue; }
        const int n = sm1 ? pixelOperands(opcode) : int((token >> 24) & 0xfu);
        if (n < 0 || i + 1 + size_t(n) > count) return false;
        for (int k = 0; k < n; ++k) {
            const uint32_t t = words[i + 1 + k];
            if (opcode == 0x51 && k > 0) continue;  // def immediates
            if (opcode == 0x1f && k == 0) continue; // dcl usage token
            if (regType(t) == Texture && regNum(t) < 8) sets |= 1u << regNum(t);
        }
        i += 1 + size_t(n);
    }
    return false;
}
// Inserts `texkill t<k>` (and, for ps_2_0, its declaration) ahead of the
// program's first non-definition instruction.
inline bool patchPixelShader(const uint32_t *words, size_t count, unsigned k, std::vector<uint32_t> &out) {
    using namespace detail;
    uint32_t sets = 0;
    if (!pixelTexcoords(words, count, sets) || k >= 8 || (sets & (1u << k))) return false;
    const uint32_t version = words[0];
    const bool sm2 = version == PsVersion20;
    if (!sm2 && k >= 4) return false;
    std::vector<uint32_t> result;
    result.reserve(count + 8);
    result.push_back(version);
    size_t i = 1;
    bool inserted = false;
    const auto insert = [&] {
        if (sm2) {
            result.push_back(0x1fu | (2u << 24));                    // dcl t<k>
            result.push_back(0x80000000u);
            result.push_back(dst(Texture, k, 0xf));
            result.push_back(0x41u | (1u << 24));                    // texkill t<k>
        } else result.push_back(0x41u);
        result.push_back(dst(Texture, k, 0xf));
        inserted = true;
    };
    while (i < count && words[i] != 0x0000ffffu) {
        const uint32_t token = words[i], opcode = token & 0xffffu;
        if (opcode == 0xfffeu) { i += 1 + ((token >> 16) & 0x7fffu); continue; }
        const int n = sm2 ? int((token >> 24) & 0xfu) : pixelOperands(opcode);
        if (n < 0) return false;
        // Definitions (and ps_2_0 declarations) stay first.
        if (!inserted && opcode != 0x51 && !(sm2 && opcode == 0x1f)) insert();
        result.insert(result.end(), words + i, words + i + 1 + n);
        i += 1 + size_t(n);
    }
    if (!inserted) insert();
    result.push_back(0x0000ffffu);
    out = std::move(result);
    return true;
}

// One eye's instance record (v10..v15): misc (0,1,1,1), R rows 0..3, kill row K.
struct EyeRecord {
    float misc[4]{0, 1, 1, 1};
    float r[4][4]{};
    float kill[4]{};
};
static_assert(sizeof(EyeRecord) == StereoRowsBytes);
} // namespace ss2vr::shader_patch
