#include "common/replay_math.hpp"
#include <cassert>
#include <cstdio>
using namespace ss2vr;
using namespace ss2vr::replay;
static M4 view(Pose camera) { return m4(matrix(inverse(camera)).m, 3); }
static bool close(const M4 &a, const M4 &b, double tolerance) {
    for (unsigned i = 0; i < 16; ++i)
        if (std::abs(a[i] - b[i]) > tolerance * std::max(1., std::abs(b[i]))) return false;
    return true;
}
int main() {
    // Two eyes 64 mm apart, slightly canted, asymmetric FOVs, in a rotated body frame.
    const Quat body = normalize(Quat{.05f, .3f, -.02f, 1});
    const Vec3 centre{120.f, 4.f, -37.f};
    const Pose eye0{normalize(multiply(body, Quat{0, .02f, 0, 1})), centre + rotate(body, Vec3{-.032f, 0, 0})};
    const Pose eye1{normalize(multiply(body, Quat{0, -.02f, 0, 1})), centre + rotate(body, Vec3{.032f, 0, 0})};
    const Fov fov0{-.95f, .80f, .88f, -.92f}, fov1{-.80f, .95f, .88f, -.92f};
    const Matrix34 change = matrix(compose(inverse(eye1), eye0));
    const EyeChange e = eyeChange(change, fov0, fov1);
    assert(e.valid);
    // API adjustment A: depth 0..1 and a small drawport offset.
    const M4 adjust{1, 0, 0, .001, 0, 1, 0, -.001, 0, 0, .5, .5, 0, 0, 0, 1};
    for (float nearZ : {.05f, .01f}) {
        const M4 p0 = m4(projection(fov0, nearZ, 4000).m, 4), p1 = m4(projection(fov1, nearZ, 4000).m, 4);
        const M4 ap0 = multiply(adjust, p0), ap1 = multiply(adjust, p1);
        const M4 model = m4(matrix(Pose{normalize(Quat{.3f, .1f, .2f, 1}), Vec3{118.f, 3.5f, -40.f}}).m, 3);
        for (bool background : {false, true}) {
            M4 v0 = view(eye0), v1 = view(eye1);
            if (background) { v0[3] = v0[7] = v0[11] = 0; v1[3] = v1[7] = v1[11] = 0; }
            ViewState state;
            for (unsigned i = 0; i < 16; ++i) { state.adjusted[i] = float(ap0[i]); state.projection[i] = float(p0[i]); }
            for (unsigned i = 0; i < 12; ++i) state.view[i] = float(v0[i]);
            const Rewrite r = rewrite(state, e);
            assert(r.valid && !r.identity);
            // MVP upload (type 0x47): R * MVP0 == AP1 * V1 * M.
            const M4 mvp0 = multiply(multiply(ap0, v0), model), mvp1 = multiply(multiply(ap1, v1), model);
            float in[16], out[16];
            for (unsigned i = 0; i < 16; ++i) in[i] = float(mvp0[i]);
            rewriteUpload(0x47, r, in, 4, out);
            M4 got{}; for (unsigned i = 0; i < 16; ++i) got[i] = out[i];
            assert(close(got, mvp1, 2e-3));
            // Untagged block rows: recognized as this view's MVP and rewritten alike.
            assert(rewriteIfMvp(r, in, out));
            for (unsigned i = 0; i < 16; ++i) got[i] = out[i];
            assert(close(got, mvp1, 2e-3));
            // The frustum decomposition reproduces a standard MVP too.
            assert(rewriteFrustumMvp(r, in, out));
            for (unsigned i = 0; i < 16; ++i) got[i] = out[i];
            assert(close(got, mvp1, 2e-3));
            // Same frustum, another depth range (distant terrain): not affine
            // under AP0, but the frustum decomposition gives the eye-1 MVP.
            if (!background) {
                const M4 f0 = multiply(adjust, m4(projection(fov0, 2.f, 90000).m, 4));
                const M4 f1 = multiply(adjust, m4(projection(fov1, 2.f, 90000).m, 4));
                const M4 far0 = multiply(multiply(f0, v0), model), far1 = multiply(multiply(f1, v1), model);
                for (unsigned i = 0; i < 16; ++i) in[i] = float(far0[i]);
                assert(!rewriteIfMvp(r, in, out));
                assert(rewriteFrustumMvp(r, in, out));
                for (unsigned i = 0; i < 16; ++i) got[i] = out[i];
                assert(close(got, far1, 2e-3));
                // Depth-biased projection (z row nudged, as for decals or translucent
                // effects) on a model hundreds of metres out: must not pass as this
                // view's MVP, and the frustum path keeps its depth function.
                M4 b0 = ap0, b1 = ap1;
                b0[10] *= 1.0005; b1[10] *= 1.0005;
                const M4 farModel = m4(matrix(Pose{normalize(Quat{.1f, .4f, 0, 1}), Vec3{420.f, 12.f, -380.f}}).m, 3);
                const M4 bias0 = multiply(multiply(b0, v0), farModel), bias1 = multiply(multiply(b1, v1), farModel);
                for (unsigned i = 0; i < 16; ++i) in[i] = float(bias0[i]);
                assert(!rewriteIfMvp(r, in, out));
                assert(rewriteFrustumMvp(r, in, out));
                for (unsigned i = 0; i < 16; ++i) got[i] = out[i];
                assert(close(got, bias1, 2e-3));
                for (unsigned i = 0; i < 16; ++i) in[i] = float(mvp0[i]);
            }
            // The derived projections keep their perspective rows bit-exact.
            assert(r.projection[14] == p0[14] && r.projection[15] == p0[15] && r.adjusted[14] == ap0[14]);
            // Rows that are not an MVP of this view (e.g. a texture matrix) stay unchanged.
            float other[16]{.5f,0,0,.5f, 0,.5f,0,.5f, 0,0,1,0, 0,0,0,1};
            assert(!rewriteIfMvp(r, other, out));
            for (unsigned i = 0; i < 16; ++i) assert(out[i] == other[i]);
            // MV (0x46) and V (0x49), three rows.
            const M4 mv0 = multiply(v0, model), mv1 = multiply(v1, model);
            for (unsigned i = 0; i < 12; ++i) in[i] = float(mv0[i]);
            rewriteUpload(0x46, r, in, 3, out);
            for (unsigned i = 0; i < 12; ++i) assert(std::abs(out[i] - mv1[i]) < 2e-3 * std::max(1., std::abs(mv1[i])));
            for (unsigned i = 0; i < 12; ++i) in[i] = float(v0[i]);
            rewriteUpload(0x49, r, in, 3, out);
            for (unsigned i = 0; i < 12; ++i) assert(std::abs(out[i] - v1[i]) < 2e-3 * std::max(1., std::abs(v1[i])));
            // Adjusted projection (0x4a) and model (0x48).
            for (unsigned i = 0; i < 16; ++i) in[i] = float(ap0[i]);
            rewriteUpload(0x4a, r, in, 4, out);
            for (unsigned i = 0; i < 16; ++i) assert(std::abs(out[i] - ap1[i]) < 1e-4 * std::max(1., std::abs(ap1[i])));
            for (unsigned i = 0; i < 12; ++i) in[i] = float(model[i]);
            rewriteUpload(0x48, r, in, 3, out);
            for (unsigned i = 0; i < 12; ++i) assert(out[i] == in[i]);
        }
    }
    // A scissor of camera 0 mapped into camera 1 covers everything camera 0 sees
    // inside it from 1 m out, and stays put under an identity map.
    {
        const M4 adjust{1, 0, 0, .001, 0, 1, 0, -.001, 0, 0, .5, .5, 0, 0, 0, 1};
        const M4 ap0 = multiply(adjust, m4(projection(fov0, .05f, 4000).m, 4));
        const M4 ap1 = multiply(adjust, m4(projection(fov1, .05f, 4000).m, 4));
        ViewState state;
        const M4 v0 = view(eye0);
        for (unsigned i = 0; i < 16; ++i) { state.adjusted[i] = float(ap0[i]); state.projection[i] = float(m4(projection(fov0, .05f, 4000).m, 4)[i]); }
        for (unsigned i = 0; i < 12; ++i) state.view[i] = float(v0[i]);
        const Rewrite r = rewrite(state, e);
        assert(r.valid && !r.identity);
        const double W = 1000, H = 1100, centre[4] = {300, 200, 520, 450};
        double mapped[4] = {centre[0], centre[1], centre[2], centre[3]};
        assert(mapScissor(r.clip, ap0, 0, 0, W, H, mapped));
        assert(mapped[0] != centre[0] && mapped[2] - mapped[0] < 1.3 * (centre[2] - centre[0]));
        M4 inv0;
        assert(invert(ap0, inv0));
        const M4 to1 = multiply(ap1, e.view);
        for (double px = centre[0]; px <= centre[2]; px += 22)
            for (double py = centre[1]; py <= centre[3]; py += 25)
                for (double depth : {1., 3., 30., 3000.}) {
                    const double n[4] = {2 * px / W - 1, 1 - 2 * py / H, .5, 1};
                    double p[4]{};
                    for (unsigned k = 0; k < 4; ++k) for (unsigned c = 0; c < 4; ++c) p[k] += inv0[4 * k + c] * n[c];
                    const double s = depth / -(p[2] / p[3]);
                    const double q[4] = {p[0] / p[3] * s, p[1] / p[3] * s, -depth, 1};
                    double c1[4]{};
                    for (unsigned k = 0; k < 4; ++k) for (unsigned c = 0; c < 4; ++c) c1[k] += to1[4 * k + c] * q[c];
                    const double ex = (c1[0] / c1[3] + 1) * .5 * W, ey = (1 - c1[1] / c1[3]) * .5 * H;
                    assert(ex >= mapped[0] && ex <= mapped[2] && ey >= mapped[1] && ey <= mapped[3]);
                }
        const M4 identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        double same[4] = {centre[0], centre[1], centre[2], centre[3]};
        assert(mapScissor(identity, ap0, 0, 0, W, H, same));
        for (unsigned k = 0; k < 4; ++k) assert(std::abs(same[k] - centre[k]) <= 2);
    }
    // Canted eyes: the far plane moves out so what the centre keeps is kept in
    // each eye; the near plane stays.
    {
        const Pose c{Quat{0, 0, 0, 1}, Vec3{0, 0, 0}};
        const Fov cfov{-1.f, 1.f, .9f, -.9f};
        const M4 adjust{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, .5, .5, 0, 0, 0, 1};
        const M4 ap = multiply(adjust, m4(projection(cfov, .05f, 400).m, 4));
        M4 api;
        assert(invert(ap, api));
        for (float cant : {.15f, -.15f}) {
            const Pose eye{normalize(Quat{0, std::sin(cant / 2), 0, std::cos(cant / 2)}), Vec3{cant > 0 ? -.032f : .032f, 0, 0}};
            const EyeChange ec = eyeChange(matrix(compose(inverse(eye), c)), cfov, cfov);
            ViewState s;
            for (unsigned i = 0; i < 16; ++i) { s.adjusted[i] = float(ap[i]); s.projection[i] = float(m4(projection(cfov, .05f, 400).m, 4)[i]); }
            const M4 v0 = view(c);
            for (unsigned i = 0; i < 12; ++i) s.view[i] = float(v0[i]);
            Rewrite r = rewrite(s, ec);
            assert(r.valid);
            const auto eyeNdcZ = [&](const M4 &clip, const double (&p)[4]) {
                double cc[4]{}, e4[4]{};
                for (unsigned k = 0; k < 4; ++k) for (unsigned j = 0; j < 4; ++j) cc[k] += ap[4 * k + j] * p[j];
                for (unsigned k = 0; k < 4; ++k) for (unsigned j = 0; j < 4; ++j) e4[k] += clip[4 * k + j] * cc[j];
                return e4[2] / e4[3];
            };
            // Just inside the centre far plane, near the frustum's edge on the side the eye turns to.
            const double side = cant > 0 ? -1 : 1, D = 398;
            const double far[4] = {side * .95 * D, 0, -D, 1};
            M4 extended = r.clip;
            assert(eyeNdcZ(r.clip, far) > 1);
            assert(extendFar(extended, ap, ec.view));
            const double z = eyeNdcZ(extended, far);
            assert(z > 0 && z <= 1);
            // A point near the eye (5 cm) keeps its depth.
            M4 ei; assert(invert(ec.view, ei));
            const double qe[4] = {0, 0, -.05 * 1.0001, 1};
            double q[4]{};
            for (unsigned k = 0; k < 4; ++k) for (unsigned j = 0; j < 4; ++j) q[k] += ei[4 * k + j] * qe[j];
            assert(std::abs(eyeNdcZ(extended, q) - eyeNdcZ(r.clip, q)) < 1e-5);
            // Uncanted: nothing to do.
            const EyeChange straight = eyeChange(matrix(compose(inverse(Pose{c.q, Vec3{.032f, 0, 0}}), c)), cfov, cfov);
            M4 same = rewrite(s, straight).clip;
            assert(!extendFar(same, ap, straight.view));
        }
    }
    // Orthographic passes are eye independent.
    ViewState ortho;
    for (unsigned i = 0; i < 16; ++i) ortho.adjusted[i] = ortho.projection[i] = (i % 5 == 0) ? 1.f : 0.f;
    ortho.projection[0] = .01f; ortho.adjusted[0] = .01f;
    const Rewrite o = rewrite(ortho, e);
    assert(o.valid && o.identity);
    float in[16]{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}, out[16]{};
    rewriteUpload(0x47, o, in, 4, out);
    for (unsigned i = 0; i < 16; ++i) assert(out[i] == in[i]);
    std::puts("replay math ok");
}
