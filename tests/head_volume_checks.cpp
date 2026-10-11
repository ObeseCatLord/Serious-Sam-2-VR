#include "common/head_volume.hpp"
#include <cassert>
#include <random>
using namespace ss2vr;
int main() {
    roomscale::BodyGeometry body;body.hullCount=2;
    auto still=coverHeadVolumeSweep(body.hullCount,{1,2,3},{1,2,3},.12f,.001f);
    assert(still.valid&&still.body.hullCount==2&&still.body.queryCount==2);
    assert(still.maximumParameter>0&&still.direction.x==1);
    assert(still.body.hulls[0].sphere[0].radius>=.12f);
    assert(!coverHeadVolumeSweep(body.hullCount,{},{10,0,0},.12f,.001f).valid);
    assert(!coverHeadVolumeSweep(body.hullCount,{},{NAN,0,0},.12f,.001f).valid);
    assert(!coverHeadVolumeSweep(body.hullCount,{},{},0,.001f).valid);
    assert(!coverHeadVolumeSweep(body.hullCount,{},{},.12f,0).valid);
    assert(!coverHeadVolumeSweep(body.hullCount,{},{},.12f,.12f).valid);
    assert(!coverHeadVolumeSweep(body.hullCount,{},{},INFINITY,.001f).valid);
    body.hullCount=0;assert(!coverHeadVolumeSweep(body.hullCount,{},{},.12f,.001f).valid);
    body.hullCount=3;assert(!coverHeadVolumeSweep(body.hullCount,{},{},.12f,.001f).valid);
    body.hullCount=2;
    Request request{};request.sequence=1;request.trackingGeneration=2;request.session=3;request.reference=4;
    request.input.sequence=5;request.input.session=3;request.input.reference=4;
    request.input.focused=request.input.headValid=1;
    Pose anchor{},head{};head.p.x=.1f;
    HeadVolumeObservation observation{anchor,head,6,2,3,4,1,5,7,8,1000,.12f,.05f,.002f,true,true};
    auto visible=[&] {return headVolumeVisible(observation,anchor,head,6,request,7,8,1050);};
    assert(visible());
    observation.clear=false;assert(!visible());observation.clear=true;
    observation.sampled=false;assert(!visible());observation.sampled=true;
    ++observation.simulationRevision;assert(!visible());--observation.simulationRevision;
    ++observation.rigRevision;assert(!visible());--observation.rigRevision;
    ++request.sequence;assert(!visible());--request.sequence;
    ++request.input.sequence;assert(!visible());--request.input.sequence;
    ++request.trackingGeneration;assert(!visible());--request.trackingGeneration;
    ++request.input.reference;assert(!visible());--request.input.reference;
    request.input.headValid=0;assert(!visible());request.input.headValid=1;
    request.input.focused=0;assert(!visible());request.input.focused=1;
    head.p.x+=.001f;assert(!visible());head=observation.head;
    anchor.p.y=.001f;assert(!visible());anchor=observation.body;
    observation.radius=NAN;assert(!visible());observation.radius=.12f;
    assert(!headVolumeVisible(observation,anchor,head,6,request,7,8,999));
    assert(!headVolumeVisible(observation,anchor,head,6,request,7,8,1101));
    assert(!headVolumeVisible(observation,anchor,head,7,request,7,8,1050));
    assert(visible());

    // The diagnostic must retain admission semantics and first-failure order.
    using R=HeadVolumeVisibilityReason;
    auto reason=[&] {return headVolumeVisibilityReason(observation,anchor,head,6,request,7,8,1050);};
    assert(reason()==R::Clear);
    observation.sampled=false;observation.clear=false;
    assert(reason()==R::Unsampled);observation.sampled=true;
    assert(reason()==R::QueryBlocked);observation.clear=true;
    observation.owner=0;assert(reason()==R::Owner);observation.owner=6;
    request.input.headValid=0;assert(reason()==R::Input);request.input.headValid=1;
    ++observation.session;assert(reason()==R::Epoch);--observation.session;
    ++observation.requestSequence;assert(reason()==R::Sequence);--observation.requestSequence;
    ++observation.simulationRevision;assert(reason()==R::Revision);--observation.simulationRevision;
    observation.tickMs=949;assert(reason()==R::Age);observation.tickMs=1000;
    observation.radius=NAN;assert(reason()==R::Volume);observation.radius=.12f;
    anchor.p.z=.001f;assert(reason()==R::Body);anchor=observation.body;
    head.p.x+=.001f;assert(reason()==R::Head);head=observation.head;
    assert(reason()==R::Clear&&visible());
    observation.owner=0;request.input.headValid=0;
    assert(reason()==R::Owner);observation.owner=6;request.input.headValid=1;
    ++observation.requestSequence;++observation.simulationRevision;
    assert(reason()==R::Sequence);--observation.requestSequence;--observation.simulationRevision;
    observation.tickMs=949;observation.radius=NAN;
    assert(reason()==R::Age);observation.tickMs=1000;observation.radius=.12f;
    anchor.p.z=.001f;head.p.x+=.001f;
    assert(reason()==R::Body);anchor=observation.body;head=observation.head;

    // Independent frozen pre-diagnostic predicate; fuzz combined failures so
    // partitioning cannot accidentally admit an input rejected by the old gate.
    auto originalVisible=[](const HeadVolumeObservation& s,const Pose& b,const Pose& h,
                           uint32_t owner,const Request& r,uint64_t sim,uint64_t rig,uint64_t now) {
        const auto same=[](const Pose& a,const Pose& b) {
            return finite(a)&&finite(b)&&a.q.x==b.q.x&&a.q.y==b.q.y&&a.q.z==b.q.z&&a.q.w==b.q.w&&
                a.p.x==b.p.x&&a.p.y==b.p.y&&a.p.z==b.p.z;
        };
        return s.sampled&&s.clear&&owner&&s.owner==owner&&r.sequence&&r.input.sequence&&
            r.trackingGeneration&&r.session&&r.reference&&r.input.focused&&r.input.headValid&&finite(r.input.head)&&
            s.generation==r.trackingGeneration&&s.session==r.session&&s.reference==r.reference&&
            r.input.session==r.session&&r.input.reference==r.reference&&s.requestSequence==r.sequence&&
            s.inputSequence==r.input.sequence&&s.simulationRevision==sim&&s.rigRevision==rig&&
            now>=s.tickMs&&now-s.tickMs<=100&&std::isfinite(s.radius)&&s.radius>0&&
            std::isfinite(s.nearZ)&&s.nearZ>0&&std::isfinite(s.numericalGuard)&&s.numericalGuard>=.002f&&
            s.numericalGuard<s.radius&&same(s.body,b)&&finite(s.head)&&finite(h)&&
            std::hypot(double(s.head.p.x)-h.p.x,double(s.head.p.y)-h.p.y,double(s.head.p.z)-h.p.z)<=double(s.numericalGuard)*.25;
    };
    std::mt19937 mutations(83017);
    for(unsigned i=0;i<10000;++i) {
        auto s=observation;auto r=request;auto b=anchor;auto h=head;
        const auto bits=mutations();
        if(bits&1)s.sampled=false;if(bits&2)s.clear=false;
        if(bits&4)s.owner=7;if(bits&8)r.input.focused=0;
        if(bits&16)r.reference=0;if(bits&32)r.input.session=9;
        if(bits&64)++s.inputSequence;if(bits&128)++s.rigRevision;
        if(bits&256)s.tickMs=1051;if(bits&512)s.numericalGuard=INFINITY;
        if(bits&1024)b.q.w=0;if(bits&2048)h.p.x+=.001f;
        if(bits&4096)r.input.head.p.x=NAN;
        assert(headVolumeVisible(s,b,h,6,r,7,8,1050)==originalVisible(s,b,h,6,r,7,8,1050));
    }

    std::mt19937 random(15973);
    std::uniform_real_distribution<float> point(-1000,1000),step(-1,1),unit(0,1);
    for(unsigned i=0;i<10000;++i) {
        const Vec3 from{point(random),point(random),point(random)};
        const Vec3 to=from+Vec3{step(random),step(random),step(random)};
        const float radius=.05f+unit(random)*.2f;
        const auto cover=coverHeadVolumeSweep(body.hullCount,from,to,radius,.001f);
        assert(cover.valid);
        const auto sphere=cover.body.hulls[0].sphere[0];
        assert(sphere.radius>=radius&&double(sphere.radius)-radius<=.001);
        assert(sphere.centre.x==from.x&&sphere.centre.y==from.y&&sphere.centre.z==from.z);
        for(unsigned sample=0;sample<=20;++sample) {
            const long double f=static_cast<long double>(sample)/20;
            const long double trueX=from.x+f*(static_cast<long double>(to.x)-from.x);
            const long double trueY=from.y+f*(static_cast<long double>(to.y)-from.y);
            const long double trueZ=from.z+f*(static_cast<long double>(to.z)-from.z);
            const long double time=f*cover.maximumParameter;
            const long double rayX=from.x+time*cover.direction.x;
            const long double rayY=from.y+time*cover.direction.y;
            const long double rayZ=from.z+time*cover.direction.z;
            const long double error=std::hypot(trueX-rayX,trueY-rayY,trueZ-rayZ);
            assert(error+radius<=sphere.radius);
        }
    }
}
