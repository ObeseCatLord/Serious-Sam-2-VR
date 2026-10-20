#pragma once
#include "scope_uv.hpp"

namespace ss2vr {
struct ScopeShaderConstants {
    // Exact c0..c6 interface of shaders/scope_image.hlsl. This owns values only.
    std::array<std::array<float, 4>, 7> rows{};
    bool valid = false;
};
// Construct constants from already-correlated optical/UV evidence. This does
// not admit a source texture, native draw, zoom state or visibility policy.
// Visibility and reticle dimensions are explicit caller inputs, not inferred
// from a controller pose or hard-coded replacement for native zoom timing.
inline bool scopeShaderConstants(const ScopeOpticalFrame &frame,
                                 const ScopeOpticalProjection &projection,
                                 const ScopeImageCoordinates &coordinates,
                                 Vec3 worldEye, float visibility,
                                 float reticleHalfWidth, float reticleHalfLength,
                                 ScopeShaderConstants &out) noexcept {
    out = {};
    if (!scopeOpticalFrameValid(frame) || !scopeOpticalProjectionValid(projection) ||
        !coordinates.valid || !std::isfinite(worldEye.x) || !std::isfinite(worldEye.y) ||
        !std::isfinite(worldEye.z) || !std::isfinite(visibility) || visibility < 0 || visibility > 1 ||
        !std::isfinite(reticleHalfWidth) || !std::isfinite(reticleHalfLength) ||
        reticleHalfWidth < 0 || reticleHalfLength < reticleHalfWidth || reticleHalfLength > .5f)
        return false;
    ScopeShaderConstants next;
    for (unsigned row = 0; row < 3; ++row) {
        for (float value : coordinates.rows[row]) if (!std::isfinite(value)) return false;
        next.rows[row] = coordinates.rows[row];
    }
    // Subtract before rotation, in double, rather than cancelling two large
    // translated matrix products. The proper optical basis already owns roll.
    const double delta[3]{double(worldEye.x) - frame.camera.m[3],
                          double(worldEye.y) - frame.camera.m[7],
                          double(worldEye.z) - frame.camera.m[11]};
    for (unsigned row = 0; row < 3; ++row) {
        double value = 0;
        for (unsigned axis = 0; axis < 3; ++axis)
            value += double(frame.inverseCamera.m[4 * row + axis]) * delta[axis];
        next.rows[3][row] = float(value);
    }
    // Rear-cap outward +Z is the eye side. Behind/parallel observations cannot
    // be reinterpreted as a valid looking-through-the-scope projection.
    if (!(next.rows[3][2] > 1e-6f)) return false;
    next.rows[4] = {.5f / (projection.magnification * projection.tangentX),
                   -.5f / (projection.magnification * projection.tangentY), .5f, .5f};
    next.rows[5] = {visibility, 0, 0, 0};
    next.rows[6] = {.5f,.5f,reticleHalfWidth,reticleHalfLength};
    for (const auto &row : next.rows)
        for (float value : row) if (!std::isfinite(value)) return false;
    next.valid = true;
    out = next;
    return true;
}
// Re-express the aperture rows (c0..c2) and eye (c3) of constants built in the
// current optic frame in an earlier source camera's frame. The shader's ray
// arithmetic is unchanged, so its lookup becomes the exact projection into the
// source image: a one-frame-old image stays registered to the moving scope
// (only scene parallax and animation are a frame old).
inline bool scopeReprojectConstants(const ScopeOpticalFrame &current,const ScopeOpticalFrame &source,
                                    ScopeShaderConstants &c) noexcept {
    if (!c.valid || !scopeOpticalFrameValid(current) || !scopeOpticalFrameValid(source)) return false;
    double rotation[3][3]{}, translation[3]{};
    const double delta[3]{double(current.camera.m[3]) - source.camera.m[3],
                          double(current.camera.m[7]) - source.camera.m[7],
                          double(current.camera.m[11]) - source.camera.m[11]};
    for (unsigned row = 0; row < 3; ++row) {
        for (unsigned col = 0; col < 3; ++col)
            for (unsigned k = 0; k < 3; ++k)
                rotation[row][col] += double(source.inverseCamera.m[4*row+k]) * current.camera.m[4*k+col];
        for (unsigned k = 0; k < 3; ++k) translation[row] += double(source.inverseCamera.m[4*row+k]) * delta[k];
    }
    ScopeShaderConstants next = c;
    for (unsigned row = 0; row < 3; ++row) {
        for (unsigned uv = 0; uv < 3; ++uv) {
            double value = uv == 2 ? translation[row] : 0;
            for (unsigned k = 0; k < 3; ++k) value += rotation[row][k] * c.rows[k][uv];
            next.rows[row][uv] = float(value);
        }
        double eye = translation[row];
        for (unsigned k = 0; k < 3; ++k) eye += rotation[row][k] * c.rows[3][k];
        next.rows[3][row] = float(eye);
    }
    for (unsigned row = 0; row < 4; ++row)
        for (float value : next.rows[row]) if (!std::isfinite(value)) return false;
    c = next;
    return true;
}
// Place a reticle at an already-frozen native aim/collision point in the
// source camera. This is source projection, not the magnification-compressed
// eye ray used to sample through the aperture. No guessed optical-axis zero.
inline bool scopeReticleTarget(const ScopeOpticalFrame &frame,const ScopeOpticalProjection &projection,
                               Vec3 target,float halfWidth,float halfLength,ScopeShaderConstants &out) noexcept {
    out.rows[6] = {}; // Missing/behind/out-of-source target never leaves a center cross.
    if (!out.valid || !scopeOpticalFrameValid(frame) || !scopeOpticalProjectionValid(projection) ||
        !std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z) ||
        !std::isfinite(halfWidth) || !std::isfinite(halfLength) ||
        halfWidth<=0 || halfLength<halfWidth || halfLength>2.f) return false; // Up to lines across the whole lens.
    const double delta[]{double(target.x)-frame.camera.m[3],double(target.y)-frame.camera.m[7],
                         double(target.z)-frame.camera.m[11]};
    double local[3]{};
    for (unsigned row=0;row<3;++row) for (unsigned axis=0;axis<3;++axis)
        local[row]+=double(frame.inverseCamera.m[row*4+axis])*delta[axis];
    if (!std::isfinite(local[2]) || local[2]>=-1e-6) return false;
    const double u=.5+.5*local[0]/(-local[2]*projection.tangentX);
    const double v=.5-.5*local[1]/(-local[2]*projection.tangentY);
    if (!std::isfinite(u) || !std::isfinite(v) || u<0 || u>1 || v<0 || v>1) return false;
    out.rows[6]={float(u),float(v),halfWidth,halfLength};
    return true;
}
} // namespace ss2vr
