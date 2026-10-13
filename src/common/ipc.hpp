#pragma once
#define WIN32_LEAN_AND_MEAN
#include "protocol.hpp"
#include <string>
#include <windows.h>
namespace ss2vr {
inline uint32_t trackingEpoch(const Shared &shared) {
    return static_cast<uint32_t>(InterlockedCompareExchange(
        reinterpret_cast<volatile LONG *>(const_cast<uint32_t *>(&shared.trackingGeneration)), 0, 0));
}
inline void publishTrackingEpoch(Shared &shared, uint32_t epoch) {
    // Unsigned monotonic publication also protects against callbacks completing out of order.
    uint32_t observed = trackingEpoch(shared);
    while (epoch > observed) {
        uint32_t previous = static_cast<uint32_t>(
            InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(&shared.trackingGeneration),
                                       static_cast<LONG>(epoch), static_cast<LONG>(observed)));
        if (previous == observed)
            break;
        observed = previous;
    }
}
struct Channel {
    HANDLE mapping = nullptr, mutex = nullptr, ready = nullptr;
    Shared *shared = nullptr;
    std::wstring token;
    ~Channel() {
        close();
    }
    Channel() = default;
    Channel(const Channel &) = delete;
    Channel &operator=(const Channel &) = delete;
    bool open(const std::wstring &id, bool create) {
        token = id;
        auto n = L"Local\\SS2VR-" + id;
        mapping = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                              sizeof(Shared), n.c_str())
                         : OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, n.c_str());
        if (!mapping)
            return false;
        shared = static_cast<Shared *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
        if (!shared) {
            close();
            return false;
        }
        mutex = create ? CreateMutexW(nullptr, FALSE, (n + L"-lock").c_str())
                       : OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (n + L"-lock").c_str());
        ready = create ? CreateEventW(nullptr, FALSE, FALSE, (n + L"-ready").c_str())
                       : OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (n + L"-ready").c_str());
        if (!mutex || !ready) {
            close();
            return false;
        }
        if (create) {
            ZeroMemory(shared, sizeof(Shared));
            shared->magic = Magic;
            shared->abi = Abi;
            shared->bytes = sizeof(Shared);
            shared->gamePid = GetCurrentProcessId();
        }
        return shared->magic == Magic && shared->abi == Abi && shared->bytes == sizeof(Shared);
    }
    bool lock(DWORD ms = 0) {
        if (!mutex)
            return false;
        auto result = WaitForSingleObject(mutex, ms);
        if (result == WAIT_ABANDONED) {
            if (shared) {
                shared->error = 1;
                shared->shutdown = 1;
            }
            ReleaseMutex(mutex);
            return false;
        }
        return result == WAIT_OBJECT_0;
    }
    void unlock() {
        ReleaseMutex(mutex);
    }
    void close() {
        if (shared)
            UnmapViewOfFile(shared);
        shared = nullptr;
        for (auto h : {mapping, mutex, ready})
            if (h)
                CloseHandle(h);
        mapping = mutex = ready = nullptr;
    }
};
// One section per pixel buffer: a 32-bit game never needs a single huge view.
struct PixelStore {
    HANDLE mapping[PixelBufferCount]{};
    uint8_t *view[PixelBufferCount]{};
    uint32_t generation = 0, width = 0, height = 0;
    PixelStore() = default;
    PixelStore(const PixelStore &) = delete;
    PixelStore &operator=(const PixelStore &) = delete;
    ~PixelStore() { close(); }
    static std::wstring name(const std::wstring &token, uint32_t generation, unsigned buffer) {
        return L"Local\\SS2VR-" + token + L"-pixels-" + std::to_wstring(generation) + L"-" + std::to_wstring(buffer);
    }
    uint64_t bytes() const { return pixelBufferBytes(width, height); }
    uint8_t *buffer(unsigned index) const { return index < PixelBufferCount ? view[index] : nullptr; }
    bool matches(uint32_t g, uint32_t w, uint32_t h) const { return view[0] && generation == g && width == w && height == h; }
    // A smaller image (the scaled flat screen) fits in a buffer of this generation.
    bool holds(uint32_t g, uint32_t w, uint32_t h) const {
        return view[0] && generation == g && w && h && uint64_t(w) * h <= uint64_t(width) * height;
    }
    bool open(const std::wstring &token, uint32_t g, uint32_t w, uint32_t h, bool create) {
        close();
        const uint64_t size = pixelBufferBytes(w, h);
        if (!g || !w || !h || w > MaxDimension || h > MaxDimension || size > SIZE_MAX) return false;
        for (unsigned i = 0; i < PixelBufferCount; ++i) {
            const auto n = name(token, g, i);
            mapping[i] = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                                     DWORD(size >> 32), DWORD(size), n.c_str())
                                : OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, n.c_str());
            view[i] = mapping[i] ? static_cast<uint8_t *>(MapViewOfFile(mapping[i], FILE_MAP_ALL_ACCESS, 0, 0,
                                                                        size_t(size))) : nullptr;
            if (!view[i]) { close(); return false; }
        }
        generation = g; width = w; height = h;
        return true;
    }
    void close() {
        for (unsigned i = 0; i < PixelBufferCount; ++i) {
            if (view[i]) UnmapViewOfFile(view[i]);
            if (mapping[i]) CloseHandle(mapping[i]);
            view[i] = nullptr; mapping[i] = nullptr;
        }
        generation = width = height = 0;
    }
};
struct Lock {
    Channel &c;
    bool held;
    explicit Lock(Channel &x, DWORD ms = 0) : c(x), held(c.lock(ms)) {}
    ~Lock() {
        if (held)
            c.unlock();
    }
    explicit operator bool() const {
        return held;
    }
};
} // namespace ss2vr
