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
    assert(patched.texcoordsWritten == 0xff);         // oT1..oT7 carry the eye test.
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
    std::puts("shader patch ok");
}
