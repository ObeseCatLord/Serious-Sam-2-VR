// Lab (SS2VR_LAB_PROFILE=1): a sampling profiler. Every 2 ms a thread suspends
// each other thread of the game, reads its instruction pointer and resumes it;
// every 5 s the busiest code (module + 4 KB-page offset), excluding threads
// sitting in a wait, is logged. For finding where loads spend their time.
#include "game.hpp"
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <vector>

namespace ss2vr::game {
namespace {
DWORD WINAPI profile(void *) {
    const DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
    std::map<std::pair<HMODULE, uintptr_t>, unsigned> hits;
    std::map<DWORD, unsigned> threadHits;
    std::map<DWORD, uint64_t> cpuAt, cpuDelta; // 100 ns units of user+kernel time
    std::map<DWORD, std::map<std::pair<HMODULE, uintptr_t>, unsigned>> perThread;
    unsigned samples = 0;
    uint64_t reported = GetTickCount64();
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
            if (SuspendThread(thread) != DWORD(-1)) {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                FILETIME created{}, exited{}, kernel{}, user{};
                if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
                    const uint64_t total = (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                                           (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
                    if (!cpuAt.count(entry.th32ThreadID)) cpuAt[entry.th32ThreadID] = total;
                    cpuDelta[entry.th32ThreadID] = total - cpuAt[entry.th32ThreadID];
                }
                if (GetThreadContext(thread, &context)) {
                    HMODULE module = nullptr;
                    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(uintptr_t(context.Eip)), &module);
                    const uintptr_t offset = module ? (uintptr_t(context.Eip) - uintptr_t(module)) & ~uintptr_t(0xff) : uintptr_t(context.Eip) & ~uintptr_t(0xfff);
                    ++hits[{module, offset}];
                    ++perThread[entry.th32ThreadID][{module, offset}];
                    ++threadHits[entry.th32ThreadID];
                    ++samples;
                }
                ResumeThread(thread);
            }
            CloseHandle(thread);
        }
        CloseHandle(snapshot);
        const uint64_t now = GetTickCount64();
        if (now - reported < 5000) continue;
        reported = now;
        std::vector<std::pair<unsigned, std::pair<HMODULE, uintptr_t>>> top;
        for (const auto &h : hits) top.push_back({h.second, h.first});
        std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        char line[1400];
        int n = std::snprintf(line, sizeof line, "Lab profile samples=%u:", samples);
        for (size_t i = 0; i < top.size() && i < 14 && n < 1200; ++i) {
            wchar_t path[MAX_PATH]{};
            const wchar_t *name = L"?";
            if (top[i].second.first && GetModuleFileNameW(top[i].second.first, path, MAX_PATH)) {
                name = path;
                for (const wchar_t *c = path; *c; ++c) if (*c == L'\\' || *c == L'/') name = c + 1;
            }
            n += std::snprintf(line + n, sizeof line - n, " %ls+%x=%u", name, unsigned(top[i].second.second), top[i].first);
        }
        log("%s", line);
        // The busiest threads by CPU time over the interval, and the hottest code of each.
        std::vector<std::pair<uint64_t, DWORD>> busy;
        for (const auto &c : cpuDelta) busy.push_back({c.second, c.first});
        std::sort(busy.begin(), busy.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        for (size_t t = 0; t < busy.size() && t < 3; ++t) {
            const DWORD id = busy[t].second;
            std::vector<std::pair<unsigned, std::pair<HMODULE, uintptr_t>>> spots;
            for (const auto &h : perThread[id]) spots.push_back({h.second, h.first});
            std::sort(spots.begin(), spots.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            int m = std::snprintf(line, sizeof line, "Lab profile thread %lu cpu=%.2fs:", id, double(busy[t].first) / 1e7);
            for (size_t i = 0; i < spots.size() && i < 10 && m < 1200; ++i) {
                wchar_t path[MAX_PATH]{};
                const wchar_t *name = L"?";
                if (spots[i].second.first && GetModuleFileNameW(spots[i].second.first, path, MAX_PATH)) {
                    name = path;
                    for (const wchar_t *c = path; *c; ++c) if (*c == L'\\' || *c == L'/') name = c + 1;
                }
                m += std::snprintf(line + m, sizeof line - m, " %ls+%x=%u", name, unsigned(spots[i].second.second), spots[i].first);
            }
            log("%s", line);
        }
        for (auto &c : cpuAt) c.second += cpuDelta[c.first];
        cpuDelta.clear();
        perThread.clear();
        hits.clear();
        threadHits.clear();
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
