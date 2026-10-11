#include "common/scope_shader_constants.hpp"
#include <cassert>
#include <limits>
using namespace ss2vr;
static bool close(float a, float b) { return std::abs(a-b) < 2e-5f; }
int main() {
    ScopeOpticalFrame frame;
    frame.camera = frame.inverseCamera = matrix({});
    frame.radiusX = frame.radiusY = .02f;
    frame.valid = true;
    ScopeOpticalProjection projection;
    assert(scopeOpticalProjection(frame, .1f, 4, projection));
    ScopeImageCoordinates coordinates;
    coordinates.rows = {{{.04f,0,-.02f,0},{0,-.04f,.02f,0},{0,0,0,0}}};
    coordinates.valid = true;
    ScopeShaderConstants constants;
    for (Vec3 eye : {Vec3{0,0,.1f}, Vec3{.01f,-.005f,.1f}, Vec3{-.005f,.004f,.2f}}) {
        assert(scopeShaderConstants(frame, projection, coordinates, eye, .7f, .001f, .01f, constants));
        assert(constants.valid && constants.rows[5][0] == .7f);
        for (float u : {0.f,.25f,.5f,.75f,1.f}) for (float v : {0.f,.5f,1.f}) {
            // Evaluate the HLSL constant interface separately from the builder.
            const Vec3 aperture{.04f*u-.02f, -.04f*v+.02f, 0};
            const Vec3 ray = aperture-eye;
            const float shaderU = (constants.rows[0][0]*u+constants.rows[0][1]*v+
                constants.rows[0][2]-constants.rows[3][0])/constants.rows[3][2]*constants.rows[4][0]+constants.rows[4][2];
            const float shaderV = (constants.rows[1][0]*u+constants.rows[1][1]*v+
                constants.rows[1][2]-constants.rows[3][1])/constants.rows[3][2]*constants.rows[4][1]+constants.rows[4][3];
            std::array<float,2> expected;
            assert(scopeImageUv(frame, projection, ray, expected));
            assert(close(shaderU,expected[0]) && close(shaderV,expected[1]));
        }
    }
    assert(scopeShaderConstants(frame,projection,coordinates,{0,0,.1f},1,0,0,constants));
    assert(constants.rows[6][2]==0); // Missing native aim is explicitly disabled.
    assert(scopeReticleTarget(frame,projection,{1,0,-40},.001f,.012f,constants));
    assert(close(constants.rows[6][0],.75f) && close(constants.rows[6][1],.5f));
    assert(constants.rows[6][2]==.001f && constants.rows[6][3]==.012f);
    assert(scopeReticleTarget(frame,projection,{0,1,-40},.001f,.012f,constants));
    assert(close(constants.rows[6][0],.5f) && close(constants.rows[6][1],.25f));
    for (Vec3 bad : {Vec3{0,0,1},Vec3{0,0,0},Vec3{10,0,-1},
                    Vec3{0,0,std::numeric_limits<float>::quiet_NaN()}}) {
        assert(!scopeReticleTarget(frame,projection,bad,.001f,.012f,constants));
        assert((constants.rows[6]==std::array<float,4>{}));
    }
    const Pose placement{normalize({.2f,-.3f,.4f,.8f}), {10,-20,30}};
    frame.camera = matrix(placement); frame.inverseCamera = matrix(inverse(placement));
    const Vec3 relative{.01f,-.005f,.1f};
    const auto eye = placement.p+rotate(placement.q,relative);
    assert(scopeShaderConstants(frame,projection,coordinates,eye,1,0,0,constants));
    assert(close(constants.rows[3][0],relative.x) && close(constants.rows[3][1],relative.y) &&
           close(constants.rows[3][2],relative.z));
    assert(scopeReticleTarget(frame,projection,placement.p+rotate(placement.q,{1,0,-40}),.001f,.012f,constants));
    assert(close(constants.rows[6][0],.75f) && close(constants.rows[6][1],.5f));
    // Reprojection into an earlier source camera: the unchanged shader arithmetic
    // must equal projecting the current frame's world ray into the source camera.
    {
        auto rigid = [](Pose pose) {
            ScopeOpticalFrame f;
            f.camera = matrix(pose); f.inverseCamera = matrix(inverse(pose));
            f.radiusX = f.radiusY = .02f; f.valid = true;
            return f;
        };
        const Pose currentPose{normalize(Quat{.05f,.2f,-.03f,1}),{.3f,1.2f,-.4f}};
        const Pose sourcePose{normalize(Quat{.02f,.17f,.01f,1}),{.31f,1.19f,-.38f}};
        const auto current = rigid(currentPose), source = rigid(sourcePose);
        const Vec3 eye{.004f,-.003f,.12f};
        ScopeShaderConstants c;
        const Vec3 worldEye = currentPose.p + rotate(currentPose.q, eye); // The builder takes world space.
        assert(scopeShaderConstants(current, projection, coordinates, worldEye, 1, 0, 0, c));
        assert(scopeReprojectConstants(current, source, c));
        for (float u : {0.f,.3f,.5f,1.f}) for (float v : {0.f,.6f,1.f}) {
            const Vec3 aperture{.04f*u-.02f, -.04f*v+.02f, 0};
            const Vec3 worldRay = rotate(currentPose.q, aperture-eye);
            const Vec3 sourceRay = rotate(inverse(sourcePose.q), worldRay);
            const float expectedU = sourceRay.x/-sourceRay.z*c.rows[4][0]+c.rows[4][2];
            const float expectedV = sourceRay.y/-sourceRay.z*c.rows[4][1]+c.rows[4][3];
            float ray[3];
            for (unsigned row = 0; row < 3; ++row)
                ray[row] = c.rows[row][0]*u + c.rows[row][1]*v + c.rows[row][2] - c.rows[3][row];
            assert(close(ray[0]/-ray[2]*c.rows[4][0]+c.rows[4][2], expectedU));
            assert(close(ray[1]/-ray[2]*c.rows[4][1]+c.rows[4][3], expectedV));
        }
        ScopeShaderConstants same;
        assert(scopeShaderConstants(current, projection, coordinates, worldEye, 1, 0, 0, same));
        const auto before = same.rows;
        assert(scopeReprojectConstants(current, current, same));
        for (unsigned row = 0; row < 4; ++row) for (unsigned k = 0; k < 3; ++k)
            assert(std::abs(same.rows[row][k]-before[row][k]) < 1e-6f); // Same frame is identity.
        ScopeShaderConstants invalid;
        assert(!scopeReprojectConstants(current, source, invalid));
    }
    frame.camera = frame.inverseCamera = matrix({});
    for (Vec3 badEye : {Vec3{0,0,0},Vec3{0,0,-1},Vec3{0,0,std::numeric_limits<float>::infinity()}}) {
        assert(!scopeShaderConstants(frame,projection,coordinates,badEye,1,0,0,constants));
        assert(!constants.valid && constants.rows[4][0] == 0);
    }
    for (float visibility : {-1.f,1.1f,std::numeric_limits<float>::quiet_NaN()})
        assert(!scopeShaderConstants(frame,projection,coordinates,{0,0,.1f},visibility,0,0,constants));
    assert(!scopeShaderConstants(frame,projection,coordinates,{0,0,.1f},1,.02f,.01f,constants));
    assert(!scopeShaderConstants(frame,projection,coordinates,{0,0,.1f},1,0,.6f,constants));
    coordinates.rows[2][0] = std::numeric_limits<float>::quiet_NaN();
    assert(!scopeShaderConstants(frame,projection,coordinates,{0,0,.1f},1,0,0,constants));
}
