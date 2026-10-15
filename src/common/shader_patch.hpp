#pragma once
#include <algorithm>
#include <iterator>
#include <cstdint>
#include <cstddef>
#include <cstring>
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
    unsigned constants = 0;      // Highest constant register read + 1 (relative reads: 256).
    bool eyeClip = false;        // Patched with the per-eye world-fixed reflection lookup (water).
    unsigned clipMatrix = 0;     // With eyeClip: first of the model -> clip rows (c#..c#+3).
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
constexpr unsigned Temp = 0, Input = 1, Const = 2, RastOut = 4, TexCrdOut = 6;
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
                if (k > 0 && type == Const)
                    info.constants = std::max(info.constants, (op[k] & 0x2000u) ? 256u : num + 1);
                if (k == 0 && type == RastOut && num == 0) ++info.positionWrites;
                if (k == 0 && type == TexCrdOut && num < 8) info.texcoordsWritten |= 1u << num;
            }
            ++info.instructions;
        }
        i += 1 + size_t(n);
    }
    return false; // No end token.
}

// The water program (WaterShader_VP_mainCalc) derives its reflection texcoords
// from the clip-space position it also writes to oPos: `mad rX.xy, -rN.xz,
// c3.x, rX` (ripple) followed by `mov rX.y, -rX.y`. That screen mapping cannot
// work for two eyes (in single pass it was the centre camera's: the image lay
// on the water and turned with the head). Ahead of the mad the patch puts a
// world-fixed lookup in rX instead: the view ray from the eye (c<eyeClip>.xyz,
// set for each half) to the vertex (rP, the position the m4x4 projected),
// reflected about the water normal rN, mapped like a sky dome:
// (R.x, R.z) / (1 + |R.y|) * c<eyeClip+1>.x, with z = w = 1. The game's own
// ripple, flip and bias then apply as before. Returns the word index of the
// mad (and its registers), or 0.
inline size_t eyeClipSite(const uint32_t *words, size_t count, unsigned &reg, unsigned *normal = nullptr,
                          unsigned *position = nullptr, unsigned *matrix = nullptr) {
    using namespace detail;
    int lastProjection = -1;
    for (size_t i = 1; i + 8 < count;) {
        const uint32_t token = words[i], opcode = token & 0xffffu;
        if (token == 0x0000ffffu) break;
        if (opcode == 0xfffeu) { i += 1 + ((token >> 16) & 0x7fffu); continue; }
        const int n = operands(opcode);
        if (n < 0) break;
        const uint32_t *op = words + i + 1;
        if (opcode == 0x14 && regType(op[0]) == Temp && regType(op[1]) == Temp) // m4x4 rX, rP, c#
            lastProjection = int(i);
        if (opcode == 0x04 && regType(op[0]) == Temp && ((op[0] >> 16) & 0xfu) == 0x3 &&
            regType(op[1]) == Temp && ((op[1] >> 24) & 0xfu) == 1 && regType(op[2]) == Const &&
            regNum(op[2]) == 3 && ((op[2] >> 16) & 0xffu) == 0x00 && regType(op[3]) == Temp &&
            regNum(op[3]) == regNum(op[0]) && words[i + 5] == 0x01u &&
            words[i + 6] == dst(Temp, regNum(op[0]), 0x2) &&
            words[i + 7] == (src(Temp, regNum(op[0]), 0x55u) | (1u << 24))) {
            if (lastProjection < 0 || regNum(words[size_t(lastProjection) + 1]) != regNum(op[0])) return 0;
            reg = regNum(op[0]);
            if (normal) *normal = regNum(op[1]);
            if (position) *position = regNum(words[size_t(lastProjection) + 2]);
            if (matrix) *matrix = regNum(words[size_t(lastProjection) + 3]);
            return i;
        }
        i += 1 + size_t(n);
    }
    return 0;
}
// Builds the stereo variant. False (out untouched) if the program cannot be patched.
// `eyeClip` (a free constant register, 0 = off) enables the water texcoord fix.
// With `probe` the water variant instead writes the world-space reflection
// direction (model-space R turned by the rows in c<eyeClip+2..+4>) to the
// texcoord set the reflection lookup wrote, for a cube map of the surroundings
// (env_probe.cpp, and patchWaterPixelShader below).
inline bool patchVertexShader(const uint32_t *words, size_t count, std::vector<uint32_t> &out, VertexInfo *infoOut = nullptr,
                              unsigned eyeClip = 0, bool probe = false) {
    using namespace detail;
    VertexInfo info;
    if (!inspect(words, count, info) || !info.positionWrites) return false;
    constexpr uint32_t stereoInputs = ((1u << StereoInputCount) - 1) << FirstStereoInput;
    if (info.inputsDeclared & stereoInputs) return false;
    if (info.temps > 11) return false; // vs_1_1 has r0..r11; r11 holds the original position.
    const unsigned position = 11;
    unsigned clipReg = 0, normalReg = 0, positionReg = 0;
    size_t clipAt = eyeClip ? eyeClipSite(words, count, clipReg, &normalReg, &positionReg, &info.clipMatrix) : 0;
    if (clipAt && (info.temps > 9 || info.constants > eyeClip || eyeClip + (probe ? 5 : 2) > 96)) clipAt = 0; // r9, r10 and c<eyeClip..> must be free.
    bool reflectionWritten = !probe || !clipAt;
    info.eyeClip = clipAt != 0;
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
        if (clipAt && i == clipAt) {
            const uint32_t neg = 1u << 24, e = eyeClip, k = eyeClip + 1;
            const uint32_t dome[] = {
                0x03u, dst(Temp, 9, 0x7), src(Temp, positionReg), src(Const, e),              // sub r9.xyz, rP, cE
                0x08u, dst(Temp, 9, 0x8), src(Temp, 9), src(Temp, 9),                        // dp3 r9.w, r9, r9
                0x07u, dst(Temp, 9, 0x8), src(Temp, 9, 0xffu),                                // rsq r9.w, r9.w
                0x05u, dst(Temp, 9, 0x7), src(Temp, 9), src(Temp, 9, 0xffu),                  // mul r9.xyz, r9, r9.w  (V)
                0x08u, dst(Temp, 10, 0x1), src(Temp, 9), src(Temp, normalReg),                // dp3 r10.x, V, N
                0x02u, dst(Temp, 10, 0x1), src(Temp, 10, 0x00u), src(Temp, 10, 0x00u),        // add r10.x, r10.x, r10.x
                0x04u, dst(Temp, 9, 0x7), src(Temp, 10, 0x00u) | neg, src(Temp, normalReg), src(Temp, 9), // R = V - 2(V.N)N
                0x0bu, dst(Temp, 10, 0x2), src(Temp, 9, 0x55u), src(Temp, 9, 0x55u) | neg,    // max r10.y, R.y, -R.y
                0x02u, dst(Temp, 10, 0x2), src(Temp, 10, 0x55u), src(Const, k, 0x55u),        // add r10.y, |R.y|, 1
                0x06u, dst(Temp, 10, 0x2), src(Temp, 10, 0x55u),                              // rcp r10.y
                0x05u, dst(Temp, clipReg, 0x3), src(Temp, 9, 0xa8u), src(Temp, 10, 0x55u),    // mul rX.xy, R.xz, r10.y
                0x05u, dst(Temp, clipReg, 0x3), src(Temp, clipReg), src(Const, k, 0x00u),     // mul rX.xy, rX, cK.x
                0x01u, dst(Temp, clipReg, 0xc), src(Const, k, 0x55u),                         // mov rX.zw, cK.y (1)
            };
            // The view ray and its reflection (the first seven instructions), then
            // either the dome mapping or the world direction for the probe.
            result.insert(result.end(), std::begin(dome), probe ? std::begin(dome) + 28 : std::end(dome));
            if (probe) {
                const uint32_t world[] = {
                    0x08u, dst(Temp, 10, 0x1), src(Temp, 9), src(Const, e + 2), // dp3 r10.x, R, cE+2
                    0x08u, dst(Temp, 10, 0x2), src(Temp, 9), src(Const, e + 3), // dp3 r10.y, R, cE+3
                    0x08u, dst(Temp, 10, 0x4), src(Temp, 9), src(Const, e + 4), // dp3 r10.z, R, cE+4
                    0x01u, dst(Temp, 10, 0x8), src(Const, k, 0x55u),            // mov r10.w, 1
                };
                result.insert(result.end(), std::begin(world), std::end(world));
            }
        }
        // Probe: the texcoord the reflection lookup wrote (from rX) takes the direction.
        if (clipAt && probe && !reflectionWritten && i > clipAt && n >= 2 && regType(words[i + 1]) == TexCrdOut) {
            bool fromLookup = false;
            for (int k = 1; k < n; ++k)
                if (regType(words[i + 1 + k]) == Temp && regNum(words[i + 1 + k]) == clipReg) fromLookup = true;
            if (fromLookup) {
                result.push_back(0x01u);
                result.push_back(dst(TexCrdOut, regNum(words[i + 1]), 0xf));
                result.push_back(src(Temp, 10));
                reflectionWritten = true;
                i += 1 + size_t(n);
                continue;
            }
        }
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
    if (!reflectionWritten) return false; // Probe mode needs the lookup's texcoord.
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

// The water pixel program (WaterShader_PP20_bump) looks the reflection image up
// projectively: `mad rA.xy, rB, tC.z, tC` (ripple offset scaled by w), `mov
// rA.w, tC.w`, `texldp rA, rA, sN`. For the environment cube (the vertex
// variant writes the world reflection direction to tC) the ripple turns the
// direction instead: `mad rA.xyz, rB.x, c30, tC`, `mad rA.xyz, rB.y, c31, rA`
// with c30 = (Ripple,0,0,0), c31 = (0,0,Ripple,0), then `texld rA, rA, sN`
// from sN declared as a cube and `mul rA.xyz, rA, c29` (c29 = `gain`: the
// program scales the painted image's dark values up, the cube holds the real
// scene). `sampler` returns N. False if no such sequence, or the program
// already uses c29..c31.
constexpr float WaterRipple = .15f; // Direction change for the ripple's texture offset.
inline bool patchWaterPixelShader(const uint32_t *words, size_t count, std::vector<uint32_t> &out, unsigned &sampler,
                                  float gain = 1) {
    using namespace detail;
    constexpr unsigned Sampler = 10;
    if (!words || count < 2 || words[0] != PsVersion20) return false;
    size_t site = 0, dclAt = 0;
    unsigned a = 0, b = 0, c = 0, n = 0;
    for (size_t i = 1; i < count && words[i] != 0x0000ffffu;) {
        const uint32_t token = words[i], opcode = token & 0xffffu;
        if (opcode == 0xfffeu) { i += 1 + ((token >> 16) & 0x7fffu); continue; }
        const size_t len = (token >> 24) & 0xfu;
        if (i + 1 + len > count) return false;
        for (size_t k = 1; k <= len && opcode != 0x51 && opcode != 0x1f; ++k)
            if (regType(words[i + k]) == Const && regNum(words[i + k]) >= 29 && regNum(words[i + k]) <= 31) return false;
        const uint32_t *op = words + i + 1;
        if (!site && opcode == 0x04 && len == 4 && regType(op[0]) == Temp && ((op[0] >> 16) & 0xfu) == 0x3 &&
            regType(op[1]) == Temp && regType(op[2]) == Texture && ((op[2] >> 16) & 0xffu) == 0xaa &&
            regType(op[3]) == Texture && regNum(op[3]) == regNum(op[2]) && ((op[3] >> 16) & 0xffu) == 0xe4 &&
            !(op[1] >> 24 & 0xf) && !(op[2] >> 24 & 0xf) && !(op[3] >> 24 & 0xf)) {
            const size_t mov = i + 5, tex = i + 8;
            a = regNum(op[0]); b = regNum(op[1]); c = regNum(op[2]);
            if (tex + 3 < count && (words[mov] & 0xffffu) == 0x01 && ((words[mov] >> 24) & 0xfu) == 2 &&
                regType(words[mov + 1]) == Temp && regNum(words[mov + 1]) == a && ((words[mov + 1] >> 16) & 0xfu) == 0x8 &&
                words[mov + 2] == src(Texture, c, 0xffu) && (words[tex] & 0xffffu) == 0x42 && ((words[tex] >> 24) & 0xfu) == 3 &&
                regType(words[tex + 1]) == Temp && regNum(words[tex + 1]) == a && regType(words[tex + 2]) == Temp &&
                regNum(words[tex + 2]) == a && regType(words[tex + 3]) == Sampler) {
                site = i; n = regNum(words[tex + 3]);
            }
        }
        i += 1 + len;
    }
    if (!site) return false;
    // The sampler's declaration (D3DSTT_2D) becomes a cube (D3DSTT_CUBE).
    for (size_t i = 1; i < count && words[i] != 0x0000ffffu;) {
        const uint32_t token = words[i], opcode = token & 0xffffu;
        if (opcode == 0xfffeu) { i += 1 + ((token >> 16) & 0x7fffu); continue; }
        const size_t len = (token >> 24) & 0xfu;
        if (opcode == 0x1f && len == 2 && regType(words[i + 2]) == Sampler && regNum(words[i + 2]) == n) {
            if ((words[i + 1] & 0x78000000u) != 0x10000000u) return false;
            dclAt = i;
        }
        i += 1 + len;
    }
    if (!dclAt) return false;
    const auto bits = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
    std::vector<uint32_t> result;
    result.reserve(count + 16);
    result.push_back(words[0]);
    result.insert(result.end(), {0x05000051u, dst(Const, 29, 0xf), bits(gain), bits(gain), bits(gain), bits(1),
                                 0x05000051u, dst(Const, 30, 0xf), bits(WaterRipple), 0u, 0u, 0u,
                                 0x05000051u, dst(Const, 31, 0xf), 0u, 0u, bits(WaterRipple), 0u});
    const uint32_t pp = words[site + 1] & 0x00200000u; // Keep partial precision.
    for (size_t i = 1; i < count && words[i] != 0x0000ffffu;) {
        const uint32_t token = words[i], opcode = token & 0xffffu;
        if (opcode == 0xfffeu) { i += 1 + ((token >> 16) & 0x7fffu); continue; }
        const size_t len = (token >> 24) & 0xfu;
        if (i == dclAt) {
            result.insert(result.end(), {token, (words[i + 1] & ~0x78000000u) | 0x18000000u, words[i + 2]});
        } else if (i == site) {
            result.insert(result.end(), {
                0x04000004u, dst(Temp, a, 0x7) | pp, src(Temp, b, 0x00u), src(Const, 30), src(Texture, c),
                0x04000004u, dst(Temp, a, 0x7) | pp, src(Temp, b, 0x55u), src(Const, 31), src(Temp, a),
                0x03000042u, words[site + 9], words[site + 10], words[site + 11],
                0x03000005u, dst(Temp, a, 0x7) | pp, src(Temp, a), src(Const, 29)});
            i = site + 12; // mad, mov and texldp replaced.
            continue;
        } else result.insert(result.end(), words + i, words + i + 1 + len);
        i += 1 + len;
    }
    result.push_back(0x0000ffffu);
    sampler = n;
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
