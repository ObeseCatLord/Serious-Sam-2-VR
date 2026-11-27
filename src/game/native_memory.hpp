#pragma once
#include <windows.h>
#include "common/readable_ranges.hpp"
#include <atomic>
#include <cstdint>

namespace ss2vr::game {
// readableMemory asks the system about a page region (VirtualQuery: a system call,
// about a microsecond in the 32-bit game; several hundred a frame came to roughly
// half a millisecond of every frame on Windows). Regions found readable are
// remembered per thread for a short while: until the next frame boundary
// (noteReadableEpoch, at Present and at the eye render) and never longer than
// about 30 ms. Regions found unreadable are always asked about again.
// Lab: SS2VR_LAB_READABLE_CACHE=0 asks the system every time.
inline std::atomic<uint32_t> readableEpoch{1};
inline void noteReadableEpoch() noexcept { readableEpoch.fetch_add(1, std::memory_order_relaxed); }
inline bool readableMemory(const void *pointer, size_t bytes, bool writable = false) {
    uintptr_t begin = reinterpret_cast<uintptr_t>(pointer);
    if (!begin || begin > UINTPTR_MAX - bytes)
        return false;
    const uintptr_t end = begin + bytes;
    static const bool remembered = [] { wchar_t v[2]{}; return !(GetEnvironmentVariableW(L"SS2VR_LAB_READABLE_CACHE", v, 2) == 1 && v[0] == L'0'); }();
    struct Region { uintptr_t begin = 0, end = 0; uint32_t epoch = 0; bool writable = false; };
    static thread_local Region regions[8];
    static thread_local unsigned next = 0;
    const uint32_t epoch = (readableEpoch.load(std::memory_order_relaxed) << 16) | ((GetTickCount() >> 4) & 0xffffu);
    while (begin < end) {
        if (remembered && !writable) { // A write check always asks.
            bool known = false;
            for (const auto &r : regions)
                if (r.epoch == epoch && begin >= r.begin && begin < r.end && (!writable || r.writable)) { begin = r.end; known = true; break; }
            if (known) continue;
        }
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void *>(begin), &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const DWORD protection = info.Protect & 0xff;
        if (protection != PAGE_READONLY && protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE &&
            protection != PAGE_EXECUTE_WRITECOPY)
            return false;
        const bool canWrite = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        if (writable && !canWrite)
            return false;
        const uintptr_t region = reinterpret_cast<uintptr_t>(info.BaseAddress);
        if (region > UINTPTR_MAX - info.RegionSize || region + info.RegionSize <= begin)
            return false;
        if (remembered) regions[next++ & 7] = {region, region + info.RegionSize, epoch, canWrite};
        begin = region + info.RegionSize;
    }
    return true;
}
class NativeReadLease {
    ReadableRanges<> ranges;
public:
    void clear() noexcept {ranges.clear();}
    void seal() noexcept {ranges.seal();}
    bool contains(const void* pointer,size_t bytes) const noexcept {
        return ranges.contains(reinterpret_cast<uintptr_t>(pointer),bytes,[](uintptr_t at,ReadableRange& out) {
            MEMORY_BASIC_INFORMATION info{};
            if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||
               (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
            const DWORD p=info.Protect&0xff;
            const bool readable=p==PAGE_READONLY||p==PAGE_READWRITE||p==PAGE_WRITECOPY||
                p==PAGE_EXECUTE_READ||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;
            const auto base=reinterpret_cast<uintptr_t>(info.BaseAddress);
            if(base>UINTPTR_MAX-info.RegionSize)return false;
            out={base,base+info.RegionSize,readable};return true;
        });
    }
};
} // namespace ss2vr::game
