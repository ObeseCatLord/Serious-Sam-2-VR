#include "common/water_mirror.hpp"
#include <cassert>
#include <cstdio>
using namespace ss2vr;
using namespace ss2vr::water_mirror;
using replay::M4;
static void ndc(const M4 &vp, Vec3 x, double &u, double &v, double &w) {
    double c[4]{};
    const double p[4] = {x.x, x.y, x.z, 1};
    for (unsigned r = 0; r < 4; ++r) for (unsigned k = 0; k < 4; ++k) c[r] += vp[4 * r + k] * p[k];
    w = c[3]; u = c[0] / c[3]; v = c[1] / c[3];
}
static M4 viewProjection(const Pose &camera, const Fov &fov) {
    return replay::multiply(replay::m4(projection(fov, .05f, 4000).m, 4), replay::m4(matrix(inverse(camera)).m, 3));
}
int main() {
    // A water mesh placed as in Branchester (V728): turned, scaled, slightly tilted.
    const M4 model{1.6577, -0.0086, 0.4288, 198.9062, 0.0088, 1.7122, 0.0000, -2.2526,
                   -0.4287, 0.0022, 1.6577, -142.5312, 0, 0, 0, 1};
    double plane[4];
    assert(modelPlane(model, plane));
    assert(plane[1] > .999);
    // Mesh points at model y = 0 lie on it.
    const double points[2][3] = {{-18.132, 0, 6.402}, {-12.919, 0, -21.965}};
    for (const auto &m : points) {
        Vec3 w{float(model[0] * m[0] + model[2] * m[2] + model[3]), float(model[4] * m[0] + model[6] * m[2] + model[7]),
               float(model[8] * m[0] + model[10] * m[2] + model[11])};
        assert(std::abs(signedDistance(plane, w)) < 1e-3);
    }
    double same[4] = {plane[0], plane[1], plane[2], plane[3] + .01};
    assert(samePlane(plane, same));
    same[3] = plane[3] + .5;
    assert(!samePlane(plane, same));

    // The mirror camera sees a point where the original camera sees its reflection, left for right.
    const double flat[4] = {0, 1, 0, -1}; // y = 1
    const Pose camera{normalize(Quat{-.25f, .3f, .05f, 1}), Vec3{2, 3.5f, -1}};
    const Fov fov{-.9f, .8f, .85f, -.95f};
    const Pose mirror = mirrorCamera(camera, flat);
    assert(std::abs(mirror.p.y - (-1.5f)) < 1e-5 && std::abs(mirror.p.x - 2) < 1e-5);
    const Fov mfov = mirrorFov(fov, 0);
    const M4 original = viewProjection(camera, fov), reflected = viewProjection(mirror, mfov);
    for (Vec3 x : {Vec3{0, 4, -8}, Vec3{5, 2, -3}, Vec3{-3, 6, -10}, Vec3{1, 1.2f, -2}}) {
        double u0, v0, w0, u1, v1, w1;
        ndc(original, Vec3{x.x, 2 - x.y, x.z}, u0, v0, w0); // The reflection of x.
        ndc(reflected, x, u1, v1, w1);
        assert(std::abs(u1 + u0) < 1e-4 && std::abs(v1 - v0) < 1e-4 && std::abs(w1 - w0) < 1e-3);
    }
    // Its clip plane keeps what is above the water, drops what is below.
    float clip[4];
    assert(clipPlane(reflected, flat, clip, 0));
    for (Vec3 x : {Vec3{0, 4, -8}, Vec3{1, .5f, -6}, Vec3{-2, 1.3f, -9}, Vec3{3, -1, -4}}) {
        double c[4]{};
        const double p[4] = {x.x, x.y, x.z, 1};
        for (unsigned r = 0; r < 4; ++r) for (unsigned k = 0; k < 4; ++k) c[r] += reflected[4 * r + k] * p[k];
        const double side = clip[0] * c[0] + clip[1] * c[1] + clip[2] * c[2] + clip[3] * c[3];
        assert((side > 0) == (x.y > 1));
    }
    // Oblique near plane: the mirror camera keeps what is above the water inside
    // depth -1..1 and puts what is below in front of its near plane.
    {
        double inCamera[4];
        planeInCamera(flat, mirror, inCamera);
        Matrix44 p = projection(mfov, .05f, 4000);
        const Matrix44 before = p;
        assert(obliqueNear(p, inCamera));
        for (unsigned k : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 12u, 13u, 14u, 15u}) assert(p.m[k] == before.m[k]);
        const M4 vp = replay::multiply(replay::m4(p.m, 4), replay::m4(matrix(inverse(mirror)).m, 3));
        for (Vec3 x : {Vec3{0, 4, -8}, Vec3{5, 2, -3}, Vec3{-3, 6, -10}, Vec3{1, 1.2f, -2}, Vec3{0, .5f, -6}, Vec3{2, -1, -5}}) {
            double c[4]{};
            const double q[4] = {x.x, x.y, x.z, 1};
            for (unsigned r = 0; r < 4; ++r) for (unsigned k = 0; k < 4; ++k) c[r] += vp[4 * r + k] * q[k];
            const double z = c[2] / c[3];
            if (c[3] <= 0) continue; // Behind the mirror camera.
            if (x.y > 1) assert(z > -1 && z <= 1);
            else assert(z < -1);
        }
    }
    // Per eye, exact: where an eye sees an object's reflection on the water (the
    // plane point on the line from the eye to the object's mirror image), that
    // eye's mirror camera images the water point and the object at the same place,
    // so looking the water point up (distance 0) is right for any object distance.
    for (const Pose eye : {Pose{normalize(Quat{-.3f, .1f, .02f, 1}), Vec3{-.032f, 2.6f, .1f}},
                           Pose{normalize(Quat{-.3f, .1f, .02f, 1}), Vec3{.032f, 2.6f, .1f}}}) {
        const Pose m = mirrorCamera(eye, flat);
        const M4 vp = viewProjection(m, mirrorFov(fov, 0));
        for (Vec3 x : {Vec3{.4f, 1.3f, -1.5f}, Vec3{-1, 2.5f, -6}, Vec3{3, 8, -40}}) {
            const Vec3 image{x.x, 2 - x.y, x.z}; // Mirrored about y = 1.
            const float t = (eye.p.y - 1) / (eye.p.y - image.y);
            const Vec3 water{eye.p.x + (image.x - eye.p.x) * t, 1, eye.p.z + (image.z - eye.p.z) * t};
            double u0, v0, w0, u1, v1, w1;
            ndc(vp, water, u0, v0, w0);
            ndc(vp, x, u1, v1, w1);
            assert(std::abs(u0 - u1) < 1e-4 && std::abs(v0 - v1) < 1e-4);
        }
    }
    std::puts("water mirror ok");
}
