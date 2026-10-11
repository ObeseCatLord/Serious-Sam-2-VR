#include "common/scope_source_layout.hpp"
#include <array>
#include <cassert>
#include <limits>
using namespace ss2vr;
int main() {
    for (uint32_t format : {21u,22u}) {
        const ScopeSourceSurface valid{1,0,1920,1080,1,0,0,format};
        assert(scopeSourcePair(valid,valid,1920,1080));
        for (unsigned field=0;field<8;++field) {
            auto bad=valid;
            switch(field) {
            case 0: bad.type=3; break;
            case 1: bad.pool=1; break;
            case 2: --bad.width; break;
            case 3: --bad.height; break;
            case 4: bad.usage=0x403; break;
            case 5: bad.samples=2; break;
            case 6: bad.quality=1; break;
            case 7: bad.format=113; break;
            }
            assert(!scopeSourcePair(valid,bad,1920,1080));
            assert(!scopeSourcePair(bad,valid,1920,1080));
        }
        auto mipmapped=valid; mipmapped.usage=0x401;
        assert(scopeSourcePair(valid,mipmapped,1920,1080));  // Destination may autogenerate mips.
        assert(!scopeSourcePair(mipmapped,valid,1920,1080)); // The native source never does.
        auto different=valid; different.format=format==21 ? 22:21;
        assert(!scopeSourcePair(valid,different,1920,1080));
        assert(!scopeSourcePair(valid,valid,0,1080));
        assert(!scopeSourcePair(valid,valid,1920,0));
    }
    const ScopeSourceViewport full{0,0,1920,1080,0,1};
    assert(scopeSourceViewport(full,1920,1080));
    auto changed=full; changed.x=1; assert(!scopeSourceViewport(changed,1920,1080));
    changed=full; changed.y=1; assert(!scopeSourceViewport(changed,1920,1080));
    changed=full; --changed.width; assert(!scopeSourceViewport(changed,1920,1080));
    changed=full; --changed.height; assert(!scopeSourceViewport(changed,1920,1080));
    for (auto depth : {std::array<float,2>{-1,1},{0,2},{1,1},{1,0},
                       {0,std::numeric_limits<float>::quiet_NaN()},
                       {std::numeric_limits<float>::infinity(),1}}) {
        changed=full; changed.minimum=depth[0]; changed.maximum=depth[1];
        assert(!scopeSourceViewport(changed,1920,1080));
    }
    changed=full; changed.minimum=.2f; changed.maximum=.8f;
    assert(scopeSourceViewport(changed,1920,1080) && changed!=full);
}
