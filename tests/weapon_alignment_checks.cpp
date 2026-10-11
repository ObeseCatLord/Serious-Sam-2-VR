#include "common/weapon_alignment.hpp"
#include <cassert>
#include <limits>
using namespace ss2vr;
int main() {
    WeaponRenderRoute parent{20,1,true,true,true,nullptr};
    WeaponRenderRoute child{20,2,false,true,true,&parent};
    assert(nativeDesktopSniperPlacement(&child,20,-1,true));
    for(int eye : {-2,0,1})assert(!nativeDesktopSniperPlacement(&child,20,eye,true));
    assert(!nativeDesktopSniperPlacement(nullptr,20,-1,true));
    assert(!nativeDesktopSniperPlacement(&child,21,-1,true));
    assert(!nativeDesktopSniperPlacement(&child,20,-1,false));
    for(unsigned change=0;change<10;++change) {
        auto p=parent,c=child;c.parent=&p;
        switch(change) {
            case 0:c.parent=nullptr;break;
            case 1:++p.weapon;break;
            case 2:++c.weapon;break;
            case 3:p.depth=2;break;
            case 4:c.depth=3;break;
            case 5:p.sniper=false;break;
            case 6:c.sniper=true;break;
            case 7:p.desktop=false;break;
            case 8:c.originalActive=false;break;
            case 9:p.originalActive=false;break;
        }
        assert(!nativeDesktopSniperPlacement(&c,20,-1,true));
    }
    child.desktop=false;
    assert(!nativeDesktopSniperPlacement(&child,20,-1,true));
    assert(alignmentBorrowPhase(1,true) && alignmentBorrowPhase(0,false));
    for(unsigned depth : {0u,1u,2u,UINT32_MAX}) {
        if(depth!=1)assert(!alignmentBorrowPhase(depth,true));
        if(depth!=0)assert(!alignmentBorrowPhase(depth,false));
    }
    Vec3 s;
    assert(id1RenderedStretch(1,{-.7f,1.2f,0.f},s));
    assert(s.x==-.7f && s.y==1.2f && s.z==0.f); // Direct native branch has no setter.
    assert(id1RenderedStretch(0,{-.7f,1.2f,-.8f},s));
    assert(s.x==.7f && s.y==1.2f && s.z==-.8f); // Already negative X must become positive.
    const float epsilon=std::bit_cast<float>(uint32_t{0x3727c5ac});
    assert(id1RenderedStretch(0,{0.f,-0.f,-1e-8f},s));
    assert(s.x==-epsilon && s.y==-epsilon && s.z==-epsilon);
    assert(id1RenderedStretch(0,{-0.f,1e-8f,.8f},s));
    assert(s.x==epsilon && s.y==epsilon && s.z==.8f);
    for(float bad : {std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        assert(!id1RenderedStretch(0,{bad,1,1},s));
        assert(!id1RenderedStretch(1,{1,bad,1},s));
        assert(!id1RenderedStretch(0,{1,1,bad},s));
    }
    const WeaponAlignmentBinding retained{10,20,30,40,1,1,50,60,70,{1,1,1},1};
    assert(retained==retained);
    // A same-weapon in-place model/resource replacement must invalidate the
    // ordinary gun calibration too; an authored grip correction is not needed.
    for (uint32_t id = 0; id < WeaponCount; ++id) {
        auto cached=retained;cached.nativeId=id;
        const bool corrected=weaponAlignmentSupported(id);
        assert(weaponModelCalibrationMatches(cached,cached,corrected)==(id!=14));
        assert(!weaponModelCalibrationMatches(cached,cached,!corrected));
        for (unsigned change=0; change<5; ++change) {
            auto live=cached;
            switch(change) {
                case 0: ++live.model;break;
                case 1: ++live.instance;break;
                case 2: ++live.configuration;break;
                case 3: ++live.resource;break;
                case 4: live.baseStretch.x=-live.baseStretch.x;break;
            }
            assert(!weaponModelCalibrationMatches(cached,live,corrected));
        }
    }
    for (uint32_t id : {WeaponCount,UINT32_MAX}) {
        auto unsupported=retained;unsupported.nativeId=id;
        assert(!weaponModelBindingSupported(id));
        assert(!weaponModelCalibrationMatches(unsupported,unsupported,false));
    }
    auto changed=retained;
    for(unsigned field=0;field<13;++field) {
        changed=retained;
        switch(field) {
            case 0:++changed.owner;break;
            case 1:++changed.weapon;break;
            case 2:++changed.model;break;
            case 3:++changed.instance;break;
            case 4:changed.selector=0;break;
            case 5:changed.hand=0;break;
            case 6:++changed.configuration;break;
            case 7:++changed.file;break;
            case 8:++changed.resource;break;
            case 9:changed.baseStretch.x=-1;break;
            case 10:changed.baseStretch.y=.8f;break;
            case 11:changed.baseStretch.z=1.1f;break;
            case 12:changed.nativeId=13;break;
        }
        assert(changed!=retained);
    }
    changed=retained;changed.baseStretch.x=0.f;
    auto signedZero=changed;signedZero.baseStretch.x=-0.f;
    assert(changed!=signedZero); // Native flooring/reflection distinguishes zero signs.
    // The fixed reference uses the native model frame, before stretch. It is
    // not continuously inverted through animated bones.
    const Quat q=normalize(multiply(yaw(.7f),Quat{.12f,0,0,.99f}));
    Vec3 reference;
    for(uint32_t unsupported : {14u,WeaponCount,UINT32_MAX}) {
        assert(!weaponAlignmentSupported(unsupported));
        assert(!weaponModelHandleReference(unsupported,reference));
        assert(dot(reference,reference)==0);
    }
    for(uint32_t nativeId : {0u,1u,2u,3u,4u,5u,6u,7u,8u,9u,10u,11u,12u,13u,15u,16u}) for(uint32_t selector : {0u,1u}) {
        assert(weaponAlignmentSupported(nativeId) && weaponModelHandleReference(nativeId,reference));
        assert(id1RenderedStretch(selector,{1.1f,.9f,.8f},s));
        const Vec3 scaled{s.x*reference.x,s.y*reference.y,s.z*reference.z};
        const Vec3 delta=modelAnchorDisplacement(q,s,reference);
        const Vec3 anchored=delta+rotate(q,scaled);
        assert(dot(anchored,anchored)<1e-12f);
        const Vec3 animatedLocalResidual{.0003f,-.0002f,.0005f};
        const Vec3 movedReference=delta+rotate(q,scaled+animatedLocalResidual);
        const Vec3 expected=rotate(q,animatedLocalResidual);
        const Vec3 error=movedReference-expected;
        assert(dot(error,error)<1e-12f);
    }
}
