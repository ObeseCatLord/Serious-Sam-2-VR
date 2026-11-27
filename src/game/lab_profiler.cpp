#include "game.hpp"
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <vector>

namespace ss2vr::game {
namespace {
// While a thread is suspended only system calls run here (times, context, a
// copy of its stack): it may hold the loader lock or the heap lock, so module
// lookups and allocations wait until it runs again.
constexpr unsigned StackWords = 192;
struct Range { HMODULE module; uintptr_t begin, end; };
std::vector<Range> gameModules() {
    std::vector<Range> ranges;
    for (const wchar_t *name : {L"Sam2.exe", L"Core.dll", L"Engine.dll", L"Sam2Game.dll", L"GfxD3D.dll", L"D3D9.dll",
                                L"Shaders.dll", L"ProcRender.dll", L"SfxOAL.dll", L"CodecOGG.dll", L"OpenAL32.dll"}) {
        const auto module = GetModuleHandleW(name);
        if (!module) continue;
        const auto base = reinterpret_cast<uintptr_t>(module);
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
        ranges.push_back({module, base, base + nt->OptionalHeader.SizeOfImage});
    }
    return ranges;
}
const wchar_t *moduleName(HMODULE module, wchar_t (&path)[MAX_PATH]) {
    const wchar_t *name = L"?";
    if (module && GetModuleFileNameW(module, path, MAX_PATH)) {
        name = path;
        for (const wchar_t *c = path; *c; ++c) if (*c == L'\\' || *c == L'/') name = c + 1;
    }
    return name;
}
// System call stubs and other exported system code: the nearest export at or
// below an offset of ntdll, win32u, KERNELBASE or kernel32 (which wait or call
// the thread is in), read from the loaded image's export table.
bool systemModule(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    const wchar_t *name = moduleName(module, path);
    return !_wcsicmp(name, L"ntdll.dll") || !_wcsicmp(name, L"win32u.dll") || !_wcsicmp(name, L"KERNELBASE.dll") ||
           !_wcsicmp(name, L"KERNEL32.DLL");
}
const char *exportAt(HMODULE module, uintptr_t offset, uintptr_t &distance) {
    if (!module || !systemModule(module)) return nullptr;
    const auto base = reinterpret_cast<const uint8_t *>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    const auto &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress || !directory.Size) return nullptr;
    const auto *exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(base + directory.VirtualAddress);
    const auto *names = reinterpret_cast<const DWORD *>(base + exports->AddressOfNames);
    const auto *ordinals = reinterpret_cast<const WORD *>(base + exports->AddressOfNameOrdinals);
    const auto *functions = reinterpret_cast<const DWORD *>(base + exports->AddressOfFunctions);
    const char *best = nullptr;
    uintptr_t bestAt = 0;
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        if (ordinals[i] >= exports->NumberOfFunctions) continue;
        const uintptr_t at = functions[ordinals[i]];
        // Forwarders point into the export directory itself.
        if (at > offset || at < bestAt || (at >= directory.VirtualAddress && at < directory.VirtualAddress + directory.Size)) continue;
        if (at == bestAt && best) continue;
        best = reinterpret_cast<const char *>(base + names[i]); bestAt = at;
    }
    distance = offset - bestAt;
    return best && distance < 0x400 ? best : nullptr;
}
using Spot = std::pair<HMODULE, uintptr_t>;
using Spots = std::map<Spot, unsigned>;
int describe(char *line, int n, size_t size, const Spots &spots, size_t count) {
    std::vector<std::pair<unsigned, Spot>> top;
    for (const auto &h : spots) top.push_back({h.second, h.first});
    std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (size_t i = 0; i < top.size() && i < count && n < int(size) - 160; ++i) {
        wchar_t path[MAX_PATH]{};
        n += std::snprintf(line + n, size - n, " %ls+%x=%u", moduleName(top[i].second.first, path),
                           unsigned(top[i].second.second), top[i].first);
        uintptr_t distance = 0;
        if (const char *name = exportAt(top[i].second.first, top[i].second.second, distance))
            n += std::snprintf(line + n, size - n, "(%.40s+%x)", name, unsigned(distance));
    }
    return n;
}
DWORD WINAPI profile(void *) {
    const DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
    Spots hits;
    std::map<DWORD, uint64_t> cpuAt, cpuDelta; // 100 ns units of user+kernel time
    std::map<DWORD, Spots> perThread, callers, perModule; // perModule: every sample of a thread, by module.
    std::vector<HMODULE> systemModules;
    for (const wchar_t *name : {L"ntdll.dll", L"win32u.dll", L"KERNELBASE.dll", L"kernel32.dll"})
        if (const auto module = GetModuleHandleW(name)) systemModules.push_back(module);
    std::map<DWORD, std::map<std::vector<Spot>, unsigned>> chains;
    std::vector<Range> modules = gameModules();
    unsigned samples = 0;
    uint64_t reported = GetTickCount64();
    DWORD stack[StackWords];
    for (;;) {
        Sleep(2);
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) continue;
        THREADENTRY32 entry{sizeof entry};
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                       entry.th32ThreadID);
            if (!thread) continue;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            FILETIME created{}, exited{}, kernel{}, user{};
            bool timed = false, sampled = false;
            SIZE_T copied = 0;
            if (SuspendThread(thread) != DWORD(-1)) {
                timed = GetThreadTimes(thread, &created, &exited, &kernel, &user);
                sampled = GetThreadContext(thread, &context);
                if (sampled && !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(uintptr_t(context.Esp)),
                                                  stack, sizeof stack, &copied))
                    copied = 0;
                ResumeThread(thread);
            }
            CloseHandle(thread);
            const DWORD id = entry.th32ThreadID;
            if (timed) {
                const uint64_t total = (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                                       (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
                if (!cpuAt.count(id)) cpuAt[id] = total;
                cpuDelta[id] = total - cpuAt[id];
            }
            if (!sampled) continue;
            HMODULE module = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(uintptr_t(context.Eip)), &module);
            // Exact in system modules (a thread inside a system call sits at one address,
            // named in the report); 256-byte buckets elsewhere.
            const bool exact = module && std::find(systemModules.begin(), systemModules.end(), module) != systemModules.end();
            const uintptr_t offset = module ? (uintptr_t(context.Eip) - uintptr_t(module)) & ~uintptr_t(exact ? 0 : 0xff)
                                            : uintptr_t(context.Eip) & ~uintptr_t(0xfff);
            ++perModule[id][{module, 0}];
            ++hits[{module, offset}];
            ++perThread[id][{module, offset}];
            ++samples;
            // The nearest game code on the stack (who called into the wait or the
            // driver), and the first few game return addresses as a chain.
            std::vector<Spot> chain{{module, module ? uintptr_t(context.Eip) - uintptr_t(module) : uintptr_t(context.Eip)}};
            bool gameCaller = false;
            for (unsigned w = 0; w < copied / sizeof(DWORD) && chain.size() < 12; ++w) {
                const uintptr_t value = stack[w];
                const auto range = std::find_if(modules.begin(), modules.end(),
                                                [&](const Range &r) { return value >= r.begin + 0x1000 && value < r.end; });
                if (range != modules.end() && !gameCaller) {
                    gameCaller = true;
                    ++callers[id][{range->module, (value - range->begin) & ~uintptr_t(0xf)}];
                }
                // Any module's code address (system and runtime frames too).
                HMODULE owner = range != modules.end() ? range->module : nullptr;
                if (!owner && value > 0x10000 &&
                    !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        reinterpret_cast<LPCWSTR>(value), &owner))
                    owner = nullptr;
                if (owner && value - reinterpret_cast<uintptr_t>(owner) >= 0x1000)
                    chain.push_back({owner, value - reinterpret_cast<uintptr_t>(owner)});
            }
            ++chains[id][chain];
        }
        CloseHandle(snapshot);
        const uint64_t now = GetTickCount64();
        if (now - reported < 5000) continue;
        reported = now;
        modules = gameModules();
        char line[1400];
        int n = std::snprintf(line, sizeof line, "Lab profile samples=%u:", samples);
        describe(line, n, sizeof line, hits, 14);
        log("%s", line);
        std::vector<std::pair<uint64_t, DWORD>> busy;
        for (const auto &c : cpuDelta) busy.push_back({c.second, c.first});
        std::sort(busy.begin(), busy.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        for (size_t t = 0; t < busy.size() && t < 3; ++t) {
            const DWORD id = busy[t].second;
            int m = std::snprintf(line, sizeof line, "Lab profile thread %lu cpu=%.2fs:", id, double(busy[t].first) / 1e7);
            describe(line, m, sizeof line, perThread[id], 10);
            log("%s", line);
            // All of the thread's samples by module (the list above is its ten hottest spots only).
            m = std::snprintf(line, sizeof line, "Lab profile thread %lu modules:", id);
            describe(line, m, sizeof line, perModule[id], 20);
            log("%s", line);
        }
        // Every thread: its nearest game caller, if any, and its most common
        // chain of code addresses (raw stack scan: may include stale ones).
        for (const auto &t : chains) {
            int m = std::snprintf(line, sizeof line, "Lab profile caller thread %lu cpu=%.2fs:", t.first,
                                  double(cpuDelta[t.first]) / 1e7);
            describe(line, m, sizeof line, callers[t.first], 6);
            log("%s", line);
            const auto best = std::max_element(t.second.begin(), t.second.end(),
                                               [](const auto &a, const auto &b) { return a.second < b.second; });
            if (best == t.second.end()) continue;
            m = std::snprintf(line, sizeof line, "Lab profile chain thread %lu n=%u:", t.first, best->second);
            for (const auto &spot : best->first) {
                wchar_t path[MAX_PATH]{};
                m += std::snprintf(line + m, sizeof line - m, " %ls+%x", moduleName(spot.first, path), unsigned(spot.second));
            }
            log("%s", line);
        }
        for (auto &c : cpuAt) c.second += cpuDelta[c.first];
        cpuDelta.clear();
        perThread.clear();
        perModule.clear();
        callers.clear();
        chains.clear();
        hits.clear();
        samples = 0;
    }
}
} // namespace
void startLabProfiler() noexcept {
    wchar_t v[2]{};
    static bool started = false;
    if (started || GetEnvironmentVariableW(L"SS2VR_LAB_PROFILE", v, 2) != 1 || v[0] != L'1') return;
    started = true;
    if (HANDLE thread = CreateThread(nullptr, 0, profile, nullptr, 0, nullptr)) CloseHandle(thread);
}
} // namespace ss2vr::game
