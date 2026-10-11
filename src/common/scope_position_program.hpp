#pragma once
#include "scope_program.hpp"
#include "scope_geometry.hpp"
#include "scope_buffer_layout.hpp"
#include <algorithm>
#include <bit>

namespace ss2vr {
// Optional image admission, never a replacement vertex/animation evaluator.
// Only the captured stock cap's vertex positions are checked. Unknown inputs or
// unsupported arithmetic propagate unknown, so they cannot certify oPos.
namespace scope_position {
struct Scalar { float value = 0; bool known = false; };
using Vector = std::array<Scalar,4>;
// Optional COPIED rigid input for offline multi-palette assessment. This is not
// native draw admission or a skin/animation replacement. Legacy callers retain
// their exact zero-index/one-weight inputs by leaving this pointer null.
struct RigidPaletteInput {
    std::array<uint8_t,4> indices{},weights{255,0,0,0};
    uint32_t paletteCount=0;
    bool valid() const noexcept {
        return paletteCount>=1 && paletteCount<=3 && indices[0]<paletteCount &&
               !indices[1] && !indices[2] && !indices[3] &&
               weights==std::array<uint8_t,4>{255,0,0,0};
    }
};
// Separate vehicle policy. Never widens the existing <=3 weapon replay policy.
struct VehicleRigidPaletteInput {
    std::array<uint8_t,4> indices{},weights{255,0,0,0};
    uint32_t paletteCount=0;
    bool valid() const noexcept {
        return paletteCount>=1 && paletteCount<=32 && indices[0]<paletteCount &&
            !indices[1] && !indices[2] && !indices[3] && weights==std::array<uint8_t,4>{255,0,0,0};
    }
};
inline Scalar value(float v) noexcept { return {v,std::isfinite(v)}; }
inline Scalar add(Scalar a,Scalar b) noexcept { return a.known && b.known ? value(a.value+b.value) : Scalar{}; }
inline Scalar mul(Scalar a,Scalar b) noexcept { return a.known && b.known ? value(a.value*b.value) : Scalar{}; }
inline Vector constant(const std::array<float,4> &v) noexcept {
    return {value(v[0]),value(v[1]),value(v[2]),value(v[3])};
}
inline bool position(std::span<const uint32_t> words,std::span<const std::array<float,4>> uploaded,
                     Vec3 p,std::array<float,2> uv,bool weightsBound,std::array<float,4> &out,
                     GeometryInputLayout layout=GeometryInputLayout::Legacy56,
                     const RigidPaletteInput *copiedRigid=nullptr,
                     const VehicleRigidPaletteInput *vehicleRigid=nullptr) noexcept {
    using namespace scope_program;
    if(uploaded.size()>256 || (copiedRigid && vehicleRigid) ||
       (copiedRigid && (!weightsBound || !copiedRigid->valid() || !vertexPositionProgram(words))) ||
       (vehicleRigid && (!weightsBound || !vehicleRigid->valid() || !vertexPositionProgram(words))))return false;
    if(layout!=GeometryInputLayout::Legacy56 && layout!=GeometryInputLayout::Observed78 &&
       layout!=GeometryInputLayout::NoUV56 && layout!=GeometryInputLayout::ObservedMultiUV78 &&
       layout!=GeometryInputLayout::NoUV78)return false;
    if(layout!=GeometryInputLayout::Legacy56 && !weightsBound)return false;
    std::array<std::array<float,4>,256> constants{};
    std::array<bool,256> available{};
    for (size_t i=0;i<uploaded.size();++i) { constants[i]=uploaded[i]; available[i]=true; }
    // DEF is shader-local constant state, not a runtime assignment. Collect
    // every admitted definition before evaluating any position instruction.
    for (size_t cursor=1;cursor<words.size();) {
        const auto opcode=words[cursor++];
        if (opcode==0xffffu) break;
        if ((opcode&0xffffu)==0xfffeu) { cursor+=(opcode>>16)&0x7fff; continue; }
        if (opcode==31) { cursor+=2; continue; }
        if (opcode==81) {
            const auto index=words[cursor++]&0x7ffu;
            for (auto &v:constants[index]) v=std::bit_cast<float>(words[cursor++]);
            available[index]=true;
        } else if (opcode) cursor+=sources(opcode)+1;
    }
    std::array<Vector,12> temporary{};
    std::array<Vector,16> inputs{};
    inputs[0]=constant({p.x,p.y,p.z,1}); // Actual admitted FLOAT3 position.
    if(layout!=GeometryInputLayout::NoUV56 && layout!=GeometryInputLayout::ObservedMultiUV78 &&
       layout!=GeometryInputLayout::NoUV78)
        inputs[3]=constant({uv[0],uv[1],0,1}); // Actual admitted FLOAT2 diffuse UV.
    // These families use the UV copy only for asset identification.
    // MultiUV78/NoUV78 leave every v1-v6 unknown, including declared v2.
    const bool uses78=layout==GeometryInputLayout::Observed78 || layout==GeometryInputLayout::ObservedMultiUV78 ||
                      layout==GeometryInputLayout::NoUV78;
    const unsigned local=uses78?7:5,weight=uses78?8:6;
    inputs[local]=constant({0,0,0,0}); // Exact hashed first-local-palette indices, UBYTE4N.
    if(copiedRigid || vehicleRigid) {
        // D3DDECLTYPE_UBYTE4N is component-order byte/255, not D3DCOLOR's swizzle.
        // Fractional/unavailable shader addresses still decline below; no GPU
        // conversion/rounding rule or native transform reference is invented.
        std::array<float,4> normalized{};
        const auto &indices=copiedRigid?copiedRigid->indices:vehicleRigid->indices;
        for(unsigned i=0;i<4;++i)normalized[i]=float(indices[i])/255.f;
        inputs[local]=constant(normalized);
    }
    if (weightsBound) inputs[weight]=constant({1,0,0,0}); // Exact 255/0/0/0 UBYTE4N weights.
    Scalar address;
    Vector clip{};
    const auto read = [&](uint32_t token) noexcept {
        Vector source{};
        unsigned index=token&0x7ffu;
        switch (type(token)) {
        case 0: source=temporary[index]; break;
        case 1: source=inputs[index]; break;
        case 2:
            if (token&0x2000u) {
                // Admitted stock indices are exact integers. Do not guess GPU
                // address rounding for fractional or unavailable a0.x values.
                if (!address.known || address.value!=std::trunc(address.value) ||
                    address.value < -255 || address.value > 255) break;
                const int relative=int(index)+int(address.value);
                if (relative<0 || relative>=256) break;
                index=unsigned(relative);
            }
            if (available[index]) source=constant(constants[index]);
            break;
        default: break;
        }
        Vector result;
        for (unsigned channel=0;channel<4;++channel) {
            result[channel]=source[(token>>(16+2*channel))&3u];
            if (((token>>24)&15u)==1 && result[channel].known) result[channel].value=-result[channel].value;
        }
        return result;
    };
    for (size_t cursor=1;cursor<words.size();) {
        const auto opcode=words[cursor++];
        if (opcode==0xffffu) {
            for (unsigned i=0;i<4;++i) { if (!clip[i].known) return false; out[i]=clip[i].value; }
            return true;
        }
        if ((opcode&0xffffu)==0xfffeu) { cursor+=(opcode>>16)&0x7fff; continue; }
        if (opcode==31) { cursor+=2; continue; }
        if (opcode==81) { cursor+=5; continue; }
        if (!opcode) continue;
        const auto dest=words[cursor++];
        const unsigned count=sources(opcode);
        const auto operands=words.subspan(cursor,count);
        cursor+=count;
        Vector a=read(operands[0]), b=count>1?read(operands[1]):Vector{}, c=count>2?read(operands[2]):Vector{};
        Vector result{};
        if (opcode==1) result=a;
        else if (opcode==2 || opcode==3 || opcode==4 || opcode==5)
            for (unsigned i=0;i<4;++i) {
                if (opcode==3 && b[i].known) b[i].value=-b[i].value;
                result[i]=opcode==5?mul(a[i],b[i]):opcode==4?add(mul(a[i],b[i]),c[i]):add(a[i],b[i]);
            }
        else if (opcode==8 || opcode==9) {
            Scalar sum=value(0);
            for (unsigned i=0;i<(opcode==8?3u:4u);++i) sum=add(sum,mul(a[i],b[i]));
            result.fill(sum);
        } else if (opcode>=20 && opcode<=24) {
            const unsigned rows=opcode==20 || opcode==22?4:opcode==24?2:3;
            const unsigned columns=opcode==20 || opcode==21?4:3;
            for (unsigned row=0;row<rows;++row) {
                const auto coefficient=read(operands[1]+row);
                Scalar sum=value(0);
                for (unsigned i=0;i<columns;++i) sum=add(sum,mul(a[i],coefficient[i]));
                result[row]=sum;
            }
        }
        // Other accepted UV-program opcodes remain unknown for position proof.
        // Their color/normal results cannot become a known position by accident.
        Vector *target=nullptr;
        if (type(dest)==0) target=&temporary[dest&0x7ffu];
        else if (type(dest)==4 && (dest&0x7ffu)==0) target=&clip;
        if (target) for (unsigned i=0;i<4;++i) if (dest&(1u<<(16+i))) (*target)[i]=result[i];
        if (type(dest)==3) {
            address=result[0];
            if(vehicleRigid) {
                // VS1.1 MOV a0.x uses round-to-nearest. Microsoft's reference
                // does not specify halfway ties; those decline rather than guess.
                // Legacy weapon callers retain their exact-integer requirement.
                if(opcode!=1 || !address.known)address={};
                else {
                    const double low=std::floor(double(address.value));
                    const double fraction=double(address.value)-low;
                    if(fraction==.5 || low < -256 || low>255)address={};
                    else address=value(float(low+(fraction>.5?1:0)));
                }
            }
        }
    }
    return false;
}
} // namespace scope_position

inline bool scopeCapClip(const Matrix44 &projection,const Matrix34 &view,const Matrix34 &affine,Matrix44 &out) noexcept {
    out={};
    if (!finiteMatrix(view) || !finiteMatrix(affine)) return false;
    for (float v:projection.m) if (!std::isfinite(v)) return false;
    const auto modelView=affineMultiply(view,affine);
    for (unsigned row=0;row<4;++row) for (unsigned col=0;col<4;++col) {
        double v=col==3?projection.m[row*4+3]:0;
        for (unsigned k=0;k<3;++k) v+=double(projection.m[row*4+k])*modelView.m[k*4+col];
        out.m[row*4+col]=float(v);
        if (!std::isfinite(out.m[row*4+col])) return false;
    }
    return true;
}
// Preconditions: exact admitted position/UV/zero-index/one-weight stock bytes
// and matching stream declarations; immutable program and actual constants.
// CPU float evaluation is an offline/admission bound, not GPU precision proof.
// The program runs in float through several native matrix products whose
// world-space translations are large, so the comparison is scaled by the
// vertex's clip magnitude (at least |w|). Lab errors reach 1.6e-4 by position in
// the level; 1e-3 (a few millimetres) still rejects any wrong pose or matrix.
inline bool scopeCapPosition(std::span<const uint32_t> program,std::span<const std::array<float,4>> constants,
                             const ScopeCapGeometry &cap,bool weightsBound,const Matrix44 &expected,
                             double *worst=nullptr) noexcept {
    if (worst) *worst = -1;
    if (!cap.valid || constants.empty() || constants.size()>256 || !scopeUvProgram(program)) return false;
    for (float v:expected.m) if (!std::isfinite(v)) return false;
    double largest=0;
    for (size_t vertex=0;vertex<cap.positions.size();++vertex) {
        std::array<float,4> actual{};
        const auto p=cap.positions[vertex];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
            !scope_position::position(program,constants,p,cap.uv[vertex],weightsBound,actual)) return false;
        std::array<double,4> reference{};
        double scale=1e-3;
        for (unsigned row=0;row<4;++row) {
            reference[row]=double(expected.m[row*4])*p.x + double(expected.m[row*4+1])*p.y +
                double(expected.m[row*4+2])*p.z + expected.m[row*4+3];
            if (!std::isfinite(reference[row]) || !std::isfinite(actual[row])) return false;
            scale=std::max({scale,std::abs(reference[row]),std::abs(double(actual[row]))});
        }
        for (unsigned row=0;row<4;++row) largest=std::max(largest,std::abs(actual[row]-reference[row])/scale);
    }
    if (worst) *worst = largest;
    return largest<=1e-3;
}
} // namespace ss2vr
