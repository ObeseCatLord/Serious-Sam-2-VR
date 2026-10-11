#pragma once
#include <cstdint>
#include <cmath>

namespace ss2vr {
struct ScopeSourceSurface {
    uint32_t type = 0, pool = 0, width = 0, height = 0, usage = 0;
    uint32_t samples = 0, quality = 0, format = 0;
};
struct ScopeSourceViewport {
    uint32_t x = 0, y = 0, width = 0, height = 0;
    float minimum = 0, maximum = 1;
    bool operator==(const ScopeSourceViewport &) const = default;
};
// Strict unscaled non-MSAA UNORM copy subset. D3D9 constants are asserted at
// the native adapter. Resource/device identities and color interpretation are
// separate prerequisites, not inferred from dimensions or format names.
inline bool scopeSourceSurface(const ScopeSourceSurface &s, uint32_t width, uint32_t height) noexcept {
    return width && height && s.type == 1 && s.pool == 0 && s.width == width && s.height == height &&
        s.usage == 1 && s.samples == 0 && s.quality == 0 && (s.format == 21 || s.format == 22);
}
// The destination (b) may also carry automatic mipmaps (D3DUSAGE_AUTOGENMIPMAP,
// 0x400): the lens samples a filtered chain instead of the minified full image.
inline bool scopeSourcePair(const ScopeSourceSurface &a, const ScopeSourceSurface &b,
                            uint32_t width, uint32_t height) noexcept {
    ScopeSourceSurface destination = b;
    if (destination.usage == 0x401) destination.usage = 1;
    return scopeSourceSurface(a,width,height) && scopeSourceSurface(destination,width,height) && a.format == b.format;
}
inline bool scopeSourceViewport(const ScopeSourceViewport &v, uint32_t width, uint32_t height) noexcept {
    return width && height && !v.x && !v.y && v.width == width && v.height == height &&
        std::isfinite(v.minimum) && std::isfinite(v.maximum) &&
        v.minimum >= 0 && v.maximum <= 1 && v.minimum < v.maximum;
}
} // namespace ss2vr
