#include "common/shader_patch.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
using namespace ss2vr::shader_patch;
int main() {
    using namespace detail;
    // vs_1_1: dcl_position v0; dcl_texcoord v2; m4x4 oPos, v0, c1; mov oT0, v2; mov oD0, c0.
    const std::vector<uint32_t> program{
        VsVersion11,
        0x1f, 0x80000000u | 0u, dst(Input, 0, 0xf),
        0x1f, 0x80000000u | 5u | (2u << 16), dst(Input, 2, 0xf),
        0x14, dst(RastOut, 0, 0xf), src(Input, 0), src(2, 1),
        0x01, dst(TexCrdOut, 0, 0xf), src(Input, 2),
        0x01, dst(5, 0, 0xf), src(2, 0),
        0x0000ffffu};
    VertexInfo info;
    assert(inspect(program.data(), program.size(), info) && info.positionWrites == 1 && info.texcoordsWritten == 1);
    std::vector<uint32_t> out;
    assert(patchVertexShader(program.data(), program.size(), out));
    VertexInfo patched;
    assert(inspect(out.data(), out.size(), patched));
    assert(patched.inputsDeclared == ((1u << 0) | (1u << 2) | (0x3fu << 10)));
    assert(patched.positionWrites == 4);              // Four dp4 rows, the m4x4 now writes r11.
    assert(patched.texcoordsWritten == 0xff);         // oT1 carries the eye test, oT2..oT7 v10.
    assert(killSet(info) == 1);
    for (size_t i = 0; i + 3 < out.size(); ++i)        // The only dp4 into a texcoord output is oT1.x.
        if (out[i] == 0x09 && ((out[i + 1] >> 28) & 7) == TexCrdOut) assert(out[i + 1] == dst(TexCrdOut, 1, 0x1));
    assert(patched.temps == 12);
    // The m4x4 destination is r11 with the full mask.
    bool found = false;
    for (size_t i = 0; i + 1 < out.size(); ++i)
        if (out[i] == 0x14) { assert(out[i + 1] == dst(Temp, 11, 0xf)); found = true; break; }
    assert(found);
    // Programs that already declare a stereo input, or use r11, are refused.
    std::vector<uint32_t> busy = program;
    busy.insert(busy.begin() + 1, {0x1f, 0x80000000u | 5u | (12u << 16), dst(Input, 12, 0xf)});
    assert(!patchVertexShader(busy.data(), busy.size(), out));
    std::vector<uint32_t> temps = program;
    temps.insert(temps.end() - 1, {0x01, dst(Temp, 11, 0xf), src(Input, 0)});
    assert(!patchVertexShader(temps.data(), temps.size(), out));
    // Water-like program: mov r4, v0; mov r5, v0; m4x4 r2, r4, c7; mov oPos, r2;
    // mad r2.xy, -r5.xz, c3.x, r2; mov r2.y, -r2.y; mov oT2, r2. With eyeClip the
    // reflection lookup in r2 is replaced ahead of the mad by the world-fixed
    // dome lookup (eye in c30, scale/1 in c31), after oPos took r2.
    {
        const std::vector<uint32_t> water{
            VsVersion11,
            0x1f, 0x80000000u | 0u, dst(Input, 0, 0xf),
            0x01, dst(Temp, 4, 0xf), src(Input, 0),
            0x01, dst(Temp, 5, 0xf), src(Input, 0),
            0x14, dst(Temp, 2, 0xf), src(Temp, 4), src(Const, 7),
            0x01, dst(RastOut, 0, 0xf), src(Temp, 2),
            0x04, dst(Temp, 2, 0x3), src(Temp, 5, 0x08u) | (1u << 24), src(Const, 3, 0x00u), src(Temp, 2),
            0x01, dst(Temp, 2, 0x2), src(Temp, 2, 0x55u) | (1u << 24),
            0x01, dst(TexCrdOut, 2, 0xf), src(Temp, 2),
            0x0000ffffu};
        unsigned reg = 0, normal = 0, position = 0;
        assert(eyeClipSite(water.data(), water.size(), reg, &normal, &position) == 17 && reg == 2 && normal == 5 &&
               position == 4);
        VertexInfo w;
        assert(patchVertexShader(water.data(), water.size(), out, &w, 30) && w.eyeClip);
        size_t first = 0, mad = 0;
        for (size_t i = 0; i + 3 < out.size(); ++i) {
            if (!first && out[i] == 0x03 && out[i + 1] == dst(Temp, 9, 0x7)) {
                assert(out[i + 2] == src(Temp, 4) && out[i + 3] == src(Const, 30));
                first = i;
            }
            if (!mad && out[i] == 0x04 && out[i + 1] == dst(Temp, 2, 0x3)) mad = i;
        }
        assert(first && mad > first && out[mad - 3] == 0x01 && out[mad - 2] == dst(Temp, 2, 0xc) &&
               out[mad - 1] == src(Const, 31, 0x55u));
        bool redirected = false;
        for (size_t i = 0; i + 2 < first; ++i)
            if (out[i] == 0x01 && out[i + 1] == dst(Temp, 11, 0xf) && out[i + 2] == src(Temp, 2)) redirected = true;
        assert(redirected);
        VertexInfo patched2;
        assert(inspect(out.data(), out.size(), patched2) && patched2.temps == 12);
        VertexInfo plain;
        assert(patchVertexShader(water.data(), water.size(), out, &plain) && !plain.eyeClip);
        // A program reading c30 or above keeps the plain variant.
        std::vector<uint32_t> busyConstants = water;
        busyConstants.insert(busyConstants.end() - 1, {0x01, dst(TexCrdOut, 3, 0xf), src(Const, 31)});
        assert(patchVertexShader(busyConstants.data(), busyConstants.size(), out, &w, 30) && !w.eyeClip);
        assert(!eyeClipSite(program.data(), program.size(), reg));
        // A matrix read ending in c30 (m4x4 from c27 reads c27..c30) keeps the plain variant too.
        std::vector<uint32_t> matrixRead = water;
        matrixRead.insert(matrixRead.end() - 1, {0x14, dst(Temp, 6, 0xf), src(Temp, 4), src(Const, 27)});
        assert(patchVertexShader(matrixRead.data(), matrixRead.size(), out, &w, 30) && !w.eyeClip);
        // Probe mode: the world reflection direction (R turned by c32..c34) in r10
        // replaces the texcoord the lookup wrote.
        VertexInfo probe;
        assert(patchVertexShader(water.data(), water.size(), out, &probe, 30, true) && probe.eyeClip);
        bool turned = false, written = false, lookupLeft = false;
        for (size_t i = 0; i + 3 < out.size(); ++i) {
            if (out[i] == 0x08 && out[i + 1] == dst(Temp, 10, 0x1) && out[i + 2] == src(Temp, 2) && out[i + 3] == src(Const, 32))
                turned = true; // The parallax lookup L (in the lookup's register r2) turned to world.
            if (out[i] == 0x01 && out[i + 1] == dst(TexCrdOut, 2, 0xf) && out[i + 2] == src(Temp, 10)) written = true;
            if (out[i] == 0x01 && out[i + 1] == dst(TexCrdOut, 2, 0xf) && out[i + 2] == src(Temp, 2)) lookupLeft = true;
        }
        assert(turned && written && !lookupLeft);
        // The parallax sphere is centred on the cube's anchor (c35), not on the eye (c30).
        bool anchoredCentre = false;
        for (size_t i = 0; i + 3 < out.size(); ++i)
            if (out[i] == 0x03 && out[i + 3] == src(Const, 35)) anchoredCentre = true;
        assert(anchoredCentre);
        // Per pixel: o = P - anchor (c35) turned into the lookup's texcoord, the view
        // ray P - eye (c30) turned into r9 and written to set 7; no sphere in the program.
        VertexInfo perPixel;
        assert(patchVertexShader(water.data(), water.size(), out, &perPixel, 30, true, 0, true) && perPixel.eyeClip);
        bool fromAnchor = false, fromEye = false, rayTurned = false, viewWritten = false, lookupWritten = false, sphere = false;
        for (size_t i = 0; i + 3 < out.size(); ++i) {
            if (out[i] == 0x03 && out[i + 1] == dst(Temp, 10, 0x7) && out[i + 3] == src(Const, 35)) fromAnchor = true;
            if (out[i] == 0x03 && out[i + 1] == dst(Temp, 10, 0x7) && out[i + 3] == src(Const, 30)) fromEye = true;
            if (out[i] == 0x08 && out[i + 1] == dst(Temp, 9, 0x4) && out[i + 3] == src(Const, 34)) rayTurned = true;
            if (out[i] == 0x01 && out[i + 1] == dst(TexCrdOut, 7, 0xf) && out[i + 2] == src(Temp, 9)) viewWritten = true;
            if (out[i] == 0x01 && out[i + 1] == dst(TexCrdOut, 2, 0xf) && out[i + 2] == src(Temp, 10)) lookupWritten = true;
            if (out[i] == 0x07 && out[i + 1] == dst(Temp, 10, 0x8)) sphere = true; // The per-vertex sphere's rsq.
        }
        assert(fromAnchor && fromEye && rayTurned && viewWritten && lookupWritten && !sphere);
        // A program writing set 7 itself has no per-pixel variant.
        std::vector<uint32_t> seventh = water;
        seventh.insert(seventh.end() - 1, {0x01, dst(TexCrdOut, 7, 0xf), src(Temp, 0)});
        assert(!patchVertexShader(seventh.data(), seventh.size(), out, &w, 30, true, 0, true));
        // Probe mode needs constants up to c34 free and a texcoord fed by the lookup.
        assert(patchVertexShader(busyConstants.data(), busyConstants.size(), out, &w, 30, true) && !w.eyeClip);
        // Mirror mode: the lookup's clip position becomes the point's position in
        // the mirror camera (m4x4 r2, r4, c35) ahead of the game's ripple, which
        // still reads r2; the texcoord stays the lookup's.
        VertexInfo mirrored;
        assert(patchVertexShader(water.data(), water.size(), out, &mirrored, 30, false, 35) && mirrored.eyeClip);
        size_t matrix = 0, ripple = 0;
        bool stillLookup = false, dome = false;
        for (size_t i = 0; i + 3 < out.size(); ++i) {
            if (out[i] == 0x14 && out[i + 1] == dst(Temp, 2, 0xf) && out[i + 2] == src(Temp, 10) && out[i + 3] == src(Const, 35)) matrix = i;
            if (!ripple && out[i] == 0x04 && out[i + 1] == dst(Temp, 2, 0x3)) ripple = i;
            if (out[i] == 0x01 && out[i + 1] == dst(TexCrdOut, 2, 0xf) && out[i + 2] == src(Temp, 2)) stillLookup = true;
            if (out[i] == 0x0bu && out[i + 1] == dst(Temp, 10, 0x2)) dome = true; // The dome's |R.y| (not here).
        }
        // The reflected ray (eye in c30, normal r5), then the point cK.z along it into the mirror camera.
        bool along = false;
        for (size_t i = 0; i + 4 < out.size(); ++i)
            if (out[i] == 0x04 && out[i + 1] == dst(Temp, 10, 0x7) && out[i + 2] == src(Temp, 9) &&
                out[i + 3] == src(Const, 31, 0xaau) && out[i + 4] == src(Temp, 4)) along = true;
        assert(matrix && ripple == matrix + 4 && stillLookup && !dome && along);
        // Refused without the water site, in probe mode, or over the eye constants.
        assert(!patchVertexShader(program.data(), program.size(), out, &w, 30, false, 35));
        assert(!patchVertexShader(water.data(), water.size(), out, &w, 30, true, 35));
        assert(!patchVertexShader(water.data(), water.size(), out, &w, 30, false, 32));
    }
    // Water pixel program: ripple offset + projective reflection lookup becomes a
    // cube lookup turned by the ripple.
    {
        constexpr uint32_t Ps20 = 0xffff0200u, Tex = 3, Smp = 10, Out = 8;
        const auto reg = [](unsigned type, unsigned num) { return ((type & 7u) << 28) | ((type & 0x18u) << 8) | num; };
        const std::vector<uint32_t> ps{
            Ps20,
            0x0200001fu, 0x80000000u, 0x80000000u | reg(Tex, 0) | (0x3u << 16),
            0x0200001fu, 0x80000000u, 0x80000000u | reg(Tex, 2) | (0xfu << 16),
            0x0200001fu, 0x90000000u, 0x80000000u | reg(Smp, 0) | (0xfu << 16),
            0x0200001fu, 0x90000000u, 0x80000000u | reg(Smp, 2) | (0xfu << 16),
            0x03000042u, dst(Temp, 0, 0xf), 0x80000000u | reg(Tex, 0) | (0xe4u << 16), 0x80000000u | reg(Smp, 0) | (0xe4u << 16),
            0x04000004u, dst(Temp, 2, 0x3), src(Temp, 0), 0x80000000u | reg(Tex, 2) | (0xaau << 16), 0x80000000u | reg(Tex, 2) | (0xe4u << 16),
            0x02000001u, dst(Temp, 2, 0x8), 0x80000000u | reg(Tex, 2) | (0xffu << 16),
            0x03010042u, dst(Temp, 2, 0xf), src(Temp, 2), 0x80000000u | reg(Smp, 2) | (0xe4u << 16),
            0x02000001u, 0x80000000u | reg(Out, 0) | (0xfu << 16), src(Temp, 2),
            0x0000ffffu};
        std::vector<uint32_t> cube;
        unsigned sampler = 99;
        assert(patchWaterPixelShader(ps.data(), ps.size(), cube, sampler) && sampler == 2);
        assert(cube[1] == 0x05000051u && cube[2] == dst(Const, 29, 0xf) && cube[8] == dst(Const, 30, 0xf) && cube[13] == 0x05000051u);
        bool cubeDcl = false, plainLookup = false, projective = false, turnedX = false, turnedZ = false;
        for (size_t i = 0; i + 4 < cube.size(); ++i) {
            if (cube[i] == 0x0200001fu && cube[i + 1] == 0x98000000u && cube[i + 2] == (0x80000000u | reg(Smp, 2) | (0xfu << 16))) cubeDcl = true;
            if (cube[i] == 0x03000042u && cube[i + 1] == dst(Temp, 2, 0xf)) plainLookup = true;
            if (cube[i] == 0x03010042u) projective = true;
            if (cube[i] == 0x04000004u && cube[i + 1] == dst(Temp, 2, 0x7) && cube[i + 2] == src(Temp, 0, 0x00u) &&
                cube[i + 3] == src(Const, 30)) turnedX = true;
            if (cube[i] == 0x04000004u && cube[i + 1] == dst(Temp, 2, 0x7) && cube[i + 2] == src(Temp, 0, 0x55u) &&
                cube[i + 3] == src(Const, 31) && cube[i + 4] == src(Temp, 2)) turnedZ = true;
        }
        assert(cubeDcl && plainLookup && !projective && turnedX && turnedZ && cube.back() == 0x0000ffffu);
        assert(cube.size() == ps.size() + 18 + 6); // Three defs; mad+mov+texldp (12 words) -> mad+mad+texld+mul (18).
        // Projective (mirror image): the 2D sampler, ripple and texldp stay; the gain follows.
        std::vector<uint32_t> mirror;
        sampler = 99;
        assert(patchWaterPixelShader(ps.data(), ps.size(), mirror, sampler, .5f, true) && sampler == 2);
        bool keptDcl = false, keptLookup = false, gained = false;
        for (size_t i = 0; i + 4 < mirror.size(); ++i) {
            if (mirror[i] == 0x0200001fu && mirror[i + 1] == 0x90000000u && mirror[i + 2] == (0x80000000u | reg(Smp, 2) | (0xfu << 16))) keptDcl = true;
            if (mirror[i] == 0x03010042u && mirror[i + 1] == dst(Temp, 2, 0xf)) {
                keptLookup = true;
                gained = mirror[i + 4] == 0x03000005u && mirror[i + 5] == dst(Temp, 2, 0x7) && mirror[i + 7] == src(Const, 29);
            }
        }
        assert(keptDcl && keptLookup && gained && mirror.size() == ps.size() + 18 + 4);
        // Per pixel: t7 declared, the view ray normalized and reflected about c26, the
        // sphere (c27) met per pixel, then the ripple, the cube lookup and the gain.
        std::vector<uint32_t> perPixel;
        sampler = 99;
        assert(patchWaterPixelShader(ps.data(), ps.size(), perPixel, sampler, .5f, false, true) && sampler == 2);
        bool viewDcl = false, normalized = false, reflected = false, met = false, looked = false;
        for (size_t i = 0; i + 4 < perPixel.size(); ++i) {
            if (perPixel[i] == 0x0200001fu && perPixel[i + 1] == 0x80000000u && perPixel[i + 2] == dst(Tex, 7, 0x7)) viewDcl = true;
            if (perPixel[i] == 0x02000024u && perPixel[i + 2] == src(Tex, 7)) normalized = true;
            if (perPixel[i] == 0x04000004u && perPixel[i + 3] == src(Const, 26)) reflected = true;
            if (perPixel[i] == 0x03000002u && perPixel[i + 3] == (src(Const, 27, 0xaau) | (1u << 24))) met = true;
            if (perPixel[i] == 0x03000042u && perPixel[i + 1] == dst(Temp, 2, 0xf)) looked = true;
        }
        assert(viewDcl && normalized && reflected && met && looked && perPixel.back() == 0x0000ffffu);
        // A program using c26 or reading t7 keeps the per-vertex variant only.
        std::vector<uint32_t> busy26 = ps;
        busy26.insert(busy26.end() - 1, {0x02000001u, dst(Temp, 3, 0xf), src(Const, 26)});
        assert(patchWaterPixelShader(busy26.data(), busy26.size(), cube, sampler) &&
               !patchWaterPixelShader(busy26.data(), busy26.size(), cube, sampler, 1, false, true));
        // Programs already using c30 are left alone; so are programs without the sequence.
        std::vector<uint32_t> busy = ps;
        busy.insert(busy.end() - 1, {0x02000001u, dst(Temp, 3, 0xf), src(Const, 30)});
        assert(!patchWaterPixelShader(busy.data(), busy.size(), cube, sampler));
        std::vector<uint32_t> other(ps.begin(), ps.begin() + 17);
        other.push_back(0x0000ffffu);
        assert(!patchWaterPixelShader(other.data(), other.size(), cube, sampler));
    }
    // Optional private check: every dumped engine vertex program patches.
    if (const char *dir = std::getenv("SS2VR_SHADER_DUMPS")) {
        unsigned total = 0, ok = 0;
        for (const auto &entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().filename().string().rfind("vs_", 0) != 0) continue;
            std::ifstream f(entry.path(), std::ios::binary);
            std::vector<uint32_t> words(std::filesystem::file_size(entry.path()) / 4);
            f.read(reinterpret_cast<char *>(words.data()), std::streamsize(words.size() * 4));
            ++total;
            if (patchVertexShader(words.data(), words.size(), out)) ++ok;
            else std::printf("unpatched %s\n", entry.path().filename().string().c_str());
        }
        std::printf("dumped vertex programs patched %u/%u\n", ok, total);
        assert(ok == total);
    }
    // Pixel programs: ps_1_1 tex t0; mov r0, t0 -> texkill on a free set ahead of tex.
    const std::vector<uint32_t> pixel{PsVersion11, 0x42, dst(3, 0, 0xf), 0x01, dst(0, 0, 0xf), src(3, 0), 0x0000ffffu};
    uint32_t sets = 0;
    assert(pixelTexcoords(pixel.data(), pixel.size(), sets) && sets == 1);
    assert(!patchPixelShader(pixel.data(), pixel.size(), 0, out)); // t0 is read.
    assert(patchPixelShader(pixel.data(), pixel.size(), 2, out));
    assert(out.size() == pixel.size() + 2 && out[1] == 0x41 && out[2] == dst(3, 2, 0xf));
    assert(pixelTexcoords(out.data(), out.size(), sets) && sets == 5);
    if (const char *dir = std::getenv("SS2VR_SHADER_DUMPS")) {
        unsigned total = 0, covered = 0;
        for (const auto &entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().filename().string().rfind("ps_", 0) != 0) continue;
            std::ifstream f(entry.path(), std::ios::binary);
            std::vector<uint32_t> words(std::filesystem::file_size(entry.path()) / 4);
            f.read(reinterpret_cast<char *>(words.data()), std::streamsize(words.size() * 4));
            ++total;
            bool any = false;
            for (unsigned k = 0; k < 8 && !any; ++k) any = patchPixelShader(words.data(), words.size(), k, out);
            if (any) ++covered; else std::printf("no free texcoord %s\n", entry.path().filename().string().c_str());
        }
        std::printf("dumped pixel programs with a free texcoord set %u/%u\n", covered, total);
    }
    std::puts("shader patch ok");
}
