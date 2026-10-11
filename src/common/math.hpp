#pragma once
#include "protocol.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>
namespace ss2vr {
constexpr float Pi = 3.14159265358979323846f;
inline Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 operator*(Vec3 a, float b) {
    return {a.x * b, a.y * b, a.z * b};
}
inline float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Quat inverse(Quat q) {
    return {-q.x, -q.y, -q.z, q.w};
}
inline Quat multiply(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat normalize(Quat q) {
    float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return n > 0.00001f ? Quat{q.x / n, q.y / n, q.z / n, q.w / n} : Quat{};
}
inline Vec3 rotate(Quat q, Vec3 v) {
    Vec3 u{q.x, q.y, q.z};
    auto t = cross(u, v) * 2;
    return v + t * q.w + cross(u, t);
}
inline Quat yaw(float a) {
    return {0, std::sin(a / 2), 0, std::cos(a / 2)};
}
inline Pose compose(Pose a, Pose b) {
    return {multiply(a.q, b.q), a.p + rotate(a.q, b.p)};
}
inline Pose inverse(Pose a) {
    auto q = inverse(a.q);
    return {q, rotate(q, a.p * -1)};
}
inline bool finite(Pose p) {
    return std::isfinite(p.p.x) && std::isfinite(p.p.y) && std::isfinite(p.p.z) && std::isfinite(p.q.x) &&
           std::isfinite(p.q.y) && std::isfinite(p.q.z) && std::isfinite(p.q.w);
}
inline Pose weaponTracking(const Input &input, unsigned hand) {
    Pose tracked = input.hand[hand];
    if (input.gripValid[hand] && finite(input.grip[hand]))
        tracked.p = input.grip[hand].p;
    // Grip is the physical attachment point; aim retains the runtime's forward axis.
    return tracked;
}
struct Matrix34 {
    float m[12];
};
struct Matrix44 {
    float m[16];
};
inline Matrix34 matrix(Pose p) {
    auto q = normalize(p.q);
    float x = q.x, y = q.y, z = q.z, w = q.w;
    return {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), p.p.x, 2 * (x * y + z * w),
             1 - 2 * (x * x + z * z), 2 * (y * z - x * w), p.p.y, 2 * (x * z - y * w), 2 * (y * z + x * w),
             1 - 2 * (x * x + y * y), p.p.z}};
}
inline float yawAngle(Quat q) {
    return std::atan2(2 * (q.w * q.y + q.x * q.z), 1 - 2 * (q.y * q.y + q.z * q.z));
}
inline Matrix44 projection(Fov f, float nearZ = .05f, float farZ = 10000) {
    float l = std::tan(f.left), r = std::tan(f.right), u = std::tan(f.up), d = std::tan(f.down);
    return {{2 / (r - l), 0, (r + l) / (r - l), 0, 0, 2 / (u - d), (u + d) / (u - d), 0, 0, 0,
             -(farZ + nearZ) / (farZ - nearZ), -2 * farZ * nearZ / (farZ - nearZ), 0, 0, -1, 0}};
}
// Native Render3D constructs a row-major rigid transform from xyzw quaternion.
// Its camera inverse and mthFrustum use a -Z-forward right-handed view.
inline Pose relativeTracking(Pose origin, Pose tracked) {
    return compose(inverse(origin), tracked);
}
inline Vec3 boundTranslation(Vec3 p, float bound) {
    float n = std::sqrt(dot(p, p));
    return n > bound ? p * (bound / n) : p;
}
// Numerical sanity bounds for room-scale tracking, not a comfort or safety limit:
// head motion inside a play space is applied 1:1 and never blacks out the view.
// Only actual geometry contact (head clearance) can hide the world.
constexpr float HeadHorizontalLimit = 4.f, HeadDownLimit = 2.5f, HeadUpLimit = 2.5f;
constexpr float HandReachLimit = 1.3f, TrackingBoundsSlack = .0002f;
constexpr float MaximumHeadTranslation = 4.75f, MaximumHandTranslation = 6.05f;
static_assert(MaximumHeadTranslation * MaximumHeadTranslation >=
              HeadHorizontalLimit * HeadHorizontalLimit + HeadUpLimit * HeadUpLimit);
static_assert(MaximumHandTranslation >= MaximumHeadTranslation + HandReachLimit);
inline Vec3 boundHeadTranslation(Vec3 p) {
    Vec3 horizontal = boundTranslation({p.x, 0, p.z}, HeadHorizontalLimit);
    return {horizontal.x, std::clamp(p.y, -HeadDownLimit, HeadUpLimit), horizontal.z};
}
inline bool headTranslationValid(Vec3 p) {
    const float horizontal2 = p.x * p.x + p.z * p.z;
    const float radius = HeadHorizontalLimit + TrackingBoundsSlack;
    return std::isfinite(horizontal2) && std::isfinite(p.y) && horizontal2 <= radius * radius &&
           p.y >= -HeadDownLimit - TrackingBoundsSlack && p.y <= HeadUpLimit + TrackingBoundsSlack;
}
inline bool handTranslationValid(Vec3 head, Vec3 hand) {
    const Vec3 reach = hand - head;
    const float reach2 = dot(reach, reach), radius = HandReachLimit + TrackingBoundsSlack;
    return std::isfinite(reach2) && reach2 <= radius * radius;
}
inline bool handTranslationInVolume(Vec3 hand) {
    // Retained taps can predate the packet head. Require a grip producible
    // from some point in the head volume, without adding historical state.
    return handTranslationValid(boundHeadTranslation(hand), hand);
}
inline Pose bodyHeadTracking(Pose origin, float turn, Pose head) {
    Pose relative = relativeTracking(origin, head);
    relative.p = boundHeadTranslation(relative.p);
    return compose(Pose{yaw(turn), {}}, relative);
}
inline Pose bodyHandTracking(Pose origin, float turn, Pose head, Pose hand) {
    const Pose center = relativeTracking(origin, head);
    Pose relative = relativeTracking(origin, hand);
    // Apply the same stage correction to the whole rig. Bound exceptional
    // reach relative to the head, never independently relative to the body.
    relative.p = boundHeadTranslation(center.p) + boundTranslation(relative.p - center.p, HandReachLimit);
    return compose(Pose{yaw(turn), {}}, relative);
}
inline Pose worldHeadTracking(Pose anchor, Pose origin, float turn, Pose head) {
    return compose(anchor, bodyHeadTracking(origin, turn, head));
}
inline Pose worldHandTracking(Pose anchor, Pose origin, float turn, Pose head, Pose hand) {
    return compose(anchor, bodyHandTracking(origin, turn, head, hand));
}
inline Pose worldEyeTracking(Pose anchor, Pose origin, float turn, Pose head, Pose eye) {
    Pose center = relativeTracking(origin, head);
    Pose relative = relativeTracking(origin, eye);
    // Bound the head center once. Clamping the eyes separately changes IPD at the boundary.
    relative.p = relative.p + (boundHeadTranslation(center.p) - center.p);
    return compose(anchor, compose(Pose{yaw(turn), {}}, relative));
}
inline bool applyNativeViewHeight(Pose &base, uint32_t heightBits) {
    // Serious Engine's temporary unset marker is finite; finite() alone misses it.
    if (heightBits == 0x7f61b1e6)
        return false;
    float correction = std::bit_cast<float>(heightBits);
    if (!std::isfinite(correction))
        return false;
    base.p.y += correction;
    return finite(base);
}
// Model calibration is relative to the native camera, never an old world position.
// A moved/turned player must not contribute motion to the grip-to-muzzle offset.
// The static reference precedes native instance stretch. Reflection is a signed
// stretch component, not a second rotation or an animated inverse-bone transform.
inline Vec3 modelAnchorDisplacement(Quat nativeRotation, Vec3 stretch, Vec3 modelReference) {
    return rotate(nativeRotation, {stretch.x * modelReference.x, stretch.y * modelReference.y,
                                  stretch.z * modelReference.z}) * -1;
}
// Nearest of the 24 axis-aligned rotations. Keeps an authored quarter or half
// turn of a model while dropping a small tilt.
inline Quat snapToAxes(Quat q) {
    q = normalize(q);
    const Vec3 x = rotate(q, {1, 0, 0}), y = rotate(q, {0, 1, 0});
    const auto axis = [](Vec3 v, int skip) {
        const float c[3]{v.x, v.y, v.z};
        int best = skip == 0 ? 1 : 0;
        for (int i = 0; i < 3; ++i)
            if (i != skip && std::abs(c[i]) > std::abs(c[best])) best = i;
        Vec3 out{};
        (best == 0 ? out.x : best == 1 ? out.y : out.z) = c[best] < 0 ? -1.f : 1.f;
        return std::pair<Vec3, int>{out, best};
    };
    const auto [ax, ix] = axis(x, -1);
    const auto [ay, iy] = axis(y, ix);
    (void)iy;
    const Vec3 az = cross(ax, ay);
    // Rotation matrix columns ax, ay, az -> quaternion.
    const float m00 = ax.x, m11 = ay.y, m22 = az.z, trace = m00 + m11 + m22;
    Quat r;
    if (trace > 0) {
        const float s = std::sqrt(trace + 1) * 2;
        r = {(ay.z - az.y) / s, (az.x - ax.z) / s, (ax.y - ay.x) / s, s / 4};
    } else if (m00 >= m11 && m00 >= m22) {
        const float s = std::sqrt(1 + m00 - m11 - m22) * 2;
        r = {s / 4, (ay.x + ax.y) / s, (az.x + ax.z) / s, (ay.z - az.y) / s};
    } else if (m11 >= m22) {
        const float s = std::sqrt(1 + m11 - m00 - m22) * 2;
        r = {(ay.x + ax.y) / s, s / 4, (az.y + ay.z) / s, (az.x - ax.z) / s};
    } else {
        const float s = std::sqrt(1 + m22 - m00 - m11) * 2;
        r = {(az.x + ax.z) / s, (az.y + ay.z) / s, s / 4, (ax.y - ay.x) / s};
    }
    return normalize(r);
}
// Uniform model scale about the placement origin: rotation columns only.
inline Matrix34 scaledModel(Matrix34 m, float scale) {
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 3; ++column)
            m.m[row * 4 + column] *= scale;
    return m;
}
// scale enlarges the hand-held model about the hand; the shot origin follows
// the rendered barrel so bullets and lasers still leave the visible muzzle.
inline Pose retargetShot(Pose anchor, Pose nativeShot, Vec3 nativeModelLocal, Pose hand,
                         float muzzleBound = .5f, Vec3 nativeDisplacement = {},
                         Vec3 nativeAlignment = {}, bool preserveNativeReach = false, float scale = 1) {
    Pose shotLocal = compose(inverse(anchor), nativeShot);
    // Native model placement includes charge displacement, while the native
    // attachment getter does not. Remove it from the reference root, then add
    // it outside the attachment bound so it is neither cancelled nor clamped.
    // A static model-root correction changes the neutral grip-to-attachment
    // vector BEFORE its reach bound. Bounding relative to the old distant model
    // root and then shifting the result can put the laser behind the handle.
    Vec3 muzzle = shotLocal.p - (nativeModelLocal - nativeDisplacement) + nativeAlignment;
    if (!preserveNativeReach) {
        const float n = std::sqrt(dot(muzzle, muzzle));
        if (n > muzzleBound)
            muzzle = muzzle * (muzzleBound / n);
    }
    return {normalize(multiply(hand.q, shotLocal.q)), hand.p + rotate(hand.q, (muzzle + nativeDisplacement) * scale)};
}
} // namespace ss2vr
