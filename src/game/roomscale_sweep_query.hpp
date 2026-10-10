#pragma once
#include "common/roomscale_body_sweep.hpp"
#include "common/sphere_query_subject.hpp"
#include <windows.h>
namespace ss2vr::game {
using RoomscaleQueryOwnerCurrent=bool(__cdecl *)(void *) noexcept;
// Call only after Engine fingerprint validation; this resolves native exports
// and does not install hooks, start a query, or move anything.
bool configureRoomscaleSweepQueries(HMODULE engine) noexcept;
void resetRoomscaleSweepQueriesAfterQuiescence() noexcept;
// Requires an enclosing owned resource scope and math frame, plus
// enabled triangle/primitive/resource hooks. ownerCurrent must establish phase,
// worker/lifetime/scratch ownership and an inspected normal ray-cleanup list.
// All copied query data must remain current through the complete scope;
// movement callers also retain it through their separately guarded commit.
// rejectInitialContact is for visibility/occupancy probes: even tangential
// initial penetration is unusable. Movement callers retain their contact policy.
// Every sphere must finish unobstructed. Native results are copied before the
// ordinary next rayInit; abnormal native unwind is never "repaired" here.
bool runOwnedSphereQueries(const roomscale::SphereQuerySubject&,const roomscale::BodySweepCover&,
    float contactDepthBudget,DWORD simulationThread,RoomscaleQueryOwnerCurrent ownerCurrent,
    void *ownerContext,bool &failed,bool rejectInitialContact) noexcept;
// Lab diagnostic only: where the last runOwnedSphereQueries call stopped.
// stage 0 not run, 1 entry validation, 2 sphere validation, 3 scope/query,
// 4 final owner/count, 5 clear. hit is the copied native rayIsHit value.
struct SphereQueryOutcome {
    uint32_t stage=0,completed=0;
    int hit=-1;
    bool scopeCompleted=false,scopeFailed=false,ownerLost=false;
};
SphereQueryOutcome lastSphereQueryOutcome() noexcept;
void resetSphereQueryOutcome() noexcept;
// Body-movement adapter retains each actually captured source-hull category.
bool runRoomscaleSweepQueries(const roomscale::BodyGeometry&,const roomscale::BodySweepCover&,
    float contactDepthBudget,DWORD simulationThread,RoomscaleQueryOwnerCurrent ownerCurrent,
    void *ownerContext,bool &failed,bool rejectInitialContact=false) noexcept;
} // namespace ss2vr::game
