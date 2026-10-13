#pragma once
#include "weapon_view.hpp"
#include <algorithm>
#include <cmath>

namespace ss2vr {
struct WorldMarkerDimensions {
    int32_t width = 0, height = 0;
};
inline bool worldMarkerDimensions(const int32_t (&rectangle)[4], uint32_t width, uint32_t height,
                                  WorldMarkerDimensions &out) {
    const int64_t w = int64_t(rectangle[2]) - rectangle[0],
                  h = int64_t(rectangle[3]) - rectangle[1];
    if (rectangle[0] || rectangle[1] || w <= 0 || h <= 0 ||
        w > INT32_MAX || h > INT32_MAX || uint64_t(w) != width || uint64_t(h) != height)
        return false;
    out = {int32_t(w), int32_t(h)};
    return true;
}
// The camera a world view places (rigid world->view rows, -Z forward) is near
// the tracked eye: native camera bob and shake move it by centimetres.
inline bool nearTrackedView(const Matrix34 &view, const Pose &eye, float metres = .3f, float degrees = 10.f) {
    const Matrix34 expected = matrix(inverse(eye));
    float position[3]{}, expectedPosition[3]{};
    for (unsigned c = 0; c < 3; ++c)
        for (unsigned r = 0; r < 3; ++r) {
            position[c] -= view.m[4 * r + c] * view.m[4 * r + 3];
            expectedPosition[c] -= expected.m[4 * r + c] * expected.m[4 * r + 3];
        }
    const float dx = position[0] - expectedPosition[0], dy = position[1] - expectedPosition[1],
                dz = position[2] - expectedPosition[2];
    double trace = 0; // trace(R * Re^T) = 1 + 2 cos(angle)
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 3; ++c) trace += double(view.m[4 * r + c]) * expected.m[4 * r + c];
    const double cosine = std::clamp((trace - 1) / 2, -1., 1.);
    return std::isfinite(dx + dy + dz) && dx * dx + dy * dy + dz * dz <= metres * metres &&
           cosine >= std::cos(double(degrees) * 3.14159265358979 / 180);
}
inline bool worldMarkerViewValid(const WeaponWorldView &world, Pose eye, Fov fov,
                                 WorldMarkerDimensions dimensions, uint32_t width, uint32_t height) {
    return width && height && dimensions.width > 0 && dimensions.height > 0 &&
           uint32_t(dimensions.width) == width && uint32_t(dimensions.height) == height &&
           world.valid && finite(eye) && finiteMatrix(world.view) && finiteProjection(world.projection) &&
           validDepthRange(world.nearDepth, world.farDepth) &&
           nearTrackedView(world.view, eye) &&
           sameProjectionXY(world.projection, projection(fov));
}
// Used only by the admitted eye loop. Never replay this phase from an original
// render callback, recursive fallback, or desktop restoration.
template <class CheckTarget, class PostRender>
bool finishEyeRender(bool rendered, CheckTarget checkTarget, PostRender postRender) {
    return rendered && checkTarget() && postRender() && checkTarget();
}
} // namespace ss2vr
