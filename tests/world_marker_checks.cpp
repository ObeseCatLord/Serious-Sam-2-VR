#include "common/world_markers.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
using namespace ss2vr;
static void check(bool ok, const char *message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
// Independent native clip oracle: Core's Project clips XYZ against +/-W.
static std::array<float, 4> clip(const WeaponWorldView &world, Vec3 point) {
    float input[4]{point.x, point.y, point.z, 1}, view[4]{0, 0, 0, 1};
    std::array<float, 4> result{};
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 4; ++c) view[r] += world.view.m[4*r+c]*input[c];
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 4; ++c) result[r] += world.projection.m[4*r+c]*view[c];
    return result;
}
static bool inside(const std::array<float, 4> &p) {
    return p[3] > 0 && std::abs(p[0]) <= p[3] && std::abs(p[1]) <= p[3] &&
           std::abs(p[2]) <= p[3];
}
int main() {
    const Pose center{normalize(multiply(yaw(.6f), Quat{.12f, .02f, .15f, .98f})), {8, 2, -11}};
    const Fov fovs[2]{{-.91f, .72f, .87f, -.81f}, {-.72f, .91f, .87f, -.81f}};
    WeaponWorldView worlds[2];
    Pose eyes[2];
    WorldMarkerDimensions dimensions;
    const int32_t rectangle[4]{0, 0, 1024, 896};
    check(worldMarkerDimensions(rectangle, 1024, 896, dimensions), "Full native drawport accepted");
    for (unsigned i = 0; i < 2; ++i)
        eyes[i] = compose(center, Pose{{}, {i ? .032f : -.032f, 0, 0}});
    for (unsigned i = 0; i < 2; ++i) {
        const WeaponWorldView prepared{matrix(inverse(eyes[i])), projection(fovs[i], .05f, 10000), 0, 1, true};
        worlds[i] = executedWeaponView(prepared, prepared.view, projection(fovs[i], .19f, 71), 0, 1);
        check(worldMarkerViewValid(worlds[i], eyes[i], fovs[i], dimensions, 1024, 896),
              "Actual adjusted full native eye projection admitted without replacing its Z rows");
        // Native camera bob/shake moves the executed view by centimetres; an
        // unrelated camera a metre away (cinematic, another player) is rejected.
        check(worldMarkerViewValid(worlds[i], compose(eyes[i], Pose{yaw(.02f), {.012f, -.005f, .001f}}), fovs[i],
                                   dimensions, 1024, 896),
              "A shaken native camera near the tracked eye supplies view provenance");
        check(!worldMarkerViewValid(worlds[i], compose(eyes[i], Pose{{}, {1.f, 0, 0}}), fovs[i], dimensions, 1024, 896),
              "A camera a metre away cannot supply view provenance");
        check(!worldMarkerViewValid(worlds[i], compose(eyes[i], Pose{yaw(.6f), {}}), fovs[i], dimensions, 1024, 896),
              "A camera turned away cannot supply view provenance");
        check(!worldMarkerViewValid(worlds[i], eyes[i], fovs[1-i], dimensions, 1024, 896),
              "Opposite asymmetric FOV cannot supply projection provenance");
        for (const float distance : {.10f, 1.f, 80.f}) {
            const Vec3 point = eyes[i].p + rotate(eyes[i].q, {0, 0, -distance});
            check(inside(clip(worlds[i], point)) == (distance == 1.f),
                  "Native adjusted near/far distances exclude markers in front/behind world clip limits");
            if (distance != 1.f)
                check(inside(clip(prepared, point)), "Original pre-execution Z would incorrectly admit marker");
        }
        auto invalid = worlds[i];
        invalid.projection.m[11] = std::numeric_limits<float>::quiet_NaN();
        check(!worldMarkerViewValid(invalid, eyes[i], fovs[i], dimensions, 1024, 896),
              "A nonfinite adjusted depth coefficient rejects marker phase");
        invalid = worlds[i]; invalid.valid = false;
        check(!worldMarkerViewValid(invalid, eyes[i], fovs[i], dimensions, 1024, 896),
              "Uncaptured native root cannot produce markers");
    }
    const Vec3 point = center.p + rotate(center.q, {.2f, .1f, -3});
    const auto left = clip(worlds[0], point), right = clip(worlds[1], point);
    check(std::abs(left[0]/left[3] - right[0]/right[3]) > .01f,
          "Distinct eye offset and asymmetric projection produce different marker screen positions");
    check(!worldMarkerViewValid(worlds[0], eyes[0], fovs[0], dimensions, 896, 1024),
          "Viewport dimensions cannot silently substitute a rotated or resized drawport");
    for (const auto rect : {std::array<int32_t,4>{1,0,1025,896}, {0,1,1024,897},
                           {0,0,0,896}, {0,0,1024,-1}, {INT32_MIN,0,INT32_MAX,896}}) {
        int32_t native[4]; std::copy(rect.begin(), rect.end(), native);
        check(!worldMarkerDimensions(native, 1024, 896, dimensions),
              "Offset, empty, negative and overflowing native rectangles are rejected");
    }
    std::string order;
    bool current = true;
    auto target = [&] { order += 'T'; return current; };
    auto markers = [&] { order += 'M'; return true; };
    check(finishEyeRender(true, target, markers) && order == "TMT",
          "Completed eye target is checked both before and after native marker callback");
    order.clear();
    check(!finishEyeRender(false, target, markers) && order.empty(),
          "Failed original world render never invokes added phase");
    current = false;
    check(!finishEyeRender(true, target, markers) && order == "T",
          "A redirected target rejects before any native marker draw");
    order.clear(); current = true;
    check(!finishEyeRender(true, target, [&] { order += 'M'; current = false; return true; }) && order == "TMT",
          "Native callback invalidating eye target rejects completion");
    order.clear(); current = true;
    check(!finishEyeRender(true, target, [&] { order += 'M'; return false; }) && order == "TM",
          "A partially completed phase never publishes its eye");
    // Same short-circuit admission used by the bridge: readback/publication only
    // follows successful completion of both eyes. This is not a renderer run.
    bool pair = true;
    unsigned phases = 0, readbacks = 0;
    for (unsigned eye = 0; eye < 2 && pair; ++eye) {
        pair = finishEyeRender(pair, [] { return true; }, [&] { ++phases; return eye == 0; });
        if (pair) ++readbacks;
    }
    check(!pair && phases == 2 && readbacks == 1, "Second-eye marker failure rejects a partially copied pair");
    order.clear();
    check(finishEyeRender(true, target, [] { return true; }) && order == "TT",
          "An absent callback retains original rendering with target checks");
    std::cout << "World-marker eye provenance, native clip/dimensions and completion checks passed; no runtime executed\n";
}
