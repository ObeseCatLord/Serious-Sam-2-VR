#pragma once
#include "model_tree.hpp"
#include <array>
#include <limits>

namespace ss2vr {
struct NativeUiViewport {
    uint32_t x = 0, y = 0, width = 0, height = 0;
    float minZ = 0, maxZ = 1;
};
struct NativeUiRect { int32_t left = 0, top = 0, right = 0, bottom = 0; };
struct NativeUiProjection {
    Matrix44 constants{};
    std::array<std::array<float, 4>, 6> planes{};
    bool empty = false;
};
inline bool nativeUiCoefficient(double value, float &out) {
    const float narrowed = float(value);
    if (!std::isfinite(narrowed) || std::fpclassify(narrowed) == FP_SUBNORMAL ||
        (value != 0 && narrowed == 0)) return false;
    out = narrowed;
    return true;
}

// The original programs still evaluate their original position/UV/color data.
// X/Y/W place a strictly flat panel. Synthetic clip-Z retains source Z without
// physical thickness or eye-world depth occlusion. Destination depth testing/
// writes must be disabled; source fog/depth-dependent shading must be absent.
// Eye is the actual executed D3D clip projection (Z0..W), not Core's Z+-W
// projection. This helper does not set GPU state or establish owner provenance.
inline bool nativeUiProjection(const Matrix44 &source, const Matrix44 &eye,
                               Pose panelInEye, float panelWidth, float panelHeight,
                               uint32_t canvasWidth, uint32_t canvasHeight,
                               NativeUiViewport viewport, bool scissorEnabled,
                               NativeUiRect scissor, NativeUiProjection &out) {
    for (const auto *m : {&source, &eye})
        for (float value : m->m)
            if (!std::isfinite(value) || std::fpclassify(value) == FP_SUBNORMAL) return false;
    const auto &q = panelInEye.q;
    const double norm = double(q.x)*q.x + double(q.y)*q.y + double(q.z)*q.z + double(q.w)*q.w;
    if (!finite(panelInEye) || norm < .99 || norm > 1.01 ||
        !std::isfinite(panelWidth) || !std::isfinite(panelHeight) ||
        panelWidth <= 0 || panelHeight <= 0 || !canvasWidth || !canvasHeight ||
        !viewport.width || !viewport.height ||
        uint64_t(viewport.x) + viewport.width > canvasWidth ||
        uint64_t(viewport.y) + viewport.height > canvasHeight ||
        !std::isfinite(viewport.minZ) || !std::isfinite(viewport.maxZ) ||
        viewport.minZ < 0 || viewport.maxZ > 1 || viewport.minZ > viewport.maxZ)
        return false;
    int64_t left = viewport.x, top = viewport.y;
    int64_t right = left + viewport.width, bottom = top + viewport.height;
    if (scissorEnabled) {
        left = std::max(left, int64_t(scissor.left));
        top = std::max(top, int64_t(scissor.top));
        right = std::min(right, int64_t(scissor.right));
        bottom = std::min(bottom, int64_t(scissor.bottom));
    }
    if (left >= right || top >= bottom) {
        out = {}; out.empty = true;
        return true; // The once-only original callback may legitimately draw no pixels.
    }
    const Matrix34 panel = matrix(panelInEye);
    double flat[4][4]{};
    // Eye clip transform of panel-local XY/0/1; keep true Z for corner admission.
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c : {0u, 1u, 3u}) {
            for (unsigned k = 0; k < 3; ++k)
                flat[r][c] += double(eye.m[4*r+k]) * panel.m[4*k+c];
            if (c == 3) flat[r][c] += eye.m[4*r+3];
        }
    double minimumW = std::numeric_limits<double>::infinity();
    // Synthetic Z must not hide natural eye near/far exclusion. Linear corner
    // tests admit the entire physical panel rectangle, including nested viewports.
    for (double x : {-double(panelWidth)*.5, double(panelWidth)*.5})
        for (double y : {-double(panelHeight)*.5, double(panelHeight)*.5}) {
            const double w = flat[3][0]*x + flat[3][1]*y + flat[3][3];
            const double z = flat[2][0]*x + flat[2][1]*y + flat[2][3];
            const double margin = std::max(1., std::abs(w)) * 1e-5;
            if (!std::isfinite(w) || !std::isfinite(z)) return false;
            if (w <= margin || z <= margin || w-z <= margin) {
                // Part of the panel is behind this eye (a head turn the panel has
                // not followed yet): nothing of it is drawn in this eye.
                out = {}; out.empty = true;
                return true;
            }
            minimumW = std::min(minimumW, w);
        }
    // Source clip -> physical canvas -> stable panel. Positive source Y points
    // up; absolute viewport/scissor coordinates count down from canvas top.
    const double sx = double(panelWidth)*viewport.width/(2.*canvasWidth);
    const double sy = double(panelHeight)*viewport.height/(2.*canvasHeight);
    const double tx = double(panelWidth)*(double(viewport.x)+viewport.width*.5-canvasWidth*.5)/canvasWidth;
    const double ty = double(panelHeight)*(canvasHeight*.5-double(viewport.y)-viewport.height*.5)/canvasHeight;
    double h[4][4]{};
    for (unsigned r : {0u, 1u, 3u}) {
        h[r][0] = flat[r][0]*sx;
        h[r][1] = flat[r][1]*sy;
        h[r][3] = flat[r][0]*tx + flat[r][1]*ty + flat[r][3];
    }
    h[2][2] = minimumW * .5; // 0<=sourceZ<=sourceW stays strictly below eye W.
    // Use the same float H that will be combined into uploaded native constants.
    for (auto &row : h)
        for (double &value : row) {
            float narrowed;
            if (!nativeUiCoefficient(value, narrowed)) return false;
            value = narrowed;
        }
    const unsigned axes[3]{0, 1, 3};
    double g[3][3]{};
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 3; ++c) g[r][c] = h[axes[r]][axes[c]];
    const double determinant = g[0][0]*(g[1][1]*g[2][2]-g[1][2]*g[2][1]) -
        g[0][1]*(g[1][0]*g[2][2]-g[1][2]*g[2][0]) +
        g[0][2]*(g[1][0]*g[2][1]-g[1][1]*g[2][0]);
    double scale = 1;
    for (auto &row : g) scale *= std::hypot(row[0], row[1], row[2]);
    if (!std::isfinite(scale) || scale <= 0 || std::abs(determinant) <= scale*1e-6 || h[2][2] <= 0)
        return false;
    double inverse[4][4]{};
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 3; ++c) {
            const unsigned r1 = (c+1)%3, r2 = (c+2)%3;
            const unsigned c1 = (r+1)%3, c2 = (r+2)%3;
            inverse[axes[r]][axes[c]] =
                (g[r1][c1]*g[r2][c2]-g[r1][c2]*g[r2][c1])/determinant;
        }
    inverse[2][2] = 1/h[2][2];
    const double l = 2.*(left-viewport.x)/viewport.width-1;
    const double r = 2.*(right-viewport.x)/viewport.width-1;
    const double t = 1-2.*(top-viewport.y)/viewport.height;
    const double b = 1-2.*(bottom-viewport.y)/viewport.height;
    const double sourcePlanes[6][4]{{1,0,0,-l}, {-1,0,0,r}, {0,1,0,-b},
                                   {0,-1,0,t}, {0,0,1,0}, {0,0,-1,1}};
    NativeUiProjection result;
    for (unsigned i = 0; i < 6; ++i) {
        double coefficients[4]{}, largest = 0;
        for (unsigned c = 0; c < 4; ++c) {
            for (unsigned k = 0; k < 4; ++k) coefficients[c] += inverse[k][c]*sourcePlanes[i][k];
            largest = std::max(largest, std::abs(coefficients[c]));
        }
        if (!std::isfinite(largest) || largest <= 0) return false;
        for (unsigned c = 0; c < 4; ++c) {
            if (!nativeUiCoefficient(coefficients[c]/largest, result.planes[i][c])) return false;
        }
    }
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 4; ++c) {
            double value = 0;
            for (unsigned k = 0; k < 4; ++k) value += h[r][k]*source.m[4*k+c];
            if (!nativeUiCoefficient(value, result.constants.m[4*r+c])) return false;
        }
    out = result;
    return true;
}
} // namespace ss2vr
