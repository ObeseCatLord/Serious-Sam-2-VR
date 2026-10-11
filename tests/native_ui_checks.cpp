#include "common/native_ui.hpp"
#include "common/native_ui_finish.hpp"
#include <cstdlib>
#include <iostream>
#include <vector>
using namespace ss2vr;
using V4 = std::array<float, 4>;
static void check(bool condition, const char *message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
// Explicit rounded scalar multiply/add models uploaded float constants. It is
// an offline numeric oracle, not a claim about a particular GPU's fused math.
static float dot32(V4 a, V4 b) {
    volatile float sum = 0;
    for (unsigned i = 0; i < 4; ++i) {
        volatile float product = a[i]*b[i];
        sum = sum + product;
    }
    return sum;
}
static V4 transformClip(const Matrix44 &m, V4 v) {
    V4 result{};
    for (unsigned r = 0; r < 4; ++r)
        result[r] = dot32({m.m[4*r], m.m[4*r+1], m.m[4*r+2], m.m[4*r+3]}, v);
    return result;
}
static Matrix44 d3dProjection(Fov f) {
    auto result = projection(f, .05f, 10000);
    for (unsigned c = 0; c < 4; ++c)
        result.m[8+c] = .5f*(result.m[8+c]+result.m[12+c]);
    return result;
}
static bool clipped(const NativeUiProjection &p, V4 v) {
    for (const auto &plane : p.planes) if (dot32(plane, v) < 0) return true;
    return false;
}
int main() {
    // Recorded native position, UV and color elements from an actual admitted
    // builtin draw. Only the first supplies the shader's declared v0 input.
    check(nativeUiPositionInput(0,0,2,0,5,0),"Native TEXCOORD0 position input must admit");
    check(!nativeUiPositionInput(2,0,2,0,5,2) && !nativeUiPositionInput(3,0,4,0,5,3),
          "Other native vertex streams are not position aliases");
    for(const auto &e:std::vector<std::array<uint32_t,6>>{
        {0,0,2,0,0,0}, {0,0,2,0,9,0}, {0,0,2,0,5,1},
        {1,0,2,0,5,0}, {0,4,2,0,5,0}, {0,0,3,0,5,0}, {0,0,2,1,5,0}})
        check(!nativeUiPositionInput(e[0],e[1],e[2],e[3],e[4],e[5]),
              "Unproved semantic/register, transformed/type, stream, offset or method rejects");
    for (uint32_t topology : {0u,1u,2u,3u,7u,0xffffffffu})
        check(!nativeUiTriangleTopology(topology,3),
              "Unsupported point/line/unknown raster footprints cannot complete panel UI");
    for (uint32_t topology : {4u,5u,6u}) {
        check(nativeUiTriangleTopology(topology,3),"Admitted filled triangles retain geometric panel coverage");
        for (uint32_t fill : {0u,1u,2u,4u,0xffffffffu})
            check(!nativeUiTriangleTopology(topology,fill),
                  "Point/wireframe/unknown fill modes cannot complete otherwise admitted triangle UI");
    }
    {
        struct Surface {} surface;
        Surface *owned=&surface;
        bool fault=false,halt=false;
        unsigned calls=0,rejections=0;
        const auto reject=[&] { fault=halt=true; ++rejections; };
        check(!retireNativeUiLock(owned,[&](Surface *value) {
            check(value==&surface,"Failed unlock targets the actual retained lock owner");
            ++calls; return false;
        },reject) && owned==&surface && fault && halt && calls==1 && rejections==1,
              "Failed unlock retains ownership and prevents subsequent readback adaptation");
        check(!retireNativeUiLock(owned,[&](Surface *) { ++calls; return false; },reject) &&
              owned==&surface && halt && calls==2 && rejections==2,
              "Bounded cleanup failure keeps the unresolved owner available for reset quarantine");
        check(retireNativeUiLock(owned,[&](Surface *) { ++calls; return true; },reject) &&
              !owned && halt && calls==3 && rejections==2,
              "Confirmed retirement clears lock ownership without reviving a halted generation");
        check(retireNativeUiLock(owned,[&](Surface *) { ++calls; return false; },reject) && calls==3,
              "Repeated cleanup cannot unlock an already retired surface");
    }
    {
        for (bool quit : {false,true}) for (bool loss : {false,true}) {
            std::vector<int> layers{1,2,3,4}; // Projection, both wheels, fallback HUD.
            bool observedLoss=false;
            const auto fallbackUpload=[&] { observedLoss=loss; };
            fallbackUpload(); // Models a newly reported loss at the wait/upload boundary.
            bool world=finalNativeUiLayers(quit,observedLoss,layers,true);
            unsigned submissions=0;
            if (world) ++submissions;
            check(world==(!quit && !loss) && layers.size()==(world ? 4u : 0u) && submissions==unsigned(world),
                  "Loss during fallback upload suppresses every layer and surviving-world accounting");
        }
        std::vector<int> fallbackOnly{2,3,4};
        check(!finalNativeUiLayers(false,true,fallbackOnly,false) && fallbackOnly.empty(),
              "Session loss also suppresses wheel/HUD-only frames without a world projection");
    }

    const Matrix44 source{{2.f/640,0,0,-1-.99f/1024,
                           0,-2.f/480,0,1+.99f/896,
                           0,0,1,0, 0,0,0,1}};
    const NativeUiViewport full{0,0,1024,896,0,1}, nested{40,50,800,600,.1f,.9f};
    const NativeUiRect stale{INT32_MAX, INT32_MIN, -17, -19};
    const Fov fovs[2]{{-.91f,.72f,.87f,-.81f}, {-.72f,.91f,.87f,-.81f}};
    const float width = 2.2f, height = 1.65f;
    unsigned signs = 0;
    // The exact independent physical-thickness failure fixture is retained as
    // a regression for the chosen flat/synthetic-Z helper, without adding a
    // second production renderer or keeping the rejected thickness algorithm.
    const Matrix44 identity{{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}};
    const auto failureEye = d3dProjection({std::atan(-1.15f),std::atan(.85f),
                                          std::atan(.95f),std::atan(-1.05f)});
    for (float angle : {0.f,.4f,.9f}) {
        NativeUiProjection output;
        check(nativeUiProjection(identity,failureEye,{yaw(angle),{.2f,-.2f,-2}},width,height,
                                 1024,896,full,false,{},output), "Original numeric failure geometry admitted");
        for (float x : {-.9f,-.4f,0.f,.6f,.9f})
            for (float y : {-.8f,0.f,.8f})
                for (float edge : {0.f,1.f})
                    for (float delta : {-.01f,-.001f,-.0001f,.0001f,.001f,.01f}) {
                        const V4 q{x,y,edge+delta,1};
                        check(clipped(output,transformClip(output.constants,q)) == (q[2]<0 || q[2]>1),
                              "Synthetic clip-Z fixes the physical-volume boundary-sign counterexample");
                        ++signs;
                    }
    }
    for (unsigned eye = 0; eye < 2; ++eye)
        for (float angle : {0.f,.4f,.9f}) {
            const Pose panel{yaw(angle), {.2f + (eye ? -.032f : .032f),-.2f,-2}};
            const auto projection = d3dProjection(fovs[eye]);
            NativeUiProjection output;
            check(nativeUiProjection(source, projection, panel, width, height, 1024,896,
                                     full,false,stale,output) && !output.empty,
                  "Flat panel admitted; disabled stale scissor ignored");
            for (float x : {32.f,192.f,320.f,512.f,608.f})
                for (float y : {48.f,240.f,432.f})
                    for (float z : {-.01f,-.001f,-.0001f,.0001f,.001f,.5f,
                                    .999f,.9999f,1.0001f,1.001f,1.01f}) {
                        const V4 vertex{x,y,z,1};
                        const auto q = transformClip(source,vertex), actual = transformClip(output.constants,vertex);
                        const Vec3 local{width*.5f*q[0],height*.5f*q[1],0};
                        const auto point = panel.p + rotate(panel.q,local);
                        const auto expected = transformClip(projection,{point.x,point.y,point.z,1});
                        for (unsigned axis : {0u,1u,3u})
                            check(std::abs(expected[axis]-actual[axis]) < 2e-6f,
                                  "Synthetic Z retains exactly flat X/Y/W in both asymmetric eyes");
                        check(clipped(output,actual) == (z<0 || z>1),
                              "Uploaded combined matrix/planes preserve near/far signs adjacent to boundaries");
                        ++signs;
                    }
            // Same original 3D effect vertices at any sourceZ have the same
            // flat projected XY/W and perspective interpolation weights.
            for (V4 vertex : {V4{40,40,0,1}, {590,80,0,1}, {330,440,0,1}}) {
                const auto flat = transformClip(output.constants,vertex);
                vertex[2] = .97f;
                const auto animated = transformClip(output.constants,vertex);
                for (unsigned axis : {0u,1u,3u})
                    check(flat[axis] == animated[axis], "Native effectZ cannot add physical UI thickness");
            }
            for (unsigned boundary : {4u,5u}) {
                const V4 a{120,140,boundary==4 ? -.4f : 1.4f,1};
                const V4 b{510,310,.6f,1};
                const auto qa = transformClip(source,a), qb = transformClip(source,b);
                const auto ea = transformClip(output.constants,a), eb = transformClip(output.constants,b);
                const float sa = boundary==4 ? qa[2] : qa[3]-qa[2];
                const float sb = boundary==4 ? qb[2] : qb[3]-qb[2];
                const float expectedT = sa/(sa-sb);
                const float da = dot32(output.planes[boundary],ea), db = dot32(output.planes[boundary],eb);
                const float actualT = da/(da-db);
                check(std::abs(expectedT-actualT)<2e-6f,
                      "Crossing triangle edge is clipped at the original native source parameter");
                // Hardware interpolates original vertex outputs at that edge;
                // equal t preserves both UV and color values without replacing programs.
                for (const auto values : {std::array<float,2>{.2f,.9f}, {.8f,.1f}, {.3f,1.f}})
                    check(std::abs((values[0]+(values[1]-values[0])*expectedT)-
                                   (values[0]+(values[1]-values[0])*actualT))<2e-6f,
                          "Transported clipping retains the original edge UV/color interpolation");
            }
            check(nativeUiProjection(source,projection,panel,width,height,1024,896,
                                     nested,true,{240,150,640,450},output),
                  "Offset/resized native viewport and actual scissor accepted");
            for (float x : {110.f,220.f,330.f,440.f,550.f})
                for (float y : {90.f,180.f,270.f,360.f}) {
                    const V4 vertex{x,y,.5f,1};
                    const auto q = transformClip(source,vertex), mapped = transformClip(output.constants,vertex);
                    // Independent source screen oracle; nested viewport must
                    // retain its position in the full canvas rather than fill it.
                    const float screenX = 40 + 400*(q[0]+1), screenY = 50 + 300*(1-q[1]);
                    const bool outside = screenX<240 || screenX>640 || screenY<150 || screenY>450;
                    check(clipped(output,mapped)==outside,"Projected scissor retains source geometric halfspaces");
                    const Vec3 local{width*(screenX/1024-.5f),height*(.5f-screenY/896),0};
                    const auto point = panel.p + rotate(panel.q,local);
                    const auto expected = transformClip(projection,{point.x,point.y,point.z,1});
                    for (unsigned axis : {0u,1u,3u})
                        check(std::abs(expected[axis]-mapped[axis])<2e-6f,
                              "Nested drawport preserves global native panel placement");
                }
        }
    auto mixed = source;
    mixed.m[8]=.00025f; mixed.m[9]=-.0005f; mixed.m[10]=.5f; mixed.m[11]=.2f;
    mixed.m[12]=.0005f; mixed.m[13]=.00025f;
    auto mixedVertex = [](float x, float y, float ratio) -> V4 {
        const float w=.0005f*x+.00025f*y+1;
        return {x,y,(ratio*w-.00025f*x+.0005f*y-.2f)/.5f,1};
    };
    for (unsigned eyeIndex=0; eyeIndex<2; ++eyeIndex) {
        const auto eye = d3dProjection(fovs[eyeIndex]);
        const Pose panel{normalize(multiply(yaw(.4f),Quat{.08f,.1f,.05f,.99f})),{.2f,-.2f,-2}};
        NativeUiProjection output;
        check(nativeUiProjection(mixed,eye,panel,width,height,1024,896,nested,true,
                                 {240,150,640,450},output), "Nontrivial sourceZ/W matrix admitted");
        for (float x : {32.f,220.f,400.f,608.f})
            for (float y : {48.f,180.f,300.f,432.f})
                for (float ratio : {-.0001f,.0001f,.9999f,1.0001f}) {
                    const auto v=mixedVertex(x,y,ratio), q=transformClip(mixed,v);
                    const auto e=transformClip(output.constants,v);
                    const bool near=q[2]<0, far=q[3]-q[2]<0;
                    check((dot32(output.planes[4],e)<0)==near && (dot32(output.planes[5],e)<0)==far,
                          "Actual rounded original sourceZ/W is the clipping reference");
                    const float screenX=40+400*(q[0]/q[3]+1), screenY=50+300*(1-q[1]/q[3]);
                    const bool outside=screenX<240 || screenX>640 || screenY<150 || screenY>450;
                    check(clipped(output,e)==(near || far || outside), "Perspective source scissor oracle agrees");
                    const Vec3 local{width*(screenX/1024-.5f),height*(.5f-screenY/896),0};
                    const auto point=panel.p+rotate(panel.q,local);
                    const auto expected=transformClip(eye,{point.x,point.y,point.z,1});
                    for (unsigned axis : {0u,1u})
                        check(std::abs(expected[axis]/expected[3]-e[axis]/e[3])<2e-6f,
                              "Varying sourceW retains the original flat normalized screen geometry");
                    ++signs;
                }
        for (unsigned boundary : {4u,5u}) {
            const auto a=mixedVertex(120,140,boundary==4 ? -.3f : 1.4f), b=mixedVertex(510,310,.6f);
            const auto qa=transformClip(mixed,a), qb=transformClip(mixed,b);
            const auto ea=transformClip(output.constants,a), eb=transformClip(output.constants,b);
            check(qa[3]!=qb[3], "Crossing-edge source W values are genuinely unequal");
            const float sa=boundary==4 ? qa[2] : qa[3]-qa[2], sb=boundary==4 ? qb[2] : qb[3]-qb[2];
            const float da=dot32(output.planes[boundary],ea), db=dot32(output.planes[boundary],eb);
            check(std::abs(sa/(sa-sb)-da/(da-db))<2e-6f,
                  "Unequal sourceW crossing edge retains native clipping interpolation parameter");
        }
    }
    NativeUiProjection output;
    const auto eye = d3dProjection(fovs[0]);
    const Pose panel{{},{0,0,-2}};
    check(nativeUiProjection(source,eye,panel,width,height,1024,896,full,true,{9,9,8,8},output) && output.empty,
          "Empty clipped draw is a valid no-pixel result");
    // A panel (partly) behind the eye, too near or beyond far is simply not drawn
    // in this eye: a fast head turn must not drop the stereo pair.
    for (Pose hidden : {Pose{{},{0,0,2}}, Pose{{},{0,0,-.04f}}, Pose{{},{0,0,-10001}}})
        check(nativeUiProjection(source,eye,hidden,width,height,1024,896,full,false,{},output) && output.empty,
              "Panel outside this eye's near/far range is an empty no-pixel result");
    output.constants.m[0] = 123;
    for (Pose invalid : {Pose{yaw(Pi/2),{0,0,-2}}, Pose{Quat{0,0,0,0},{0,0,-2}}})
        check(!nativeUiProjection(source,eye,invalid,width,height,1024,896,full,false,{},output) &&
              output.constants.m[0]==123, "Rejected edge-on/invalid panel does not publish constants");
    auto bad = source; bad.m[3] = std::numeric_limits<float>::quiet_NaN();
    check(!nativeUiProjection(bad,eye,panel,width,height,1024,896,full,false,{},output),
          "Nonfinite native constants reject before projection");
    auto subnormal = identity;
    for (unsigned i : {0u,5u,10u,15u}) subnormal.m[i]=std::numeric_limits<float>::denorm_min();
    check(!nativeUiProjection(subnormal,eye,{{},{0,0,-1}},width,height,1024,896,full,false,{},output) &&
          output.constants.m[0]==123, "Subnormal source cannot lose negative clipZ through float narrowing");
    auto normal = identity; normal.m[10]=std::numeric_limits<float>::min();
    check(!nativeUiProjection(normal,eye,{{},{0,0,-1}},width,height,1024,896,full,false,{},output) &&
          output.constants.m[0]==123, "Normal source input whose computed depth becomes subnormal rejects");
    normal=identity; normal.m[0]=std::numeric_limits<float>::min();
    const auto symmetric=d3dProjection({-.8f,.8f,.8f,-.8f});
    check(!nativeUiProjection(normal,symmetric,panel,1e-37f,height,1024,896,full,false,{},output) &&
          output.constants.m[0]==123, "Normal inputs whose combined MVP coefficient rounds to zero reject");
    check(!nativeUiProjection(source,eye,panel,width,height,1024,896,{1024,0,1,896,0,1},false,{},output),
          "Overflowing source viewport rejected");
    check(!nativeUiProjection(source,eye,panel,width,height,1024,896,{0,0,1024,896,.9f,.1f},false,{},output),
          "Invalid actual depth range rejected");
    auto negativeW = source; negativeW.m[15] = -1;
    check(nativeUiProjection(negativeW,eye,panel,width,height,1024,896,full,false,{},output) &&
          clipped(output,transformClip(output.constants,{320,240,.5f,1})),
          "Negative sourceW cannot enter the retained source clip volume");
    std::cout << "Native UI flat homography, uploaded fp32 clipping and layout checks passed ("
              << signs << " near/far witnesses); no GPU/runtime executed\n";
}
