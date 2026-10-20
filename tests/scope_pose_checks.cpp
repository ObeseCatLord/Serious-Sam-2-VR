#include "common/scope_pose.hpp"
#include <cassert>
#include <limits>
using namespace ss2vr;
int main() {
    std::vector<uintptr_t> instances{0, 100};
    std::vector<PaletteModelRange> models{{-1, 0}, {0, 1}};
    std::vector<PaletteMeshRange> meshes{{1, 0, 2}};
    std::vector<PaletteDrawRange> draws{{0, 0, 1}, {0, 1, 1}};
    std::vector<uint32_t> names{20, 21};
    std::vector<PaletteBone> bones{{0, -1, 0, false}, {1, 0, 31, true}};
    std::vector<PaletteMap> maps{{0, 1}, {1, 1}};
    const Matrix34 identity = matrix({});
    Matrix34 nativeWorld = matrix({normalize({.2f, -.3f, .1f, .8f}), {2, 3, 4}});
    // Native left reflection, nonuniform stretch and shear stay in the matrix.
    const Matrix34 stretch{{-1.3f, .1f, 0, 0, 0, .7f, .2f, 0, 0, 0, 1.1f, 0}};
    nativeWorld = affineMultiply(nativeWorld, stretch);
    const Matrix34 animated = matrix({normalize({.1f, .2f, -.1f, .9f}), {.04f, -.02f, .08f}});
    std::vector<Matrix34> worlds{identity, nativeWorld}, palette{identity, animated};
    ScopePaletteView view{instances, models, meshes, draws, names, bones, maps, worlds, palette};
    Matrix34 out;
    assert(observeScopeAffine(view, 100, 21, 31, out));
    ScopeDrawSelection selected;
    assert(selectScopeDraw(view,100,21,31,selected));
    assert(selected.owner==1 && selected.mesh==0 && selected.draw==1 && selected.palette==1);
    for (unsigned i=0;i<12;++i) assert(selected.affine.m[i]==out.m[i]);
    assert(!selectScopeDraw(view,200,21,31,selected));
    assert(selected.owner==-1 && selected.mesh==-1 && selected.draw==-1 && selected.palette==-1);
    const auto expected = affineMultiply(nativeWorld, animated);
    for (unsigned i = 0; i < 12; ++i) assert(out.m[i] == expected.m[i]);
    assert(!observeScopeAffine(view, 200, 21, 31, out));
    names[0] = 21;
    assert(!observeScopeAffine(view, 100, 21, 31, out)); // Ambiguous surface.
    names[0] = 20;
    maps[1].draw = 0;
    assert(!observeScopeAffine(view, 100, 21, 31, out)); // Stale draw backlink.
    maps[1].draw = 1;
    bones[1].owner = 0;
    assert(!observeScopeAffine(view, 100, 21, 31, out));
    bones[1].owner = 1;
    bones[1].name = 32;
    assert(!observeScopeAffine(view, 100, 21, 31, out));
    bones[1].name = 31;
    palette[1].m[3] = std::numeric_limits<float>::quiet_NaN();
    assert(!observeScopeAffine(view, 100, 21, 31, out));
    palette[1] = animated;
    worlds[1] = {};
    assert(!observeScopeAffine(view, 100, 21, 31, out)); // Singular transform.
    worlds[1] = nativeWorld;
    instances.push_back(100); models.push_back({-1, 0}); worlds.push_back(identity);
    view.instances = instances; view.models = models; view.worlds = worlds;
    assert(!observeScopeAffine(view, 100, 21, 31, out)); // Duplicate native instance.
    ScopePoseObservation observation;
    assert(!observation.valid && !observation.contentVerified);
    assert(scopeObservationThread(7, 7, true));
    const uint32_t owner = 7;
    assert(!scopeObservationThread(owner, 8, true) && owner == 7);
    assert(!scopeObservationThread(0, 7, true));
    assert(!scopeObservationThread(UINT32_MAX, 7, true));
    assert(!scopeObservationThread(7, 7, false));
    ScopeObservationBank bank;
    ScopePoseObservation accepted{expected, 10, 20, 30, 40, 50, 60, true, false};
    accepted.layout.vertices=884; accepted.layout.triangles=928;
    accepted.layout.channels[2]={151520,0x80,0};
    accepted.geometry.valid=true;
    accepted.geometry.positions[0]={.01f,.303f,.0486f};
    accepted.geometry.indices[0]=7;
    accepted.imageCoordinates.valid=true;
    accepted.imageCoordinates.rows[0]={.2f,.1f,-.18f,0};
    accepted.opaqueColorCandidate=true;
    accepted.nativeBaseFovRadians=Pi/2;
    accepted.zoom=scopeNativeZoom(1,1,1,.25f,.75f);
    assert(scopeImagePoseMatches(accepted,accepted));
    {   // An earlier frame's source of the same weapon, one or two requests back.
        auto next = accepted; next.requestSequence += 1; next.inputSequence += 1; next.affine.m[3] += .01f;
        assert(scopeImageSourceRecent(accepted,next));
        next.requestSequence = accepted.requestSequence + ScopeSourceMaxRequestAge;
        assert(scopeImageSourceRecent(accepted,next));
        next.requestSequence += 1; assert(!scopeImageSourceRecent(accepted,next)); // Too old.
        next.requestSequence = accepted.requestSequence; assert(!scopeImageSourceRecent(accepted,next)); // Not earlier.
        next.requestSequence = accepted.requestSequence + 1;
        auto other = next; other.weaponHandle++; assert(!scopeImageSourceRecent(accepted,other));
        other = next; other.modelHandle++; assert(!scopeImageSourceRecent(accepted,other));
        other = next; other.generation++; assert(!scopeImageSourceRecent(accepted,other));
        other = next; other.ownerHandle++; assert(!scopeImageSourceRecent(accepted,other));
        other = next; other.zoom = {}; assert(!scopeImageSourceRecent(accepted,other));
        assert(scopeImageSourceRecent(accepted,other,true)); // Always-live scope: unzoomed is admitted.
    }
    {
        // Always-live scope: both unzoomed match; one zoomed and one not never does.
        auto a = accepted, b = accepted;
        a.zoom = {}; b.zoom = {};
        assert(!scopeImagePoseMatches(a,b) && scopeImagePoseMatches(a,b,true));
        assert(!scopeImagePoseMatches(a,accepted,true) && !scopeImagePoseMatches(accepted,a,true));
        assert(scopeImagePoseMatches(accepted,accepted,true));
        b.requestSequence++; assert(!scopeImagePoseMatches(a,b,true));
    }
    auto changed = accepted;
    changed.requestSequence++; assert(!scopeImagePoseMatches(accepted,changed)); changed=accepted;
    changed.inputSequence++; assert(!scopeImagePoseMatches(accepted,changed)); changed=accepted;
    changed.ownerHandle++; assert(!scopeImagePoseMatches(accepted,changed)); changed=accepted;
    changed.weaponHandle++; assert(!scopeImagePoseMatches(accepted,changed)); changed=accepted;
    changed.modelHandle++; assert(!scopeImagePoseMatches(accepted,changed)); changed=accepted;
    changed.generation++; assert(!scopeImagePoseMatches(accepted,changed)); changed=accepted;
    changed.affine.m[3]+=.001f; assert(!scopeImagePoseMatches(accepted,changed)); changed=accepted;
    changed.layout.vertices++; assert(!scopeImagePoseMatches(accepted,changed)); changed=accepted;
    changed.nativeBaseFovRadians=0; assert(!scopeImagePoseMatches(accepted,changed)); changed=accepted;
    changed.zoom.progress+=.01f; assert(!scopeImagePoseMatches(accepted,changed)); changed=accepted;
    changed.zoom.valid=false; assert(!scopeImagePoseMatches(accepted,changed)); changed=accepted;
    changed.nativeBaseFovRadians=std::numeric_limits<float>::infinity();
    assert(!scopeImagePoseMatches(changed,changed)); changed=accepted;
    changed.zoom.progress=2; assert(!scopeImagePoseMatches(changed,changed)); changed=accepted;
    bank.finishDraw(0, accepted, true);
    assert(bank.copy(0, 10, 20, 30, 40, 50, 60, false, observation));
    assert(observation.valid && !observation.contentVerified && observation.layout==accepted.layout);
    assert(observation.geometry.valid && observation.geometry.positions[0].x==.01f &&
           observation.geometry.indices[0]==7);
    assert(observation.imageCoordinates.valid && observation.imageCoordinates.rows==accepted.imageCoordinates.rows);
    assert(observation.opaqueColorCandidate && observation.nativeBaseFovRadians==Pi/2);
    assert(observation.zoom.valid && observation.zoom.progress==.75f &&
           observation.zoom.startMultiplier==1 && observation.zoom.endMultiplier==.25f);
    auto otherHand=accepted;
    otherHand.weaponHandle=41;
    otherHand.zoom=scopeNativeZoom(1,1,.5f,.125f,.25f);
    bank.finishDraw(1,otherHand,true);
    assert(bank.copy(1,10,20,30,41,50,60,false,observation));
    assert(observation.zoom.valid && observation.zoom.progress==.25f &&
           observation.zoom.startMultiplier==.5f && observation.zoom.endMultiplier==.125f);
    assert(bank.copy(0,10,20,30,40,50,60,false,observation));
    assert(observation.zoom.progress==.75f && observation.zoom.endMultiplier==.25f);
    assert(!bank.copy(0, 10, 20, 30, 40, 51, 60, false, observation)); // Same weapon, new model.
    assert(!observation.geometry.valid && !observation.imageCoordinates.valid && !observation.opaqueColorCandidate);
    assert(!observation.zoom.valid && observation.nativeBaseFovRadians==0);
    assert(!bank.copy(0, 10, 20, 30, 40, 50, 60, true, observation)); // Physical pair fault.
    assert(!observation.opaqueColorCandidate);
    assert(!observation.zoom.valid && observation.nativeBaseFovRadians==0);
    assert(!bank.copy(0, 11, 20, 30, 40, 50, 60, false, observation));
    bank.beginDraw(0); // An early return/unwind in a later draw supersedes earlier success.
    assert(!bank.copy(0, 10, 20, 30, 40, 50, 60, false, observation));
    assert(!observation.opaqueColorCandidate);
    assert(!observation.zoom.valid && observation.nativeBaseFovRadians==0);
    bank.finishDraw(0, accepted, true);
    bank.finishDraw(0, accepted, false); // Failed/ambiguous completion clears it too.
    assert(!bank.copy(0, 10, 20, 30, 40, 50, 60, false, observation));
    assert(!observation.opaqueColorCandidate);
    bank.finishDraw(1, accepted, true);
    bank.beginDraw(0);
    assert(bank.copy(1, 10, 20, 30, 40, 50, 60, false, observation)); // Independent hand.
    assert(observation.geometry.valid && !observation.contentVerified);
    assert(observation.opaqueColorCandidate);
    bank = {};
    assert(!bank.copy(1, 10, 20, 30, 40, 50, 60, false, observation)); // Eye retirement.
    assert(!observation.geometry.valid && !observation.imageCoordinates.valid && !observation.opaqueColorCandidate);
    assert(!observation.zoom.valid && observation.nativeBaseFovRadians==0);
}
