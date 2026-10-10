#pragma once
#include "roomscale_body_sweep.hpp"

namespace ss2vr {
// A conservative sphere-path query for head visibility. This supplies geometry,
// not native ownership, world freshness, collision availability or a fade policy.
// Call with the last admitted head centre (or a separately validated body anchor)
// to test travel as well as final occupancy. Do not advance that centre on a miss
// caused by unavailable query data. It must be used with rejectInitialContact.
inline roomscale::BodySweepCover coverHeadVolumeSweep(unsigned categoryCount,
    Vec3 from,Vec3 to,float radius,float numericalBudget) {
    using namespace roomscale;
    using namespace roomscale::detail;
    BodySweepCover result;
    if(!arithmeticSupported()||!categoryCount||categoryCount>BodyGeometry::MaximumHulls||
       !detail::finite({from.x,from.y,from.z})||!detail::finite({to.x,to.y,to.z})||
       !std::isfinite(radius)||radius<=0||!std::isfinite(numericalBudget)||numericalBudget<=0||
       numericalBudget>=radius)return result;
    const std::array<float,3> first{from.x,from.y,from.z},last{to.x,to.y,to.z};
    IntervalVector delta{};
    for(unsigned c=0;c<3;++c)delta[c]=minus(exact(last[c]),exact(first[c]));
    const auto distance=length(delta);
    if(!valid(distance)||distance.hi>MaximumHeadTranslation*2)return result;
    Vector direction{1,0,0};
    float parameter=.0001f;
    double allowance=0;
    if(from.x!=to.x||from.y!=to.y||from.z!=to.z) {
        parameter=outwardFloat(distance.hi);
        if(!parameter)return result;
        std::array<float,3> components{};
        IntervalVector error{};
        for(unsigned c=0;c<3;++c) {
            components[c]=float((double(last[c])-double(first[c]))/double(parameter));
            if(!std::isfinite(components[c]))return {};
            error[c]=minus(delta[c],times(exact(components[c]),exact(parameter)));
        }
        const auto bound=length(error);
        if(!valid(bound))return {};
        allowance=bound.hi;
        direction={components[0],components[1],components[2]};
    }
    // Even at rest a short nonzero ray tests occupancy at its initial sphere.
    // Extra travel is conservative; it is never interpreted as player movement.
    const float inflated=outwardFloat(up(double(radius)+allowance));
    if(!inflated||double(inflated)-double(radius)>numericalBudget)return {};
    for(unsigned h=0;h<categoryCount;++h) {
        auto& hull=result.body.hulls[h];
        hull.sphere[0]={{from.x,from.y,from.z},inflated};
        hull.count=1;hull.valid=true;
    }
    result.body.hullCount=categoryCount;result.body.queryCount=categoryCount;
    result.body.valid=true;result.direction=direction;result.maximumParameter=parameter;
    result.valid=true;return result;
}
} // namespace ss2vr

namespace ss2vr {
struct HeadVolumeObservation {
    Pose body{},head{};
    uint32_t owner=0,generation=0,session=0,reference=0;
    uint64_t requestSequence=0,inputSequence=0,simulationRevision=0,rigRevision=0,tickMs=0;
    float radius=0,nearZ=0,numericalGuard=0;
    bool sampled=false,clear=false;
};
// Unlike the legacy optional lean fade, unavailable/stale clearance is opaque.
// The same exact native anchor and requested head must be used for both eyes.
enum class HeadVolumeVisibilityReason : uint32_t {
    Clear, Unsampled, QueryBlocked, Owner, Input, Epoch, Sequence, Revision, Age, Volume, Body, Head
};
// This is the existing admission predicate, partitioned in its original order.
// Returning the first failed group avoids evaluating it again for diagnostics.
inline HeadVolumeVisibilityReason headVolumeVisibilityReason(const HeadVolumeObservation& sample,const Pose& body,const Pose& head,
    uint32_t owner,const Request& request,uint64_t simulationRevision,uint64_t rigRevision,uint64_t now) {
    const auto same=[](const Pose& a,const Pose& b) {
        return finite(a)&&finite(b)&&a.q.x==b.q.x&&a.q.y==b.q.y&&a.q.z==b.q.z&&a.q.w==b.q.w&&
            a.p.x==b.p.x&&a.p.y==b.p.y&&a.p.z==b.p.z;
    };
    using R=HeadVolumeVisibilityReason;
    if(!sample.sampled)return R::Unsampled;
    if(!sample.clear)return R::QueryBlocked;
    if(!(owner&&sample.owner==owner))return R::Owner;
    if(!(request.sequence&&request.input.sequence&&request.trackingGeneration&&request.session&&request.reference&&
         request.input.focused&&request.input.headValid&&finite(request.input.head)))return R::Input;
    if(!(sample.generation==request.trackingGeneration&&sample.session==request.session&&
         sample.reference==request.reference&&request.input.session==request.session&&
         request.input.reference==request.reference))return R::Epoch;
    if(!(sample.requestSequence==request.sequence&&sample.inputSequence==request.input.sequence))return R::Sequence;
    if(!(sample.simulationRevision==simulationRevision&&sample.rigRevision==rigRevision))return R::Revision;
    if(!(now>=sample.tickMs&&now-sample.tickMs<=100))return R::Age;
    if(!(std::isfinite(sample.radius)&&sample.radius>0&&std::isfinite(sample.nearZ)&&sample.nearZ>0&&
         std::isfinite(sample.numericalGuard)&&sample.numericalGuard>=.002f&&sample.numericalGuard<sample.radius))return R::Volume;
    if(!same(sample.body,body))return R::Body;
    if(!(finite(sample.head)&&finite(head)&&
         std::hypot(double(sample.head.p.x)-head.p.x,double(sample.head.p.y)-head.p.y,
                    double(sample.head.p.z)-head.p.z)<=double(sample.numericalGuard)*.25))return R::Head;
    return R::Clear;
}
inline bool headVolumeVisible(const HeadVolumeObservation& sample,const Pose& body,const Pose& head,
    uint32_t owner,const Request& request,uint64_t simulationRevision,uint64_t rigRevision,uint64_t now) {
    return headVolumeVisibilityReason(sample,body,head,owner,request,simulationRevision,rigRevision,now)==
        HeadVolumeVisibilityReason::Clear;
}
} // namespace ss2vr
