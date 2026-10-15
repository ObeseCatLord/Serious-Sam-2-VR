#include "common/weapon_view.hpp"
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
using namespace ss2vr;
static void check(bool ok, const char *message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static std::array<float, 3> projected(const WeaponWorldView &camera, Vec3 world) {
    float input[4]{world.x, world.y, world.z, 1}, view[4]{0,0,0,1}, clip[4]{};
    for (unsigned r=0;r!=3;++r)
        for (unsigned c=0;c!=4;++c) view[r] += camera.view.m[r*4+c]*input[c];
    for (unsigned r=0;r!=4;++r)
        for (unsigned c=0;c!=4;++c) clip[r] += camera.projection.m[r*4+c]*view[c];
    const float z = (clip[2]/clip[3]+1)*.5f;
    return {clip[0]/clip[3], clip[1]/clip[3], camera.nearDepth+z*(camera.farDepth-camera.nearDepth)};
}
int main() {
    const Pose center{normalize(multiply(yaw(.6f), Quat{.12f,.02f,.15f,.98f})), {8,2,-11}};
    for (unsigned eye=0; eye!=2; ++eye) {
        const Pose camera=compose(center, Pose{{}, {eye ? .032f : -.032f,0,0}});
        const Fov fov=eye ? Fov{-.72f,.91f,.87f,-.81f} : Fov{-.91f,.72f,.87f,-.81f};
        WeaponWorldView prepared{matrix(inverse(camera)),projection(fov,.05f,10000),.1f,.9f,true};
        const Matrix44 executionP=projection(fov,.19f,71);
        const auto executed=executedWeaponView(prepared,prepared.view,executionP,.1f,.9f);
        check(executed.valid, "Native execution clip-distance changes retain eye provenance");
        auto fullRoot=prepared;fullRoot.nearDepth=0;fullRoot.farDepth=1;
        const float worldFar=std::bit_cast<float>(0x3f666666u);
        const auto partitioned=executedRootWeaponView(fullRoot,fullRoot.view,executionP,0,worldFar);
        check(partitioned.valid && partitioned.farDepth==worldFar && fullRoot.farDepth==1,
              "Native sky partition validates only a copy and retains actual world depth");
        check(executedRootWeaponView(fullRoot,fullRoot.view,executionP,0,1).valid &&
              !executedWeaponView(fullRoot,fullRoot.view,executionP,0,worldFar).valid,
              "Unpartitioned root remains valid and generic endpoint equality remains strict");
        for(float far:{std::nextafter(worldFar,0.f),std::nextafter(worldFar,1.f),.8f,.1f})
            check(!executedRootWeaponView(fullRoot,fullRoot.view,executionP,0,far).valid,
                  "Adjacent floats and unrelated contained depth intervals are rejected");
        check(!executedRootWeaponView(prepared,prepared.view,executionP,0,worldFar).valid,
              "Only full prepared depth admits the native world partition");
        auto partitionWrongEye=fullRoot;
        partitionWrongEye.projection=projection(eye ? Fov{-.91f,.72f,.87f,-.81f} : Fov{-.72f,.91f,.87f,-.81f});
        auto invalidRootView=fullRoot.view;invalidRootView.m[0]=std::numeric_limits<float>::quiet_NaN();
        check(!executedRootWeaponView(partitionWrongEye,fullRoot.view,executionP,0,worldFar).valid &&
              !executedRootWeaponView(fullRoot,invalidRootView,executionP,0,worldFar).valid,
              "The partition cannot authorize a different eye or an invalid world matrix");
        const Vec3 wall=camera.p+rotate(camera.q,{0,0,-.8f});
        const Vec3 behindWall=camera.p+rotate(camera.q,{0,0,-1.1f});
        WeaponViewPass partitionedGun{partitioned};
        Matrix44 partitionGunProjection{};Matrix34 partitionGunView{};float partitionNear=0,partitionFar=0;
        check(partitionedGun.projection(partitionGunProjection)&&partitionedGun.view(partitionGunView)&&
              partitionedGun.depth(partitionNear,partitionFar)&&partitionedGun.placed(true),
              "Physical weapon acquires the same native partitioned world interval");
        WeaponWorldView partitionGun{partitionGunView,partitionGunProjection,partitionNear,partitionFar,true};
        check(projected(partitionGun,behindWall)[2]>projected(partitioned,wall)[2] &&
              projected(partitionGun,behindWall)==projected(partitioned,behindWall),
              "Terrain occludes a weapon behind it under the actual world interval");
        WeaponViewPass pass{executed};
        Matrix44 gunP{}; Matrix34 gunView{}; float nz=0, fz=.1f;
        check(pass.projection(gunP) && pass.view(gunView) && pass.depth(nz,fz) && pass.placed(true),
              "A gun must admit projection/view/depth/hand placement together");
        WeaponWorldView gun{gunView,gunP,nz,fz,true};
        for (Vec3 local : {Vec3{.2f,-.1f,-.8f},Vec3{.2f,-.1f,-1.1f},Vec3{-.4f,.7f,-6}}) {
            const Vec3 point=camera.p+rotate(camera.q,local);
            const auto expected=projected(executed,point), actual=projected(gun,point);
            for (unsigned i=0;i!=3;++i) check(expected[i]==actual[i],
                "Physical gun and world must share XY and Z for translated/rotated asymmetric eyes");
        }
        const Vec3 nearPoint=camera.p+rotate(camera.q,{0,0,-.8f});
        const Vec3 behind=camera.p+rotate(camera.q,{0,0,-1.1f});
        const float wallDepth=projected(executed,nearPoint)[2], gunDepth=projected(gun,behind)[2];
        WeaponWorldView compressed=gun; compressed.nearDepth=0; compressed.farDepth=.1f;
        check(gunDepth>wallDepth && projected(compressed,behind)[2]<wallDepth,
              "World-depth gun behind a wall rejects the old compressed foreground mapping");
        check(pass.restored() && pass.complete(), "Observed native restoration completes the invocation");
        auto wrongEye=prepared; wrongEye.projection=projection(eye ? Fov{-.91f,.72f,.87f,-.81f} : Fov{-.72f,.91f,.87f,-.81f});
        check(!executedWeaponView(wrongEye,prepared.view,executionP,.1f,.9f).valid,
              "Another eye cannot supply this gun's projection");
        auto moved=prepared.view; moved.m[3]+=.02f;
        check(!executedWeaponView(prepared,moved,executionP,.1f,.9f).valid,
              "Another view cannot inherit root preparation provenance");
        check(!executedWeaponView(prepared,prepared.view,executionP,0,.1f).valid,
              "Root execution depth must match the prepared native depth range");
        WeaponViewPass failed{executed};
        check(failed.projection(gunP) && failed.view(gunView) && failed.depth(nz,fz) &&
              !failed.placed(false) && failed.cleanupRequired() && !failed.complete(),
              "Placement failure after mutation requires cleanup and pair rejection");
        WeaponViewPass reordered{executed};
        check(!reordered.view(gunView) && reordered.failed && !reordered.complete(),
              "Missing/reordered native stages cannot start a physical view");
        WeaponViewPass incomplete{executed};
        check(incomplete.projection(gunP) && !incomplete.complete(),
              "An early native return after projection cannot silently complete");
        WeaponViewPass faultBeforeSetup{executed};
        faultBeforeSetup.failed = true; // Nested native render faults the outer before its frustum.
        check(!faultBeforeSetup.cleanupRequired(), "Failure before any graphics setup needs no cleanup");
        check(!faultBeforeSetup.projection(gunP) && !faultBeforeSetup.view(gunView) &&
              !faultBeforeSetup.depth(nz,fz) && !faultBeforeSetup.placed(false) &&
              faultBeforeSetup.stage == 0 && faultBeforeSetup.cleanupRequired(),
              "Rejected substitutions still reach native setup/fallback and require saved state restoration");
        WeaponViewPass untouched{executed};
        check(untouched.complete() && !untouched.cleanupRequired(),
              "Native early return before mutation needs no fabricated draw or cleanup");
    }
    WeaponViewPass outer, inner; WeaponViewPass *active=&outer;
    try {
        ScopedWeaponContext<WeaponViewPass> nested(active,&inner);
        check(active==&inner,"Owned nested calls use their own context");
        {
            ScopedWeaponContext<WeaponViewPass> unowned(active,nullptr);
            check(!active,"Unowned nested calls suppress inherited physical adaptation");
        }
        check(active==&inner,"Suppression restores the immediate parent context");
        throw 1;
    } catch (int) {}
    check(active==&outer,"Supported C++ unwind restores context without swallowing native exceptions");
    auto invalid=projection({-.8f,.8f,.8f,-.8f});
    invalid.m[0]=std::numeric_limits<float>::quiet_NaN();
    check(!finiteProjection(invalid) && !validDepthRange(0,0) && !validDepthRange(-.1f,1),
          "Nonfinite projection and invalid native depth ranges cannot be admitted");
    {
        Matrix34 eyeView{}; eyeView.m[0] = eyeView.m[5] = eyeView.m[10] = 1;
        Matrix34 bob = eyeView; bob.m[7] += .04f; bob.m[1] = .03f;
        Matrix34 other = eyeView; other.m[11] += 2;
        check(shakenWeaponView(bob, eyeView) && !sameWeaponView(bob, eyeView) && !shakenWeaponView(other, eyeView),
              "Camera shake on the eye view is kept; another camera is not");
        // Hundreds of metres from the world origin, as levels are played: a
        // degree of tilt plus 3 cm of bob moves the view's translation by metres,
        // still the same eye; a camera 2 m away or turned 15 degrees is not.
        const Pose eye{normalize(Quat{0, .38f, 0, .92f}), {263.2f, .74f, -241.9f}};
        const Matrix34 far = matrix(inverse(eye));
        const float tilt = std::sin(.0087f) * .7071f; // A degree about the view's x and z (pitch and roll).
        const Pose tilted{normalize(multiply(eye.q, Quat{tilt, 0, tilt, std::cos(.0087f)})), eye.p + Vec3{0, .03f, 0}};
        const Pose moved{eye.q, eye.p + Vec3{2, 0, 0}};
        const Pose turned{normalize(multiply(eye.q, Quat{0, std::sin(.13f), 0, std::cos(.13f)})), eye.p};
        check(shakenWeaponView(matrix(inverse(tilted)), far) && !sameWeaponView(matrix(inverse(tilted)), far) &&
                  !shakenWeaponView(matrix(inverse(moved)), far) && !shakenWeaponView(matrix(inverse(turned)), far),
              "Shake is judged by camera position and turn, not the view's translation");
    }
    std::cout << "Physical weapon view/depth checks passed; no native runtime executed\n";
}
