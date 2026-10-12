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
