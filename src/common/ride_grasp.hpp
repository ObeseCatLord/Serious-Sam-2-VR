#pragma once
#include "ride_grip_frame.hpp"

namespace ss2vr {
// Contact is measured against the actual submitted triangle surface, not a
// guessed handle centre or wheel pivot. Double intermediates bound division.
inline double ridePointTriangle(Vec3 point,Vec3 a,Vec3 b,Vec3 c) {
    const auto ab=b-a,ac=c-a,ap=point-a;
    const auto inner=[](Vec3 x,Vec3 y) {return double(x.x)*y.x+double(x.y)*y.y+double(x.z)*y.z;};
    const double aa=inner(ab,ab),bb=inner(ab,ac),cc=inner(ac,ac),d=inner(ap,ab),e=inner(ap,ac);
    const double determinant=aa*cc-bb*bb;
    double best=INFINITY;
    if(std::isfinite(determinant) && determinant>1e-18) {
        const double u=(d*cc-e*bb)/determinant,v=(e*aa-d*bb)/determinant;
        if(u>=0 && v>=0 && u+v<=1) {
            const auto delta=ap-ab*float(u)-ac*float(v);best=inner(delta,delta);
        }
    }
    for(const auto &edge:std::array<std::array<Vec3,2>,3>{{{{a,b}},{{b,c}},{{c,a}}}}) {
        const auto direction=edge[1]-edge[0];const double length=inner(direction,direction);
        if(!std::isfinite(length)||length<=1e-18)continue;
        const auto delta=point-(edge[0]+direction*float(std::clamp(inner(point-edge[0],direction)/length,0.,1.)));
        best=std::min(best,inner(delta,delta));
    }
    return best;
}
inline int rideContact(const RideGripFrame &frame,Vec3 point,int occupied=-1) {
    double nearest=.08*.08;int selected=-1;
    for(unsigned handle=0;handle<2;++handle) {
        if(int(handle)==occupied)continue;
        const auto &surface=frame.surfaces[handle];
        if(!surface.vertices||surface.vertices>35)continue;
        for(unsigned i=0;i<90;i+=3) {
            const auto a=surface.indices[i],b=surface.indices[i+1],c=surface.indices[i+2];
            if(a>=surface.vertices||b>=surface.vertices||c>=surface.vertices)return -1;
            const auto distance=ridePointTriangle(point,surface.positions[a],surface.positions[b],surface.positions[c]);
            if(std::isfinite(distance) && distance<nearest) {nearest=distance;selected=int(handle);}
        }
    }
    return selected;
}
// Callback-capable native conversion may revoke the borrowed owner. Mutation
// occurs only after both checks; tests can inject loss without executing natives.
template<class Convert,class Admitted>
inline bool commitRideHeading(float &heading,float base,float delta,Convert &&convert,Admitted &&admitted) {
    if(!admitted())return false;
    float converted=0;
    if(!convert(delta,converted) || !std::isfinite(converted) || !std::isfinite(base+converted) ||
       !admitted())return false;
    heading=base+converted;return true;
}
inline void finishRideHeading(float &heading,float fallback,bool intent,bool admitted) noexcept {
    if(intent && !admitted)heading=fallback; // Live normal return only, never abnormal cleanup.
}
struct RideGrasp {
    struct Hand {
        InputSampleBoundary blocked;
        uint32_t squeeze=0,pose=0;
        int handle=-1;
        bool armed=false,held=false;
    };
    Hand hands[2];
    struct Identity {
        RideGripRig rig;
        RideRenderIdentity render;
        uint32_t configuration=0,file=0,resource=0,parameter=0,session=0,reference=0;
        std::array<uint32_t,3> stretch{};
        RideHandleProfile profile=RideHandleProfile::Unknown;
    } identity;
    uint32_t continuity=0,mask=0;
    uint64_t sequence=0;
    float angle=0,travel=0,baseHeading=0,returnedHeading=0;
    bool keyed=false;
    uint32_t mismatch=0,rekeys=0; // Diagnostics, after the verified layout.
    void cancel(const Input &input,uint32_t producer) {
        for(auto &hand:hands) {
            hand.armed=hand.held=false;hand.handle=-1;
            hand.blocked={input.session,input.reference,input.sequence,input.tickMs,producer};
        }
        keyed=false;mask=0;
    }
    void interrupt() noexcept {keyed=false;mask=0;}
    void accept(float heading) {if(std::isfinite(heading))returnedHeading=heading;}
    bool sample(const RideGripFrame &frame,const Input &input,const uint32_t poseGeneration[2],
                uint32_t worldContinuity,bool eligible,float nativeHeading,float &delta,float &base) {
        delta=0;base=nativeHeading;
        const auto producer=frame.rig.producer;
        const bool same=keyed && identity.render==frame.render && identity.configuration==frame.configuration &&
            identity.file==frame.file && identity.resource==frame.resource && identity.parameter==frame.parameter &&
            identity.stretch==frame.stretch && identity.rig.graphics==frame.rig.graphics &&
            !std::memcmp(&identity.rig.origin,&frame.rig.origin,sizeof(Pose)) && identity.rig.turn==frame.rig.turn &&
            identity.profile==frame.profile && identity.rig.revision==frame.rig.revision &&
            identity.rig.generation==frame.rig.generation && identity.rig.producer==producer &&
            identity.session==input.session && identity.reference==input.reference && identity.rig.rider==frame.rig.rider &&
            continuity==worldContinuity;
        if(!eligible || !worldContinuity || !std::isfinite(nativeHeading)) {cancel(input,producer);return false;}
        if(!same || input.sequence<sequence) {
            // Which part of the identity changed (diagnostics only).
            mismatch=(keyed?0u:1u)|(identity.render==frame.render?0u:2u)|
                (identity.configuration==frame.configuration&&identity.file==frame.file&&identity.resource==frame.resource?0u:4u)|
                (identity.parameter==frame.parameter&&identity.stretch==frame.stretch?0u:8u)|
                (identity.rig.graphics==frame.rig.graphics?0u:16u)|
                (!std::memcmp(&identity.rig.origin,&frame.rig.origin,sizeof(Pose))&&identity.rig.turn==frame.rig.turn?0u:32u)|
                (identity.profile==frame.profile&&identity.rig.revision==frame.rig.revision&&identity.rig.generation==frame.rig.generation?0u:64u)|
                (identity.rig.producer==producer&&identity.rig.rider==frame.rig.rider?0u:128u)|
                (identity.session==input.session&&identity.reference==input.reference?0u:256u)|
                (continuity==worldContinuity?0u:512u)|(input.sequence<sequence?1024u:0u);
            ++rekeys;
            cancel(input,producer);identity={frame.rig,frame.render,frame.configuration,frame.file,frame.resource,
                frame.parameter,input.session,input.reference,frame.stretch,frame.profile};
            continuity=worldContinuity;keyed=true;
            sequence=input.sequence;returnedHeading=nativeHeading;return false;
        }
        const bool fresh=input.sequence!=sequence;
        uint32_t nextMask=0;
        for(unsigned h=0;h<2;++h) {
            auto &hand=hands[h];
            const bool changed=hand.squeeze!=input.wheelInputEpoch[h] || hand.pose!=poseGeneration[h];
            const bool available=poseGeneration[h] && rideGripPoseEligible(input,h) &&
                (input.wheelAdmissionMask&(1u<<h)) && input.wheelInputEpoch[h];
            if(changed || !available) {
                hand.armed=hand.held=false;hand.handle=-1;
                hand.blocked={input.session,input.reference,input.sequence,input.tickMs,producer};
                hand.squeeze=input.wheelInputEpoch[h];hand.pose=poseGeneration[h];
            } else if(fresh) {
                const bool down=(input.buttons[h]&Wheel)!=0;
                if(!down) {
                    hand.held=false;hand.handle=-1;
                    if(hand.blocked.permits(input,input.tickMs,producer))hand.armed=true;
                } else if(hand.armed && !hand.held) {
                    hand.armed=false;
                    const auto grip=bodyHandTracking(frame.rig.origin,frame.rig.turn,input.head,input.grip[h]);
                    const int occupied=hands[1-h].held?hands[1-h].handle:-1;
                    hand.handle=rideContact(frame,grip.p,occupied);hand.held=hand.handle>=0;
                }
            }
            if(hand.held)nextMask|=1u<<h;
        }
        if(!nextMask) {mask=0;sequence=input.sequence;return false;}
        Vec3 direction;
        if(nextMask==3)direction=input.grip[1].p-input.grip[0].p;
        else direction=rotate(input.grip[nextMask==1?0:1].q,{0,0,-1});
        const double horizontal=double(direction.x)*direction.x+double(direction.z)*direction.z;
        if(!std::isfinite(horizontal)||horizontal<1e-6) {cancel(input,producer);return false;}
        const float nowAngle=std::atan2(-direction.x,-direction.z);
        if(nextMask!=mask) {baseHeading=mask?returnedHeading:nativeHeading;travel=0;angle=nowAngle;}
        else if(fresh) {const float difference=nowAngle-angle;travel+=std::atan2(std::sin(difference),std::cos(difference));angle=nowAngle;}
        mask=nextMask;sequence=input.sequence;
        delta=travel;base=baseHeading;return std::isfinite(delta)&&std::isfinite(base);
    }
};
} // namespace ss2vr
