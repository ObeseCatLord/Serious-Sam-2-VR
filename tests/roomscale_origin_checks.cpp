#include "common/roomscale_origin.hpp"
#include <cassert>
#include <random>
using namespace ss2vr;
using namespace ss2vr::roomscale;
namespace {
bool close(Vec3 a,Vec3 b,double tolerance) {
    return double(a.x-b.x)*(a.x-b.x)+double(a.y-b.y)*(a.y-b.y)+double(a.z-b.z)*(a.z-b.z)<=tolerance*tolerance;
}
}
int main() {
    std::mt19937 random(3421);std::uniform_real_distribution<float> angle(-Pi,Pi),unit(-1,1);
    for(unsigned i=0;i<3000;++i) {
        Pose origin{yaw(angle(random)),{unit(random),1.7f,unit(random)}};
        Pose head=compose(origin,Pose{yaw(angle(random)),{.2f,unit(random)*.3f,-.1f}});
        Pose anchor{yaw(angle(random)),{unit(random)*20,unit(random)*10,unit(random)*20}};
        const float turn=angle(random);
        auto moved=anchor;moved.p.x+=unit(random)*.03f;moved.p.z+=unit(random)*.03f;
        const auto settled=settleTranslatedAnchor(origin,turn,head,anchor,moved,.1f,.00003f);
        assert(settled.valid);
        assert(close(worldHeadTracking(anchor,origin,turn,head).p,
                     worldHeadTracking(moved,settled.origin,turn,head).p,.00003));
        // The same stage translation preserves both eyes and independently
        // tracked hands; it must never alter physical IPD or hand orientation.
        for(Vec3 offset: {Vec3{-.032f,0,0},Vec3{.032f,0,0},Vec3{-.25f,-.3f,-.4f},Vec3{.25f,-.3f,-.4f}}) {
            const Pose tracked=compose(head,Pose{yaw(.3f),offset});
            const auto first=worldEyeTracking(anchor,origin,turn,head,tracked);
            const auto second=worldEyeTracking(moved,settled.origin,turn,head,tracked);
            assert(close(first.p,second.p,.00003));
            const auto firstHand=worldHandTracking(anchor,origin,turn,head,tracked);
            const auto secondHand=worldHandTracking(moved,settled.origin,turn,head,tracked);
            assert(close(firstHand.p,secondHand.p,.00003));
            assert(first.q.x==second.q.x&&first.q.y==second.q.y&&first.q.z==second.q.z&&first.q.w==second.q.w);
        }
        auto turned=moved;turned.q=multiply(yaw(.01f),turned.q);
        assert(!settleTranslatedAnchor(origin,turn,head,anchor,turned,.1f,.00003f).valid);
        auto tooFar=moved;tooFar.p.x=anchor.p.x+.2f;
        assert(!settleTranslatedAnchor(origin,turn,head,anchor,tooFar,.1f,.00003f).valid);
    }
    // Repeated accepted translations around a closed physical path must not
    // accumulate a visible camera jump from body/origin double application.
    {
        Pose origin{yaw(.7f),{.2f,1.7f,-.3f}};
        const Pose startOrigin=origin;
        Pose anchor{yaw(1.2f),{10,2,20}};
        const float turn=-.4f;
        Pose head=origin;
        const Pose initialWorld=worldHeadTracking(anchor,origin,turn,head);
        Vec3 last{};
        for(unsigned step=1;step<=10000;++step) {
            const float phase=float(step%100)*2*Pi/100;
            const Vec3 tracked{.2f*std::sin(phase),0,.2f*(std::cos(phase)-1)};
            head.p=startOrigin.p+tracked;
            const auto nativeDelta=rotate(anchor.q,rotate(yaw(turn),rotate(inverse(origin.q),tracked-last)));
            Pose moved=anchor;moved.p=moved.p+nativeDelta;
            const auto settled=settleTranslatedAnchor(origin,turn,head,anchor,moved,.1f,.00003f);
            assert(settled.valid);
            origin=settled.origin;anchor=moved;last=tracked;
        }
        assert(close(worldHeadTracking(anchor,origin,turn,head).p,initialWorld.p,.001));
    }
    Pose origin{},head{{0,0,0,1},{HeadHorizontalLimit+.25f,0,0}},before{},after{{0,0,0,1},{.1f,0,0}};
    assert(!settleTranslatedAnchor(origin,0,head,before,after,.2f,.001f).valid);
    head.p.x=.1f;after.p.x=NAN;
    assert(!settleTranslatedAnchor(origin,0,head,before,after,.2f,.001f).valid);
}
