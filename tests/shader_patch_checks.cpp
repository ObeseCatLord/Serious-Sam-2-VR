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
    // Water-like program: m4x4 r2, v0, c7; mov oPos, r2; mad r2.xy, -r5.xz, c3.x, r2;
    // mov r2.y, -r2.y; mov oT2, r2. With eyeClip the clip position the texcoords are
    // derived from becomes K . r2 (K in c30..c33) ahead of the mad, after oPos took r2.
    {
        const std::vector<uint32_t> water{
            VsVersion11,
            0x1f, 0x80000000u | 0u, dst(Input, 0, 0xf),
            0x14, dst(Temp, 2, 0xf), src(Input, 0), src(Const, 7),
            0x01, dst(RastOut, 0, 0xf), src(Temp, 2),
            0x04, dst(Temp, 2, 0x3), src(Temp, 5, 0x08u) | (1u << 24), src(Const, 3, 0x00u), src(Temp, 2),
            0x01, dst(Temp, 2, 0x2), src(Temp, 2, 0x55u) | (1u << 24),
            0x01, dst(TexCrdOut, 2, 0xf), src(Temp, 2),
            0x0000ffffu};
        unsigned reg = 0;
        assert(eyeClipSite(water.data(), water.size(), reg) == 11 && reg == 2);
        VertexInfo w;
        assert(patchVertexShader(water.data(), water.size(), out, &w, 30) && w.eyeClip);
        size_t first = 0, mad = 0;
        for (size_t i = 0; i + 3 < out.size(); ++i) {
            if (!first && out[i] == 0x09 && out[i + 1] == dst(Temp, 10, 0x1)) {
                assert(out[i + 2] == src(Temp, 2) && out[i + 3] == src(Const, 30));
                first = i;
            }
            if (!mad && out[i] == 0x04 && out[i + 1] == dst(Temp, 2, 0x3)) mad = i;
        }
        assert(first && mad == first + 16 + 3 && out[mad - 3] == 0x01 && out[mad - 2] == dst(Temp, 2, 0xf) &&
               out[mad - 1] == src(Temp, 10));
        // Before the mad, oPos already took r2 (now r11): the eye's position is unchanged.
        bool redirected = false;
        for (size_t i = 0; i + 2 < first; ++i)
            if (out[i] == 0x01 && out[i + 1] == dst(Temp, 11, 0xf) && out[i + 2] == src(Temp, 2)) redirected = true;
        assert(redirected);
        VertexInfo plain;
        assert(patchVertexShader(water.data(), water.size(), out, &plain) && !plain.eyeClip);
        for (size_t i = 0; i + 1 < out.size(); ++i) assert(!(out[i] == 0x09 && out[i + 1] == dst(Temp, 10, 0x1)));
        // A program reading c30 or above keeps the plain variant.
        std::vector<uint32_t> busyConstants = water;
        busyConstants.insert(busyConstants.end() - 1, {0x01, dst(TexCrdOut, 3, 0xf), src(Const, 31)});
        assert(patchVertexShader(busyConstants.data(), busyConstants.size(), out, &w, 30) && !w.eyeClip);
        // An ordinary program has no water site.
        assert(!eyeClipSite(program.data(), program.size(), reg));
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
