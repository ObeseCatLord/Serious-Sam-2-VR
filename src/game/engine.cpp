#include "eye_replay.hpp"
#include "single_pass.hpp"
#include "common/turret_look.hpp"
#include "common/two_hand.hpp"
#include "common/grenade_throw.hpp"
#include "common/bomb_fuse.hpp"
#include "common/controls.hpp"
#include "common/lab_preparation.hpp"
#include "common/ride_control_observation.hpp"
#include "common/ride_model_join.hpp"
#include "common/ride_grasp.hpp"
#include "common/presentation_identity.hpp"
#include "common/ui.hpp"
#include "common/build_contract.hpp"
#include "common/rig_revision.hpp"
#include "common/native_ray_cleanup.hpp"
#include "common/roomscale_origin.hpp"
#include "common/roomscale_publication.hpp"
#include "roomscale_math_frame.hpp"
#include "roomscale_placement.hpp"
#include "roomscale_sweep_query.hpp"
#include "common/wear_calibration.hpp"
#include "steering_wheel_mesh.hpp"
#include "common/swimming_input.hpp"
#include "common/native_zoom.hpp"
#include "common/native_primary_projection.hpp"
#include "common/native_primary_dispatch.hpp"
#include "common/native_consumption_binding.hpp"
#include "common/physical_gesture.hpp"
#include "common/frame_policy.hpp"
#include "common/head_volume.hpp"
#include "common/head_query_binding.hpp"
#include "common/hook_transaction.hpp"
#include "common/lasers.hpp"
#include "common/muzzle.hpp"
#include "common/attachment_observation.hpp"
#include "common/win_settings.hpp"
#include "common/winproc.hpp"
#include "common/winpath.hpp"
#include "common/weapon_view.hpp"
#include "common/weapon_alignment.hpp"
#include "common/world_markers.hpp"
#include "game.hpp"
#include "menu.hpp"
#include "scope_gpu.hpp"
#include "multiplayer.hpp"
#include "voice.hpp"
#include "spatial_audio.hpp"
#include "hud_wrist.hpp"
#include "common/spatial_audio.hpp"
#include "native_tracking.hpp"
#include "native_memory.hpp"
#include "native_finally.hpp"
#include "x86_predicate_entry.hpp"
#include "remote_render.hpp"
#include "scope_observer.hpp"
#include "idle_observer.hpp"
#include "scope_views.hpp"
#include "common/scope_position_program.hpp"
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
extern "C" const ss2vr::BuildContract ss2vrBuildContract;
namespace ss2vr::game {
namespace spatial_audio_common = ::ss2vr::spatial_audio;
std::atomic<bool> hooksReady = false;
using VoidThis = void(__thiscall *)(void *);
using IntThis = int(__thiscall *)(void *);
using HandInt = int(__thiscall *)(void *, int);
using HandlePointer = void *(__cdecl *)(uint32_t);
using PointerHandle = uint32_t(__cdecl *)(void *);
using PoseGet = Pose *(__thiscall *)(void *, Pose *);
using ViewOrigin = Pose *(__thiscall *)(void *, Pose *, int);
using ProjGet = Matrix44 *(__thiscall *)(void *, Matrix44 *);
using MatrixPose = Pose *(__cdecl *)(Pose *, const Matrix34 &);
using SetWeapon = void(__thiscall *)(void *, int, int, int);
using Toggle = void(__thiscall *)(void *, int);
using InventoryInt = int(__thiscall *)(void *, int);
using Frustum = Matrix44 *(__cdecl *)(Matrix44 *, float, float, float, float);
using WeaponAbs = int(__thiscall *)(void *, const Matrix34 &, Matrix34 &);
using WeaponCharge = Matrix34 *(__thiscall *)(void *, Matrix34 *);
using RenderWeapon = void(__thiscall *)(void *, Matrix34);
using NativeFire = int(__thiscall *)(void *, float);
static NativeFire originalNativeFire = nullptr;
static VrSettings settings;
static std::atomic<uint32_t> fireFeedback[2]{}, damageFeedback{0};
struct Box1 {
    float min, max;
};
using ViewPrepare = void(__thiscall *)(void *, const Matrix34 &, const Matrix44 &, const Box1 &, uint32_t);
static ViewPrepare originalViewPrepare = nullptr;
static uintptr_t rootPrepareReturn = 0;
static uintptr_t rootProjectionReturn = 0, nativeFrustumReturn = 0, playerFovGetter = 0;
static PoseGet originalCamera = nullptr, originalShot = nullptr, originalSniperShot = nullptr;
static ViewOrigin baseViewOrigin = nullptr;
static PoseGet nativeBodyPlacement = nullptr;
using TeleportFn = int(__thiscall *)(void *, Pose);
static TeleportFn nativeTeleport = nullptr; // Lab only (SS2VR_LAB_TELEPORT).
static const uint32_t *invalidSeatIdent = nullptr;
static ProjGet originalProjection = nullptr;
static VoidThis originalRender = nullptr, originalStep = nullptr, originalDelete = nullptr,
                originalWeaponDelete = nullptr, originalSniperDelete = nullptr;
static VoidThis originalSimulationStep = nullptr, originalEntityStep = nullptr;
static void *(__cdecl *currentSimulation)() = nullptr, *(__cdecl *currentWorld)() = nullptr;
static VoidThis originalViewExecute = nullptr;
using RayInit = void(__cdecl *)();
using RayFloat = void(__cdecl *)(float);
struct NativeRay {
    Vec3 origin, direction;
};
static RayInit originalRayInit = nullptr;
static void(__cdecl *setRay)(const NativeRay &) = nullptr;
static RayFloat rayMaximum = nullptr, rayMinimum = nullptr, rayRadius = nullptr;
static int(__cdecl *checkRay)() = nullptr, (__cdecl * rayHit)() = nullptr;
static int(__cdecl *originalCheckRay)() = nullptr, (__cdecl *originalContinueRay)() = nullptr;
using ModelCheckRay = int(__cdecl *)(void *, const Matrix34 &, int);
static ModelCheckRay originalModelCheckRay = nullptr;
struct NativeRayFrame { const NativeRayFrame *previous; };
static thread_local const NativeRayFrame *activeNativeRay = nullptr;
static thread_local bool laserQuerying = false;
static std::atomic<bool> nativeRayFaulted{false};
static uintptr_t roomscaleEngineBase=0,roomscaleSimulationReturn=0;
static uintptr_t roomscaleWorldInfoTable=0,roomscaleBrainTable=0;
static bool roomscaleConfigured=false;
static thread_local bool roomscaleBusy=false;
static thread_local uint64_t simulationRevision=0;
static std::atomic<bool> roomscaleFaulted{false};
// Observe native query extents without serializing or changing their results.
// This records same-thread nesting only; worker ownership is a separate gate.
template<class Body> static void observeNativeRay(Body &&body) noexcept {
    NativeRayFrame frame{activeNativeRay};
    const bool priorLaserQuerying = laserQuerying;
    withNativeFinally([&] {
        activeNativeRay = &frame;
        body();
    }, [&](bool aborted) noexcept {
        activeNativeRay = frame.previous;
        laserQuerying = priorLaserQuerying;
        if (aborted) {
            nativeRayFaulted.store(true, std::memory_order_release);
            nativeInputFailed();
        }
    });
}
static int __cdecl observedCheckRay() {
    int result = 0;
    observeNativeRay([&] { result = originalCheckRay(); });
    return result;
}
static int __cdecl observedContinueRay() {
    int result = 0;
    observeNativeRay([&] { result = originalContinueRay(); });
    return result;
}
static int __cdecl observedModelCheckRay(void *model, const Matrix34 &placement, int mode) {
    int result = 0;
    observeNativeRay([&] { result = originalModelCheckRay(model, placement, mode); });
    return result;
}

static float(__cdecl *hitDistance)() = nullptr;
static void(__cdecl *rayCategory)(uint32_t) = nullptr, (__cdecl * thickCategory)(uint32_t) = nullptr;
static void(__cdecl *rayAvatar)(void *) = nullptr, (__cdecl * rayMechanism)(void *) = nullptr;
static void(__cdecl *rayFluids)(int) = nullptr;
static void *(__thiscall *getMechanism)(void *) = nullptr;
static void(__cdecl *drawLine)(const Vec3 &, const Vec3 &, uint32_t, uint32_t) = nullptr;
static RayInit *enableDepth = nullptr, *disableDepth = nullptr, *enableDepthWrite = nullptr,
               *disableDepthWrite = nullptr;
static int *depthEnabled = nullptr, *depthWriting = nullptr;
static RayInit *enableBlend = nullptr, *disableBlend = nullptr, *enableAlpha = nullptr,
               *disableAlpha = nullptr;
static int *blending = nullptr, *alphaTesting = nullptr, *depthComparison = nullptr;
static RayInit *enableFog = nullptr, *disableFog = nullptr;
static int *fogging = nullptr;
using RasterComparison = void(__cdecl *)(int);
static RasterComparison *setDepthComparison = nullptr;
static RayInit ortho = nullptr;
static VoidThis activateView = nullptr;
// Native resource/gfx entries for the virtual steering wheel (pinned exports).
using ResourceObtain = void *(__cdecl *)(void **, void *, int, int, int, const char *, const char *);
static ResourceObtain resourceObtain = nullptr;
static void **textureDataType = nullptr;
static void(__cdecl *currentTexture)(int, void *, int) = nullptr;
static void(__cdecl *useSimpleShader)(int) = nullptr;
static void(__cdecl *constantColor)(uint32_t) = nullptr;
using GfxBindArray = void(__cdecl *)(int, const void *, int, int, int);
using GfxDrawVertices = void(__cdecl *)(int, int, int);
static RayInit *unbindVertexBuffers = nullptr;
using SmartReference = void(__thiscall *)(void *, const void *);
using SmartRelease = int32_t(__thiscall *)(void *, const void *);
static SmartReference smartAddRef = nullptr;
static SmartRelease smartRemRef = nullptr;
static uintptr_t staticTextureVtable = 0;
static GfxBindArray *bindVertexArray = nullptr;
static GfxDrawVertices *drawVertices = nullptr;
static void **currentCanvas = nullptr;
static uintptr_t allowedRayReturn = 0;
static uintptr_t laserEngineBase = 0, laserTurretVtable = 0, turretShootDirection = 0;
using ModelInstanceGet = void *(__thiscall *)(void *);
using AttachmentGet = int(__cdecl *)(void *, uint32_t, Matrix34 &);
using AttachmentIdent = uint32_t *(__thiscall *)(void *, uint32_t *);
using ShootDirection = Vec3 *(__thiscall *)(void *, Vec3 *);
static ModelInstanceGet laserModelInstance = nullptr;
static AttachmentGet laserAttachment = nullptr;
static AttachmentGet originalObservedAttachment=nullptr;
static uintptr_t nativeShotAttachmentReturn=0;
static thread_local AttachmentObservation *activeAttachmentObservation=nullptr;
// No native evaluation is added. Every invocation forwards once and returns the
// untouched native result/output. Only a successful, exact existing-call return
// can copy 48 bytes; an unwind is retired by the enclosing shooting finally.
static int __cdecl observedAttachment(void *model,uint32_t ident,Matrix34 &out) {
#ifdef _MSC_VER
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    auto *receipt=caller==nativeShotAttachmentReturn?activeAttachmentObservation:nullptr;
    if(receipt)receipt->begin(reinterpret_cast<uintptr_t>(model),ident);
    const int result=originalObservedAttachment(model,ident,out);
    if(receipt)receipt->complete(result,out);
    return result;
}
static uint32_t laserIdleAttachment = 0;
static uint32_t bulletCategory = 0,headQueryCategory = 0;
static VoidThis originalOperatorFiring = nullptr;
static HandInt originalFireButtonPressed = nullptr;
using CanonicalPrimary = void(__thiscall *)(void *, int32_t);
using WeaponHeld = int(__thiscall *)(void *, uint32_t);
using WeaponCopy = void *(__thiscall *)(void *, const void *);
using WeaponPutDown = void(__thiscall *)(void *, int);
static CanonicalPrimary originalPrimaryPress = nullptr, originalPrimaryRelease = nullptr;
static WeaponHeld originalWeaponHeld = nullptr;
static WeaponCopy originalSawCopy = nullptr, originalSawAssign = nullptr;
static WeaponPutDown originalSawPutDown = nullptr;
static VoidThis originalSawDelete = nullptr, originalSawWeaponPutDown = nullptr;
static uintptr_t sawVtable = 0, sawReleaseReturn = 0, weaponHeldReturn = 0,
                 baseHeldReturn[2]{}, operatorPrimaryReturn[2]{};
static bool meleeConfigured = false;
using WeaponButton = int32_t(__thiscall *)(void *, uint32_t);
using GameInfoGet = uint32_t *(__cdecl *)(uint32_t *);
static WeaponButton nativeWeaponButton = nullptr;
static IntThis nativeFlipButtons = nullptr, nativeComboWeapons = nullptr;
static GameInfoGet nativeGameInfo = nullptr;
// Explicit private-fixture preparation only; no production inventory behavior.
using LabPlayerGet = uint32_t *(__thiscall *)(void *,uint32_t *,int);
using LabSelect = void(__thiscall *)(void *,int,int,int,int);
static IntThis labCheatsEnabled=nullptr;
static VoidThis labGiveAll=nullptr;
static LabSelect labSelectSniper=nullptr;
static void(__cdecl *labSaveSniper)(const char *)=nullptr;
// Lab (SS2VR_LAB_LOAD=1): the native load of a saved game (Sam2Game 0x26980, the
// load menu's call), of Temp/SS2VR/user.sav once Temp/SS2VR/load-request exists.
static void(__cdecl *labLoadGame)(const char *)=nullptr;
static std::atomic<bool> labLoadPending{false}, labLoadIssued{false};
static uintptr_t labBrainGetter=0,labPuppetGetter=0;
static bool labSniperConfigured=false;
static const LabPreparationTarget *labPreparation=nullptr;
static uintptr_t labAutoShotgunVtable=0;
using LabSniperStage=LabPreparationStage;
static struct : LabPreparationProgress {
    uint64_t deadline=0;
    void *simulation=nullptr,*world=nullptr;
    uint32_t player=0,brain=0,game=0;
    char savePath[1024]{};
} labSniper;
static uintptr_t primaryOperatorReturn = 0, primaryHeldReturn = 0;
static IntThis originalSniperAlternativePress = nullptr;
static int(__cdecl *nativeMainThread)() = nullptr;
bool nativePresentationThreadCurrent() noexcept {
    return hooksReady.load(std::memory_order_acquire) && nativeMainThread && nativeMainThread();
}
static uintptr_t playerAlternativePressReturn = 0;
static WeaponAbs originalWeaponAbs = nullptr;
static WeaponCharge originalWeaponCharge = nullptr;
static uintptr_t weaponChargeReturn = 0;
struct NativePlacementCharge {
    void *weapon = nullptr;
    Vec3 translation;
    unsigned calls = 0;
    bool valid = false, invalidated = false;
};
static thread_local NativePlacementCharge *placementCharge = nullptr;
static Matrix34 *__fastcall weaponCharge(void *weapon, void *, Matrix34 *out) {
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    Matrix34 *result = originalWeaponCharge(weapon, out);
    auto *observation = placementCharge;
    if (!observation || observation->weapon != weapon || caller != weaponChargeReturn)
        return result;
    if (observation->calls != UINT_MAX) ++observation->calls;
    // The audited base displacement is translation-only. Other native getter
    // implementations are not inferred from a finite-looking matrix.
    const bool valid = out && result == out && finiteMatrix(*out) &&
        out->m[0] == 1 && out->m[1] == 0 && out->m[2] == 0 &&
        out->m[4] == 0 && out->m[5] == 1 && out->m[6] == 0 &&
        out->m[8] == 0 && out->m[9] == 0 && out->m[10] == 1;
    if (observation->calls == 1 && !observation->invalidated && valid) {
        observation->translation = {out->m[3], out->m[7], out->m[11]};
        observation->valid = true;
    } else {
        observation->invalidated = true;
        observation->valid = false;
    }
    return result;
}
static int nativePlacementWithCharge(void *weapon, const Matrix34 &view, Matrix34 &out,
                                     Vec3 &translation, bool &invalid) {
    translation = {};
    auto *previous = placementCharge;
    if (previous) previous->invalidated = true;
    NativePlacementCharge observation{weapon, {}, 0, false, previous != nullptr};
    placementCharge = &observation;
    int result = 0;
    withNativeFinally([&] { result = originalWeaponAbs(weapon, view, out); },
                      [&](bool aborted) noexcept {
        placementCharge = previous;
        if (aborted) {
            observation.invalidated = true;
            if (previous) previous->invalidated = true;
            nativeInputFailed();
        }
    });
    invalid = observation.invalidated || (observation.calls && !observation.valid);
    if (observation.valid && !invalid) translation = observation.translation;
    // A different virtual getter not reaching the audited base hook retains
    // the prior placement policy; no residual is invented for it.
    return result;
}
static RenderWeapon originalWeaponRender = nullptr, originalSniperRender = nullptr;
static Frustum originalFrustum = nullptr;
using MatrixInverse = Matrix34 *(__cdecl *)(Matrix34 *, const Matrix34 &);
using DepthRange = void(__cdecl *)(float, float);
using ProjectionSet = void(__cdecl *)(const Matrix44 &);
static MatrixInverse originalMatrixInverse = nullptr;
static DepthRange originalDepthRange = nullptr, *nativeDepthRange = nullptr;
static ProjectionSet *nativeProjectionSet = nullptr;
static Matrix44 *nativeCurrentProjection = nullptr, *nativeAdjustedProjection = nullptr;
static Matrix34 *nativeCurrentView = nullptr;
static float *nativeDepthNear = nullptr, *nativeDepthFar = nullptr;
static uint32_t *nativeCachedMatrices = nullptr;
static uintptr_t weaponFrustumReturn = 0, weaponInverseReturn = 0, weaponDepthReturn = 0,
                 weaponPlacementReturn = 0, weaponRestoreReturn = 0, rootDepthReturn = 0,
                 ordinaryWeaponRenderReturn = 0;
static DepthRange expectedDepthRange = nullptr;
static ProjectionSet expectedProjectionSet = nullptr;
static HandlePointer resolve = nullptr;
// Seated in a turret: the right stick aims it as the mouse does on the
// desktop, instead of turning the view. Turrets are CTurretPuppetEntity,
// CTurretCharacterEntity and CAutoTurretPuppetEntity; they share the turret
// GetShootDirection (vtable+0x5A4), which no other ride class uses. Matching
// only the first class left the others on hand aim, which turned the turret
// and with it the frame the hand is measured in: it spun on by itself.
static bool seatedInTurret(const RiderIdentity &rider) {
    if (!rider.seated() || !turretShootDirection) return false;
    void *ride = resolve(rider.ride);
    uint32_t table = 0, shoot = 0;
    if (!ride || !readableMemory(ride, 4)) return false;
    std::memcpy(&table, ride, 4);
    const auto *slot = reinterpret_cast<const void *>(uintptr_t(table) + 0x5a4);
    return table && readableMemory(slot, 4) && (std::memcpy(&shoot, slot, 4), shoot == turretShootDirection);
}
static PointerHandle pointerHandle = nullptr;
static IntThis isLocal = nullptr, isDual = nullptr, isNetricsa = nullptr, health = nullptr, armor = nullptr;
static IntThis isAlive = nullptr, thirdPerson = nullptr;
static IntThis originalThirdPerson = nullptr;
static uintptr_t mountedAvatarReturn = 0, mountedClampReturn = 0;
static uintptr_t markerParentReturn = 0;
static int32_t *uiVertexHandle = nullptr, *uiPixelHandle = nullptr, *uiSimplePrograms = nullptr;
static uint32_t *uiPrograms = nullptr;
using MarkerDraw = void(__thiscall *)(void *, const Matrix34 &, const Matrix44 &,
                                     const WorldMarkerDimensions &, float);
using MarkerFade = float(__thiscall *)(void *);
static MarkerDraw nativeNavigation = nullptr, nativeObjectives = nullptr;
static void *(__cdecl *nativeDrawPort)() = nullptr, *(__cdecl *nativeWorldInfo)() = nullptr;
static void(__cdecl *nativeBlendType)(int) = nullptr;
static int(__cdecl *singlePlayer)() = nullptr;
static SetWeapon setWeapon = nullptr;
static Toggle toggleDual = nullptr;
static HandInt canChange = nullptr;
static InventoryInt inInventory = nullptr, ammo = nullptr;
// Hand weapon wheel presentation (optional pinned exports): Sam2Game weapon
// icons and native weapon-menu order, plus the gfx blend state it restores.
using WeaponIcon = void *(__cdecl *)(int);
using WeaponMenuSlot = int32_t(__cdecl *)(int);
using BlendFunction = void(__cdecl *)(int, int);
static WeaponIcon nativeWeaponIcon = nullptr;
static WeaponMenuSlot nativeWeaponMenuDirection = nullptr, nativeWeaponMenuOrder = nullptr;
static int *blendSource = nullptr, *blendDestination = nullptr;
static BlendFunction *setBlendFunction = nullptr;
// Native model entries for the wheel's floating weapon meshes (optional pinned
// Engine exports; CBaseWeaponEntity::Render draws its own model the same way).
using ModelInstanceCreate = void *(__cdecl *)();
using ModelInstanceDelete = void(__cdecl *)(void *);
using ModelInstanceConfigure = void(__cdecl *)(void *, void *);
using ModelInstanceStretch = void(__cdecl *)(void *, const Vec3 &);
using ModelInstanceBox = WheelBox *(__cdecl *)(WheelBox *, const void *);
using ModelInstanceRender = void(__cdecl *)(void *, const Matrix34 &, float, int);
// Render-mesh bounds, read as mdlModelConfigurationRecalculateBoundingBox
// (RVA D3640) forms them, without storing anything into the shared resource.
using ModelMeshCount = int32_t(__cdecl *)(void *);
using ModelMesh = void *(__cdecl *)(void *, int32_t);
using MeshRenderMesh = void *(__cdecl *)(void *);
using RenderMeshBox = WheelBox *(__cdecl *)(WheelBox *, const void *);
static ModelInstanceCreate createModelInstance = nullptr;
static ModelInstanceDelete deleteModelInstance = nullptr;
static ModelInstanceConfigure configureModelInstance = nullptr;
static ModelInstanceStretch stretchModelInstance = nullptr;
static ModelInstanceBox modelInstanceBox = nullptr;
static ModelInstanceRender renderModelInstance = nullptr;
static ModelMeshCount modelMeshCount = nullptr, renderMeshLodCount = nullptr;
static ModelMesh modelMesh = nullptr;
static MeshRenderMesh meshRenderMesh = nullptr;
static RenderMeshBox renderMeshBox = nullptr;
static void **modelDataType = nullptr;
static uintptr_t modelConfigurationVtable = 0;
static MatrixPose matrixPose = nullptr;
static SRWLOCK snapshotLock = SRWLOCK_INIT;
struct Calibration {
    bool valid = false;
    uint32_t handle = 0;
    Pose nativeModelLocal;
    uint64_t tickMs = 0;
    Vec3 nativeDisplacement;
    Vec3 nativeAlignment;
    WeaponAlignmentBinding modelBinding;
    bool alignmentApplied = false;
    // Private copied provenance only; never used as a production admission gate.
    uint64_t labSerial=0,labRequest=0,labInput=0;
    int labEye=-1;
    Matrix34 labNativeModel{};
};
static uint64_t labCalibrationSerial=0; // Publication is under snapshotLock.
thread_local int eyeIndex = -1;
static thread_local unsigned nativeWeaponRenderDepth=0;
static thread_local unsigned nativeShotDepth = 0;
static uint32_t weaponConfigurationVtable=0;
static thread_local const WeaponRenderRoute *weaponRenderRoute=nullptr;
static uint32_t primaryField(const void *object,unsigned offset) noexcept;
static RigRevision rigPublication;
static thread_local unsigned snapshotNativeReadDepth=0;
static thread_local bool rigMutationActive=false;
static WearCalibration wearCalibration; // Main thread: snapshot updates only.
struct Snapshot {
    void *player = nullptr;
    RiderIdentity rider;
    uint32_t handle[2]{};
    uint32_t intentEpoch[2]{}; // Captured client admission; not a restamped input.
    uint32_t playerHandle = 0;
    Input input;
    InputSampleBoundary interruption;
    uint32_t inputProducer = 0;
    uint32_t gripPoseGeneration[2]{};
    WorldSubmission submittedWorld;
    Pose origin;
    uint64_t rigRevision = 0;
    float turn = 0;
    bool initialized = false, fire[2]{}, use = false, jump = false, sprint = false, grenade = false;
    bool selecting[2]{}, zoom[2]{};
    uint32_t generation = 0, networkGeneration = 0;
    Ui ui;
};
// Stationary input-consumption ownership lives beside the copied pose payload.
// Snapshot publication/reset cannot copy or discard an accounted native level.
struct MeleeHandRecord {
    NativeConsumptionBinding consumption;
    NativeConsumptionBindingKey key;
    void *player = nullptr, *weapon = nullptr, *world = nullptr;
    RiderIdentity rider;
    uint64_t epoch = 0;
    unsigned teardownDepth = 0;
    bool blocked = false, resetGesture = false;
    bool gestureArmed = false;
    uint32_t gestureGeneration = 0;
    uint64_t gestureSequence = 0, quietAfter = 0;
};
struct LocalMeleeHand : MeleeHandRecord {
    PhysicalGestureInput gesture; // Physical sampling remains local-only.
    void *gesturePlayer = nullptr, *gestureWeapon = nullptr;
    bool gestureInvalidated = false;
    uint32_t quietGeneration = 0;
    uint64_t quietSequence = 0, quietTickMs = 0;
};
struct LocalSnapshotOwner {
    Snapshot payload;
    std::array<LocalMeleeHand, 2> melee;
    uint64_t nextMeleeEpoch = 1; // Zero means exhausted, never wraps to a live epoch.
};
static LocalSnapshotOwner localSnapshotOwner;
static Snapshot &current = localSnapshotOwner.payload;
static void retireAuthorityMelee(void *) noexcept;
static bool authorityMeleeReleaseObligation(void *, uint32_t) noexcept;
static void retireLocalMelee(void *entity) noexcept {
    // Native lifetime entry owns the incoming receiver. Only adapter metadata
    // is touched, before any resource/native callback can replace that receiver.
    AcquireSRWLockExclusive(&snapshotLock);
    for (auto &hand : localSnapshotOwner.melee)
    {
        if (entity && (hand.gesturePlayer == entity || hand.gestureWeapon == entity))
            hand.gestureInvalidated = true; // Normal sampling resets motion, not native cleanup.
        if (entity && (hand.player == entity || hand.weapon == entity)) {
            hand.consumption.retire();
            hand.blocked = true;
            hand.epoch = 0;
            hand.player = hand.weapon = hand.world = nullptr;
        }
    }
    ReleaseSRWLockExclusive(&snapshotLock);
    retireAuthorityMelee(entity);
}
static bool meleeReleaseObligation(void *subject, uint32_t player) noexcept {
    if (!meleeConfigured || !settings.physicalMelee || !subject || !player) return false;
    bool result = false;
    AcquireSRWLockShared(&snapshotLock);
    for (const auto &hand : localSnapshotOwner.melee)
        if (hand.key.player == player && hand.epoch && hand.player == subject &&
            hand.consumption.edge(hand.key, false) == NativeConsumptionBinding::Edge::Release) {
            result = true;
            break;
        }
    ReleaseSRWLockShared(&snapshotLock);
    return result || authorityMeleeReleaseObligation(subject, player);
}
static std::atomic<DWORD> simulationThread{0};
static LaserAim laserAim[2];
static HeadVolumeObservation headVolumeObservation;
// Mod-owned diagnostic values, published with the observation under laserLock.
// No pointers, extra native reads or alternative clearance policy.
enum class HeadQueryStage : uint32_t {
    NotEntered, Phase, Cleanup, Binding, RelativePose, Translation, NearPose,
    EyePoseFov, Guard, Radius, Cover, Owner, ResourceQuery, Complete
};
struct HeadQueryDiagnostic {
    HeadQueryStage stage=HeadQueryStage::NotEntered;
    double computedRadius=0,computedGuard=0;
    uint32_t categoryCount=0;
    bool prepared=false,clear=false,failed=false,mathReturned=false,coverValid=false;
    SphereQueryOutcome sphere;
    RoomscaleScopeFailure kernel;
    uint32_t ownerFailure=0,phaseFailure=0;
    RoomscaleResourceCancel cancel;
};
static HeadQueryDiagnostic headQueryDiagnostic;
static std::atomic<unsigned> headDiagnosticReceipts{0};
static HeadClearance eyeHeadClearance;
static Pose protectedWorldHead;
static float protectedWorldRadius=0;
static bool headVolumeDrawSafe=true;
static std::atomic<uint32_t> headNearBits{std::bit_cast<uint32_t>(.05f)};
static uint32_t pairHeadNearBits=0;
static SRWLOCK laserLock = SRWLOCK_INIT;
static TrackingEpochs generations, networkGenerations;
// Called while holding snapshotLock; publish invalidation before native state changes.
static uint32_t advanceEpochUnlocked() {
    uint32_t epoch = generations.advance();
    if (channel.shared)
        publishTrackingEpoch(*channel.shared, epoch ? epoch : ExhaustedEpoch);
    return epoch;
}
static void advanceGeneration(Snapshot &s) {
    AcquireSRWLockExclusive(&snapshotLock);
    s.generation = advanceEpochUnlocked();
    ReleaseSRWLockExclusive(&snapshotLock);
}
static void recoverRigForNewOrigin(Snapshot &s) {
    AcquireSRWLockExclusive(&snapshotLock);
    s.rigRevision = rigPublication.recoverForNewOrigin();
    ReleaseSRWLockExclusive(&snapshotLock);
}
static Calibration calibration[2];
static void clearCalibration(int h) {
    AcquireSRWLockExclusive(&snapshotLock);
    calibration[h] = {};
    ReleaseSRWLockExclusive(&snapshotLock);
}
static WeaponWheel wheels[2];
static GripJump gripJumps[2]; // Game thread only.
// Game-local presentation of each open wheel; WheelUi is its fixed IPC subset.
// Kept beside the copied Snapshot rather than growing every snapshot copy.
struct WheelPresentation {
    WheelRing ring;
    int32_t hoverSlot = -1;
    bool open = false;
};
static SRWLOCK wheelPresentationLock = SRWLOCK_INIT;
static WheelPresentation wheelPresentation[2];
static void publishWheelPresentation(unsigned hand, const WeaponWheel &wheel) {
    AcquireSRWLockExclusive(&wheelPresentationLock);
    auto &out = wheelPresentation[hand];
    out.open = wheel.open && wheel.ring.anchored;
    if (out.open) {
        out.ring = wheel.ring;
        out.hoverSlot = wheel.hoverSlot;
    }
    ReleaseSRWLockExclusive(&wheelPresentationLock);
}
// Bloom (ProcRender efxBloom, called from CEffectRenCmd::Execute) draws a
// view-covering glow quad about 0.4 m in front of the camera; two-pass renders
// draw it in front of each eye's own camera. The single pass draws it
// screen-locked: reprojected per eye, the eye offset at that distance left one
// eye's glow ending in a straight edge inside the view (lab, V341-V375).
using BloomRender = void(__thiscall *)(void *, void *);
static BloomRender originalBloomRender = nullptr;
static void __fastcall bloomRenderEffect(void *bloom, void *, void *command) {
    // Unlocked even if native code unwinds through the bloom render (a stuck
    // lock flattened every later single-pass draw).
    single_pass::setScreenLocked(true);
    withNativeFinally([&] { originalBloomRender(bloom, command); },
                      [](bool) noexcept { single_pass::setScreenLocked(false); });
}
// The host's VR options panel saves SS2VR.user.ini while the game runs: the
// settings that are safe to change in play are re-applied when it changes.
static std::wstring settingsFile;
static FILETIME settingsSeen{};
static bool settingsWritten(FILETIME &time) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    time = {};
    if (settingsFile.empty() || !GetFileAttributesExW(userSettingsFile(settingsFile).c_str(), GetFileExInfoStandard, &data))
        return true; // No user file yet: an empty time, so its creation counts as a change.
    time = data.ftLastWriteTime;
    return true;
}
static void pollSettingsFile() {
    static ULONGLONG next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 500;
    FILETIME written{};
    if (!settingsWritten(written) || !CompareFileTime(&written, &settingsSeen)) return;
    settingsSeen = written;
    const VrSettings fresh = loadSettings(settingsFile);
    settings.lasers = fresh.lasers;
    settings.laserDistance = fresh.laserDistance;
    settings.snapTurn = fresh.snapTurn;
    settings.smoothTurnRate = fresh.smoothTurnRate;
    settings.singlePassInstancing = fresh.singlePassInstancing;
    settings.voice = fresh.voice;
    single_pass::setInstancing(fresh.singlePassInstancing);
    log("Settings reloaded: lasers=%d snapTurn=%d turnRate=%.0f instancing=%d", int(fresh.lasers), int(fresh.snapTurn),
        double(fresh.smoothTurnRate * 180 / Pi), int(fresh.singlePassInstancing));
}
// One-shot actions chosen on a wheel bubble, by WheelBubble value. Simulation
// stores the release tick; the player input poll (or the presentation, for the
// game menu) consumes it once. Stale requests expire instead of firing late.
static std::atomic<uint64_t> wheelRequests[WheelBubbleCount]{};
static void requestWheelAction(WheelBubble action) {
    static const char *const names[WheelBubbleCount] = {"Netricsa", "game menu", "quick save", "quick load", "VR options"};
    if (action == WheelBubble::None) return;
    wheelRequests[uint32_t(action)].store(GetTickCount64(), std::memory_order_relaxed);
    log("Weapon wheel requested %s", names[uint32_t(action)]);
}
static bool takeWheelRequest(WheelBubble action, uint64_t now) {
    if (action == WheelBubble::None) return false;
    const uint64_t tick = wheelRequests[uint32_t(action)].exchange(0, std::memory_order_relaxed);
    return tick && now >= tick && now - tick < 500;
}
bool wheelMenuRequested(uint64_t now) noexcept {
    return takeWheelRequest(WheelBubble::Menu, now);
}
// Netricsa and the game menu are always offered; native quick save/load only in
// single player, where the game owns the save.
static uint32_t wheelBubbles() {
    uint32_t mask = wheelBubbleBit(WheelBubble::Netricsa) | wheelBubbleBit(WheelBubble::Menu) | wheelBubbleBit(WheelBubble::VrOptions);
    if (singlePlayer && singlePlayer())
        mask |= wheelBubbleBit(WheelBubble::QuickSave) | wheelBubbleBit(WheelBubble::QuickLoad);
    return mask;
}
// Native weapon-menu direction/order (samGetWeaponsMenuDir/Order) groups related
// weapons around the ring; id order remains when those optional exports are absent.
static void orderWheelWeapons(WheelUi &ui) {
    if (!nativeWeaponMenuDirection || !nativeWeaponMenuOrder) return;
    const uint32_t count = std::min(ui.count, WeaponCount);
    int32_t key[WeaponCount];
    for (uint32_t i = 0; i != count; ++i) {
        const int32_t direction = nativeWeaponMenuDirection(ui.weapon[i]), order = nativeWeaponMenuOrder(ui.weapon[i]);
        key[i] = direction < 0 || order < 0 ? INT32_MAX : direction * 64 + order * 32 + ui.weapon[i];
    }
    for (uint32_t i = 1; i < count; ++i)
        for (uint32_t j = i; j && key[j] < key[j - 1]; --j) {
            std::swap(key[j], key[j - 1]);
            std::swap(ui.weapon[j], ui.weapon[j - 1]);
            std::swap(ui.ammo[j], ui.ammo[j - 1]);
        }
}
static TriggerGate gates[2];
static HeldZoomInput zoomInputs[2];
static uint32_t lastButtons[2]{};
static const char *lastWeaponSelection = nullptr; // Weapon-change trace provenance.
static bool turnLatched = false;
static int pendingSelection[2]{-1, -1};
static uint64_t selectionTick[2]{}, physicalSequence = 0;
static bool physicalDown[2]{};
static uint32_t releasedSerial[2]{};
static uint32_t zoomReleasedSerial[2]{}, primaryInputGeneration[2]{};
static void *sniperVtable = nullptr;
static uintptr_t sniperBaseStepReturn = 0, sniperCrossDeleteReturn = 0;
static VoidThis originalSniperStep = nullptr, originalBaseWeaponStep = nullptr;
static VoidThis originalZoomActivate = nullptr, originalZoomDeactivate = nullptr;
static IntThis nativeZoomFlag = nullptr, nativeBaseAlternativePress = nullptr, nativeOwnWeapons = nullptr;
static VoidThis nativeBaseAlternativeRelease = nullptr;
static NativeFire originalSniperFire = nullptr;
static Toggle originalSniperPutDown = nullptr;
struct ZoomManagerFrame {
    void *manager=nullptr,*simulation=nullptr,*world=nullptr;
    ZoomManagerFrame *previous=nullptr;
    bool current=false,preparation=false,execution=false;
    uint32_t frozenOwner=0;
};
static thread_local ZoomManagerFrame *zoomManagerFrame=nullptr;
static bool nativeSniper(void *weapon) {
    return weapon && *reinterpret_cast<void **>(weapon) == sniperVtable;
}

static network::PosePacket lastNetworkPose;

static Snapshot copySnapshot() {
    AcquireSRWLockShared(&snapshotLock);
    auto s = current;
    ReleaseSRWLockShared(&snapshotLock);
    return s;
}
// Private lab only: bounded value receipts, never new native queries or cached
// pointer dereferences. Per-stage caps keep noisy admission from hiding drawing.
enum class LaserTraceStage : unsigned { Cache, Muzzle, Query, Sample, Freeze, Draw, Count };
static bool laserTraceTicket(LaserTraceStage stage, uint64_t &order) {
    static const bool enabled=[] { wchar_t value[2]{};
        return GetEnvironmentVariableW(L"SS2VR_LAB_TRACE",value,2)==1 && value[0]==L'1'; }();
    if(!enabled)return false;
    static std::atomic<uint64_t> counts[unsigned(LaserTraceStage::Count)]{};
    static std::atomic<uint64_t> sequence{0};
    constexpr uint64_t cap=1024;
    const auto index=unsigned(stage);
    const auto count=counts[index].fetch_add(1,std::memory_order_relaxed);
    order=sequence.fetch_add(1,std::memory_order_relaxed);
    if(count==cap)log("Lab laser saturation schema=1 stage=%u cap=%llu dropped=1",index,cap);
    // Counts include suppressed events. Other stages report the latest totals.
    if(count>=cap)return false;
    uint64_t dropped[unsigned(LaserTraceStage::Count)]{};
    for(unsigned i=0;i<unsigned(LaserTraceStage::Count);++i) {
        const auto n=counts[i].load(std::memory_order_relaxed);
        dropped[i]=n>cap?n-cap:0;
    }
    log("Lab laser budget schema=1 order=%llu stage=%u dropped=%llu,%llu,%llu,%llu,%llu,%llu",
        order,index,dropped[0],dropped[1],dropped[2],dropped[3],dropped[4],dropped[5]);
    return true;
}
static void laserTraceValue(LaserTraceStage stage,const char *event,const Snapshot &s,
                            unsigned hand,uint64_t request,unsigned reason,
                            const Calibration *cache=nullptr,uintptr_t caller=0) {
    // Lab (SS2VR_LAB_SHOT_LOG=1): each hand's laser sample outcome, on change.
    static const bool sampleLog = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SHOT_LOG", v, 2) == 1 && v[0] == L'1'; }();
    if (sampleLog && hand < 2 && (stage == LaserTraceStage::Sample || stage == LaserTraceStage::Muzzle)) {
        static std::atomic<uint32_t> last[2][2]{};
        const uint32_t key = uint32_t(reinterpret_cast<uintptr_t>(event)) ^ reason * 2654435761u ^ s.handle[hand];
        if (last[stage == LaserTraceStage::Muzzle][hand].exchange(key) != key)
            log("Lab laser %s hand=%u %s reason=%u handValid=%d,%d gripValid=%d,%d weapon=%u",
                stage == LaserTraceStage::Muzzle ? "muzzle-state" : "sample-state", hand, event, reason,
                int(s.input.handValid[0]), int(s.input.handValid[1]), int(s.input.gripValid[0]), int(s.input.gripValid[1]),
                s.handle[hand]);
    }
    uint64_t order;
    if(!laserTraceTicket(stage,order))return;
    const Calibration empty{};
    const auto &c=cache?*cache:empty;
    log("Lab laser event schema=1 order=%llu event=%s request=%llu input=%llu generation=%u owner=%u weapon=%u hand=%u eye=%d reason=%u renderDepth=%u shotDepth=%u caller=%p cacheValid=%u cacheWeapon=%u cacheTick=%llu alignment=%u",
        order,event,request,s.input.sequence,s.generation,s.playerHandle,
        hand<2?s.handle[hand]:0,hand,eyeIndex,reason,nativeWeaponRenderDepth,nativeShotDepth,
        reinterpret_cast<void *>(caller),unsigned(c.valid),c.handle,c.tickMs,unsigned(c.alignmentApplied));
}
static void invalidateMatchingCalibration(const Snapshot &snapshot, unsigned hand,
                                           unsigned reason=0,uintptr_t caller=0) noexcept {
    if (hand >= 2) return;
    AcquireSRWLockExclusive(&snapshotLock);
    const auto before=calibration[hand];
    const bool matched=current.playerHandle == snapshot.playerHandle && current.generation == snapshot.generation &&
        current.handle[hand] == snapshot.handle[hand] &&
        calibration[hand].handle == snapshot.handle[hand];
    if(matched)calibration[hand].valid = false;
    ReleaseSRWLockExclusive(&snapshotLock);
    {
        static const bool shotLog = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SHOT_LOG", v, 2) == 1 && v[0] == L'1'; }();
        static unsigned logged = 0;
        if (shotLog && matched && before.valid && (++logged <= 120 || logged % 200 == 0))
            log("Lab calibration invalidated hand=%u reason=%u caller=%p eye=%d handValid=%d,%d gripValid=%d,%d n=%u", hand, reason,
                reinterpret_cast<void *>(caller), eyeIndex, int(snapshot.input.handValid[0]), int(snapshot.input.handValid[1]),
                int(snapshot.input.gripValid[0]), int(snapshot.input.gripValid[1]), logged);
    }
    laserTraceValue(LaserTraceStage::Cache,matched?"invalidate":"invalidation-unmatched",snapshot,
                    hand,0,reason,&before,caller);
}
static bool primaryLocalRecognized(const Snapshot &snapshot, void *subject, uint32_t handle) noexcept {
    return nativePrimaryLocalRecognized(subject, handle, snapshot.player, snapshot.playerHandle,
                                       snapshot.initialized) || meleeReleaseObligation(subject, handle);
}
static bool fresh(const Input &i) {
    return i.focused && i.headValid && GetTickCount64() - i.tickMs < 200 && finite(i.head);
}
static bool vrSession(const Snapshot &s) {
    return hooksReady.load(std::memory_order_acquire) && s.initialized && rigPublication.usable(s.rigRevision) &&
           validTrackingEpoch(s.generation) &&
           channel.shared && trackingEpoch(*channel.shared) == s.generation && s.input.session &&
           GetTickCount64() - s.input.tickMs < 1000;
}
static bool local(void *p) {
    return p && isLocal && isLocal(p);
}
// Native Brain RenderView renders WorldInfo::GetCurrentCamera (resolve(+0x6C))
// instead of the player's view while a cinematic camera is active. Those frames
// are presented as flat 2D content, never as tracked stereo gameplay.
bool cinematicCameraActive() noexcept {
    if(!nativeWorldInfo || !resolve || !roomscaleWorldInfoTable || !nativeMainThread || !nativeMainThread())
        return false;
    void *info=nativeWorldInfo();
    if(!info || !readableMemory(info,0x70))return false;
    uintptr_t table=0;std::memcpy(&table,info,4);
    if(table!=roomscaleWorldInfoTable)return false;
    uint32_t camera=0;std::memcpy(&camera,static_cast<uint8_t*>(info)+0x6c,4);
    return camera && resolve(camera);
}
static bool trackingEligible(void *p) {
    RiderIdentity rider;
    return nativeInputHealthy() && hooksReady.load(std::memory_order_acquire) && local(p) &&
           (singlePlayer() || multiplayer::server() || multiplayer::negotiatedLocal()) && isAlive(p) &&
           !isNetricsa(p) && !menus::active() && !cinematicCameraActive() && readNativeRider(p, rider) &&
           (rider.seated() || (rider.handheld() && !thirdPerson(p)));
}
static bool trackingAnchor(void *p, Pose &out) {
    if (!trackingEligible(p))
        return false;
    return nativeTrackingAnchor(p, out);
}
bool readNativeRider(void *player, RiderIdentity &out,void **rideToken) {
    out = {};
    if(rideToken)*rideToken=nullptr;
    if (!player || !resolve || !pointerHandle || !invalidSeatIdent)
        return false;
    RiderIdentity value;
    value.player = pointerHandle(player);
    if (!value.player || resolve(value.player) != player)
        return false;
    const auto *bytes = static_cast<const uint8_t *>(player);
    memcpy(&value.ride, bytes + 0x544, 4);
    memcpy(&value.state, bytes + 0x548, 4);
    memcpy(&value.seat, bytes + 0x54c, 4);
    value.seatValid = value.seat != *invalidSeatIdent;
    void *ride=value.ride?resolve(value.ride):nullptr;
    if (value.ride && !ride)
        return false;
    if (value.state == 3 && !value.seated())
        return false;
    out = value;
    if(rideToken)*rideToken=ride; // Equality token only; no new native lookup.
    return true;
}
bool nativeRiderCurrent(void *player, const RiderIdentity &expected,void **rideToken) {
    if(rideToken)*rideToken=nullptr;
    RiderIdentity live;
    void *ride=nullptr;
    const bool current=expected.player && resolve && resolve(expected.player) == player &&
           readNativeRider(player, live,rideToken?&ride:nullptr) && live == expected;
    if(current && rideToken)*rideToken=ride;
    return current;
}
// Riding: the view must not bob with the ride. The input poll low-passes the
// seated anchor's height and publishes the correction for that player and ride;
// every anchor read applies it, so head, hands, weapons and lasers move together.
// Except in aircraft (which must pitch and roll) the anchor also keeps only the
// ride's heading: a walking dinosaur pitched and rolled the whole view.
// The ride anchor's tracked motion, published by the input poll and predicted
// to each rendered frame (position + velocity x elapsed time): the native
// placement advances per simulation tick, and offsets taken against it at the
// tick were applied to a different placement at render (the view juddered).
struct SteadyState {
    uint32_t player = 0, ride = 0;
    Vec3 x{}, v{};
    float heading = 0, headingRate = 0, fixedHeading = 0;
    double stamp = 0; // QPC seconds of the last tracker step
    bool valid = false, level = false, fixed = false;
};
static SRWLOCK steadyLock = SRWLOCK_INIT;
static SteadyState steadyState;
static double qpcSeconds() {
    LARGE_INTEGER t{}, f{};
    QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
    return double(t.QuadPart) / double(f.QuadPart);
}
// A fine real-time clock for per-frame prediction: under this Proton the
// performance counter advanced only every ~16-32 ms (several frames shared a
// time), so the CPU timestamp counter, calibrated against it over seconds,
// gives the time between frames.
static std::atomic<double> tscPerSecond{0};
static double fineSeconds() {
    static const uint64_t tsc0 = __rdtsc();
    static const double q0 = qpcSeconds();
    const uint64_t tsc = __rdtsc();
    const double q = qpcSeconds(), elapsed = q - q0;
    double rate = tscPerSecond.load(std::memory_order_relaxed);
    if (elapsed > 2 && elapsed < 60 && tsc > tsc0) { // Calibrate over the first minute, then hold.
        rate = double(tsc - tsc0) / elapsed;
        tscPerSecond.store(rate, std::memory_order_relaxed);
    }
    return rate > 0 && tsc > tsc0 ? q0 + double(tsc - tsc0) / rate : q;
}
// The simulation's own clock (Engine simNow, ticks); the ride tracker steps
// on it, so a step always matches the motion simulated since the last one.
static int64_t *(__cdecl *nativeSimNow)(int64_t *) = nullptr;
static float(__cdecl *ticksToSeconds)(int64_t) = nullptr;
// The rolling ball is steered by looking (mountedLookClamp): its anchor keeps a
// fixed heading, turned only by the right stick as on foot (the snapshot's
// turn), never by the ball. Following the ball's own heading shook the view
// and made looking steer the view as well as the ball.
static thread_local bool steadyBypass = false;
static uintptr_t aircraftVtables[3]{}, ballVtables[2]{}, dinoVtables[2]{};
static bool rideOfClass(const RiderIdentity &rider, const uintptr_t *tables, unsigned count) {
    void *ride = rider.ride && resolve ? resolve(rider.ride) : nullptr;
    uintptr_t table = 0;
    if (!ride || !readableMemory(ride, 4)) return false;
    std::memcpy(&table, ride, 4);
    for (unsigned i = 0; i < count; ++i) if (tables[i] && table == tables[i]) return true;
    return false;
}
static bool aircraftRide(const RiderIdentity &rider) { return rideOfClass(rider, aircraftVtables, 3); }
static bool ballRide(const RiderIdentity &rider) { return rider.seated() && rideOfClass(rider, ballVtables, 2); }
// Dinosaurs (legged rides) aim where the right hand points ([Rides]
// HandAimCreatures): a laser from the hand shows where shots land. Their body
// turns to the aim, so, as in the ball, the view keeps its own heading (the
// right stick turns it) and the left stick walks where you face.
static bool creatureAimRide(const RiderIdentity &rider) {
    // Lab (SS2VR_LAB_CREATURE_ANY=1): any ride except the ball is aimed as a dinosaur.
    static const bool labAny = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_CREATURE_ANY", v, 2) == 1 && v[0] == L'1'; }();
    return settings.creatureHandAim && rider.seated() &&
           (rideOfClass(rider, dinoVtables, 2) || (labAny && !rideOfClass(rider, ballVtables, 2)));
}
// The ball's centre: its placement (GetAbsPlacement, vtable +0x54; the ball's
// override, Sam2Game 0x15aca0, reads its body entity's).
static bool ballCentre(const RiderIdentity &rider, Vec3 &out) {
    void *ride = rider.ride && resolve ? resolve(rider.ride) : nullptr;
    if (!ride || !readableMemory(ride, 4)) return false;
    using AbsPlacement = Pose *(__thiscall *)(void *, Pose *);
    const auto table = *static_cast<uintptr_t **>(ride);
    Pose placement{};
    reinterpret_cast<AbsPlacement>(table[0x54 / 4])(ride, &placement);
    if (!finite(placement) || !(std::abs(placement.p.x) + std::abs(placement.p.y) + std::abs(placement.p.z) < 1e6f)) return false;
    out = placement.p;
    return true;
}
bool nativeTrackingAnchor(void *player, Pose &out, const RiderIdentity *expected) {
    RiderIdentity identity;
    if (!baseViewOrigin || !nativeBodyPlacement || !readNativeRider(player, identity) ||
        (expected && identity != *expected))
        return false;
    // The pinned base getter returns a global fallback pose when this resource link
    // is null. That is not a usable world anchor, including while seated.
    uint32_t viewResource=0;memcpy(&viewResource,static_cast<uint8_t*>(player)+0x47c,4);
    if(!viewResource)return false;
    Pose view, body;
    baseViewOrigin(player, &view, 0);
    memcpy(&viewResource,static_cast<uint8_t*>(player)+0x47c,4);
    if (!viewResource || !nativeRiderCurrent(player, identity))
        return false;
    uint32_t heightBits;
    memcpy(&heightBits, static_cast<uint8_t *>(player) + 0x8c0, 4);
    if (identity.seated()) {
        // Reject the getter's native identity fallback when no native pose
        // source exists. These are the exact borrowed sources it reads.
        uint32_t mechanismHandle = 0, modelHandle = 0;
        memcpy(&mechanismHandle, static_cast<uint8_t *>(player) + 0x114, 4);
        memcpy(&modelHandle, static_cast<uint8_t *>(player) + 0x120, 4);
        void *mechanism = mechanismHandle ? resolve(mechanismHandle) : nullptr;
        if (mechanism) {
            uint32_t rootHandle = 0;
            memcpy(&rootHandle, static_cast<uint8_t *>(mechanism) + 0x38, 4);
            if (!rootHandle || !resolve(rootHandle))
                return false;
        } else if (!modelHandle || !resolve(modelHandle))
            return false;
        nativeBodyPlacement(player, &body);
        if (!nativeRiderCurrent(player, identity))
            return false;
    }
    Pose result;
    if (!nativeRiderAnchor(identity, view, heightBits, body, result) ||
        !nativeRiderCurrent(player, identity))
        return false;
    if (identity.seated() && !steadyBypass) {
        AcquireSRWLockShared(&steadyLock);
        const SteadyState st = steadyState;
        ReleaseSRWLockShared(&steadyLock);
        if (st.valid && identity.player == st.player && identity.ride == st.ride) {
            const float ahead = float(std::clamp(fineSeconds() - st.stamp, 0.0, .1));
            const Vec3 p = st.x + st.v * ahead;
            if (std::isfinite(p.x + p.y + p.z)) result.p = p;
            if (st.fixed && std::isfinite(st.fixedHeading)) result.q = yaw(st.fixedHeading);
            else if (st.level) {
                const float h = st.heading + st.headingRate * ahead;
                if (std::isfinite(h)) result.q = yaw(h);
            }
        }
    }
    // On foot the anchor is the game's view, whose pitch and roll the game can
    // change by itself (camera shake from explosions and damage, swimming):
    // that tilted the whole VR world until the game's view came back. Only its
    // heading is used; the headset gives all pitch and roll.
    if (!identity.seated()) result.q = yaw(horizontalHeading(result.q));
    out = result;
    return true;
}
// Per poll while riding: the anchor's position and heading follow the ride's
// through an alpha-beta tracker (position and velocity), which follows steady
// motion and turns without lag but lets through little above ~1 Hz: the gait
// bob and sway of a walking ride are smoothed away, hills, jumps and turns kept.
// Jumps over 1.5 m (teleports, boarding) are taken at once.
struct RideTracker {
    float x = 0, v = 0;
    bool seeded = false;
    float step(float z, float dt, float omega) {
        if (!seeded) { x = z; v = 0; seeded = true; return x; }
        if (!(dt > 0)) return x; // Several polls in one instant: no new information.
        const float predicted = x + v * dt, residual = z - predicted;
        const float alpha = std::min(1.f, 2 * .7f * omega * dt), beta = std::min(1.f, omega * omega * dt * dt);
        x = predicted + alpha * residual;
        v += beta / dt * residual;
        return x;
    }
};
// Turrets follow the head; the right stick turns the view (and so the turret
// with it): an extra heading on the turret's fixed view, per ride.
static std::atomic<float> turretViewTurn{0};
static std::atomic<uint32_t> turretViewRide{0};
static void updateRideSteadying(void *player, const RiderIdentity &rider) {
    static int64_t lastSim = 0;
    static RideTracker axes[3], heading;
    static float unwrapped = 0, previousRaw = 0;
    static uint32_t ride = 0;
    const double now = fineSeconds();
    int64_t sim = 0;
    if (nativeSimNow) nativeSimNow(&sim);
    Pose raw;
    steadyBypass = true;
    const bool ok = rider.seated() && nativeTrackingAnchor(player, raw, &rider);
    steadyBypass = false;
    if (!ok || !finite(raw)) {
        // A missed poll keeps the motion (prediction carries it); a lasting one ends it.
        AcquireSRWLockExclusive(&steadyLock);
        if (!rider.seated() || now - steadyState.stamp > .25) { steadyState.valid = false; ride = 0; }
        ReleaseSRWLockExclusive(&steadyLock);
        return;
    }
    if (ride == rider.ride && sim == lastSim && nativeSimNow) return; // No simulation since: nothing new.
    const float dt = lastSim && nativeSimNow && ticksToSeconds ? std::clamp(ticksToSeconds(sim - lastSim), 0.f, .1f) : 0.f;
    lastSim = sim;
    const float rawHeading = horizontalHeading(raw.q);
    // In the ball the view follows the ball's centre at the eye height it had on
    // boarding: the rider's head inside it sways and bobs with its run.
    static float ballLift = .5f, ballHeading = 0;
    const bool ball = ballRide(rider);
    Vec3 centre{}, target = raw.p;
    // Rides aimed by head or hand keep the view's heading from boarding: the
    // aim turning the ride must not turn the frame the aim is measured in.
    const bool fixedView = ball || creatureAimRide(rider) || seatedInTurret(rider);
    if (fixedView && ride != rider.ride) {
        ballHeading = rawHeading;
        turretViewTurn = 0;
        turretViewRide = rider.ride;
    }
    if (ball && ballCentre(rider, centre)) {
        if (ride != rider.ride) ballLift = std::clamp(raw.p.y - centre.y, -.5f, 1.5f);
        target = centre + Vec3{0, ballLift, 0};
        static uint64_t lastTrace = 0;
        static unsigned traced = 0;
        if (GetTickCount64() - lastTrace > 1000 && traced < 120) {
            lastTrace = GetTickCount64(); ++traced;
            log("Ball ride centre=%.2f,%.2f,%.2f view=%.2f,%.2f,%.2f lift=%.2f heading=%.3f rideHeading=%.3f", centre.x, centre.y,
                centre.z, raw.p.x, raw.p.y, raw.p.z, ballLift, ballHeading, rawHeading);
        }
    }
    const bool reseed = ride != rider.ride ||
        std::abs(axes[0].x - target.x) + std::abs(axes[1].x - target.y) + std::abs(axes[2].x - target.z) > 1.5f;
    if (reseed) {
        for (auto &a : axes) a.seeded = false;
        heading.seeded = false;
        unwrapped = rawHeading;
    } else unwrapped += std::remainder(rawHeading - previousRaw, 2 * Pi);
    previousRaw = rawHeading;
    ride = rider.ride;
    const float omega = ball ? 3.f : 5.f, turnOmega = 9.f; // rad/s
    const Vec3 tracked{axes[0].step(target.x, dt, omega), axes[1].step(target.y, dt, omega), axes[2].step(target.z, dt, omega)};
    const float trackedHeading = heading.step(unwrapped, dt, turnOmega);
    if (rideOfClass(rider, dinoVtables, 2)) {
        // Bounded trace of a legged ride: body heading vs its aim angles (+0x618 heading, +0x61c pitch).
        static uint64_t lastTrace = 0;
        static unsigned traced = 0;
        void *legged = resolve(rider.ride);
        Pose placement{};
        if (legged && GetTickCount64() - lastTrace > 500 && traced < 160) {
            lastTrace = GetTickCount64(); ++traced;
            using AbsPlacement = Pose *(__thiscall *)(void *, Pose *);
            reinterpret_cast<AbsPlacement>((*static_cast<uintptr_t **>(legged))[0x54 / 4])(legged, &placement);
            float aimHeading = 0, aimPitch = 0;
            std::memcpy(&aimHeading, static_cast<uint8_t *>(legged) + 0x618, 4);
            std::memcpy(&aimPitch, static_cast<uint8_t *>(legged) + 0x61c, 4);
            log("Dino ride body heading=%.3f pos=%.2f,%.2f,%.2f aim=%.3f,%.3f riderHeading=%.3f", horizontalHeading(placement.q),
                placement.p.x, placement.p.y, placement.p.z, aimHeading, aimPitch, rawHeading);
        }
    }
    SteadyState st;
    st.player = rider.player;
    st.ride = rider.ride;
    st.x = tracked;
    st.v = Vec3{axes[0].v, axes[1].v, axes[2].v};
    st.heading = trackedHeading;
    st.headingRate = heading.v;
    st.fixedHeading = ballHeading + (seatedInTurret(rider) && turretViewRide.load() == rider.ride ? turretViewTurn.load() : 0.f);
    st.fixed = fixedView;
    st.level = !aircraftRide(rider);
    st.stamp = now;
    st.valid = finite(Pose{{0, 0, 0, 1}, tracked}) && std::isfinite(trackedHeading) && std::isfinite(heading.v) &&
               std::isfinite(st.v.x + st.v.y + st.v.z);
    AcquireSRWLockExclusive(&steadyLock);
    steadyState = st;
    ReleaseSRWLockExclusive(&steadyLock);
}
static uint32_t nativeHandle(void *p, int hand) {
    return *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(p) + (hand == 1 ? 0x800 : 0x804));
}
// Authoritative samples use native player/weapon identity and never depend on
// an eye render, IPC channel, or render-populated calibration.
struct Authority {
    uint32_t player = 0, hand[2]{}, weaponGeneration = 0;
    RiderIdentity rider;
    multiplayer::Sample sample;
    bool observer = false; // Client replica storage; never server intent authority.
    // Dual mode for this remote player (as for the local one): retries per lone weapon.
    uint32_t dualKey = 0;
    unsigned dualTries = 0;
    uint64_t dualToggle = 0;
};
static bool dualIntent(void *p);
static bool pairable(void *p);
static int soloSlot(void *p);
static uint32_t heldHandle(void *p, int hand);
static void syncWeaponChoices(void *p);
// Pose rows remain copyable. Native consumption belongs to the existing row's
// stationary owner and must never disappear when a new pose is published.
struct AuthorityOwner {
    Authority payload;
    std::array<MeleeHandRecord, 2> melee;
    uint64_t nextMeleeEpoch = 1;
};
static std::array<AuthorityOwner, 18> authorities;
static SRWLOCK authorityLock = SRWLOCK_INIT;
static void retireAuthorityMelee(void *entity) noexcept {
    if (!entity) return;
    AcquireSRWLockExclusive(&authorityLock);
    for (auto &owner : authorities)
        for (auto &hand : owner.melee)
            if (hand.player == entity || hand.weapon == entity) {
                hand.consumption.retire();
                hand.blocked = true; hand.epoch = 0;
                hand.player = hand.weapon = hand.world = nullptr;
            }
    ReleaseSRWLockExclusive(&authorityLock);
}
static bool authorityMeleeReleaseObligation(void *subject, uint32_t player) noexcept {
    bool result = false;
    AcquireSRWLockShared(&authorityLock);
    for (const auto &owner : authorities)
        for (const auto &hand : owner.melee)
            if (hand.player == subject && hand.key.player == player && hand.epoch &&
                hand.consumption.edge(hand.key, false) == NativeConsumptionBinding::Edge::Release)
                result = true;
    ReleaseSRWLockShared(&authorityLock);
    return result;
}
static Authority copyAuthority(void *player) {
    Authority result;
    const uint32_t handle = player ? pointerHandle(player) : 0;
    AcquireSRWLockShared(&authorityLock);
    for (const auto &entry : authorities)
        if (handle && entry.payload.player == handle) {
            result = entry.payload;
            break;
        }
    ReleaseSRWLockShared(&authorityLock);
    return result;
}
static void saveAuthority(const Authority &value) {
    bool held = false;
    withNativeFinally([&] {
        AcquireSRWLockExclusive(&authorityLock);
        held = true;
        AuthorityOwner *destination = nullptr;
        for (auto &entry : authorities)
            if (entry.payload.player == value.player) {
                destination = &entry;
                break;
            }
        if (!destination)
            for (auto &entry : authorities)
                if (!entry.payload.player || !resolve(entry.payload.player)) {
                    destination = &entry;
                    break;
                }
        if (destination) {
            // A replaced native actor or incarnation does not inherit receipts.
            // Same-owner sampled pose replacement changes only the payload.
            if (destination->payload.player != value.player ||
                destination->payload.sample.incarnation != value.sample.incarnation ||
                destination->payload.observer != value.observer)
                for (auto &hand : destination->melee) {
                    hand.consumption.retire();
                    hand.blocked = true; hand.epoch = 0;
                    hand.player = hand.weapon = hand.world = nullptr;
                }
            destination->payload = value;
        }
    }, [&](bool aborted) noexcept {
        if (held) {
            held = false;
            ReleaseSRWLockExclusive(&authorityLock);
        }
        if (aborted)
            nativeInputFailed();
    });
}
static bool bodyAnchor(void *player, Pose &pose) {
    return nativeTrackingAnchor(player, pose);
}
static bool captureWeaponModelBinding(void *player,void *weapon,unsigned hand,bool placement,
                                   WeaponAlignmentBinding &out,unsigned *failureStage=nullptr) {
    out={};
    if(failureStage)*failureStage=1;
    if(hand>=2 || !alignmentBorrowPhase(nativeWeaponRenderDepth,placement) ||
       simulationThread.load(std::memory_order_relaxed)!=GetCurrentThreadId() ||
       !nativeMainThread || !nativeMainThread() || !readableMemory(weapon,0xc0) ||
       !weaponModelBindingSupported(primaryField(weapon,0xb4)))return false;
    if(failureStage)*failureStage=2;
    const uint32_t nativeId=primaryField(weapon,0xb4);
    const uint32_t owner=primaryField(weapon,0x28),model=primaryField(weapon,0x24);
    const uint32_t selector=primaryField(weapon,0xbc),handle=pointerHandle(weapon);
    if(!owner || resolve(owner)!=player || !handle || resolve(handle)!=weapon ||
       nativeHandle(player,int(hand))!=handle || nativeHandle(player,int(1-hand))==handle ||
       selector!=hand || !model)return false;
    if(failureStage)*failureStage=3;
    void *instance=resolve(model);
    IdleConfigIdentity config;Vec3 stretch;
    if(!instance || !remote_render::copyModelConfigurationStretch(instance,weaponConfigurationVtable,
                                                                 config,stretch))return false;
    if(failureStage)*failureStage=4;
    // No cached address is dereferenced. Resource-capable native getters must
    // be bracketed by fresh copies and equality at their owning call sites.
    if(primaryField(weapon,0xb4)!=nativeId || !weaponModelBindingSupported(nativeId) || primaryField(weapon,0x28)!=owner ||
       primaryField(weapon,0x24)!=model || primaryField(weapon,0xbc)!=selector ||
       resolve(owner)!=player || resolve(handle)!=weapon || resolve(model)!=instance ||
       nativeHandle(player,int(hand))!=handle)return false;
    out={owner,handle,model,uint32_t(reinterpret_cast<uintptr_t>(instance)),selector,hand,
         config.configuration,config.file,config.resource,stretch,nativeId};
    if(failureStage)*failureStage=0;
    return true;
}
static bool alignmentDisplacement(const WeaponAlignmentBinding &binding,Quat rotation,Vec3 &out) {
    Vec3 stretch,reference;
    if(!weaponModelHandleReference(binding.nativeId,reference) ||
       !id1RenderedStretch(binding.selector,binding.baseStretch,stretch))return false;
    out=modelAnchorDisplacement(rotation,stretch,reference);
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}
static bool nativeWeaponReference(void *player, void *weapon, Pose &body, Pose &camera, Pose &modelPose,
                                  unsigned hand,Vec3 *displacement = nullptr,Vec3 *alignment = nullptr,
                                  WeaponAlignmentBinding *evidence=nullptr) {
    if(displacement)*displacement={};
    if(alignment)*alignment={};
    if(evidence)*evidence={};
    RiderIdentity rider;
    if (!readNativeRider(player, rider) || !rider.handheld())
        return false;
    if (!bodyAnchor(player, body))
        return false;
    originalCamera(player, &camera);
    if (!nativeRiderCurrent(player, rider) || !finite(camera))
        return false;
    auto cameraMatrix = matrix(camera);
    Matrix34 model;
    Vec3 charge;
    bool invalidCharge = false;
    const bool align=weaponAlignmentSupported(primaryField(weapon,0xb4));
    WeaponAlignmentBinding before,after;
    if(!captureWeaponModelBinding(player,weapon,hand,false,before))return false;
    if (!nativePlacementWithCharge(weapon, cameraMatrix, model, charge, invalidCharge) || invalidCharge)
        return false;
    matrixPose(&modelPose, model);
    if(!nativeRiderCurrent(player,rider) || !finite(modelPose))return false;
    if(!captureWeaponModelBinding(player,weapon,hand,false,after) || before!=after)return false;
    const auto relative=compose(inverse(camera),modelPose);
    Vec3 correction;
    if(align && !alignmentDisplacement(after,relative.q,correction))return false;
    if (displacement) *displacement = rotate(relative.q, charge);
    if(alignment)*alignment=correction;
    if(evidence)*evidence=after;
    return true;
}
static void suppressHandheld(network::PosePacket &pose) {
    network::invalidateWeaponIntents(pose, network::HandMask);
    pose.requestedWeapon[0] = pose.requestedWeapon[1] = -1;
    pose.wheelOrEquipBlockedMask |= network::HandMask;
}
static bool currentWeaponSample(const multiplayer::Sample &, const multiplayer::Sample &, unsigned);
static void authorityStepBody(void *player) {
    if (singlePlayer() || !multiplayer::server())
        return;
    auto value = copyAuthority(player);
    value.player = pointerHandle(player);
    value.sample = multiplayer::freeze(player);
    if (!value.sample.negotiated)
        return;
    if(zoomManagerFrame && zoomManagerFrame->preparation)
        zoomManagerFrame->frozenOwner=value.player;
    uint8_t changed = 0;
    RiderIdentity rider;
    const bool riderValid = readNativeRider(player, rider);
    if (value.rider.player && (!riderValid || value.rider != rider))
        changed = network::HandMask;
    value.rider = rider;
    for (unsigned hand = 0; hand < 2; ++hand) {
        uint32_t handle = nativeHandle(player, hand);
        if (value.hand[hand] != handle)
            changed |= uint8_t(1u << hand);
        value.hand[hand] = handle;
    }
    if (changed) {
        if (value.weaponGeneration != UINT32_MAX)
            ++value.weaponGeneration;
        multiplayer::invalidateWeapons(player, value.weaponGeneration, changed);
        network::invalidateWeaponIntents(value.sample.pose, changed);
        network::cancelWeaponRequests(value.sample.pose, changed);
    }
    if (!riderValid || !rider.handheld())
        suppressHandheld(value.sample.pose);
    Pose body;
    if (!isAlive(player) || !bodyAnchor(player, body)) {
        multiplayer::invalidatePlayer(player);
        value.sample.valid = false;
    }
    if (value.sample.valid && riderValid && rider.handheld()) {
        bool pending = false;
        uint8_t equipped = 0;
        for (unsigned hand = 0; hand < 2; ++hand) {
            if (!nativeRiderCurrent(player, rider)) {
                suppressHandheld(value.sample.pose);
                value.sample.valid = false;
                break;
            }
            int requested = value.sample.pose.requestedWeapon[hand];
            auto weapon = value.hand[hand] ? resolve(value.hand[hand]) : nullptr;
            int actual = weapon ? *reinterpret_cast<int *>(static_cast<uint8_t *>(weapon) + 0xb4) : -1;
            if (requested >= 0 && requested != actual &&
                inInventory(static_cast<uint8_t *>(player) + 0x8f8, requested)) {
                pending = true;
                // Single mode holds one weapon: the pick replaces it (in the other
                // slot the game dropped it again), as for the local player.
                const int slot = dualIntent(player) ? int(hand) : soloSlot(player);
                if (canChange(player, slot) && nativeRiderCurrent(player, rider) &&
                    currentWeaponSample(value.sample, multiplayer::authority(player), hand)) {
                    setWeapon(player, requested, slot, 1);
                    equipped |= uint8_t(1u << hand) | uint8_t(1u << slot);
                }
            }
        }
        // Dual mode again when the held weapons can pair, at most twice per lone
        // weapon and a second apart: a pair the game refuses (two-handed weapon,
        // carrying) no longer flipped the flag and re-sent the weapons every tick.
        const uint64_t tick = GetTickCount64();
        void *lone = heldHandle(player, soloSlot(player)) ? resolve(heldHandle(player, soloSlot(player))) : nullptr;
        const uint32_t key = lone ? primaryField(lone, 0xb4) + 1 : 0;
        if (key != value.dualKey || isDual(player)) { value.dualKey = key; value.dualTries = 0; }
        if (value.sample.valid && nativeRiderCurrent(player, rider) && !dualIntent(player) && pairable(player) &&
            value.dualTries < 2 && tick - value.dualToggle > 1000 &&
            !value.sample.pose.physicalDownMask && !pending && canChange(player, 0) &&
            canChange(player, 1) && nativeRiderCurrent(player, rider)) {
            toggleDual(player, 0);
            ++value.dualTries;
            value.dualToggle = tick;
            if (isDual(player)) equipped = 3;
        }
        if (nativeRiderCurrent(player, rider)) syncWeaponChoices(player);
        if (equipped) {
            multiplayer::invalidateWeapons(player, ++value.weaponGeneration, equipped);
            network::invalidateWeaponIntents(value.sample.pose, equipped);
        }
    }
    saveAuthority(value);
}
static void authorityStep(void *player) {
    auto *frame=zoomManagerFrame;
    const auto previous=frame ? frame->frozenOwner:0;
    withNativeFinally([&] {
        if(frame) frame->frozenOwner=0;
        authorityStepBody(player);
    },[&](bool aborted) noexcept {
        if(frame) frame->frozenOwner=previous;
        if(aborted) nativeInputFailed();
    });
}
static void authorityAfterStep(void *player) {
    if (singlePlayer() || !multiplayer::server())
        return;
    auto value = copyAuthority(player);
    if (!value.sample.negotiated)
        return;
    uint8_t changed = 0;
    RiderIdentity rider;
    const bool riderValid = readNativeRider(player, rider);
    if (!riderValid || value.rider != rider)
        changed = network::HandMask;
    value.rider = rider;
    for (unsigned hand = 0; hand < 2; ++hand)
        if (value.hand[hand] != nativeHandle(player, hand))
            changed |= uint8_t(1u << hand);
    if (!isAlive(player)) {
        multiplayer::invalidatePlayer(player);
        value.sample.valid = false;
    } else if (changed) {
        multiplayer::invalidateWeapons(player, ++value.weaponGeneration, changed);
        network::invalidateWeaponIntents(value.sample.pose, changed);
        network::cancelWeaponRequests(value.sample.pose, changed);
    }
    if (!riderValid || !rider.handheld())
        suppressHandheld(value.sample.pose);
    saveAuthority(value);
}
static bool currentWeaponSample(const multiplayer::Sample &captured, const multiplayer::Sample &live,
                                unsigned hand) {
    const auto now = GetTickCount64();
    return hand < 2 && captured.valid && live.negotiated && live.valid &&
           captured.avatar == live.avatar && captured.incarnation == live.incarnation &&
           now >= captured.receivedMs && now - captured.receivedMs <= network::MaxPoseAgeMs &&
           network::currentIntentSample(captured.pose, live.pose, hand, live.liveIntentEpoch[hand]);
}
static void *livePlayer(const Snapshot &s) {
    void *p = s.playerHandle ? resolve(s.playerHandle) : nullptr;
    return p == s.player ? p : nullptr;
}
static bool aliased(const Snapshot &s) {
    if (!s.handle[0] || !s.handle[1])
        return false;
    return s.handle[0] == s.handle[1] || resolve(s.handle[0]) == resolve(s.handle[1]);
}
// A lone weapon (two-handed, the other native hand empty) is mounted on the
// hand whose wheel chose it, whichever native slot the game put it in, unless
// only the other controller is tracked (one controller off): then it goes there.
// A lone weapon the game left by itself (dual mode dropped) keeps its own hand.
static std::atomic<unsigned> soloMountHand{1};
static std::atomic<bool> rideTransitionHold{false}; // See the input poll.
// Use held across boarding or leaving a ride is not Use again until released:
// with the instant transitions the press that boarded was still held on the
// first seated tick, and the game took it as Use to get off at once.
static bool useAwaitsRelease = false;
static int lastPickId = -1; // The weapon of the latest wheel pick (input poll).
// A hand slot's handle when its weapon still exists. The game leaves the handle
// of a weapon it put away and deleted (after dropping dual mode) in the slot:
// that hand is empty, as for IsDualWielding and ToggleDualWielding.
static uint32_t heldHandle(void *p, int hand) {
    const uint32_t handle = nativeHandle(p, hand);
    return handle && resolve(handle) ? handle : 0;
}
// Native dual wielding (CPlayerPuppetEntity::ToggleDualWielding): the flag at
// +0x9bc. The toggle refuses while riding or carrying (+0x544, +0x564), for the
// bomb, and unless each held weapon's state (+0xb0) is 1 or 7 (steady, not
// mid-switch); IsDualWielding also needs both hand slots filled.
static bool dualIntent(void *p) { return isDual(p) || primaryField(p, 0x9bc) != 0; }
static bool pairableWeapon(void *w) {
    const uint32_t state = primaryField(w, 0xb0), id = primaryField(w, 0xb4);
    return (state == 1 || state == 7) && id != 11;
}
static bool pairable(void *p) {
    // A refused toggle spends a try: carrying an object would have used up both.
    if (const uint32_t carried = primaryField(p, 0x564); carried && resolve(carried)) return false;
    unsigned held = 0;
    for (const int hand : {0, 1})
        if (void *w = heldHandle(p, hand) ? resolve(heldHandle(p, hand)) : nullptr) {
            if (!pairableWeapon(w)) return false;
            ++held;
        }
    return held != 0;
}
// Single mode's weapon slot: the one holding the lone weapon, else the right.
static bool slotHeld(void *p, int hand) { return heldHandle(p, hand) != 0; }
static int soloSlot(void *p) { return slotHeld(p, 1) || !slotHeld(p, 0) ? 1 : 0; }
// A pick that replaced the lone weapon from the other hand's wheel, given to
// the choosing hand once dual mode is back; then (`back`) the replaced weapon
// (`restore`, when it could pair) goes back to the other hand.
struct PairPick { int id = -1, hand = -1, restore = -1; bool back = false; };
static PairPick pairPick;
// Weapons this adapter replaced in a hand slot: SetCurrentWeapon leaves the
// replaced entity alive in neither hand, where it still fires with the held
// weapon (unheldWeaponFiring). Simulation thread.
static std::atomic<uint32_t> replacedWeapons[8];
static std::atomic<unsigned> replacedNext{0};
static void replaceWeapon(void *p, int id, int slot) {
    const uint32_t before = nativeHandle(p, slot);
    setWeapon(p, id, slot, 1);
    if (before && nativeHandle(p, slot) != before)
        replacedWeapons[replacedNext.fetch_add(1, std::memory_order_relaxed) % 8].store(before, std::memory_order_relaxed);
}
// The game's own weapon choice per hand (+0x988 right, +0x98c left), kept equal
// to the steady weapon in that hand. SetCurrentWeapon (our picks) leaves it,
// and with the left choice unset (dual mode switched off, as around a ride) the
// HUD's weapon list (GetActiveWeaponIndex, Sam2Game 0xfafa0, on each render)
// re-equipped the old choice on the right: a pick switched straight back.
// Only a steady weapon (state 1 or 7) with no native switch pending (+0x998,
// +0x994) is taken, so a switch the game itself makes is never undone.
static void syncWeaponChoices(void *p) {
    for (const int hand : {0, 1}) {
        void *w = heldHandle(p, hand) ? resolve(heldHandle(p, hand)) : nullptr;
        if (!w) continue;
        const uint32_t state = primaryField(w, 0xb0), id = primaryField(w, 0xb4);
        auto *choice = reinterpret_cast<int32_t *>(static_cast<uint8_t *>(p) + (hand == 1 ? 0x988 : 0x98c));
        const auto pending = int32_t(primaryField(p, hand == 1 ? 0x998 : 0x994));
        if ((state == 1 || state == 7) && id < WeaponCount && pending == -1 && *choice != int32_t(id)) *choice = int32_t(id);
    }
}
static bool replacedWeapon(uint32_t handle) {
    for (const auto &replaced : replacedWeapons)
        if (handle && replaced.load(std::memory_order_relaxed) == handle) return true;
    return false;
}
static bool controllerTracked(const Input &input, unsigned hand) {
    return hand < 2 && input.handValid[hand] && finite(input.hand[hand]);
}
static unsigned mountHand(const Snapshot &s, unsigned nativeHand) {
    if (nativeHand < 2 && s.handle[nativeHand] && !s.handle[1 - nativeHand]) {
        const unsigned chosen = soloMountHand.load(std::memory_order_relaxed) & 1;
        return !controllerTracked(s.input, chosen) && controllerTracked(s.input, 1 - chosen) ? 1 - chosen : chosen;
    }
    return nativeHand;
}
// While the serious bomb is held the other hand is its lighter (bomb_fuse.hpp).
// A weapon dual wielding keeps in that hand (the native bomb pick puts it away)
// is neither drawn, aimed nor fired meanwhile.
static bool bombLighterSlot(const Snapshot &s, unsigned slot) {
    return slot < 2 && s.rider.handheld() && s.ui.currentWeapon[1 - slot] == 11 && s.ui.currentWeapon[slot] != 11;
}
// Two-handed hold of a weapon held alone (common/two_hand.hpp): the holding
// controller, where the free hand grabbed it in the weapon's frame, and which
// weapon. Written by the input poll, read wherever the weapon is posed.
struct TwoHandHold { int holder = -1; Vec3 grabbed{}; uint32_t handle = 0; };
static SRWLOCK twoHandLock = SRWLOCK_INIT;
static TwoHandHold twoHand;
static TwoHandHold currentTwoHand() {
    AcquireSRWLockShared(&twoHandLock);
    const TwoHandHold hold = twoHand;
    ReleaseSRWLockShared(&twoHandLock);
    return hold;
}
// The weapon pose of a controller: its calibrated grip, turned toward the free
// hand while the free hand holds the weapon too.
static Pose weaponGrip(const Input &input, unsigned hand, int weapon) {
    const Pose grip = calibratedGrip(input, hand, weapon, settings);
    if (hand > 1) return grip;
    const auto hold = currentTwoHand();
    const unsigned freeHand = 1 - hand;
    if (hold.holder != int(hand) || !input.handValid[freeHand] || !finite(input.hand[freeHand])) return grip;
    return twoHandAim(grip, hold.grabbed, weaponTracking(input, freeHand).p);
}
// Immersive hand grenade (common/grenade_throw.hpp). The input poll owns the
// state; the eye render reads which controller shows a grenade, and the throw's
// view-origin query (sim thread) takes the launch and reports it back.
static GrenadeThrow grenadeThrow;
static std::atomic<int> grenadeShownHand{-1};
static std::atomic<unsigned> grenadeLaunchHand{0};
static std::atomic<bool> grenadeLaunching{false}, grenadeTaken{false};
using AmmoQuantity = int32_t(__thiscall *)(void *, int);
static AmmoQuantity grenadeAmmoQuantity = nullptr; // CPlayerInventory::GetAmmoQuantity
constexpr int HandGrenadeAmmo = 4;                 // AmmoIndex the native throw drains
static int handOf(void *weapon, const Snapshot &s) {
    if (!s.rider.handheld() || !nativeRiderCurrent(s.player, s.rider))
        return -1;
    void *player = livePlayer(s);
    if (!player || !weapon || aliased(s))
        return -1;
    for (int h = 0; h < 2; h++)
        if (s.handle[h] && nativeHandle(player, h) == s.handle[h] && resolve(s.handle[h]) == weapon)
            return h;
    return -1;
}
static bool localWeaponCurrent(void *weapon, const Snapshot &captured, unsigned hand) {
    const auto live = copySnapshot();
    return sameHandheldRig(captured.rider, live.rider, captured.generation, live.generation) &&
           live.playerHandle == captured.playerHandle && vrSession(captured) && livePlayer(captured) &&
           trackedHandCurrent(captured.input, live.input, mountHand(captured, hand)) && handOf(weapon, captured) == int(hand);
}
static void refreshHand(Snapshot &s, void *p, int h) {
    uint32_t handle = nativeHandle(p, h);
    if (!s.rider.handheld()) {
        s.handle[h] = handle;
        return;
    }
    // A deleted weapon's leftover handle is an empty hand (as weapon deletion
    // leaves the published snapshot): held, it kept the weapon left in the other
    // hand after the game dropped dual mode from counting as alone (mountHand).
    handle = heldHandle(p, h);
    if (s.handle[h] != handle) {
        gates[h] = TriggerGate{};
        pendingSelection[h] = -1;
        clearCalibration(h);
        s.fire[h] = s.zoom[h] = false;
        advanceGeneration(s);
    }
    s.handle[h] = handle;
}
static void refreshOwnership(Snapshot &s, void *p) {
    for (int h = 0; h < 2; h++) {
        refreshHand(s, p, h);
        s.selecting[h] = pendingSelection[h] >= 0;
        s.ui.currentWeapon[h] = -1;
        s.ui.currentAmmo[h] = 0;
        void *weapon = s.handle[h] ? resolve(s.handle[h]) : nullptr;
        if (s.rider.handheld() && weapon) {
            s.ui.currentWeapon[h] = *reinterpret_cast<int *>(static_cast<uint8_t *>(weapon) + 0xb4);
            s.ui.currentAmmo[h] = ammo(static_cast<uint8_t *>(p) + 0x8f8, s.ui.currentWeapon[h]);
        }
    }
    if (s.rider.handheld() && aliased(s)) {
        for (int h = 0; h < 2; h++) {
            s.fire[h] = false;
            gates[h] = {};
            clearCalibration(h);
        }
    }
    s.ui.trackingGeneration = s.generation;
}
static void publishSnapshot(const Snapshot &s) {
    AcquireSRWLockExclusive(&snapshotLock);
    if (!channel.shared || s.rigRevision != rigPublication.current() ||
        !mayPublishSnapshot(s.generation, trackingEpoch(*channel.shared), current.generation)) {
        ReleaseSRWLockExclusive(&snapshotLock);
        return; // A deletion/transition superseded this copied snapshot.
    }
    current = s;
    ReleaseSRWLockExclusive(&snapshotLock);
    Lock l(channel);
    if (l && trackingEpoch(*channel.shared) == s.generation) {
        channel.shared->ui = s.ui;
        if (!s.ui.gameplay && rideTransitionHold) channel.shared->ui.gameplay = 2; // Host: hold the world.
        for (int h = 0; h < 2; ++h)
            channel.shared->ui.fireSequence[h] = fireFeedback[h].load(std::memory_order_relaxed);
        channel.shared->ui.damageSequence = damageFeedback.load(std::memory_order_relaxed);
    }
}
static void prepareLocalGestureSource(Snapshot &);
static void appendLocalGesturePose(const Snapshot &, network::PosePacket &);
// Combo weapons: the game's mode where each hand's weapon has its own fire
// button. In VR each trigger belongs to its hand, so the mode is on while the
// headset drives the game.
static IntThis originalComboWeapons = nullptr;
static int __fastcall comboWeapons(void *game, void *) {
    return vrInputLive() ? 1 : originalComboWeapons(game);
}
// Private-lab weapon audit natives (see auditWeaponAnchor).
static int(__cdecl *boneAbsolute)(void *, uint32_t, Matrix34 &) = nullptr;
static void(__thiscall *giveAllWeapons)(void *, int) = nullptr;
static uint32_t boneHandIdent = 0, bonePalmIdent = 0, boneArmIdent = 0;
// Private weapon audit: every bone of a model (engine CStaticStackArray<BonePlacement>).
struct BonePlacementArray { int32_t allocated = 0; uint8_t *data = nullptr; int32_t count = 0, step = 32; };
static void(__cdecl *bonesAbsolute)(void *, BonePlacementArray &) = nullptr;
static const char *(__cdecl *identToString)(uint32_t) = nullptr;
// Grip carry. A grip squeeze beside a takeable prop picks it up with that hand
// through the native carry: CPuppetEntity::TakeObject (slot 0x290) connects a
// manipulator joint that pulls the object toward a desired placement. Each
// carry tick that target becomes the hand, and releasing the grip drops the
// object (slot 0x29C, stock events and weapon restore) with the hand's velocity.
// Native Use/Fire carrying is unchanged. Single player and server only.
struct NativeHulls { int32_t allocated = 0; void **data = nullptr; int32_t count = 0, step = 16; };
static void(__cdecl *hullsInRange)(const Vec3 &, float, NativeHulls &) = nullptr;
static void *(__thiscall *hullOwner)(void *) = nullptr;
static void *(__thiscall *hullBody)(void *) = nullptr;
static void(__thiscall *jointPlacements)(void *, const Pose &, const Pose &) = nullptr;
// The carry joint's target: the hand. Native code set it again after our carry
// step (the object reached the hand, then rose ~3 m above it, V499), so every
// SetDesiredPlacements on that joint gets the hand target while we carry.
static void *carryJoint = nullptr;
static Pose carryTargetA{}, carryTargetB{};
static bool carryTargetValid = false;
static void __fastcall carryJointPlacements(void *joint, void *, const Pose &a, const Pose &b) {
    if (carryTargetValid && joint && joint == carryJoint) { jointPlacements(joint, carryTargetA, carryTargetB); return; }
    jointPlacements(joint, a, b);
}
static void(__thiscall *jointMaxLinVel)(void *, float) = nullptr;
// A carried object does not collide with its carrier (CMechanism ignore list,
// symmetric: each mechanism lists the other). Without it the player's capsule
// pushed an object held near the body up and over the head. After the drop the
// pair stays ignored until the object is clear of the body, so it does not pop
// out of the player.
static void *(__thiscall *hullMechanism)(void *) = nullptr;
static void(__thiscall *mechanismIgnore)(void *, void *) = nullptr, (__thiscall *mechanismUnignore)(void *, void *) = nullptr;
static int(__thiscall *mechanismIgnored)(void *, void *) = nullptr;
struct CarryIgnore {
    uint32_t hull = 0;              // The carried hull (its mechanism resolved afresh each time).
    void *player = nullptr, *object = nullptr;
    uint64_t releasedAt = 0;        // Set at the drop: unpair once clear of the body.
    bool active = false;
};
static CarryIgnore carryIgnore; // Game thread only.
static void(__cdecl *preDeleteArray)(const void *, void *) = nullptr;
static void(__cdecl *deleteArray)(void *) = nullptr;
static void(__cdecl *postDeleteArray)(void *) = nullptr;
static void(__thiscall *originalCarrying)(void *) = nullptr;
template <class R, class... A> static R virtualCall(void *object, unsigned offset, A... args) {
    uintptr_t table = 0;
    memcpy(&table, object, sizeof(table));
    R(__thiscall *function)(void *, A...) = nullptr;
    memcpy(&function, reinterpret_cast<void *>(table + offset), sizeof(function));
    return function(object, args...);
}
struct GripCarry {
    int requestHand = -1, hand = -1;
    uint32_t requestHull = 0, joint = 0, carriedHull = 0;
    Pose requestPose{};
    bool gripDown[2]{};
    struct Sample { double time; Vec3 p; } history[16]{};
    unsigned samples = 0;
};
static GripCarry gripCarry; // Game thread only.
static void *carriedMechanism(uint32_t hullHandle) {
    void *hull = hullHandle ? resolve(hullHandle) : nullptr;
    return hull && hullMechanism ? hullMechanism(hull) : nullptr;
}
static void beginCarryIgnore(void *puppet, uint32_t hullHandle) {
    if (carryIgnore.active || !hullMechanism || !mechanismIgnore || !mechanismUnignore || !mechanismIgnored || !getMechanism) return;
    void *player = getMechanism(puppet), *object = carriedMechanism(hullHandle);
    if (!player || !object || player == object || mechanismIgnored(player, object)) return; // Already apart: not ours to undo.
    mechanismIgnore(player, object);
    carryIgnore = {hullHandle, player, object, 0, true};
    log("Grip carry: object collision with the player off");
}
// Unpairs when dropped and clear of the body (or after 2.5 s), or at once when
// `now` is set; either mechanism gone or replaced means the engine already did.
static void updateCarryIgnore(void *puppet, bool now) {
    auto &c = carryIgnore;
    if (!c.active || (!now && !c.releasedAt)) return;
    void *player = puppet && getMechanism ? getMechanism(puppet) : nullptr, *object = carriedMechanism(c.hull);
    const bool same = player == c.player && object == c.object;
    if (!now && same) {
        void *hull = resolve(c.hull);
        void *body = hull ? hullBody(hull) : nullptr;
        void *own = resolve(primaryField(puppet, 0x118));
        if (body && own && GetTickCount64() - c.releasedAt < 2500) {
            Pose a, b;
            memcpy(&a, static_cast<uint8_t *>(body) + 0x2c, sizeof(a));
            memcpy(&b, static_cast<uint8_t *>(own) + 0x2c, sizeof(b));
            const float dx = a.p.x - b.p.x, dz = a.p.z - b.p.z;
            if (finite(a) && finite(b) && dx * dx + dz * dz < .9f * .9f) return; // Still at the body.
        }
    }
    if (same && mechanismIgnored(player, object)) mechanismUnignore(player, object);
    log("Grip carry: object collision with the player on again");
    c = {};
}
static std::atomic<bool> gripCarrying{false};
// Grab: objects within GripReach of the hand, else the first one along the
// hand's pointing ray out to GripPointReach (it then comes to the hand).
// 0.2 m alone missed nearly every box: the reach was tighter than a box's own
// size and boxes on the floor sit below a relaxed hand.
static constexpr float GripReach = .35f, GripPointReach = 3.f, ThrowBoost = 1.25f;
static bool gripCarryAuthority(void *player) {
    return hooksReady.load(std::memory_order_acquire) && hullsInRange && hullOwner && hullBody && jointPlacements &&
        deleteArray && preDeleteArray && postDeleteArray && player && local(player) &&
        (singlePlayer() || multiplayer::server());
}
static bool handWorldPose(const Snapshot &s, void *player, unsigned h, Pose &out) {
    Pose body;
    const auto &in = s.input;
    if (h >= 2 || !in.handValid[h] || !trackingAnchor(player, body)) return false;
    const Pose tracked = in.gripValid[h] && finite(in.grip[h]) ? in.grip[h] : in.hand[h];
    out = worldHandTracking(body, s.origin, s.turn, in.head, tracked);
    return finite(out);
}
// Handle of the nearest takeable hull whose shape reaches within `reach` of `at`.
struct CarryScan { int hulls = 0, owned = 0, takeable = 0; uint32_t flagsSeen = 0; };
static uint32_t nearestCarryable(void *player, Vec3 at, float reach, CarryScan *scan = nullptr) {
    NativeHulls hulls;
    uint32_t best = 0;
    float bestDistance = 1e30f;
    hullsInRange(at, reach, hulls);
    for (int32_t i = 0; hulls.data && i < hulls.count; ++i) {
        void *hull = hulls.data[i];
        void *owner = hull ? hullOwner(hull) : nullptr;
        if (scan) ++scan->hulls;
        if (!owner || owner == player) continue;
        const int flags = virtualCall<int>(owner, 0x108); // GetObjectManipulationFlags
        if (scan) { ++scan->owned; scan->flagsSeen |= uint32_t(flags); scan->takeable += (flags & 2) != 0; }
        if (!(flags & 2)) continue; // Takeable.
        if (primaryField(owner, 0x84) & 4) continue; // A prop someone already carries.
        void *body = hullBody(hull);
        if (!body) continue;
        Pose placement;
        memcpy(&placement, static_cast<uint8_t *>(body) + 0x2c, sizeof(placement));
        const Vec3 d = placement.p - at;
        if (dot(d, d) < bestDistance) {
            bestDistance = dot(d, d);
            best = pointerHandle(hull);
        }
    }
    if (hulls.allocated && hulls.data) {
        preDeleteArray(hulls.data, nullptr);
        deleteArray(hulls.data);
        postDeleteArray(nullptr);
    }
    return best;
}
static double carryClock() {
    LARGE_INTEGER frequency{}, now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) / double(frequency.QuadPart);
}
static Vec3 carryVelocity() {
    // Average over the last ~80 ms of carry ticks; the latest sample alone is noisy.
    const auto &g = gripCarry;
    if (g.samples < 2) return {};
    const auto &last = g.history[(g.samples - 1) % 16];
    for (unsigned back = 2; back <= std::min(g.samples, 16u); ++back) {
        const auto &first = g.history[(g.samples - back) % 16];
        const double dt = last.time - first.time;
        if (dt >= .08 || back == std::min(g.samples, 16u))
            return dt > .005 ? (last.p - first.p) * float(1 / dt) : Vec3{};
    }
    return {};
}
static void releaseGripCarry(void *puppet, bool throwIt) {
    const uint32_t bodyHandle = primaryField(puppet, 0x36c);
    const Vec3 velocity = carryVelocity() * ThrowBoost;
    virtualCall<void>(puppet, 0x29c); // DropObject: stock ObjectThrown events and weapon restore.
    if (void *body = throwIt && bodyHandle ? resolve(bodyHandle) : nullptr) {
        if (std::isfinite(velocity.x) && std::isfinite(velocity.y) && std::isfinite(velocity.z)) {
            virtualCall<void>(body, 0x4c, &velocity); // CBody::SetVelocity
            virtualCall<void>(body, 0x2c);            // Wake the body.
        }
    }
    log("Grip carry released hand=%d velocity=%.2f,%.2f,%.2f", gripCarry.hand, velocity.x, velocity.y, velocity.z);
    if (carryIgnore.active && !carryIgnore.releasedAt) carryIgnore.releasedAt = GetTickCount64();
    gripCarry.hand = -1;
    gripCarry.joint = 0;
    gripCarry.samples = 0;
    gripCarrying = false;
    carryTargetValid = false;
}
static void __fastcall carryStep(void *puppet, void *) {
    const auto s = copySnapshot();
    const bool ours = puppet == s.player && gripCarryAuthority(puppet);
    if (!ours) {
        originalCarrying(puppet);
        return;
    }
    auto &g = gripCarry;
    const bool live = vrSession(s) && fresh(s.input) && s.ui.gameplay && !recenterHeld(s.input);
    if (g.hand >= 0) {
        if (!primaryField(puppet, 0x564)) { // Ended natively: Use, death, object destroyed.
            log("Grip carry ended natively hand=%d", g.hand);
            if (carryIgnore.active && !carryIgnore.releasedAt) carryIgnore.releasedAt = GetTickCount64();
            g.hand = -1; g.samples = 0; gripCarrying = false; carryTargetValid = false;
        } else if (!live || !(s.input.buttons[g.hand] & Wheel)) {
            releaseGripCarry(puppet, live);
        }
    }
    if (g.requestHand >= 0) {
        const unsigned h = unsigned(g.requestHand);
        g.requestHand = -1;
        if (g.hand < 0 && live && !primaryField(puppet, 0x564) && (s.input.buttons[h] & Wheel) &&
            resolve(g.requestHull)) {
            virtualCall<void>(puppet, 0x290, g.requestHull, g.requestPose); // TakeObject(hull, world grab pose)
            if (primaryField(puppet, 0x564)) {
                g.hand = int(h); g.samples = 0; g.joint = 0; g.carriedHull = g.requestHull;
                gripCarrying = true;
                log("Grip carry taken hand=%u object=%u", h, primaryField(puppet, 0x564));
                beginCarryIgnore(puppet, g.requestHull);
            }
        }
    }
    originalCarrying(puppet); // Creates the joint and computes the native camera target.
    updateCarryIgnore(puppet, false);
    Pose hand;
    if (g.hand < 0 || !primaryField(puppet, 0x564) || !handWorldPose(s, puppet, unsigned(g.hand), hand)) return;
    void *joint = resolve(primaryField(puppet, 0x374));
    void *body = resolve(primaryField(puppet, 0x118));
    if (!joint || !body) return;
    Pose own;
    memcpy(&own, static_cast<uint8_t *>(body) + 0x2c, sizeof(own));
    Pose grab;
    memcpy(&grab, static_cast<uint8_t *>(puppet) + 0x56c, sizeof(grab));
    if (!finite(own) || !finite(grab)) return;
    const uint32_t jointHandle = primaryField(puppet, 0x374);
    if (jointHandle != g.joint) {
        g.joint = jointHandle;
        if (jointMaxLinVel) jointMaxLinVel(joint, 12.f); // Keep up with a hand without flinging the object.
    }
    // The joint holds the grab frame (object space) at the hand (player-body space).
    // The hand is often inside the player's collision capsule; an object pulled
    // there was pushed out over the top and ended above the head (V499/V500:
    // 0.65 m above the hand after overshooting to 3 m). Hold it at the hand's
    // height but at least CarryClearance out from the body's vertical axis.
    // Without the ignore pair (unavailable), still hold it out of the capsule.
    if (!carryIgnore.active) {
        constexpr float CarryClearance = .7f;
        Vec3 out{hand.p.x - own.p.x, 0, hand.p.z - own.p.z};
        float reach = std::sqrt(out.x * out.x + out.z * out.z);
        if (reach < 1e-3f) {
            const Vec3 forward = rotate(hand.q, Vec3{0, 0, -1});
            out = {forward.x, 0, forward.z};
            reach = std::sqrt(out.x * out.x + out.z * out.z);
        }
        if (reach > 1e-3f && reach < CarryClearance) {
            const float k = CarryClearance / reach;
            hand.p.x = own.p.x + out.x * k;
            hand.p.z = own.p.z + out.z * k;
        }
    }
    carryJoint = joint;
    carryTargetA = compose(inverse(own), hand);
    carryTargetB = grab;
    carryTargetValid = finite(carryTargetA) && finite(carryTargetB);
    jointPlacements(joint, carryTargetA, grab);
    {
        // Lab (SS2VR_LAB_SHOT_LOG=1): where the carried object is against the hand.
        static const bool trace = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SHOT_LOG", v, 2) == 1 && v[0] == L'1'; }();
        static uint64_t last = 0;
        if (trace && GetTickCount64() - last > 100) {
            last = GetTickCount64();
            void *hull = g.carriedHull ? resolve(g.carriedHull) : nullptr;
            void *objectBody = hull ? hullBody(hull) : nullptr;
            Pose placed{};
            if (objectBody) memcpy(&placed, static_cast<uint8_t *>(objectBody) + 0x2c, sizeof(placed));
            log("Lab carry hand=%.2f,%.2f,%.2f body=%.2f,%.2f,%.2f object=%.2f,%.2f,%.2f grab=%.2f,%.2f,%.2f q=%.2f,%.2f,%.2f,%.2f",
                hand.p.x, hand.p.y, hand.p.z, own.p.x, own.p.y, own.p.z, placed.p.x, placed.p.y, placed.p.z, grab.p.x, grab.p.y,
                grab.p.z, grab.q.x, grab.q.y, grab.q.z, grab.q.w);
        }
    }
    g.history[g.samples % 16] = {carryClock(), hand.p};
    ++g.samples;
}
// Stick walking follows where the head faces: snap/smooth turn plus the head's
// yaw relative to the tracking origin's heading.
static float moveHeading(const Input &input, const Pose &origin, float turn) {
    const float head = yawAngle(input.head.q) - yawAngle(origin.q);
    return std::isfinite(head) && input.headValid ? turn + head : turn;
}
static float moveHeading(const Snapshot &s) { return moveHeading(s.input, s.origin, s.turn); }
static int labRideKind();
static void *labFindTurret();
static bool labPlacement(void *puppet, Pose &out);
// Voice chat (voice.hpp): the scene for positional voices and push-to-talk,
// then the local player's queued packets onto the network.
static void voiceTick(void *p, const Snapshot &s) {
    voice::configure(settings.voice);
    voice::Scene scene;
    scene.session = !singlePlayer() && multiplayer::voiceSession();
    scene.self = pointerHandle(p);
    Pose body;
    if (s.initialized && fresh(s.input) && s.input.headValid && trackingAnchor(p, body)) {
        const Pose head = worldHeadTracking(body, s.origin, s.turn, s.input.head);
        if (finite(head)) {
            scene.listener = true;
            scene.head = head.p;
            scene.right = rotate(normalize(head.q), Vec3{1, 0, 0});
        }
        // Push to talk: a controller held at the mouth (tracking space, metres).
        const Vec3 mouth = s.input.head.p + rotate(normalize(s.input.head.q), Vec3{0, -.07f, -.07f});
        for (unsigned h = 0; h < 2; ++h) {
            const Pose hand = s.input.gripValid[h] && finite(s.input.grip[h]) ? s.input.grip[h] : s.input.hand[h];
            const Vec3 d = hand.p - mouth;
            if (controllerTracked(s.input, h) && finite(hand) && dot(d, d) < .15f * .15f) scene.pushToTalk = true;
        }
    } else if (baseViewOrigin) {
        Pose view{};
        baseViewOrigin(p, &view, 0);
        if (finite(view)) {
            scene.listener = true;
            scene.head = view.p;
            scene.right = rotate(normalize(view.q), Vec3{1, 0, 0});
        }
    }
    if (scene.session && baseViewOrigin) {
        unsigned count = 0;
        for (void *player : multiplayer::activePlayers()) {
            if (!player || player == p || count >= 18) continue;
            Pose view{};
            baseViewOrigin(player, &view, 0);
            scene.speakers[count++] = {pointerHandle(player), view.p, finite(view)};
        }
    }
    voice::publish(scene);
    network::VoicePacket packet;
    for (unsigned i = 0; i < 8 && voice::takeOutbound(packet); ++i) multiplayer::sendVoice(p, packet);
}
static void update(void *p) {
    validateLabOnlineIsolation(false);
    if (rigMutationActive || !hooksReady.load(std::memory_order_acquire) || !channel.shared || !local(p))
        return;
    simulationThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    auto s = copySnapshot();
    Input input = s.input;
    uint32_t inputProducer = s.inputProducer;
    uint32_t gripPoseGeneration[2]{s.gripPoseGeneration[0],s.gripPoseGeneration[1]};
    WorldSubmission submittedWorld=s.submittedWorld;
    {
        Lock l(channel);
        if (l) {
            input = channel.shared->latest;
            inputProducer = channel.shared->hostPid;
            std::copy_n(channel.shared->gripPoseGeneration,2,gripPoseGeneration);
            submittedWorld=channel.shared->worldSubmission;
        }
        if (l && !channel.shared->rendererReady)
            input.session = input.focused = 0;
    }
    RiderIdentity rider;
    const bool riderValid = readNativeRider(p, rider);
    bool changed = s.player != p || s.inputProducer != inputProducer ||
                   s.input.session != input.session || s.input.reference != input.reference ||
                   s.rider != rider;
    if (s.rider.ride != rider.ride || s.rider.state != rider.state) useAwaitsRelease = true;
    if (changed) {
        const auto interruption = s.interruption;
        if (s.player != p) lastPickId = -1; // A new player (level) made no pick yet.
        s = Snapshot{};
        s.rigRevision = rigPublication.current();
        s.interruption = interruption;
        s.player = p;
        s.playerHandle = pointerHandle(p);
        s.rider = rider;
        s.networkGeneration = networkGenerations.advance();
        advanceGeneration(s);
        for (int h = 0; h < 2; h++) {
            wheels[h] = WeaponWheel{};
            pendingSelection[h] = -1;
            gates[h] = TriggerGate{};
            clearCalibration(h);
        }
        turnLatched = false;
    }
    s.inputProducer = inputProducer;
    std::copy_n(gripPoseGeneration,2,s.gripPoseGeneration);
    s.submittedWorld=submittedWorld;
    bool enabled = riderValid && fresh(input) && s.interruption.permits(input, GetTickCount64(), inputProducer) &&
                   trackingEligible(p) && nativeRiderCurrent(p, rider);
    if (enabled != bool(s.ui.gameplay)) {
        s.networkGeneration = networkGenerations.advance();
        advanceGeneration(s);
    }
    enabled = enabled && validTrackingEpoch(s.generation);
    {
        // Entering or leaving a ride the rider is neither seated nor on foot for a
        // few ticks (the instant transitions above): the headset keeps the last
        // stereo frame (gameplay 2 to the host) instead of the flat view's flash.
        static uint64_t transitionSince = 0, lastEnabled = 0;
        const uint64_t tick = GetTickCount64();
        const bool transition = riderValid && rider.player && !rider.seated() && !rider.handheld();
        if (enabled) lastEnabled = tick;
        if (!transition) transitionSince = 0;
        else if (!transitionSince) transitionSince = tick;
        rideTransitionHold = transition && !enabled && settings.instantRides && tick - transitionSince < 800 &&
                             tick - lastEnabled < 1500;
    }
    updateRideSteadying(p, rider);
    validateLabOnlineIsolation(enabled);
    s.input = input;
    const int previousHealth = s.ui.health;
    const bool hadHealth = s.ui.tickMs != 0;
    s.ui = {};
    s.ui.tickMs = GetTickCount64();
    s.ui.gameplay = enabled ? 1 : 0;
    s.ui.health = health(p);
    s.ui.armor = armor(p);
    if (enabled && hadHealth && s.ui.health < previousHealth)
        damageFeedback.fetch_add(1, std::memory_order_relaxed);
    if (enabled && !s.initialized) {
        recoverRigForNewOrigin(s);
        s.origin = {yaw(horizontalHeading(input.head.q)), input.head.p};
        s.initialized = true;
        wearCalibration.captureOrigin(input.head, input.tickMs);
    }
    bool resettingControls = enabled && recenterHeld(input);
    bool recenter = resettingControls && !((lastButtons[0] | lastButtons[1]) & Button::Recenter);
    // An origin captured from a motionless (unworn) headset is replaced once the
    // headset has been put on; a manual recentre always wins.
    const bool wornRecenter = enabled && !recenter && s.initialized && input.headValid &&
        wearCalibration.sample(input.head, input.tickMs, s.origin.p);
    if (recenter) wearCalibration.reset();
    if (wornRecenter) log("Tracking origin recentred after the headset was put on");
    if (recenter || wornRecenter) {
        recoverRigForNewOrigin(s);
        s.origin = {yaw(horizontalHeading(input.head.q, horizontalHeading(s.origin.q))), input.head.p};
        s.turn = 0;
        s.networkGeneration = networkGenerations.advance();
        pendingSelection[0] = pendingSelection[1] = -1;
        advanceGeneration(s);
        for (auto &gate : gates)
            gate = TriggerGate{};
        for (auto &w : wheels)
            w.cancel();
    }
    float ax = input.axis[1][0];
    // Seated, the ride turns you (its heading is the anchor's); the right stick
    // steers or aims the ride instead (mountedLookClamp). The rolling ball rolls
    // where you look, and the stick turns you as on foot.
    bool turningAllowed = enabled && !resettingControls && !wheels[1].open &&
        !(input.buttons[1] & Button::WheelOpen) && std::isfinite(ax) &&
        (!rider.seated() || ballRide(rider) || creatureAimRide(rider));
    static LARGE_INTEGER lastTurnClock{};
    LARGE_INTEGER turnClock{}, turnFrequency{};
    QueryPerformanceCounter(&turnClock);
    QueryPerformanceFrequency(&turnFrequency);
    const float turnDt = lastTurnClock.QuadPart ?
        std::clamp(float(double(turnClock.QuadPart - lastTurnClock.QuadPart) / double(turnFrequency.QuadPart)), 0.f, .1f) : 0.f;
    lastTurnClock = turnClock;
    if (std::abs(ax) < .3f || !turningAllowed)
        turnLatched = false;
    if (settings.snapTurn) {
        if (turningAllowed && std::abs(ax) > .75f && !turnLatched) {
            s.turn += (ax > 0 ? -1 : 1) * Pi / 6;
            turnLatched = true;
            s.networkGeneration = networkGenerations.advance();
            advanceGeneration(s);
        }
    } else if (turningAllowed && std::abs(ax) > .15f) {
        // Smooth turn: continuous, so it never invalidates the rendered frames
        // the way a discrete snap (a new tracking generation) does.
        const float deflection = (std::abs(std::clamp(ax, -1.f, 1.f)) - .15f) / .85f;
        s.turn -= std::copysign(deflection * settings.smoothTurnRate * turnDt, ax);
        if (s.turn > Pi) s.turn -= 2 * Pi;
        if (s.turn < -Pi) s.turn += 2 * Pi;
    }
    // Two-handed hold: a weapon held alone can be grabbed by the free hand's grip
    // near it (ahead of the holding hand, at or below the barrel). That grip then
    // neither carries objects nor jumps until released.
    bool twoHandGrip[2]{};
    {
        static bool previousGrip[2]{};
        const bool alone = enabled && !resettingControls && rider.handheld() && !aliased(s) &&
            ((s.handle[0] != 0) != (s.handle[1] != 0));
        const unsigned slot = s.handle[0] ? 0u : 1u;
        const int holder = alone ? int(mountHand(s, slot)) : -1;
        const auto current = currentTwoHand();
        TwoHandHold next;
        bool grip[2]{};
        for (unsigned h = 0; h < 2; ++h)
            grip[h] = enabled && input.handValid[h] && finite(input.hand[h]) && (input.buttons[h] & Wheel) && !wheels[h].open &&
                !(grenadeThrow.holding() && grenadeThrow.hand == h);
        if (holder >= 0 && controllerTracked(input, unsigned(holder))) {
            const unsigned freeHand = 1u - unsigned(holder);
            if (grip[freeHand] && current.holder == holder && current.handle == s.handle[slot])
                next = current;
            else if (grip[freeHand] && !previousGrip[freeHand]) {
                void *w = resolve(s.handle[slot]);
                const int id = w ? int(primaryField(w, 0xb4)) : -1;
                const Vec3 local = twoHandLocal(calibratedGrip(input, unsigned(holder), id, settings),
                                                weaponTracking(input, freeHand).p);
                if (twoHandGrabbable(local, 2.5f)) next = {holder, local, s.handle[slot]};
                else {
                    static unsigned missed = 0;
                    if (++missed <= 20)
                        log("Two-handed grab missed id=%d free hand at %.2f,%.2f,%.2f in the weapon's frame", id, local.x,
                            local.y, local.z);
                }
            }
            twoHandGrip[freeHand] = next.holder >= 0 || (current.holder == holder && grip[freeHand]);
        }
        if (next.holder != current.holder || next.handle != current.handle) {
            AcquireSRWLockExclusive(&twoHandLock);
            twoHand = next;
            ReleaseSRWLockExclusive(&twoHandLock);
            if (next.holder >= 0)
                log("Two-handed hold on hand=%d grabbed at %.2f,%.2f,%.2f", next.holder, next.grabbed.x, next.grabbed.y,
                    next.grabbed.z);
            else
                log("Two-handed hold released");
        }
        previousGrip[0] = grip[0]; previousGrip[1] = grip[1];
    }
    for (int h = 0; h < 2; h++) {
        if (!nativeRiderCurrent(p, rider))
            return;
        if (primaryInputGeneration[h] != input.primaryInputGeneration[h]) {
            primaryInputGeneration[h] = input.primaryInputGeneration[h];
            gates[h] = {};
        }
        refreshHand(s, p, h);
        // The native weapon this tracked hand holds, including a mounted lone weapon.
        const unsigned slot = s.handle[h] ? unsigned(h) : (mountHand(s, 1 - h) == unsigned(h) ? 1 - h : h);
        void *weapon = s.handle[slot] ? resolve(s.handle[slot]) : nullptr;
        auto &ui = s.ui.wheel[h];
        for (int id = 0; id < int(WeaponCount); id++)
            if (id != 14 && inInventory(static_cast<uint8_t *>(p) + 0x8f8, id)) {
                ui.weapon[ui.count] = id;
                ui.ammo[ui.count] = ammo(static_cast<uint8_t *>(p) + 0x8f8, id);
                ui.count++;
            }
        orderWheelWeapons(ui);
        bool valid = enabled && !resettingControls && input.handValid[h] && finite(input.hand[h]);
        bool wheelAllowed = valid && rider.handheld() && !(input.blockedWheels & (1u << h));
        const bool gripDown = valid && (input.buttons[h] & Wheel) && !twoHandGrip[h] &&
            !(grenadeThrow.holding() && grenadeThrow.hand == unsigned(h)); // A hand holding a grenade does not grab.
        if (gripDown && !gripCarry.gripDown[h] && rider.handheld() && gripCarry.hand < 0 &&
            gripCarry.requestHand < 0 && !wheels[h].open && gripCarryAuthority(p)) {
            Snapshot reach = s;
            reach.input = input;
            Pose hand;
            if (handWorldPose(reach, p, unsigned(h), hand)) {
                uint32_t hull = nearestCarryable(p, hand.p, GripReach);
                bool pointed = false;
                // Without a jump button (all but Index) the grip is also the jump,
                // and a grab wins: pointing out to 3 m would swallow every jump
                // facing a crate. There only objects at the hand are taken.
                if (!hull && ((input.buttons[0] | input.buttons[1]) & Button::JumpBound)) {
                    // Pointing: along the controller's aim ray, nearest first.
                    Pose body;
                    if (trackingAnchor(p, body)) {
                        const Pose aim = worldHandTracking(body, s.origin, s.turn, input.head, input.hand[h]);
                        const Vec3 dir = rotate(normalize(aim.q), Vec3{0, 0, -1});
                        for (float d = GripReach; !hull && d <= GripPointReach && finite(aim); d += .25f)
                            hull = nearestCarryable(p, aim.p + dir * d, .3f);
                        // Not through walls: this hand's laser (the world hit along nearly
                        // the same ray) must not stop well short of the object.
                        if (void *native = hull ? resolve(hull) : nullptr) {
                            void *nativeBody = hullBody(native);
                            Pose centre{};
                            if (nativeBody) memcpy(&centre, static_cast<uint8_t *>(nativeBody) + 0x2c, sizeof(centre));
                            for (unsigned n = 0; n < 2 && nativeBody && finite(centre); ++n) {
                                if (!s.handle[n] || mountHand(s, n) != unsigned(h)) continue;
                                AcquireSRWLockShared(&laserLock);
                                const LaserAim beam = laserAim[n];
                                ReleaseSRWLockShared(&laserLock);
                                const Vec3 toObject = centre.p - aim.p, toHit = beam.end - aim.p, past = beam.end - centre.p;
                                if (beam.valid && beam.hit && GetTickCount64() - beam.tickMs < 250 &&
                                    dot(toHit, toHit) < (std::sqrt(dot(toObject, toObject)) - .6f) * (std::sqrt(dot(toObject, toObject)) - .6f) &&
                                    dot(past, past) > .8f * .8f && dot(toObject, toObject) > .36f) {
                                    log("Grip carry pointed object behind something at %.2f m (hit %.2f m)",
                                        std::sqrt(dot(toObject, toObject)), std::sqrt(dot(toHit, toHit)));
                                    hull = 0;
                                }
                            }
                        }
                        pointed = hull != 0;
                    }
                }
                if (hull && pointed) {
                    // Grab it at its centre so it comes into the hand, not held where it lay.
                    void *native = resolve(hull);
                    void *body = native ? hullBody(native) : nullptr;
                    Pose placement{};
                    if (body) memcpy(&placement, static_cast<uint8_t *>(body) + 0x2c, sizeof(placement));
                    const Vec3 gap = placement.p - hand.p;
                    if (body && finite(placement)) hand.p = placement.p;
                    log("Grip carry pointed hand=%d at %.2f m", h, std::sqrt(dot(gap, gap)));
                }
                if (hull) {
                    gripCarry.requestHand = h;
                    gripCarry.requestHull = hull;
                    gripCarry.requestPose = hand;
                } else {
                    // Bounded evidence: where the nearest takeable object is from this hand.
                    static unsigned traced = 0;
                    if (traced < 48) {
                        ++traced;
                        CarryScan scan;
                        const uint32_t nearby = nearestCarryable(p, hand.p, 40.f, &scan);
                        void *hull = nearby ? resolve(nearby) : nullptr;
                        void *body = hull ? hullBody(hull) : nullptr;
                        Pose placement{};
                        if (body) memcpy(&placement, static_cast<uint8_t *>(body) + 0x2c, sizeof(placement));
                        const Vec3 d = placement.p - hand.p;
                        Pose anchorPose{};
                        trackingAnchor(p, anchorPose);
                        const Vec3 fwd = rotate(worldHeadTracking(anchorPose, s.origin, s.turn, input.head).q, Vec3{0, 0, -1});
                        log("Grip carry none in reach hand=%d at=%.2f,%.2f,%.2f nearest40m=%u offset=%.2f,%.2f,%.2f "
                            "hulls=%d owned=%d takeable=%d flags=%x fwd=%.3f,%.3f", h, hand.p.x, hand.p.y, hand.p.z, nearby,
                            d.x, d.y, d.z, scan.hulls, scan.owned, scan.takeable, scan.flagsSeen, fwd.x, fwd.z);
                    }
                }
            }
        }
        gripCarry.gripDown[h] = gripDown;
        // Carry wins over jump; in vehicles the grip grasps instead.
        gripJumps[h].update(gripDown, (input.wheelAdmissionMask & (1u << h)) != 0, rider.handheld(),
                            gripCarry.requestHand == h || gripCarry.hand == h);
        int selected = wheels[h].sample(input, unsigned(h), ui.weapon, ui.count, wheelAllowed, wheelBubbles());
        ui.open = wheels[h].open;
        ui.hover = wheels[h].hover;
        publishWheelPresentation(unsigned(h), wheels[h]);
        requestWheelAction(wheels[h].action);
        const bool fireAllowed = rider.seated() || (rider.handheld() && wheelAllowed &&
            !wheels[h].open && pendingSelection[h] < 0 && weapon != nullptr);
        s.fire[h] = gates[h].update(input.trigger[h], valid && primaryActionEligible(input, unsigned(h)) &&
            fireAllowed);
        {
            // Bounded transition trace of every fire admission term, per hand.
            static unsigned traced = 0, previous[2]{~0u, ~0u};
            const unsigned state = unsigned(input.trigger[h] > .65f) | unsigned(valid) << 1 |
                unsigned(primaryActionEligible(input, unsigned(h))) << 2 | unsigned(rider.handheld()) << 3 |
                unsigned(wheelAllowed) << 4 | unsigned(wheels[h].open) << 5 | unsigned(pendingSelection[h] >= 0) << 6 |
                unsigned(weapon != nullptr) << 7 | unsigned(rider.seated()) << 8 | unsigned(s.fire[h]) << 9 |
                unsigned(gates[h].armed) << 10;
            if (state != previous[h] && traced < 160) {
                previous[h] = state;
                ++traced;
                log("Input fire hand=%d trigger=%.2f valid=%d primary=%d handheld=%d wheelAllowed=%d wheelOpen=%d "
                    "pending=%d weapon=%d seated=%d armed=%d fire=%d", h, input.trigger[h], valid,
                    primaryActionEligible(input, unsigned(h)), rider.handheld(), wheelAllowed, wheels[h].open,
                    pendingSelection[h] >= 0, weapon != nullptr, rider.seated(), gates[h].armed, s.fire[h]);
            }
        }
        // Native selection validates the inventory again and touches only the requested hand.
        if (!valid || !rider.handheld())
            pendingSelection[h] = -1;
        else if (selected >= 0) {
            pendingSelection[h] = selected;
            selectionTick[h] = GetTickCount64();
        }
        if (pendingSelection[h] >= 0 && !inInventory(static_cast<uint8_t *>(p) + 0x8f8, pendingSelection[h]))
            pendingSelection[h] = -1;
        if (pendingSelection[h] >= 0 && multiplayer::remoteClient() &&
            ((weapon &&
              *reinterpret_cast<int *>(static_cast<uint8_t *>(weapon) + 0xb4) == pendingSelection[h]) ||
             GetTickCount64() - selectionTick[h] > 1500))
            pendingSelection[h] = -1;
        if (settings.labWeaponAudit && h == settings.labWeaponAuditHand && valid && rider.handheld() && singlePlayer()) {
            // Private lab: own every weapon once, then hold each in the right hand for 5 s.
            static bool given = false;
            static uint64_t next = 0;
            static int index = -1;
            if (!given && giveAllWeapons) { giveAllWeapons(p, 1); given = true; next = GetTickCount64() + 1000ull * settings.labAuditSeconds; }
            static bool held = false;
            if (given && settings.labHoldWeapon >= 0 && !held && GetTickCount64() >= next && pendingSelection[h] < 0) {
                held = true;
                pendingSelection[h] = settings.labHoldWeapon;
                selectionTick[h] = GetTickCount64();
                log("Weapon audit hold id=%d hand=%d", settings.labHoldWeapon, h);
            }
            // Lab (SS2VR_LAB_DUAL_DROP=<id>|-1): 4 s after the hold, drop dual mode
            // as a ride or cutscene does (with SS2VR_LAB_NO_DUAL=1 it never came on),
            // and (id >= 0) pick id on the left wheel.
            static const int labDualDrop = [] {
                wchar_t v[8]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_DUAL_DROP", v, 8) ? _wtoi(v) : -2; }();
            static const bool labSingle = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_NO_DUAL", v, 2) == 1 && v[0] == L'1'; }();
            static uint64_t dropAt = 0;
            if (held && labDualDrop >= -1 && !dropAt) dropAt = GetTickCount64() + 4000;
            if (dropAt && dropAt != ~0ull && GetTickCount64() >= dropAt && (isDual(p) || labSingle) && canChange(p, 0) &&
                canChange(p, 1)) {
                dropAt = ~0ull;
                if (isDual(p)) toggleDual(p, 0);
                log("Lab dual dropped dual=%d", isDual(p));
                if (labDualDrop >= 0) { pendingSelection[0] = labDualDrop; selectionTick[0] = GetTickCount64(); }
            }
            // Lab (SS2VR_LAB_LATE_PICK=<id>): 8 s after the drop, once paired again,
            // pick id on the right wheel (a pick after dual mode came back).
            static const int labLatePick = [] {
                wchar_t v[8]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_LATE_PICK", v, 8) ? _wtoi(v) : -1; }();
            static uint64_t latePickAt = 0;
            if (dropAt == ~0ull && labLatePick >= 0 && !latePickAt) latePickAt = GetTickCount64() + 8000;
            if (latePickAt && latePickAt != ~0ull && GetTickCount64() >= latePickAt && isDual(p) && pendingSelection[1] < 0) {
                latePickAt = ~0ull;
                // As dual mode switched off (a ride) leaves it: no left choice.
                const int32_t none = -1;
                std::memcpy(static_cast<uint8_t *>(p) + 0x98c, &none, 4);
                pendingSelection[1] = labLatePick;
                selectionTick[1] = GetTickCount64();
                log("Lab late pick id=%d choices=%d,%d", labLatePick, int(primaryField(p, 0x98c)), int(primaryField(p, 0x988)));
            }
            if (given && settings.labHoldWeapon < 0 && GetTickCount64() >= next && pendingSelection[h] < 0) {
                next = GetTickCount64() + 1000ull * settings.labAuditSeconds;
                for (int step = 0; step < int(WeaponCount); ++step) {
                    index = (index + 1) % int(WeaponCount);
                    if (index != 14 && inInventory(static_cast<uint8_t *>(p) + 0x8f8, index)) {
                        pendingSelection[h] = index;
                        selectionTick[h] = GetTickCount64();
                        log("Weapon audit select id=%d hand=%d", index, h);
                        break;
                    }
                }
            }
        }
        // Single mode holds one weapon: one put in the other slot was dropped by
        // the game a moment later (the wheel seemed to unequip it). The pick
        // replaces the lone weapon, mounted on the hand that chose it; dual mode
        // is switched on again below and then gives the choosing hand its pick
        // and the other hand back the weapon the pick replaced.
        // The serious bomb goes to the right slot from either wheel (its lighter
        // hand is the left; a left-slot bomb had no lighter arm).
        const bool bombPick = pendingSelection[h] == 11;
        const int pickSlot = bombPick && dualIntent(p) ? 1 : dualIntent(p) ? int(h) : soloSlot(p);
        if (pendingSelection[h] >= 0 && !multiplayer::remoteClient() && canChange(p, pickSlot) &&
            nativeRiderCurrent(p, rider)) {
            // The serious bomb's model holds the bomb in its right hand and the
            // lighter in its left (split onto the left controller): held alone it
            // is on the right whichever wheel chose it.
            soloMountHand = pendingSelection[h] == 11 ? 1u : unsigned(h);
            lastPickId = pendingSelection[h];
            static char selectionAction[48];
            std::snprintf(selectionAction, sizeof(selectionAction), "selection id=%d hand=%d slot=%d", pendingSelection[h], int(h), pickSlot);
            lastWeaponSelection = selectionAction;
            void *replaced = pickSlot != int(h) && heldHandle(p, pickSlot) ? resolve(heldHandle(p, pickSlot)) : nullptr;
            const int restore = replaced && pairableWeapon(replaced) ? int(primaryField(replaced, 0xb4)) : -1;
            replaceWeapon(p, pendingSelection[h], pickSlot);
            if (!nativeRiderCurrent(p, rider))
                return; // A native lifecycle callback superseded this transaction.
            pairPick = pickSlot != int(h) && !bombPick ? PairPick{pendingSelection[h], int(h), restore} : PairPick{};
            pendingSelection[h] = -1;
            for (const int n : {int(h), pickSlot}) {
                s.handle[n] = heldHandle(p, n);
                gates[n] = TriggerGate{};
                s.fire[n] = false;
                clearCalibration(n);
            }
            advanceGeneration(s);
        }
    }
    {
        // A lone weapon the game left (dual mode dropped around a ride, cutscene
        // or load: the other hand's weapon put away) stays on its own hand; the
        // weapon of the latest pick stays on the hand that chose it.
        static uint32_t loneSeen = 0;
        const bool alone = rider.handheld() && !aliased(s) && (s.handle[0] != 0) != (s.handle[1] != 0);
        const unsigned slot = s.handle[0] ? 0u : 1u;
        const uint32_t lone = alone ? s.handle[slot] : 0;
        if (lone && lone != loneSeen) {
            void *w = resolve(lone);
            const int id = w ? int(primaryField(w, 0xb4)) : -1;
            if (id != lastPickId) soloMountHand = slot;
            static unsigned traced = 0;
            if (traced++ < 64)
                log("Weapon alone id=%d slot=%u on hand=%u (%s)", id, slot, mountHand(s, slot),
                    id == lastPickId ? "picked" : "left by the game");
        }
        loneSeen = lone;
    }
    // Fire flags are native-slot indexed: a lone weapon mounted on the other
    // hand fires from that hand's trigger. Seated (turrets, vehicles) the right
    // trigger is the ride's Fire and the left its AltFire, whatever weapon
    // handles the player still carries.
    for (unsigned n = 0; n < 2 && rider.handheld(); ++n)
        if (const unsigned m = mountHand(s, n); m != n) {
            s.fire[n] = s.fire[m];
            s.fire[m] = false;
        }
    // The hand holding a grenade has put its weapon away: it does not fire.
    for (unsigned n = 0; n < 2 && grenadeThrow.holding(); ++n)
        if (mountHand(s, n) == grenadeThrow.hand) s.fire[n] = false;
    {
        // The serious bomb is lit by hand (common/bomb_fuse.hpp): the other
        // hand's trigger strikes the lighter, its flame at the fuse fires it.
        // The bomb hand's own trigger still fires it too.
        static BombFuse fuse;
        int bombSlot = -1;
        for (unsigned n = 0; n < 2; ++n)
            if (void *w = s.handle[n] && rider.handheld() && !aliased(s) ? resolve(s.handle[n]) : nullptr)
                if (primaryField(w, 0xb4) == 11) bombSlot = int(n);
        if (bombSlot < 0 || !enabled || resettingControls || grenadeThrow.holding()) fuse.clear();
        else {
            const unsigned bombHand = mountHand(s, unsigned(bombSlot)), lighter = 1 - bombHand;
            // A weapon dual wielding keeps in the lighter hand (the native bomb pick
            // puts it away) does not fire with the lighter's trigger, nor after the
            // bomb until that trigger is released (bombLighterSlot hides it).
            for (unsigned n = 0; n < 2; ++n)
                if (int(n) != bombSlot && mountHand(s, n) == lighter) s.fire[n] = false;
            gates[lighter] = TriggerGate{};
            const bool tracked = controllerTracked(input, 0) && controllerTracked(input, 1) && input.handValid[lighter];
            const bool wasFiring = fuse.firing;
            if (fuse.update(tracked, input.trigger[lighter], weaponTracking(input, lighter), weaponTracking(input, bombHand),
                            GetTickCount64()))
                s.fire[bombSlot] = true;
            if (fuse.firing && !wasFiring) log("Bomb fuse lit by hand=%u", lighter);
        }
    }
    // Lab (SS2VR_LAB_NO_CHOICE_SYNC=1): leave the game's weapon choices stale.
    static const bool labNoChoiceSync = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_NO_CHOICE_SYNC", v, 2) == 1 && v[0] == L'1'; }();
    if (rider.handheld() && !multiplayer::remoteClient() && !labNoChoiceSync && nativeRiderCurrent(p, rider)) syncWeaponChoices(p);
    // Use existing dual-wield policy. Retry while native weapons cannot yet transition.
    static const char *lastWeaponAction = "start";
    {
        // Lab (SS2VR_LAB_TELEPORT=x,y,z[,heading]): put the player there once,
        // 3 s into play (CPuppetEntity::Teleport), to reach a scene by position.
        static float target[4]{};
        static int parts = [] {
            wchar_t v[96]{};
            if (!GetEnvironmentVariableW(L"SS2VR_LAB_TELEPORT", v, 96)) return 0;
            return swscanf(v, L"%f,%f,%f,%f", &target[0], &target[1], &target[2], &target[3]);
        }();
        static uint64_t playingSince = 0;
        static const bool labLoadWanted = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_LOAD", v, 2) == 1 && v[0] == L'1'; }();
        if (parts == 0 && labRideKind() >= 2 && (!labLoadWanted || labLoadIssued) && rider.handheld() && singlePlayer() && nativeTeleport) {
            static bool once = false;
            Pose ride{};
            void *found = once ? nullptr : labFindTurret();
            if (found && labPlacement(found, ride)) {
                once = true;
                target[0] = ride.p.x + 3.f; target[1] = ride.p.y + .5f; target[2] = ride.p.z; target[3] = Pi / 2;
                parts = 4;
            }
        }
        static const bool labLoad = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_LOAD", v, 2) == 1 && v[0] == L'1'; }();
        static bool labLoaded = false;
        if (labLoad && !labLoaded && labLoadGame && singlePlayer() && nativeMainThread && nativeMainThread() &&
            GetFileAttributesA("Temp\\SS2VR\\load-request") != INVALID_FILE_ATTRIBUTES) {
            labLoaded = true;
            DeleteFileA("Temp\\SS2VR\\load-request");
            log("Lab load of Temp/SS2VR/user.sav requested");
            labLoadPending = true; // Issued between frames (appForeground), as the load menu does: not mid-step.
        }
        // Lab (SS2VR_LAB_PICK_AFTER_RIDE=<id>): 3 s after leaving a ride, pick id
        // on the right wheel (a pick right after a ride switched straight back).
        static const int labPickAfterRide = [] {
            wchar_t v[8]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_PICK_AFTER_RIDE", v, 8) ? _wtoi(v) : -1; }();
        static bool labWasSeated = false;
        static uint64_t labPickAt = 0;
        if (labPickAfterRide >= 0) {
            if (rider.seated()) labWasSeated = true;
            else if (labWasSeated && rider.handheld()) { labWasSeated = false; labPickAt = GetTickCount64() + 3000; }
            if (labPickAt && GetTickCount64() >= labPickAt && rider.handheld() && pendingSelection[1] < 0) {
                labPickAt = 0;
                pendingSelection[1] = labPickAfterRide;
                selectionTick[1] = GetTickCount64();
                log("Lab pick after ride id=%d choices=%d,%d dual=%d", labPickAfterRide, int(primaryField(p, 0x98c)),
                    int(primaryField(p, 0x988)), isDual(p));
            }
        }
        if (parts >= 3 && rider.handheld() && singlePlayer() && nativeTeleport) {
            if (!playingSince) playingSince = GetTickCount64();
            if (GetTickCount64() - playingSince > 3000) {
                const float half = target[3] * .5f;
                const Pose placement{{0, std::sin(half), 0, std::cos(half)}, {target[0], target[1], target[2]}};
                const int done = nativeTeleport(p, placement);
                log("Lab teleport to %.2f,%.2f,%.2f heading %.2f result=%d", target[0], target[1], target[2], target[3], done);
                parts = 0;
            }
        }
    }
    // Dual wielding. The game pairs only one-handed weapons and drops dual mode
    // on its own around rides, destroyed turrets, cutscenes and loads, and for
    // a two-handed pick; switching it on once per player left the player with a
    // lone weapon for the rest of the run. It is switched on again whenever the
    // held weapons can pair, with no wheel pick or trigger in the last second;
    // a refused toggle (still seated, mid-transition) is retried a second later.
    static uint64_t lastSelection = 0, lastToggle = 0;
    if (lastWeaponSelection) {
        lastWeaponAction = lastWeaponSelection; lastWeaponSelection = nullptr;
        lastSelection = GetTickCount64();
    }
    // Lab (SS2VR_LAB_NO_DUAL=1): leave dual wielding off, so weapons are held alone.
    static const bool labNoDual = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_NO_DUAL", v, 2) == 1 && v[0] == L'1'; }();
    const uint64_t now = GetTickCount64();
    const bool pairReady = enabled && !labNoDual && rider.handheld() && !multiplayer::remoteClient() && !s.fire[0] &&
        !s.fire[1] && now - lastSelection > 1000 && canChange(p, 0) && canChange(p, 1) && nativeRiderCurrent(p, rider);
    // A pair the game drops again at once is not retried: twice per lone
    // weapon, until dual mode has held for a few seconds.
    static uint32_t triedKey = 0;
    static unsigned tries = 0;
    static uint64_t dualSince = 0;
    void *lone = nativeHandle(p, soloSlot(p)) ? resolve(nativeHandle(p, soloSlot(p))) : nullptr;
    const uint32_t weaponKey = lone ? primaryField(lone, 0xb4) + 1 : 0;
    if (!isDual(p)) dualSince = 0;
    else if (!dualSince) dualSince = now;
    if ((weaponKey != triedKey && !isDual(p)) || (dualSince && now - dualSince > 3000)) { triedKey = weaponKey; tries = 0; }
    {
        // Bounded trace while unpaired on foot: why dual mode is not switched on.
        static uint64_t lastTrace = 0;
        static unsigned traced = 0;
        if (!isDual(p) && rider.handheld() && now - lastTrace > 3000 && traced < 40) {
            lastTrace = now; ++traced;
            uint32_t state[2]{~0u, ~0u}, ids[2]{~0u, ~0u};
            for (const int hand : {0, 1})
                if (void *w = nativeHandle(p, hand) ? resolve(nativeHandle(p, hand)) : nullptr) {
                    state[hand] = primaryField(w, 0xb0); ids[hand] = primaryField(w, 0xb4);
                }
            log("Dual unpaired ready=%u flag=%u pairable=%u tries=%u change=%u,%u fire=%u,%u sinceSelection=%llu ids=%d,%d states=%d,%d",
                pairReady, primaryField(p, 0x9bc), pairable(p), tries, canChange(p, 0), canChange(p, 1), s.fire[0], s.fire[1],
                static_cast<unsigned long long>(now - lastSelection), int(ids[0]), int(ids[1]), int(state[0]), int(state[1]));
        }
    }
    if (pairReady && !dualIntent(p) && pairable(p) && now - lastToggle > 1000 && tries < 2) {
        toggleDual(p, 0);
        ++tries;
        lastToggle = now;
        lastWeaponAction = "dual toggle";
    }
    // Dual mode with one slot empty (switched on with the left hand empty, the
    // game brings up there the weapon of its own last selection, +0x988, which
    // may be gone): the choosing hand's pick, else the same weapon.
    if (pairReady && dualIntent(p) && !isDual(p) && pairable(p) && now - lastToggle > 500 && tries < 4) {
        const int empty = slotHeld(p, 1) ? 0 : 1;
        void *other = resolve(nativeHandle(p, 1 - empty));
        const int id = pairPick.id >= 0 && pairPick.hand == empty ? pairPick.id : other ? int(primaryField(other, 0xb4)) : -1;
        if (id >= 0 && id != 11 && inInventory(static_cast<uint8_t *>(p) + 0x8f8, id)) {
            replaceWeapon(p, id, empty);
            ++tries;
            lastToggle = now;
            lastWeaponAction = "dual fill";
        }
    }
    // Paired after a pick that replaced the lone weapon: the choosing hand gets
    // it; a later poll (both weapons steady again) gives the other hand back the
    // weapon the pick replaced, rather than the pick twice.
    if (pairReady && isDual(p) && pairPick.id >= 0) {
        const PairPick pick = pairPick;
        pairPick = !pick.back && pick.restore >= 0 ? PairPick{pick.restore, 1 - pick.hand, -1, true} : PairPick{};
        void *w = resolve(nativeHandle(p, pick.hand));
        if ((!w || int(primaryField(w, 0xb4)) != pick.id) && inInventory(static_cast<uint8_t *>(p) + 0x8f8, pick.id)) {
            replaceWeapon(p, pick.id, pick.hand);
            lastWeaponAction = pick.back ? "dual restore" : "dual pick";
        }
    }
    if (!rider.handheld() || now - lastSelection > 10000) pairPick = {};
    {
        // Bounded trace of every native weapon change per hand and what preceded it.
        static int previousIds[2]{-2, -2};
        static unsigned traced = 0;
        int ids[2]{-1, -1};
        for (unsigned h = 0; h < 2; ++h) {
            const uint32_t handle = nativeHandle(p, int(h));
            if (void *w = handle ? resolve(handle) : nullptr) ids[h] = *reinterpret_cast<int *>(static_cast<uint8_t *>(w) + 0xb4);
        }
        if ((ids[0] != previousIds[0] || ids[1] != previousIds[1]) && traced < 200) {
            ++traced;
            log("Weapons now left=%d right=%d (was %d,%d) dual=%d after %s", ids[0], ids[1], previousIds[0], previousIds[1],
                isDual(p), lastWeaponAction);
            lastWeaponAction = "native";
        }
        previousIds[0] = ids[0]; previousIds[1] = ids[1];
    }
    if (!nativeRiderCurrent(p, rider))
        return;
    refreshOwnership(s, p);
    prepareLocalGestureSource(s); // Current physical input, before the one native transport submission.
    for (unsigned h = 0; h < 2; ++h) {
        void *weapon = s.handle[h] ? resolve(s.handle[h]) : nullptr;
        const bool sniper = enabled && rider.handheld() && !aliased(s) && nativeSniper(weapon);
        // A lone two-handed sniper sits in native slot 0 but is mounted on the hand
        // that chose it: zoom comes from that physical hand's button.
        const unsigned physical = mountHand(s, h);
        s.zoom[h] = zoomInputs[h].sample(input,physical,
            sniper && !resettingControls && !(input.blockedWheels & (1u<<physical)) && !s.ui.wheel[physical].open &&
                pendingSelection[h] < 0,
            s.handle[h],s.networkGeneration,inputProducer,GetTickCount64());
        static uint32_t lastZoomKey[2]{~0u,~0u};
        const uint32_t zoomKey=(uint32_t(s.zoom[h])) | (((input.zoomDownMask>>physical)&1u)<<1) |
            (((input.zoomActiveMask>>physical)&1u)<<2) | (uint32_t(sniper)<<3) | (uint32_t(input.handValid[physical])<<4);
        if(zoomKey!=lastZoomKey[h]) {
            lastZoomKey[h]=zoomKey;
            log("Zoom slot=%u hand=%u zoom=%u down=%u active=%u sniper=%u handValid=%u generation=%u",h,physical,
                zoomKey&1u,(zoomKey>>1)&1u,(zoomKey>>2)&1u,(zoomKey>>3)&1u,(zoomKey>>4)&1u,input.zoomInputGeneration[physical]);
        }
    }
    const bool useButton = ((input.buttons[0] | input.buttons[1]) & Button::Use) != 0;
    if (!useButton) useAwaitsRelease = false;
    s.use = enabled && !resettingControls && useButton && !useAwaitsRelease;
    uint32_t contextualButtons = 0, leftContextual = 0, rightContextual = 0;
    for (unsigned h=0;h<2;++h)
    {
        // Physical hand h: the native slot mounted on it (a lone two-handed sniper
        // in slot 0 may be mounted on the right hand).
        const unsigned slot = s.handle[h] ? h : (mountHand(s, 1 - h) == h ? 1 - h : h);
        const uint32_t handButtons = contextualSniperButtons(input,h,
            enabled && rider.handheld() && !aliased(s) && nativeSniper(s.handle[slot] ? resolve(s.handle[slot]) : nullptr));
        contextualButtons |= handButtons;
        (h ? rightContextual : leftContextual) = handButtons;
    }
    // A profile with a jump button (Index B) keeps the grip for grabbing only.
    const bool gripJump = !((input.buttons[0] | input.buttons[1]) & Button::JumpBound);
    s.jump = enabled && !resettingControls && ((contextualButtons & Button::Jump) ||
        (gripJump && (gripJumps[0].jumping || gripJumps[1].jumping)));
    // Left stick click sprints; the right one zooms a sniper (above) and
    // otherwise throws a hand grenade.
    s.sprint = enabled && !resettingControls && (leftContextual & Button::Sprint);
    {
        // The right stick click takes a grenade into the right hand (its weapon
        // is put away meanwhile) or puts it away; a throwing motion of that hand
        // throws it at where the left hand aims (the native throw, plcmdTertFire,
        // aimed in playerViewOrigin).
        // On release of the click, so that clicking both sticks (recentre) does
        // not also take a grenade.
        static bool previousButton = false, armed = false;
        const bool button = enabled && !resettingControls && rider.handheld() && (rightContextual & Button::Sprint);
        if (button && !previousButton) armed = true;
        if (recenterHeld(input) || !enabled) armed = false;
        const bool pressed = !button && previousButton && armed;
        if (!button) armed = false;
        previousButton = button;
        constexpr unsigned offHand = 1; // The right hand throws; the left aims.
        const int32_t grenades = grenadeAmmoQuantity ? grenadeAmmoQuantity(static_cast<uint8_t *>(p) + 0x8f8,
                                                                               HandGrenadeAmmo) : 1;
        if (!enabled || resettingControls || !rider.handheld() || wheels[0].open || wheels[1].open)
            grenadeThrow.clear();
        else if (pressed && (grenades > 0 || grenadeThrow.holding()))
            grenadeThrow.press(offHand);
        if (grenadeTaken.exchange(false)) grenadeThrow.thrown();
        const unsigned g = grenadeThrow.hand;
        const Vec3 forward = rotate(normalize(input.head.q), Vec3{0, 0, -1});
        if (grenadeThrow.update(controllerTracked(input, g), weaponTracking(input, g).p, input.tickMs, forward))
            log("Grenade throw motion hand=%u grenades=%d", g, grenades);
        const bool launching = grenadeThrow.state == GrenadeThrow::State::Launching;
        grenadeLaunchHand = g;
        grenadeLaunching = launching;
        grenadeShownHand = grenadeThrow.state == GrenadeThrow::State::Held ? int(g) : -1;
        s.grenade = launching;
        static GrenadeThrow::State previousState = GrenadeThrow::State::Idle;
        if (grenadeThrow.state != previousState) {
            static unsigned traced = 0;
            if (traced++ < 120) log("Grenade state=%u hand=%u grenades=%d", unsigned(grenadeThrow.state), g, grenades);
            previousState = grenadeThrow.state;
        }
    }
    lastButtons[0] = input.buttons[0];
    lastButtons[1] = input.buttons[1];
    if (!singlePlayer()) {
        network::PosePacket packet;
        network::LocalPoseCapture capture;
        capture.frame.origin = s.origin;
        capture.frame.turn = s.turn;
        capture.frame.avatar = s.playerHandle;
        capture.frame.producer = inputProducer;
        capture.frame.session = input.session;
        capture.frame.reference = input.reference;
        capture.frame.trackingEpoch = s.generation;
        capture.frame.trackingGeneration = s.networkGeneration;
        capture.frame.sequence = input.sequence;
        capture.frame.tickMs = input.tickMs;
        capture.head = input.head;
        packet.trackingGeneration = s.networkGeneration;
        if (s.initialized && finite(input.head))
            packet.head = bodyHeadTracking(s.origin, s.turn, input.head);
        if (enabled)
            packet.validMask = 1;
        bool newPhysicalSample = input.sequence != physicalSequence;
        for (unsigned h = 0; h < 2; ++h) {
            const bool valid = enabled && input.handValid[h] && finite(input.hand[h]);
            packet.grip[h].p = packet.head.p; // Canonical inactive-hand pose shares the current rig origin.
            // Controller h holds the native slot mounted on it (a lone weapon may be on the other).
            const unsigned held = s.handle[h] && mountHand(s, h) == h ? h : (mountHand(s, 1 - h) == h ? 1 - h : h);
            // Seated, the controller's grip point too (weaponTracking): peers' arm
            // IK reaches for it, and the aim pose put a rider's hands ahead of it.
            const Pose grip = rider.handheld() ? weaponGrip(input, h, s.ui.currentWeapon[held])
                                               : weaponTracking(input, h);
            capture.grip[h] = grip; // Preserve the actual pre-clamp capture for retained taps.
            if (s.initialized && valid && finite(grip))
                packet.grip[h] = bodyHandTracking(s.origin, s.turn, input.head, grip);
            if (valid)
                packet.validMask |= uint8_t(2u << h);
            const bool primaryNeutral = samplePrimaryNeutral(input.trigger[h],
                input.focused && input.handValid[h] && primaryActionEligible(input, h),
                newPhysicalSample, physicalDown[h], releasedSerial[h]);
            if (valid && primaryNeutral)
                packet.primaryNeutralSampleMask |= uint8_t(1u << h);
            if (physicalDown[h])
                packet.physicalDownMask |= uint8_t(1u << h);
            if (s.fire[h] && physicalDown[h] && valid)
                packet.fireMask |= uint8_t(1u << h);
            if (!valid || s.ui.wheel[h].open || pendingSelection[h] >= 0 || recenterHeld(input))
                packet.wheelOrEquipBlockedMask |= uint8_t(1u << h);
            packet.releasedSerial[h] = releasedSerial[h];
            packet.primaryInputGeneration[h] = input.primaryInputGeneration[h];
            packet.zoomInputGeneration[h] = input.zoomInputGeneration[h];
            if (newPhysicalSample && input.focused && input.handValid[h] &&
                (input.zoomActiveMask & (1u << h)) && input.zoomInputGeneration[h] &&
                !(input.zoomDownMask & (1u << h)) && zoomReleasedSerial[h] != UINT32_MAX)
                ++zoomReleasedSerial[h];
            if (input.zoomDownMask & (1u << h))
                packet.zoomPhysicalDownMask |= uint8_t(1u << h);
            if (valid && primaryActionEligible(input, h))
                packet.primarySampleEligibleMask |= uint8_t(1u << h);
            if (valid && (input.zoomActiveMask & (1u << h)) && input.zoomInputGeneration[h])
                packet.zoomSampleEligibleMask |= uint8_t(1u << h);
            packet.zoomReleasedSerial[h] = zoomReleasedSerial[h];
            if (s.zoom[h] && valid)
                packet.zoomMask |= uint8_t(1u << h);
            packet.requestedWeapon[h] = int16_t(pendingSelection[h]);
            packet.nativeWeaponId[h] = int16_t(s.ui.currentWeapon[h]);
        }
        if (!rider.handheld())
            suppressHandheld(packet);
        else
            appendLocalGesturePose(s, packet);
        physicalSequence = input.sequence;
        bool edge = packet.physicalDownMask != lastNetworkPose.physicalDownMask ||
                    packet.fireMask != lastNetworkPose.fireMask ||
                    packet.zoomMask != lastNetworkPose.zoomMask ||
                    packet.gestureDownMask != lastNetworkPose.gestureDownMask ||
                    packet.gestureEligibleMask != lastNetworkPose.gestureEligibleMask ||
                    packet.trackingGeneration != lastNetworkPose.trackingGeneration ||
                    packet.requestedWeapon[0] != lastNetworkPose.requestedWeapon[0] ||
                    packet.requestedWeapon[1] != lastNetworkPose.requestedWeapon[1];
        if (multiplayer::submit(p, packet, edge, capture))
            lastNetworkPose = packet;
        for (unsigned h = 0; h < 2; ++h) {
            if (s.intentEpoch[h]!=packet.intentEpoch[h]) {
                s.zoom[h]=false;
                zoomInputs[h].sample(input,h,false,s.handle[h],s.networkGeneration,inputProducer,GetTickCount64());
            }
            s.intentEpoch[h] = packet.intentEpoch[h];
            if (rider.handheld() && multiplayer::remoteClient())
                s.fire[h] = s.fire[h] && (packet.fireMask & (1u << h));
            if (rider.handheld() && multiplayer::remoteClient())
                s.zoom[h] = s.zoom[h] && (packet.zoomMask & (1u << h));
        }
    }
    voiceTick(p, s);
    publishSnapshot(s);
}
struct PreparedPlayer {
    uint32_t handle = 0;
    void *subject = nullptr;
    bool recognized = false, revoked = false, prepared = false, authoritative = false;
    NativePrimaryValue primary{};
    RiderIdentity rider;
    uint32_t brainHandle = 0, weaponHandle[2]{}, gameHandle = 0;
    void *brain = nullptr, *weapon[2]{}, *game = nullptr;
    int button[2]{-1, -1}, combo = 0, dual = 0, flip = 0;
    uint8_t hands = 0;
    // Identity of the EXISTING snapshot/frozen authority, not another pose sample.
    uint64_t sequence = 0, receivedMs = 0, clientNonce = 0, serverNonce = 0;
    uint32_t generation = 0, trackingGeneration = 0, producer = 0, session = 0, reference = 0;
    uint32_t incarnation = 0, intentEpoch[2]{}, primaryGeneration[2]{};
    struct MeleeHand {
        uint64_t epoch = 0;
        PhysicalGestureSample gesture;
        uint64_t sequence = 0, tickMs = 0, quietSequence = 0, quietTickMs = 0;
        bool candidate = false;
    } melee[2];
    unsigned meleeSlot = 18; // Existing stationary authority owner, never a pointer lease.
    bool bindingCaptured = false, topologyCaptured = false;
    bool gestureSourceCaptured = false;
    bool observer = false;
    bool observerCancellationOnly = false;
    multiplayer::ObserverGestureSample observerSample;
};
struct SimulationInterval {
    void *simulation;
    bool managerPrepared = false, networkPrepared = false;
    std::array<PreparedPlayer, 18> preparedPlayers{};
    SimulationInterval *previous;
    bool failed = false;
    bool *failure = nullptr;
    void *preparedWorld=nullptr,*preparedManager=nullptr;
    uintptr_t nativeCaller=0;
};
static void runPostSimulationRoomscale(SimulationInterval &);
static void runPostSimulationHeadVisibility(SimulationInterval &);
static void runPostSimulationSniper(SimulationInterval &);
static thread_local SimulationInterval *simulationInterval = nullptr;
static thread_local const NativePrimaryInvocation *primaryInvocation = nullptr;
static PreparedPlayer *preparedPrimary(void *subject) noexcept {
    for (auto *interval = simulationInterval; interval; interval = interval->previous)
        for (auto &entry : interval->preparedPlayers)
            if (entry.subject == subject) return &entry;
    return nullptr;
}
static void revokePrimary(void *subject) noexcept {
    // At deletion/carry invalidation, before gameplay getters or callbacks. Active invocation
    // copies remain immutable; only subsequent independent admissions are lost.
    for (auto *interval = simulationInterval; interval; interval = interval->previous)
        for (auto &entry : interval->preparedPlayers)
            if (entry.subject == subject || entry.weapon[0] == subject || entry.weapon[1] == subject)
                nativePrimaryRevoke(entry.primary, entry.revoked);
}
static uint32_t primaryField(const void *object, unsigned offset) noexcept {
    uint32_t value;
    memcpy(&value, static_cast<const uint8_t *>(object) + offset, sizeof(value));
    return value;
}
static bool primaryBindingsCurrent(const PreparedPlayer &entry) {
    // Handle conversion is the existing Core identity seam. All engine getters
    // are outside scalar helpers and are followed by these identity checks.
    if (entry.revoked || !entry.handle || resolve(entry.handle) != entry.subject ||
        entry.revoked || !entry.brainHandle || !entry.brain || resolve(entry.brainHandle) != entry.brain ||
        entry.revoked || !nativeRiderCurrent(entry.subject, entry.rider) || !entry.rider.handheld() ||
        entry.revoked || primaryField(entry.subject, 0x38c) != entry.brainHandle ||
        primaryField(entry.brain, 0x28) != entry.handle || primaryField(entry.subject, 0x564) ||
        (primaryField(entry.subject, 0x10) & 2)) return false;
    for (unsigned hand = 0; hand < 2; ++hand) {
        if (nativeHandle(entry.subject, hand) != entry.weaponHandle[hand]) return false;
        if (!entry.weaponHandle[hand]) continue;
        if (resolve(entry.weaponHandle[hand]) != entry.weapon[hand] || entry.revoked ||
            !entry.weapon[hand] || primaryField(entry.weapon[hand], 0x28) != entry.handle ||
            primaryField(entry.weapon[hand], 0xbc) != hand ||
            (primaryField(entry.weapon[hand], 0x10) & 2)) return false;
    }
    return !entry.revoked;
}
static bool labDualTrace() {
    static const bool enabled=[] { wchar_t value[2]{};return GetEnvironmentVariableW(L"SS2VR_LAB_TRACE",value,2)==1 && value[0]==L'1'; }();
    return enabled;
}
static bool primaryTopology(PreparedPlayer &entry, bool capture) {
    if (!primaryBindingsCurrent(entry) || !isAlive(entry.subject) || entry.revoked ||
        !primaryBindingsCurrent(entry)) return false;
    uint32_t gameHandle = 0;
    nativeGameInfo(&gameHandle);
    if (!primaryBindingsCurrent(entry)) return false;
    void *game = gameHandle ? resolve(gameHandle) : nullptr;
    if (!game || entry.revoked) return false;
    if (!capture && (entry.gameHandle != gameHandle || entry.game != game)) return false;
    const int combo = nativeComboWeapons(game);
    if (!primaryBindingsCurrent(entry) || resolve(gameHandle) != game) return false;
    const int dual = isDual(entry.subject);
    if (!primaryBindingsCurrent(entry)) return false;
    const int flip = nativeFlipButtons(entry.subject);
    if (!primaryBindingsCurrent(entry)) return false;
    int button[2]{-1, -1};
    for (unsigned hand = 0; hand < 2; ++hand) {
        if (!entry.weaponHandle[hand]) continue;
        button[hand] = nativeWeaponButton(entry.subject, entry.weaponHandle[hand]);
        if (!primaryBindingsCurrent(entry) || button[hand] < 0 || button[hand] > 1) return false;
    }
    uint32_t liveGame = 0;
    nativeGameInfo(&liveGame);
    if (entry.revoked || liveGame != gameHandle || resolve(gameHandle) != game ||
        !primaryBindingsCurrent(entry)) return false;
    const bool compatible=entry.hands!=3 || (combo && dual && button[0]!=button[1]);
    // A full bounded lab run can precede the firing sequence. Keep a finite
    // whole-run budget and make exhaustion visible instead of losing evidence.
    static std::atomic<unsigned> topologyReceipts{0};
    const unsigned topologyOrdinal=capture&&labDualTrace()?topologyReceipts.fetch_add(1):~0u;
    if(topologyOrdinal==65536)log("Lab native weapon trace saturated event=topology");
    if(topologyOrdinal<65536) {
        log("Lab native primary topology input=%llu session=%u reference=%u tracking=%u owner=%u weapons=%u,%u receivers=%u,%u hands=%u combo=%d dual=%d flip=%d buttons=%d,%d topology_compatible=%u",
            static_cast<unsigned long long>(entry.sequence),entry.session,entry.reference,entry.authoritative?entry.trackingGeneration:entry.generation,
            entry.handle,entry.weaponHandle[0],entry.weaponHandle[1],uint32_t(reinterpret_cast<uintptr_t>(entry.weapon[0])),
            uint32_t(reinterpret_cast<uintptr_t>(entry.weapon[1])),entry.hands,combo,dual,flip,button[0],button[1],compatible);
    }
    if(!compatible)return false;
    if (capture) {
        entry.gameHandle = gameHandle; entry.game = game;
        entry.combo = combo; entry.dual = dual; entry.flip = flip;
        entry.button[0] = button[0]; entry.button[1] = button[1];
        return true;
    }
    // Validation only: never remap an already captured high.
    return entry.combo == combo && entry.dual == dual && entry.flip == flip &&
           entry.button[0] == button[0] && entry.button[1] == button[1];
}
static uint8_t primarySourceHands(PreparedPlayer &entry, bool capture, bool *valid = nullptr) {
    if (valid) *valid = false;
    if (entry.revoked) return 0;
    if (entry.observer) {
        // Replica manual commands are already delivered by the native engine.
        // No server liveIntentEpoch or projected manual high is fabricated.
        if (valid) *valid = entry.observerSample.latest.valid;
        return 0;
    }
    uint8_t fire = 0;
    if (entry.authoritative) {
        if (singlePlayer() || !multiplayer::server() || entry.revoked) return 0;
        const auto source = copyAuthority(entry.subject);
        const auto live = multiplayer::authority(entry.subject);
        if (entry.revoked || source.player != entry.handle || source.rider != entry.rider ||
            source.sample.avatar != entry.handle || !source.sample.negotiated || !live.negotiated) return 0;
        const auto &sample = source.sample;
        if (capture) {
            entry.sequence = sample.pose.sequence; entry.receivedMs = sample.receivedMs;
            entry.clientNonce = sample.pose.clientNonce; entry.serverNonce = sample.pose.serverNonce;
            entry.incarnation = sample.incarnation; entry.generation = source.weaponGeneration;
            entry.trackingGeneration = sample.pose.trackingGeneration;
            for (unsigned hand = 0; hand < 2; ++hand) entry.intentEpoch[hand] = sample.pose.intentEpoch[hand];
        }
        if (entry.sequence != sample.pose.sequence || entry.receivedMs != sample.receivedMs ||
            entry.clientNonce != sample.pose.clientNonce || entry.serverNonce != sample.pose.serverNonce ||
            entry.incarnation != sample.incarnation || entry.generation != source.weaponGeneration ||
            entry.trackingGeneration != sample.pose.trackingGeneration) return 0;
        if (valid) *valid = sample.valid && live.valid;
        for (unsigned hand = 0; hand < 2; ++hand)
            if (source.hand[hand] == entry.weaponHandle[hand] &&
                entry.intentEpoch[hand] == sample.pose.intentEpoch[hand] &&
                currentWeaponSample(sample, live, hand) &&
                (sample.pose.fireMask & live.pose.fireMask & (1u << hand))) fire |= uint8_t(1u << hand);
    } else {
        if (!primaryBindingsCurrent(entry) || !local(entry.subject) ||
            !primaryBindingsCurrent(entry)) return 0;
        const auto source = copySnapshot();
        if (source.player != entry.subject || source.playerHandle != entry.handle ||
            source.rider != entry.rider || !vrSession(source) || !fresh(source.input) ||
            !source.ui.gameplay || recenterHeld(source.input)) return 0;
        if (capture) {
            entry.sequence = source.input.sequence; entry.receivedMs = source.input.tickMs;
            entry.generation = source.generation; entry.producer = source.inputProducer;
            entry.session = source.input.session; entry.reference = source.input.reference;
            for (unsigned hand = 0; hand < 2; ++hand) {
                entry.intentEpoch[hand] = source.intentEpoch[hand];
                entry.primaryGeneration[hand] = source.input.primaryInputGeneration[hand];
            }
        }
        if (entry.sequence != source.input.sequence || entry.receivedMs != source.input.tickMs ||
            entry.generation != source.generation || entry.producer != source.inputProducer ||
            entry.session != source.input.session || entry.reference != source.input.reference) return 0;
        if (valid) *valid = true;
        for (unsigned hand = 0; hand < 2; ++hand)
            if (source.handle[hand] == entry.weaponHandle[hand] && source.fire[hand] &&
                source.input.handValid[hand] && finite(source.input.hand[hand]) &&
                !source.selecting[hand] && !source.ui.wheel[hand].open &&
                entry.intentEpoch[hand] == source.intentEpoch[hand] &&
                entry.primaryGeneration[hand] == source.input.primaryInputGeneration[hand] &&
                (!multiplayer::remoteClient() || multiplayer::localPrimaryAllowed(entry.subject, hand,
                    entry.intentEpoch[hand], entry.primaryGeneration[hand], entry.sequence)))
                fire |= uint8_t(1u << hand);
    }
    return entry.revoked ? 0 : fire;
}
static bool primaryWeaponReferences(PreparedPlayer &entry, uint8_t fire) {
    if (!entry.authoritative) return true;
    for (unsigned hand = 0; hand < 2; ++hand) {
        if (!(fire & entry.hands & (1u << hand))) continue;
        Pose body, camera, model;
        if (!primaryBindingsCurrent(entry) ||
            !nativeWeaponReference(entry.subject, entry.weapon[hand], body, camera, model,hand) ||
            !primaryBindingsCurrent(entry)) return false;
    }
    return true;
}
struct MeleeTicket {
    NativeConsumptionBindingKey key;
    uint64_t epoch = 0;
    NativeConsumptionBinding::Receipt receipt;
    bool authoritative = false;
    unsigned slot = 18;
};
static SRWLOCK *meleeRecordLock(bool authoritative) noexcept {
    return authoritative ? &authorityLock : &snapshotLock;
}
static MeleeHandRecord *meleeRecord(bool authoritative, unsigned slot, unsigned hand) noexcept {
    if (hand >= 2) return nullptr;
    if (!authoritative) return &localSnapshotOwner.melee[hand];
    return slot < authorities.size() ? &authorities[slot].melee[hand] : nullptr;
}
struct MeleeDispatchFrame {
    MeleeTicket ticket;
    void *weapon = nullptr;
    MeleeDispatchFrame *previous = nullptr;
    bool release = false, committed = false;
};
struct MeleeStepFrame {
    PreparedPlayer *entry = nullptr;
    void *weapon = nullptr;
    unsigned hand = 2;
    uint64_t epoch = 0;
    MeleeTicket observation;
    bool level = false, observe = false;
    MeleeStepFrame *previous = nullptr;
};
struct MeleeHeldFrame {
    PreparedPlayer *entry = nullptr;
    uint32_t weapon = 0;
    unsigned hand = 2;
    uint64_t epoch = 0;
    MeleeHeldFrame *previous = nullptr;
};
static thread_local MeleeDispatchFrame *meleeDispatch = nullptr;
static thread_local MeleeStepFrame *meleeStep = nullptr;
static thread_local MeleeHeldFrame *meleeHeld = nullptr;
static bool meleePreparationAllowed(const PreparedPlayer &entry) {
    return settings.physicalMelee && meleeConfigured &&
        (entry.observer ? (multiplayer::remoteClient() &&
            (entry.observerSample.latest.negotiated || entry.observerCancellationOnly)) :
         entry.authoritative ? multiplayer::server() :
            (local(entry.subject) && (singlePlayer() || multiplayer::negotiatedLocal()))) &&
        nativeInputHealthy() && nativeMainThread && nativeMainThread() &&
        simulationInterval && !simulationInterval->previous && zoomManagerFrame &&
        zoomManagerFrame->current && entry.recognized && !entry.revoked;
}
static bool meleeBindingTarget(PreparedPlayer &entry, unsigned hand) {
    if (hand >= 2 || !primaryBindingsCurrent(entry) ||
        entry.button[hand] < 0 || entry.button[hand] > 1 || unsigned(entry.flip) > 1 ||
        !entry.weapon[hand] || primaryField(entry.weapon[hand], 0) != sawVtable ||
        primaryField(entry.weapon[hand], 0xb4) != 0) return false;
    const unsigned semantic = unsigned(entry.button[hand] ^ entry.flip);
    return nativePrimarySingleTarget(semantic, entry.weaponHandle[1], entry.weaponHandle[0],
                                    entry.combo && entry.dual, entry.weaponHandle[hand]);
}
static bool meleeTargetCurrent(PreparedPlayer &entry, unsigned hand) {
    return meleePreparationAllowed(entry) && meleeBindingTarget(entry, hand);
}
static NativeConsumptionBindingKey meleeKey(const PreparedPlayer &entry, unsigned hand) noexcept {
    return {entry.handle, entry.weaponHandle[hand], hand};
}
static bool meleeOwnerMatches(const MeleeHandRecord &owner, const PreparedPlayer &entry,
                             unsigned hand) noexcept {
    return hand < 2 && owner.epoch && owner.epoch == entry.melee[hand].epoch &&
        owner.player == entry.subject && owner.weapon == entry.weapon[hand] &&
        owner.world == simulationInterval->preparedWorld && owner.rider == entry.rider &&
        owner.consumption.matches(meleeKey(entry, hand));
}
static MeleeTicket meleeTicket(PreparedPlayer &entry, unsigned hand, bool gestureReconcile = false) noexcept {
    MeleeTicket result;
    if (hand >= 2 || !simulationInterval) return result;
    auto *lock = meleeRecordLock(entry.authoritative);
    AcquireSRWLockShared(lock);
    const auto *owner = meleeRecord(entry.authoritative, entry.meleeSlot, hand);
    if (owner && meleeOwnerMatches(*owner, entry, hand) &&
        (!gestureReconcile || (!owner->blocked && !owner->teardownDepth)))
        result = {owner->key, owner->epoch, owner->consumption.prepare(owner->key),
                  entry.authoritative, entry.meleeSlot};
    ReleaseSRWLockShared(lock);
    return result;
}
static bool completeMeleeTicket(const MeleeTicket &ticket, bool high) noexcept {
    if (!ticket.epoch || ticket.key.hand >= 2) return false;
    auto *lock = meleeRecordLock(ticket.authoritative);
    AcquireSRWLockExclusive(lock);
    auto *record = meleeRecord(ticket.authoritative, ticket.slot, ticket.key.hand);
    const bool completed = record && record->epoch == ticket.epoch && record->key == ticket.key &&
        record->consumption.complete(ticket.key, ticket.receipt, high);
    if (completed && !high) {
        auto &owner = *record;
        owner.resetGesture = true; // Old prepared swings cannot survive a real native stop.
        owner.gestureArmed = false;
        owner.quietAfter = owner.gestureSequence;
    }
    ReleaseSRWLockExclusive(lock);
    return completed;
}
static bool observeMeleeLevel(const MeleeTicket &ticket, bool high) noexcept {
    if (!ticket.epoch || ticket.key.hand >= 2) return false;
    auto *lock = meleeRecordLock(ticket.authoritative);
    AcquireSRWLockExclusive(lock);
    auto *owner = meleeRecord(ticket.authoritative, ticket.slot, ticket.key.hand);
    // Normal native step completion observes an already completed, unchanged
    // level. It cannot create an initial level or supply a release/quiet witness.
    bool completed = false;
    if (owner && owner->epoch == ticket.epoch && owner->key == ticket.key &&
        owner->consumption.edge(ticket.key, high) == NativeConsumptionBinding::Edge::None)
        completed = owner->consumption.complete(ticket.key, ticket.receipt, high);
    ReleaseSRWLockExclusive(lock);
    return completed;
}
static void abortMeleeTicket(const MeleeTicket &ticket) noexcept {
    if (!ticket.epoch || ticket.key.hand >= 2) return;
    auto *lock = meleeRecordLock(ticket.authoritative);
    AcquireSRWLockExclusive(lock);
    auto *owner = meleeRecord(ticket.authoritative, ticket.slot, ticket.key.hand);
    if (owner && owner->epoch == ticket.epoch && owner->key == ticket.key &&
        owner->consumption.retire(ticket.key, ticket.receipt)) {
        owner->resetGesture = true;
        owner->blocked = true;
    }
    ReleaseSRWLockExclusive(lock);
}
static void prepareMelee(PreparedPlayer &entry, bool sourceCurrent) {
    if (!meleePreparationAllowed(entry) || !primaryBindingsCurrent(entry)) return;
    const auto source = copySnapshot();
    const auto authority = entry.authoritative ? copyAuthority(entry.subject) : Authority{};
    const bool sourceOwner = entry.authoritative ?
        (authority.player == entry.handle && authority.rider == entry.rider &&
         authority.sample.negotiated && authority.sample.incarnation == entry.incarnation) :
        (source.initialized && source.player == entry.subject && source.playerHandle == entry.handle &&
         source.rider == entry.rider);
    if (entry.authoritative)
        for (unsigned hand = 0; hand < 2; ++hand) {
            const auto bit = uint8_t(1u << hand);
            const auto &pose = entry.observer ?
                (entry.observerSample.receipt[hand].hand == hand ? entry.observerSample.pulse[hand] :
                    entry.observerSample.latest.pose) : authority.sample.pose;
            auto &capture = entry.melee[hand];
            capture.gesture = {bool(pose.gestureEligibleMask & bit),
                entry.observer ? network::ObserverGestureIntents::level(pose, hand,
                    entry.observerSample.receipt[hand].hand == hand) :
                    bool((pose.gestureDownMask | pose.gesturePulseMask) & bit), false, false,
                pose.gestureGeneration[hand]};
            capture.sequence = pose.gestureSequence[hand]; capture.tickMs = pose.gestureTickMs[hand];
            capture.quietSequence = pose.gestureQuietSequence[hand];
            capture.quietTickMs = pose.gestureQuietTickMs[hand];
        }
    bool targets[2]{};
    for (unsigned hand = 0; hand < 2; ++hand) targets[hand] = meleeTargetCurrent(entry, hand);
    if (!primaryBindingsCurrent(entry)) return;
    auto *lock = meleeRecordLock(entry.authoritative);
    AcquireSRWLockExclusive(lock);
    if (entry.authoritative) {
        entry.meleeSlot = 18;
        for (unsigned slot = 0; slot < authorities.size(); ++slot)
            if (authorities[slot].payload.player == entry.handle &&
                authorities[slot].payload.sample.incarnation == entry.incarnation) {
                entry.meleeSlot = slot; break;
            }
        if (entry.meleeSlot >= authorities.size()) {
            ReleaseSRWLockExclusive(lock); return;
        }
    }
    auto &nextEpoch = entry.authoritative ? authorities[entry.meleeSlot].nextMeleeEpoch :
                                          localSnapshotOwner.nextMeleeEpoch;
    // Native consumption is stationary and independent of the physical sampler.
    // Only the chosen execution role observes this weapon's callbacks.
    for (unsigned hand = 0; hand < 2; ++hand) {
        auto &owner = *meleeRecord(entry.authoritative, entry.meleeSlot, hand);
        const auto key = meleeKey(entry, hand);
        const bool same = targets[hand] && owner.player == entry.subject &&
            owner.weapon == entry.weapon[hand] && owner.world == simulationInterval->preparedWorld &&
            owner.rider == entry.rider && owner.consumption.matches(key);
        if (!same) {
            owner.consumption.retire();
            owner.blocked = true; owner.epoch = 0;
            owner.player = owner.weapon = owner.world = nullptr;
            owner.teardownDepth = 0; owner.resetGesture = false;
            owner.gestureArmed = false; owner.gestureGeneration = 0;
            owner.gestureSequence = owner.quietAfter = 0;
            if (sourceOwner && !entry.observerCancellationOnly && targets[hand] && nextEpoch) {
                const auto epoch = nextEpoch; nextEpoch = nextConsumptionRevision(epoch);
                if (owner.consumption.activate(key, epoch)) {
                    owner.key = key; owner.epoch = epoch; owner.player = entry.subject;
                    owner.weapon = entry.weapon[hand]; owner.world = simulationInterval->preparedWorld;
                    owner.rider = entry.rider; owner.blocked = false;
                }
            }
        }
        auto &capture = entry.melee[hand];
        capture.candidate = targets[hand] && owner.epoch && owner.consumption.matches(key);
        capture.epoch = owner.epoch;
        if (!capture.candidate) continue;
        if (owner.resetGesture) {
            // A native stop may complete after this quiet capture, during an
            // unlocked getter/submit. Fence the entire pre-preparation sample;
            // only a later source quiet can rearm this native consumer.
            owner.gestureArmed = false;
            owner.quietAfter = std::max(owner.quietAfter, capture.sequence);
            owner.resetGesture = false;
        }
        if (owner.gestureGeneration != capture.gesture.generation) {
            owner.gestureArmed = false;
            owner.gestureGeneration = capture.gesture.generation;
            owner.quietAfter = capture.sequence;
            owner.gestureSequence = capture.sequence;
        }
        owner.gestureSequence = std::max(owner.gestureSequence, capture.sequence);
        const auto bit = uint8_t(1u << hand);
        const bool allowed = sourceOwner && sourceCurrent && capture.gesture.eligible &&
            !owner.blocked && !owner.teardownDepth &&
            (entry.authoritative ? !(authority.sample.pose.wheelOrEquipBlockedMask & bit) :
                (!source.selecting[hand] && !source.ui.wheel[hand].open && !(source.input.blockedWheels & bit)));
        const bool ownQuiet = nativeGestureQuietWitness(owner.quietAfter, capture.sequence, capture.tickMs,
                                                        capture.quietSequence, capture.quietTickMs);
        if (allowed && ownQuiet) owner.gestureArmed = true;
        if (!allowed || !owner.gestureArmed || !ownQuiet) capture.gesture.down = false;
    }
    ReleaseSRWLockExclusive(lock);
}
static bool meleeGestureCurrent(PreparedPlayer &entry, unsigned hand) {
    if (hand >= 2 || !entry.melee[hand].candidate || !entry.melee[hand].gesture.eligible ||
        !entry.melee[hand].gesture.down || !meleeTargetCurrent(entry, hand)) return false;
    bool sourceCurrent = false;
    primarySourceHands(entry, false, &sourceCurrent);
    if (!sourceCurrent || !primaryTopology(entry, false)) return false;
    const auto &captured = entry.melee[hand];
    if (entry.observer) {
        if (!multiplayer::observerGesturesCurrent(entry.observerSample, hand)) return false;
        AcquireSRWLockShared(&authorityLock);
        const auto *owner = meleeRecord(true, entry.meleeSlot, hand);
        const bool valid = owner && meleeOwnerMatches(*owner, entry, hand) && owner->gestureArmed &&
            !owner->blocked && !owner->teardownDepth && !owner->resetGesture;
        ReleaseSRWLockShared(&authorityLock);
        return valid;
    }
    if (entry.authoritative) {
        const auto authority = copyAuthority(entry.subject);
        const auto live = multiplayer::authority(entry.subject);
        const auto &pose = live.pose;
        const auto bit = uint8_t(1u << hand);
        if (!currentWeaponSample(authority.sample, live, hand) ||
            pose.gestureGeneration[hand] != captured.gesture.generation ||
            pose.gestureSequence[hand] != captured.sequence || pose.gestureTickMs[hand] != captured.tickMs ||
            !(pose.gestureEligibleMask & bit) || !((pose.gestureDownMask | pose.gesturePulseMask) & bit) ||
            (pose.wheelOrEquipBlockedMask & bit)) return false;
        AcquireSRWLockShared(&authorityLock);
        const auto *owner = meleeRecord(true, entry.meleeSlot, hand);
        const bool valid = owner && meleeOwnerMatches(*owner, entry, hand) && owner->gestureArmed &&
            !owner->blocked && !owner->teardownDepth && !owner->resetGesture;
        ReleaseSRWLockShared(&authorityLock);
        return valid;
    }
    const auto source = copySnapshot();
    if (source.selecting[hand] || source.ui.wheel[hand].open || (source.input.blockedWheels & (1u << hand)) ||
        (multiplayer::remoteClient() && !multiplayer::localGestureAllowed(entry.subject, hand,
            entry.intentEpoch[hand], captured.gesture.generation, captured.sequence, entry.sequence))) return false;
    const auto now = GetTickCount64();
    AcquireSRWLockShared(&snapshotLock);
    const auto &owner = localSnapshotOwner.melee[hand];
    const bool valid = meleeOwnerMatches(owner, entry, hand) && owner.gestureArmed &&
        !owner.gestureInvalidated && !owner.blocked && !owner.teardownDepth && !owner.resetGesture &&
        owner.gesture.current(captured.gesture, source.input, source.gripPoseGeneration[hand],
            {entry.handle, entry.weaponHandle[hand], source.generation, hand}, source.inputProducer, now);
    ReleaseSRWLockShared(&snapshotLock);
    return valid;
}
static void refreshMelee(PreparedPlayer &entry) {
    for (unsigned hand = 0; hand < 2; ++hand)
        if (entry.melee[hand].gesture.down && !meleeGestureCurrent(entry, hand)) {
            entry.melee[hand].gesture.down = false;
            entry.melee[hand].gesture.eligible = false;
        }
}
static void dispatchMelee(PreparedPlayer &entry, unsigned hand, bool high,
                          CanonicalPrimary original, bool gestureReconcile = false) {
    const auto ticket = meleeTicket(entry, hand, gestureReconcile);
    if (!ticket.epoch) {
        if (!gestureReconcile) original(entry.subject, entry.button[hand] ^ entry.flip);
        return;
    }
    if (gestureReconcile)
        for (auto *active = meleeDispatch; active; active = active->previous)
            if (nativeConsumptionDispatchMatches(active->ticket.key,active->ticket.epoch,
                active->ticket.authoritative,active->ticket.slot,ticket.key,ticket.epoch,
                ticket.authoritative,ticket.slot))
                return; // Actual completion, never a speculative High, resolves this invocation.
    MeleeDispatchFrame frame{ticket, entry.weapon[hand], meleeDispatch, !high, false};
    meleeDispatch = &frame;
    withNativeFinally([&] {
        original(entry.subject, entry.button[hand] ^ entry.flip);
        if (high && primaryBindingsCurrent(entry)) frame.committed = completeMeleeTicket(ticket, true);
        if (high && frame.committed && entry.observer)
            multiplayer::finishObserverGesture(entry.observerSample, hand);
        if (!frame.committed) abortMeleeTicket(ticket);
    }, [&](bool aborted) noexcept {
        meleeDispatch = frame.previous;
        if (aborted) {
            abortMeleeTicket(ticket);
            // Scalar native retirement only. The next normal preparation
            // discards a now-inadmissible retained observer edge (or it expires).
            nativeInputFailed();
        }
    });
}
static void reconcileMelee(PreparedPlayer &entry, unsigned hand, bool manual) {
    if (!entry.melee[hand].candidate || !meleeTargetCurrent(entry, hand) ||
        !primaryTopology(entry, false)) return;
    const bool physical = meleeGestureCurrent(entry, hand);
    if (!meleeTargetCurrent(entry, hand) || !primaryTopology(entry, false)) return;
    NativeConsumptionBinding::Edge edge = NativeConsumptionBinding::Edge::Unknown;
    auto *lock = meleeRecordLock(entry.authoritative);
    AcquireSRWLockShared(lock);
    const auto *owner = meleeRecord(entry.authoritative, entry.meleeSlot, hand);
    if (owner && meleeOwnerMatches(*owner, entry, hand) && !owner->blocked && !owner->teardownDepth) {
        // Getter reentry above can complete a stop without changing topology.
        // Recheck the receiver fence after the final native validation; withdraw
        // only G, so manual demand and genuine release remain reconcilable.
        const auto &capture = entry.melee[hand];
        const bool admittedPhysical = physical && nativeGestureFenceCurrent(owner->gestureArmed,
            owner->resetGesture,owner->gestureGeneration == capture.gesture.generation,
            owner->quietAfter,capture.sequence,capture.tickMs,capture.quietSequence,capture.quietTickMs);
        const bool desired = manual || admittedPhysical;
        bool inFlight = false;
        for (auto *active = meleeDispatch; active; active = active->previous)
            if (nativeConsumptionDispatchMatches(active->ticket.key,active->ticket.epoch,
                active->ticket.authoritative,active->ticket.slot,owner->key,owner->epoch,
                entry.authoritative,entry.meleeSlot))
                inFlight = true;
        edge = nativeGestureReconcileEdge(owner->consumption.edge(owner->key, desired), true, admittedPhysical, inFlight);
    }
    ReleaseSRWLockShared(lock);
    if (edge == NativeConsumptionBinding::Edge::Press)
        dispatchMelee(entry, hand, true, originalPrimaryPress, true);
    else if (edge == NativeConsumptionBinding::Edge::Release)
        dispatchMelee(entry, hand, false, originalPrimaryRelease, true);
}
static bool capturePrimaryBinding(PreparedPlayer &entry) {
    if (entry.bindingCaptured)
        return primaryBindingsCurrent(entry);
    if (!entry.handle || resolve(entry.handle) != entry.subject || entry.revoked) return false;
    entry.brainHandle = primaryField(entry.subject, 0x38c);
    entry.brain = entry.brainHandle ? resolve(entry.brainHandle) : nullptr;
    if (entry.revoked || !readNativeRider(entry.subject, entry.rider) || entry.revoked) return false;
    entry.hands = 0;
    for (unsigned hand = 0; hand < 2; ++hand) {
        entry.weaponHandle[hand] = nativeHandle(entry.subject, hand);
        entry.weapon[hand] = entry.weaponHandle[hand] ? resolve(entry.weaponHandle[hand]) : nullptr;
        if (entry.weaponHandle[hand]) entry.hands |= uint8_t(1u << hand);
    }
    if (!entry.hands || (entry.hands == 3 && entry.weapon[0] == entry.weapon[1]) ||
        !primaryBindingsCurrent(entry)) return false;
    entry.bindingCaptured = true;
    return true;
}
static void prepareLocalGestureSource(Snapshot &source) {
    if (!meleeConfigured || !settings.physicalMelee) return;
    auto *entry = preparedPrimary(source.player);
    if (!entry || entry->prepared || entry->gestureSourceCaptured) return;
    entry->gestureSourceCaptured = true;
    entry->recognized |= primaryLocalRecognized(source, source.player, source.playerHandle);
    const auto *frame = zoomManagerFrame;
    const bool phase = meleeConfigured && settings.physicalMelee && entry->recognized && !entry->revoked &&
        frame && frame->current && frame->preparation && !frame->previous && simulationInterval &&
        !simulationInterval->previous && nativeMainThread && nativeMainThread() && nativeInputHealthy() &&
        currentSimulation() == frame->simulation && currentWorld() == frame->world;
    bool allowed = phase && source.initialized && source.ui.gameplay && fresh(source.input) &&
        source.player == entry->subject && source.playerHandle == entry->handle &&
        source.rider.handheld() && !recenterHeld(source.input);
    if (allowed) {
        allowed = capturePrimaryBinding(*entry) &&
            primaryTopology(*entry, !entry->topologyCaptured) && primaryBindingsCurrent(*entry);
        if (allowed) entry->topologyCaptured = true;
    }
    bool target[2]{};
    if (allowed)
        for (unsigned hand = 0; hand < 2; ++hand)
            target[hand] = source.handle[hand] == entry->weaponHandle[hand] &&
                meleeBindingTarget(*entry, hand);
    if (allowed && !primaryBindingsCurrent(*entry)) target[0] = target[1] = false;
    const auto now = GetTickCount64();
    AcquireSRWLockExclusive(&snapshotLock);
    for (unsigned hand = 0; hand < 2; ++hand) {
        auto &owner = localSnapshotOwner.melee[hand];
        const bool usable = target[hand] && !owner.gestureInvalidated && !source.selecting[hand] &&
            !source.ui.wheel[hand].open && !(source.input.blockedWheels & (1u << hand));
        const auto sample = owner.gesture.sample(source.input, source.gripPoseGeneration[hand],
            {source.playerHandle, source.handle[hand], source.generation, hand},
            source.inputProducer, now, usable);
        owner.gestureInvalidated = false;
        if (target[hand]) {
            owner.gesturePlayer = entry->subject; owner.gestureWeapon = entry->weapon[hand];
        }
        if (owner.quietGeneration != sample.generation) {
            owner.quietGeneration = sample.generation;
            owner.quietSequence = owner.quietTickMs = 0;
        }
        if (sample.eligible && sample.fresh && sample.quiet) {
            owner.quietSequence = source.input.sequence; owner.quietTickMs = source.input.tickMs;
        }
        auto &capture = entry->melee[hand];
        capture.gesture = sample;
        capture.sequence = source.input.sequence; capture.tickMs = source.input.tickMs;
        capture.quietSequence = owner.quietSequence; capture.quietTickMs = owner.quietTickMs;
    }
    ReleaseSRWLockExclusive(&snapshotLock);
}
static void appendLocalGesturePose(const Snapshot &source, network::PosePacket &pose) {
    if (!meleeConfigured || !settings.physicalMelee) return;
    auto *entry = preparedPrimary(source.player);
    if (!entry || entry->revoked || !entry->gestureSourceCaptured) return;
    AcquireSRWLockShared(&snapshotLock);
    for (unsigned hand = 0; hand < 2; ++hand) {
        const auto &capture = entry->melee[hand];
        const auto &physical = localSnapshotOwner.melee[hand];
        const auto bit = uint8_t(1u << hand);
        if (!capture.gesture.eligible || physical.gestureInvalidated ||
            source.handle[hand] != entry->weaponHandle[hand] || source.ui.currentWeapon[hand] != 0 ||
            !(pose.validMask & (bit << 1)) || (pose.wheelOrEquipBlockedMask & bit) ||
            !physical.gesture.current(capture.gesture, source.input, source.gripPoseGeneration[hand],
                {source.playerHandle, source.handle[hand], source.generation, hand}, source.inputProducer,
                GetTickCount64())) continue;
        pose.gestureEligibleMask |= bit;
        if (capture.gesture.down) pose.gestureDownMask |= bit;
        pose.gestureGeneration[hand] = capture.gesture.generation;
        pose.gestureSequence[hand] = capture.sequence; pose.gestureTickMs[hand] = capture.tickMs;
        if (capture.quietSequence) {
            pose.gestureQuietMask |= bit;
            pose.gestureQuietSequence[hand] = capture.quietSequence;
            pose.gestureQuietTickMs[hand] = capture.quietTickMs;
        }
    }
    ReleaseSRWLockShared(&snapshotLock);
}
// Two weapons the native couples to one Fire command (dual pistols without
// combo weapons): either trigger then fires, as the native alternates guns.
static std::atomic<bool> coupledNativeFire{false};
// Bounded transition trace of the per-interval primary (fire) preparation.
static void tracePrimary(const char *stage, uint8_t fire, const PreparedPlayer &entry) {
    static unsigned traced = 0;
    static const char *previousStage = nullptr;
    static unsigned previousState = ~0u;
    const unsigned state = unsigned(fire) | unsigned(entry.primary.bits) << 2 | unsigned(entry.hands) << 4 |
        unsigned(entry.button[0] + 1) << 6 | unsigned(entry.button[1] + 1) << 9;
    if ((stage == previousStage && state == previousState) || traced >= 200) return;
    previousStage = stage;
    previousState = state;
    ++traced;
    log("Input primary stage=%s fire=%u hands=%u buttons=%d,%d combo=%d dual=%d bits=%u mask=%u", stage, fire,
        entry.hands, entry.button[0], entry.button[1], entry.combo, entry.dual, entry.primary.bits, entry.primary.mask);
}
static void preparePrimary(PreparedPlayer &entry, bool authoritative) {
    // Recognition/neutral were reserved before update/authorityStep. Only this
    // initial preparation can introduce high; native reentry sees that neutral.
    if (!entry.recognized || entry.revoked || entry.prepared) return;
    entry.prepared = true; // One attempt, including failed/neutral preparation.
    entry.authoritative = authoritative;
    // A topology the per-hand projection cannot represent (no binding, or
    // coupled native dual wielding without combo weapons) is left unclaimed:
    // the native manual commands already carry the VR triggers through the
    // command hooks, so firing keeps native semantics instead of going silent.
    const auto unclaimed = [&](const char *stage, uint8_t fire) {
        entry.primary = {};
        coupledNativeFire.store(entry.hands == 3, std::memory_order_relaxed);
        tracePrimary(stage, fire, entry);
    };
    if (!capturePrimaryBinding(entry)) return unclaimed("binding", 0);
    bool sourceCurrent = false;
    const uint8_t fire = primarySourceHands(entry, true, &sourceCurrent) & entry.hands;
    if (!fire && !meleePreparationAllowed(entry)) return tracePrimary("idle", fire, entry);
    if (!primaryTopology(entry, !entry.topologyCaptured)) return unclaimed("topology", fire);
    if (!primaryWeaponReferences(entry, fire)) return unclaimed("weapons", fire);
    if (!nativeInputHealthy() || !primaryBindingsCurrent(entry)) return tracePrimary("bindings", fire, entry);
    entry.topologyCaptured = true;
    const uint8_t liveFire = primarySourceHands(entry, false);
    if (!nativeInputHealthy() || !primaryBindingsCurrent(entry)) return tracePrimary("live", fire, entry);
    entry.primary = nativePrimaryProjection(entry.hands, fire & liveFire,
        entry.button[0], entry.button[1], entry.combo && entry.dual);
    coupledNativeFire.store(false, std::memory_order_relaxed);
    if (entry.revoked) entry.primary.bits = 0;
    tracePrimary(entry.revoked ? "revoked" : "projected", fire & liveFire, entry);
    if (!entry.revoked) prepareMelee(entry, sourceCurrent);
}
static void prepareObserverMelee(PreparedPlayer &entry) {
    if (!meleeConfigured || !settings.physicalMelee || entry.prepared || entry.revoked ||
        !multiplayer::remoteClient()) return;
    entry.observerSample = multiplayer::observerGestures(entry.handle);
    const auto stored = copyAuthority(entry.subject);
    entry.observerCancellationOnly = !entry.observerSample.latest.negotiated && stored.observer &&
        stored.player == entry.handle && authorityMeleeReleaseObligation(entry.subject, entry.handle);
    if (!entry.observerSample.latest.negotiated && !entry.observerCancellationOnly) return;
    entry.observer = true; entry.authoritative = true; entry.recognized = true; entry.prepared = true;
    entry.primary = {}; // Native manual/history stays unclaimed on the replica.
    entry.incarnation = entry.observerCancellationOnly ? stored.sample.incarnation :
        entry.observerSample.latest.incarnation;
    if (capturePrimaryBinding(entry) && primaryTopology(entry, true) && primaryBindingsCurrent(entry)) {
        entry.topologyCaptured = true;
        if (!entry.observerCancellationOnly) {
            Authority value;
            value.player = entry.handle; value.hand[0] = entry.weaponHandle[0]; value.hand[1] = entry.weaponHandle[1];
            value.rider = entry.rider; value.sample = entry.observerSample.latest; value.observer = true;
            saveAuthority(value); // Reuses stationary rows; never freezes a server packet.
        }
        prepareMelee(entry, !entry.observerCancellationOnly && entry.observerSample.latest.valid);
    }
    for (unsigned hand = 0; hand < 2; ++hand)
        if (entry.observerSample.receipt[hand].hand == hand &&
            (!entry.melee[hand].candidate || !entry.melee[hand].gesture.down))
            multiplayer::finishObserverGesture(entry.observerSample, hand); // Explicit admission discard.
}
static void refreshPrimary(PreparedPlayer &entry) {
    refreshMelee(entry);
    if (!entry.prepared || entry.revoked || !entry.primary.bits) return;
    if (!primaryBindingsCurrent(entry)) {
        nativePrimaryRevoke(entry.primary, entry.revoked);
        return;
    }
    const uint8_t fire = primarySourceHands(entry, false);
    if (!primaryTopology(entry, false) || !primaryWeaponReferences(entry, fire) ||
        !nativeInputHealthy() || !primaryBindingsCurrent(entry)) {
        nativePrimaryRevoke(entry.primary, entry.revoked);
        return;
    }
    const uint8_t liveFire = primarySourceHands(entry, false);
    if (!nativeInputHealthy() || !primaryBindingsCurrent(entry)) {
        nativePrimaryRevoke(entry.primary, entry.revoked);
        return;
    }
    entry.primary.bits &= nativePrimaryProjection(entry.hands, fire & liveFire,
        entry.button[0], entry.button[1], entry.combo && entry.dual).bits;
    if (entry.revoked) entry.primary.bits = 0;
}
static NativePrimaryValue primaryAdmission(void *subject, uintptr_t caller, bool held,
                                          NativePrimaryValue &reserved) {
    // The wrapper has already honored an immutable same-pawn ancestor. For a
    // new invocation, establish CURRENT carry ownership before any validation
    // getter or recognized-neutral reservation, even after interval preparation.
    // Native 8E6C0/8E6F6 resolves +564 before choosing prepare/throw callbacks.
    const uint32_t carryHandle = primaryField(subject, 0x564);
    const bool resolvedCarry = carryHandle && resolve(carryHandle);
    if (nativePrimaryOwnership(false, resolvedCarry) == NativePrimaryOwnership::Carry) {
        // Passthrough belongs to this carry invocation, not the prepared record.
        // Carry ending later in this interval must not resurrect its old high.
        revokePrimary(subject);
        return {};
    }
    auto *entry = preparedPrimary(subject);
    if (entry && entry->observer) return {}; // Preserve replica native manual input.
    const auto snapshot = copySnapshot();
    const uint32_t handle = entry ? entry->handle : pointerHandle(subject);
    bool recognized = (entry && entry->recognized) || primaryLocalRecognized(snapshot, subject, handle);
    // No native game callbacks in knownVrAvatar. Handle conversion does not
    // acquire gameplay state or sample input.
    if (!recognized) recognized = multiplayer::knownVrAvatar(handle);
    if (nativePrimaryOwnership(recognized, resolvedCarry) != NativePrimaryOwnership::Handheld) return {};
    // Recognition is the boundary: reentry before it remains unclaimed; every
    // gameplay getter below sees this recognized-neutral reservation instead.
    reserved = NativePrimaryValue::neutral();
    auto value = reserved;
    if (entry) entry->recognized = true;
    const auto *frame = zoomManagerFrame;
    const unsigned refused = !entry ? 1 : !entry->prepared ? 2 : entry->revoked ? 3 : !nativeInputHealthy() ? 4 :
        caller != (held ? primaryHeldReturn : primaryOperatorReturn) ? 5 :
        (!frame || frame->previous || !frame->current || !frame->execution || frame->preparation) ? 6 :
        (!simulationInterval || simulationInterval->previous) ? 7 :
        (!hooksReady.load(std::memory_order_acquire) || !nativeMainThread || !nativeMainThread()) ? 8 :
        (currentSimulation() != frame->simulation || currentWorld() != frame->world) ? 9 : 0;
    {
        static unsigned traced = 0, previous[2]{~0u, ~0u};
        const unsigned state = refused | unsigned(entry ? entry->primary.bits : 0) << 4;
        if (state != previous[held] && traced < 200) {
            previous[held] = state;
            ++traced;
            log("Input primary admission held=%d refused=%u bits=%u", held, refused,
                entry ? entry->primary.bits : 0u);
        }
    }
    if (refused) return value;
    refreshPrimary(*entry);
    if (!entry->revoked && nativeInputHealthy() && currentSimulation() == frame->simulation &&
        currentWorld() == frame->world && !entry->revoked) value = entry->primary;
    return value;
}
static void opaqueMeleePrimary(void *subject, int32_t semantic) {
    if (!meleeConfigured || !settings.physicalMelee || unsigned(semantic) > 1) return;
    // The detoured base callback also belongs to non-player puppets. A live
    // stationary binding proves this receiver's player type BEFORE selected
    // player fields or the player-only dual getter are used. Unknown/low owners
    // need this classification too; a known-high-only guard would miss them.
    uint32_t player = 0;
    AcquireSRWLockShared(&snapshotLock);
    for (const auto &owner : localSnapshotOwner.melee)
        if (owner.player == subject && owner.epoch && owner.consumption.matches(owner.key)) {
            player = owner.key.player;
            break;
        }
    ReleaseSRWLockShared(&snapshotLock);
    if (!player) {
        AcquireSRWLockShared(&authorityLock);
        for (const auto &bank : authorities)
            for (const auto &owner : bank.melee)
                if (owner.player == subject && owner.epoch && owner.consumption.matches(owner.key))
                    player = owner.key.player;
        ReleaseSRWLockShared(&authorityLock);
    }
    if (!player || resolve(player) != subject) return;
    auto *entry = preparedPrimary(subject);
    const uint32_t right = nativeHandle(subject, 1), left = nativeHandle(subject, 0);
    void *rightWeapon = right ? resolve(right) : nullptr;
    void *leftWeapon = left ? resolve(left) : nullptr;
    const auto identitiesCurrent = [&] {
        return player && (!entry || !entry->revoked) && resolve(player) == subject &&
            nativeHandle(subject, 1) == right && nativeHandle(subject, 0) == left &&
            (!right || resolve(right) == rightWeapon) && (!left || resolve(left) == leftWeapon);
    };
    bool captured = identitiesCurrent();
    uint32_t gameHandle = 0;
    void *game = nullptr;
    int combo = 0, dual = 0;
    if (captured) {
        nativeGameInfo(&gameHandle);
        captured = identitiesCurrent();
        if (captured) game = gameHandle ? resolve(gameHandle) : nullptr;
        captured = captured && game;
    }
    if (captured) {
        combo = nativeComboWeapons(game);
        captured = identitiesCurrent() && resolve(gameHandle) == game;
    }
    if (captured) {
        dual = isDual(subject);
        uint32_t liveGame = 0;
        if (identitiesCurrent()) nativeGameInfo(&liveGame);
        captured = identitiesCurrent() && liveGame == gameHandle && resolve(gameHandle) == game;
    }
    // Classification is independent of VR gesture admission: valid native
    // callbacks may be opaque, coupled, or use a different topology. Alternative
    // callbacks do not consume the primary lane. Missing capture withdraws G but
    // preserves a known high for cancellation at the next safe native boundary.
    const auto targets = captured ? nativePrimaryReleaseTargets(unsigned(semantic),
        rightWeapon ? right : 0, leftWeapon ? left : 0, combo && dual) : NativePrimaryReleaseTargets{};
    const auto retireAffected = [&](MeleeHandRecord &owner) {
        const auto hand = owner.key.hand;
        if (owner.player != subject || owner.key.player != player || !owner.epoch || hand >= 2) return;
        if (!captured) {
            owner.resetGesture = true;
            if (entry) entry->melee[hand].gesture.eligible = entry->melee[hand].gesture.down = false;
            return;
        }
        for (unsigned i = 0; i < targets.count; ++i)
            if (owner.key.weapon == targets.primary[i] &&
                owner.weapon == (targets.primary[i] == right ? rightWeapon : leftWeapon)) {
                owner.consumption.retire();
                owner.blocked = true; owner.epoch = 0;
                owner.player = owner.weapon = owner.world = nullptr;
                if (entry) entry->melee[hand].gesture.eligible = entry->melee[hand].gesture.down = false;
                break;
            }
    };
    AcquireSRWLockExclusive(&snapshotLock);
    for (auto &owner : localSnapshotOwner.melee) retireAffected(owner);
    ReleaseSRWLockExclusive(&snapshotLock);
    AcquireSRWLockExclusive(&authorityLock);
    for (auto &bank : authorities)
        for (auto &owner : bank.melee) retireAffected(owner);
    ReleaseSRWLockExclusive(&authorityLock);
}
static void canonicalMelee(void *subject, int32_t semantic, bool press, uintptr_t caller,
                           CanonicalPrimary original) {
    auto *entry = preparedPrimary(subject);
    const auto *invocation = nativePrimaryInherited(primaryInvocation, subject);
    if (!entry || !invocation || invocation->held || unsigned(semantic) > 1 ||
        caller != operatorPrimaryReturn[press ? 0 : 1] || !meleePreparationAllowed(*entry) ||
        !primaryBindingsCurrent(*entry) || !primaryTopology(*entry, false)) {
        // Opaque primary callbacks keep their native effects. Do not let a
        // previously accounted binding pretend those effects never occurred.
        opaqueMeleePrimary(subject, semantic);
        original(subject, semantic);
        return;
    }
    for (unsigned hand = 0; hand < 2; ++hand) {
        if (!entry->melee[hand].candidate || entry->button[hand] < 0 ||
            (entry->button[hand] ^ entry->flip) != semantic || !meleeTargetCurrent(*entry, hand)) continue;
        const uint8_t bit = uint8_t(1u << entry->button[hand]);
        if (!entry->observer && !(invocation->value.mask & bit)) break;
        const bool desired = (entry->observer ? press : bool(invocation->value.bits & bit)) ||
            meleeGestureCurrent(*entry, hand);
        NativeConsumptionBinding::Edge edge = NativeConsumptionBinding::Edge::Unknown;
        auto *lock = meleeRecordLock(entry->authoritative);
        AcquireSRWLockShared(lock);
        const auto *owner = meleeRecord(entry->authoritative, entry->meleeSlot, hand);
        if (owner && meleeOwnerMatches(*owner, *entry, hand)) edge = owner->consumption.edge(owner->key, desired);
        ReleaseSRWLockShared(lock);
        if (edge == NativeConsumptionBinding::Edge::None) return; // Already completed; no invented receipt.
        if (edge == NativeConsumptionBinding::Edge::Unknown)
            dispatchMelee(*entry, hand, press, original); // Observe the actual manual callback only.
        else
            dispatchMelee(*entry, hand, desired, desired ? originalPrimaryPress : originalPrimaryRelease);
        return;
    }
    // Multitarget and alternative dispatch remain completely native.
    opaqueMeleePrimary(subject, semantic);
    original(subject, semantic);
}
static void __fastcall primaryPressed(void *subject, void *, int32_t semantic) {
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    withNativeFinally([&] {
        canonicalMelee(subject, semantic, true, caller, originalPrimaryPress);
    }, [](bool aborted) noexcept { if (aborted) nativeInputFailed(); });
}
static void __fastcall primaryReleased(void *subject, void *, int32_t semantic) {
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    withNativeFinally([&] {
        canonicalMelee(subject, semantic, false, caller, originalPrimaryRelease);
    }, [](bool aborted) noexcept { if (aborted) nativeInputFailed(); });
}
static bool observerManualLevel(PreparedPlayer &entry, unsigned hand) {
    // The trampoline still reaches installed scalar predicates. Shadow a held
    // ancestor so its independently admitted gesture cannot become manual input.
    NativePrimaryInvocation frame{entry.subject, {}, primaryInvocation, true, {}};
    primaryInvocation = &frame;
    int result = 0;
    withNativeFinally([&] { result = originalFireButtonPressed(entry.subject, entry.button[hand]); },
        [&](bool aborted) noexcept {
            primaryInvocation = frame.previous;
            if (aborted) nativeInputFailed();
        });
    return result != 0;
}
static void prepareMeleeStep(void *weapon, MeleeStepFrame &frame) {
    if (!meleeConfigured || !settings.physicalMelee) return;
    if (!zoomManagerFrame || !zoomManagerFrame->current || !zoomManagerFrame->execution ||
        !simulationInterval || simulationInterval->previous || !nativeInputHealthy()) return;
    const auto ownerHandle = primaryField(weapon, 0x28);
    auto *owner = ownerHandle ? resolve(ownerHandle) : nullptr;
    auto *entry = owner ? preparedPrimary(owner) : nullptr;
    const unsigned hand = primaryField(weapon, 0xbc);
    if (!entry || hand >= 2 || !entry->melee[hand].candidate || entry->weapon[hand] != weapon ||
        !meleeTargetCurrent(*entry, hand) || !primaryTopology(*entry, false)) return;
    refreshPrimary(*entry);
    if (!meleeTargetCurrent(*entry, hand)) return;
    frame.entry = entry; frame.weapon = weapon; frame.hand = hand; frame.epoch = entry->melee[hand].epoch;
    const bool manual = entry->observer ? observerManualLevel(*entry, hand) :
        bool(entry->primary.bits & (1u << entry->button[hand]));
    if (!meleeTargetCurrent(*entry, hand) || !primaryTopology(*entry, false)) return;
    reconcileMelee(*entry, hand, manual);
    // A normal step of the same known level supersedes an older outer receipt,
    // including paths where native held checks are skipped. It does not seed
    // unknown history, invent an edge or provide a release/quiet witness.
    const bool desired = manual || meleeGestureCurrent(*entry, hand);
    if (!meleeTargetCurrent(*entry, hand) || !primaryTopology(*entry, false)) return;
    auto *lock = meleeRecordLock(entry->authoritative);
    AcquireSRWLockShared(lock);
    const auto *record = meleeRecord(entry->authoritative, entry->meleeSlot, hand);
    if (record && meleeOwnerMatches(*record, *entry, hand) && !record->blocked && !record->teardownDepth &&
        record->consumption.edge(record->key, desired) == NativeConsumptionBinding::Edge::None) {
        frame.observation = {record->key, record->epoch, record->consumption.prepare(record->key),
                             entry->authoritative, entry->meleeSlot};
        frame.level = desired;
        frame.observe = true;
    }
    ReleaseSRWLockShared(lock);
}
// A weapon the local VR player holds in neither hand (left behind by a wheel
// selection, still holding its fire button) fired with every trigger pull of
// the held weapon, from the native placement, spending its own ammo (V443).
// Its firing button is never pressed. (Refusing OnFire instead made the game
// switch weapons, V445.)
static bool unheldWeaponFiring(void *player, uint32_t weapon) {
    if (!weapon) return false;
    const auto s = copySnapshot();
    // While dual wielding any such weapon (the ghost: a replaced hand's weapon
    // kept firing); in single-weapon mode only one this adapter replaced (a pick
    // there replaces the lone weapon; out of dual mode every weapon answers the
    // one fire button). Single mode otherwise fires its own current weapon:
    // refusing it there (after leaving a turret, which drops dual mode) left the
    // right hand unable to fire.
    if (!vrSession(s) || !s.rider.handheld() || player != livePlayer(s) ||
        (!isDual(player) && !replacedWeapon(weapon)) ||
        weapon == nativeHandle(player, 0) || weapon == nativeHandle(player, 1))
        return false;
    static unsigned refused = 0;
    if (++refused <= 8 || refused % 500 == 0) {
        void *w = resolve(weapon);
        log("Fire refused for a weapon held in neither hand id=%d (%u)", w ? int(primaryField(w, 0xb4)) : -1, refused);
    }
    return true;
}
static int __fastcall weaponFiringPressed(void *subject, void *, uint32_t handle) {
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    // Even an unadmitted nested query shadows the outer handle permission.
    MeleeHeldFrame frame;
    frame.previous = meleeHeld;
    int result = 0;
    withNativeFinally([&] {
        meleeHeld = &frame;
        auto *step = meleeStep;
        if (step && step->entry && step->entry->subject == subject && step->hand < 2 &&
            (caller == baseHeldReturn[0] || caller == baseHeldReturn[1]) &&
            step->entry->weaponHandle[step->hand] == handle && step->entry->weapon[step->hand] == step->weapon &&
            step->epoch == step->entry->melee[step->hand].epoch &&
            meleeTargetCurrent(*step->entry, step->hand)) {
            frame.entry = step->entry; frame.weapon = handle; frame.hand = step->hand; frame.epoch = step->epoch;
        }
        result = originalWeaponHeld(subject, handle);
        if (result && unheldWeaponFiring(subject, handle)) result = 0;
    }, [&](bool aborted) noexcept {
        meleeHeld = frame.previous;
        if (aborted) nativeInputFailed();
    });
    return result;
}
static void __fastcall operatorFiring(void *subject, void *) {
#ifdef _MSC_VER
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    const auto *inherited = nativePrimaryInherited(primaryInvocation, subject);
    NativePrimaryInvocation frame{subject, inherited ? inherited->value : NativePrimaryValue{},
                                  primaryInvocation, false};
    primaryInvocation = &frame; // Unclaimed until admission establishes VR ownership.
    withNativeFinally([&] {
        if (!inherited) frame.value = primaryAdmission(subject, caller, false, frame.value);
        originalOperatorFiring(subject);
    }, [&](bool aborted) noexcept {
        primaryInvocation = frame.previous;
        if (aborted) nativeInputFailed();
    });
}
static int __fastcall fireButtonPressed(void *subject, void *, int index) {
#ifdef _MSC_VER
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    const auto *inherited = nativePrimaryInherited(primaryInvocation, subject);
    NativePrimaryInvocation frame{subject, inherited ? inherited->value : NativePrimaryValue{},
                                  primaryInvocation, true};
    primaryInvocation = &frame;
    int result = 0;
    withNativeFinally([&] {
        if (!inherited) frame.value = primaryAdmission(subject, caller, true, frame.value);
        auto *held = meleeHeld;
        if (held && held->entry && held->entry->subject == subject && held->hand < 2 &&
            caller == weaponHeldReturn && index == held->entry->button[held->hand] &&
            held->weapon == held->entry->weaponHandle[held->hand] &&
            held->epoch == held->entry->melee[held->hand].epoch &&
            meleeGestureCurrent(*held->entry, held->hand)) {
            const auto bit = uint8_t(1u << index);
            frame.gesture = {bit, bit}; // Only the exact held read, before native161 blocking.
        }
        result = originalFireButtonPressed(subject, index);
    }, [&](bool aborted) noexcept {
        primaryInvocation = frame.previous;
        if (aborted) nativeInputFailed();
    });
    return result;
}
extern "C" int __cdecl ss2vrPrimaryPredicate(int original, void *subject, unsigned kind) noexcept {
    // Scalar only: no native dereference/getter, lock, allocation or exception.
    return int(nativePrimaryRead(uint32_t(original), subject, kind, primaryInvocation));
}
SS2VR_X86_PREDICATE_ENTRY(primaryDownPredicate,ss2vrPrimaryPredicate,0,4,0)
SS2VR_X86_PREDICATE_ENTRY(primaryPressPredicate,ss2vrPrimaryPredicate,24,4,1)
SS2VR_X86_PREDICATE_ENTRY(primaryReleasePredicate,ss2vrPrimaryPredicate,24,4,2)
SS2VR_X86_PREDICATE_ENTRY(primaryHistoryPredicate,ss2vrPrimaryPredicate,24,4,3)
SS2VR_X86_PREDICATE_ENTRY(primaryHeldPredicate,ss2vrPrimaryPredicate,28,4,4)
void nativeInputFailed() noexcept {
    if (simulationInterval && simulationInterval->failure)
        *simulationInterval->failure = true;
}
bool nativeInputHealthy() noexcept {
    return !simulationInterval || !simulationInterval->failure || !*simulationInterval->failure;
}
static void invalidateInterruptedInput() noexcept {
    // Snapshot metadata only: no IPC mutex, native getter or game callback in
    // an unwind tail. Clearing session forces ordinary update() to rebuild its
    // existing controls/neutral gates from a later input sample.
    AcquireSRWLockExclusive(&snapshotLock);
    current.interruption = {current.input.session, current.input.reference,
                            current.input.sequence, GetTickCount64(), current.inputProducer};
    current.generation = advanceEpochUnlocked();
    current.ui.trackingGeneration = current.generation;
    current.input.session = current.input.focused = 0;
    current.fire[0] = current.fire[1] = false;
    current.zoom[0] = current.zoom[1] = false;
    current.ui.gameplay = 0;
    ReleaseSRWLockExclusive(&snapshotLock);
}
static void __fastcall simulationStep(void *simulation, void *) {
    const auto caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (!hooksReady.load(std::memory_order_acquire)) {
        originalSimulationStep(simulation);
        return;
    }
    if(simulationRevision==UINT64_MAX) roomscaleFaulted.store(true);
    else ++simulationRevision;
    SimulationInterval interval{simulation, false, false, {}, simulationInterval};
    interval.nativeCaller=caller;
    interval.failure = interval.previous ? interval.previous->failure : &interval.failed;
    simulationInterval = &interval;
    withNativeFinally([&] {
        if(nativeMainThread && nativeMainThread())
            simulationThread.store(GetCurrentThreadId(),std::memory_order_relaxed);
        remote_render::noteSimulationThread();
        originalSimulationStep(simulation);
        runPostSimulationRoomscale(interval);
        runPostSimulationHeadVisibility(interval);
        runPostSimulationSniper(interval);
        // Retire input only after all original entity/script/physics work.
        if (interval.networkPrepared && nativeInputHealthy())
            multiplayer::completeTick();
    }, [&](bool aborted) noexcept {
        if (aborted)
            *interval.failure = true;
        // Failure in original simulation can prevent the post-step adapter
        // from being entered at all. Retain it across the next fresh interval.
        if(*interval.failure && labSniperConfigured && labSniper.stage!=LabSniperStage::Complete)
            labSniper.rejectInterval();
        simulationInterval = interval.previous;
        if (*interval.failure && !interval.previous)
            invalidateInterruptedInput();
        if (*interval.failure && interval.networkPrepared)
            multiplayer::abortTick();
    });
}
static void __fastcall entityStep(void *manager, void *) {
    if (!hooksReady.load(std::memory_order_acquire)) {
        originalEntityStep(manager);
        return;
    }
    // Lexical nesting spans BOTH preparation and original execution. Admission
    // pointers cannot be used as nesting witnesses (A -> B -> A).
    ZoomManagerFrame frame{manager,simulationInterval ? simulationInterval->simulation:nullptr,
                           nullptr,zoomManagerFrame};
    zoomManagerFrame=&frame;
    withNativeFinally([&] {
        // Existing local ownership is recognizable before even the world/game
        // getters. Remote recognition also has the callback-free MP lookup in
        // primaryAdmission until enumeration reserves its interval slot.
        if (simulationInterval && !simulationInterval->previous && !simulationInterval->managerPrepared) {
            const auto snapshot = copySnapshot();
            if (primaryLocalRecognized(snapshot, snapshot.player, snapshot.playerHandle) &&
                !preparedPrimary(snapshot.player))
                for (auto &entry : simulationInterval->preparedPlayers)
                    if (!entry.subject) {
                        entry.subject = snapshot.player; entry.handle = snapshot.playerHandle;
                        entry.recognized = true; entry.primary = NativePrimaryValue::neutral();
                        break;
                    }
        }
        frame.world=currentWorld();
        uint32_t expected=0;
        if(frame.world) memcpy(&expected,static_cast<uint8_t *>(frame.world)+0x74,4);
        frame.current=!frame.previous && simulationInterval && !simulationInterval->previous &&
            currentSimulation()==frame.simulation && reinterpret_cast<uintptr_t>(manager)==expected &&
            nativeMainThread && nativeMainThread();
        frame.preparation=frame.current && !simulationInterval->managerPrepared;
        withNativeFinally([&] {
            auto interval = simulationInterval;
            void *world = currentWorld();
            uint32_t nativeManager = 0;
            if (world)
                std::memcpy(&nativeManager, static_cast<uint8_t *>(world) + 0x74, sizeof(nativeManager));
            if (frame.current && nativeInputHealthy() && interval && !interval->previous && !interval->managerPrepared &&
                nativeMainThread && nativeMainThread() &&
                currentSimulation() == interval->simulation && reinterpret_cast<uintptr_t>(manager) == nativeManager) {
                // CSimulation has finished world changes and advanced native time before
                // entering this entity manager. Freeze every player before ANY weapon's
                // OnStep, regardless of the native entity ordering within the loop.
                interval->managerPrepared = true;
                interval->preparedWorld=world;
                interval->preparedManager=manager;
                interval->networkPrepared = !singlePlayer() && multiplayer::server() && multiplayer::beginTick();
                const auto players = multiplayer::activePlayers();
                const auto snapshot = copySnapshot();
                // Reserve ALL subjects before any update, freeze or gameplay
                // getter. Reentered OnStep must not update/freeze them twice.
                for (auto player : players) {
                    if (!player) continue;
                    auto *entry = preparedPrimary(player);
                    if (!entry)
                        for (auto &candidate : interval->preparedPlayers)
                            if (!candidate.subject) { entry = &candidate; entry->subject = player; break; }
                    if (!entry) continue;
                    const auto handle = pointerHandle(player);
                    if (entry->handle && entry->handle != handle) entry->revoked = true;
                    else entry->handle = handle;
                    entry->recognized |= primaryLocalRecognized(snapshot, player, handle) ||
                                         multiplayer::knownVrAvatar(handle);
                    if (entry->recognized) entry->primary = NativePrimaryValue::neutral();
                }
                for (auto player : players)
                    if (player) {
                        auto *entry = preparedPrimary(player);
                        if (!entry || entry->revoked || entry->prepared) continue;
                        const auto handle = pointerHandle(player);
                        if (!handle || resolve(handle) != player)
                            continue;
                        const bool localPlayer = local(player);
                        if (!interval->networkPrepared && !localPlayer) {
                            prepareObserverMelee(*entry);
                            continue;
                        }
                        if (localPlayer) {
                            { LARGE_INTEGER sectionStart{}; QueryPerformanceCounter(&sectionStart); update(player); notePerfSection(0, sectionStart); }
                            // Read the published existing snapshot, not Input:
                            // first successful tracking can establish ownership.
                            entry->recognized |= primaryLocalRecognized(copySnapshot(), player, handle);
                            if (entry->recognized) entry->primary = NativePrimaryValue::neutral();
                        }
                        // Only a successfully opened authoritative interval may freeze
                        // network input. Local preparation never owns an ACK/tick.
                        if (interval->networkPrepared)
                            authorityStep(player);
                        entry->recognized |= multiplayer::knownVrAvatar(handle);
                        if (entry->recognized) entry->primary = NativePrimaryValue::neutral();
                        preparePrimary(*entry, interval->networkPrepared &&
                            multiplayer::authority(player).negotiated);
                    }
            }
        }, [&](bool aborted) noexcept {
            if (aborted)
                nativeInputFailed();
        });
        frame.preparation=false;
        frame.execution=frame.current && simulationInterval && simulationInterval->managerPrepared;
        originalEntityStep(manager);
    },[&](bool aborted) noexcept {
        zoomManagerFrame=frame.previous;
        if(aborted) nativeInputFailed();
    });
}
// Native zoom owns every flag, timer, damage value and sound. These bounded
// records retain only adapter provenance across copied input/authority state.
using ZoomClaim=NativeZoomClaim;
static NativeZoomClaims zoomClaims;
static SRWLOCK zoomLock = SRWLOCK_INIT;
enum class ZoomOperation { Step, Fire, Activate, Deactivate, PutDown, Delete, ExternalActivate };
struct ZoomContext {
    void *weapon;
    ZoomOperation operation;
    ZoomClaim claim;
    ZoomContext *previous;
    bool revoked = false, sourceRevoked = false;
    NativeZoomDeleteRouting deleteRouting;
};
static thread_local ZoomContext *zoomContext = nullptr;
static ZoomClaim zoomClaim(void *weapon) noexcept {
    ZoomClaim result;
    AcquireSRWLockShared(&zoomLock);
    result=zoomClaims.find(weapon);
    ReleaseSRWLockShared(&zoomLock);
    return result;
}
static bool zoomToken(const ZoomContext &context,ZoomClaim &out) noexcept {
    if(context.revoked || !context.claim.serial) return false;
    out=zoomClaim(context.weapon);
    return out.serial==context.claim.serial && out.revision==context.claim.revision;
}
static void zoomRevokeFrames(void *weapon,bool source=false) noexcept {
    for(auto *frame=zoomContext;frame;frame=frame->previous)
        if(frame->weapon==weapon) {
            frame->revoked=true;
            frame->sourceRevoked|=source;
        }
}
static void zoomChange(uint64_t serial,bool deletion,bool takeover,bool abort) noexcept {
    AcquireSRWLockExclusive(&zoomLock);
    zoomClaims.change(serial,deletion,takeover,abort);
    ReleaseSRWLockExclusive(&zoomLock);
}
static void zoomMarkDeleting(uint64_t serial) noexcept {
    // Caller captures subject before setting the deletion context.
    AcquireSRWLockExclusive(&zoomLock);
    zoomClaims.change(serial,true,false,false);
    ReleaseSRWLockExclusive(&zoomLock);
}
static void zoomRetire(uint64_t serial) noexcept {
    AcquireSRWLockExclusive(&zoomLock);
    zoomClaims.sourceDeleted(serial);
    ReleaseSRWLockExclusive(&zoomLock);
}
static void zoomOwnerDeleting(void *owner) noexcept {
    AcquireSRWLockExclusive(&zoomLock);
    zoomClaims.ownerDeleting(owner);
    for(auto *frame=zoomContext;frame;frame=frame->previous)
        if(frame->claim.owner==owner) frame->revoked=true;
    ReleaseSRWLockExclusive(&zoomLock);
}
static ZoomClaim zoomReserve(void *weapon,void *owner,uint32_t wh,uint32_t oh,unsigned hand) noexcept {
    ZoomClaim result;
    AcquireSRWLockExclusive(&zoomLock);
    result=zoomClaims.reserve(weapon,owner,wh,oh,hand);
    ReleaseSRWLockExclusive(&zoomLock);
    return result; // Exhaustion never evicts an outstanding cleanup obligation.
}
static bool zoomBorrowed(ZoomContext &context,ZoomClaim &claim) {
    // Metadata FIRST: a nested native deletion may already have freed weapon.
    if(!zoomToken(context,claim) || claim.deleting || context.revoked)
        return false;
    if(!claim.weaponHandle || resolve(claim.weaponHandle)!=context.weapon || !nativeSniper(context.weapon))
        return false;
    uint32_t deleted=0;
    memcpy(&deleted,static_cast<uint8_t *>(context.weapon)+0x10,4);
    return !(deleted&2); // Callback-specific resource admission, not an allocation pin.
}
struct ZoomIntent { bool admitted=false, desired=false; void *owner=nullptr; uint32_t wh=0,oh=0; unsigned hand=2; };
static ZoomIntent zoomIntent(ZoomContext &context,NativeZoomQuery purpose=NativeZoomQuery::Effect) {
    ZoomIntent out;
    if(context.revoked) return out;
    void *weapon=context.weapon;
    const auto *frame=zoomManagerFrame;
    if(!frame || !nativeZoomPhaseAllowed(frame->previous!=nullptr,frame->current,frame->preparation,
        frame->execution,purpose) || !simulationInterval || simulationInterval->previous ||
        currentSimulation()!=frame->simulation || currentWorld()!=frame->world || !nativeInputHealthy() ||
        !hooksReady.load(std::memory_order_acquire) || !nativeMainThread || !nativeMainThread() ||
        !nativeSniper(weapon)) return out;
    out.wh=pointerHandle(weapon);
    if(context.revoked) return {};
    memcpy(&out.oh,static_cast<uint8_t *>(weapon)+0x28,4);
    out.owner=out.oh ? resolve(out.oh):nullptr;
    if(!out.wh || resolve(out.wh)!=weapon || !out.owner) return out;
    RiderIdentity rider;
    if(!readNativeRider(out.owner,rider) || !rider.handheld() || !isAlive(out.owner) || thirdPerson(out.owner) ||
        context.revoked || !nativeOwnWeapons(out.owner) || context.revoked || resolve(out.wh)!=weapon ||
        resolve(out.oh)!=out.owner) return out;
    uint32_t actualOwner=0,ownerView=0;
    memcpy(&actualOwner,static_cast<uint8_t *>(weapon)+0x28,4);
    memcpy(&ownerView,static_cast<uint8_t *>(out.owner)+0x558,4);
    if(actualOwner!=out.oh || ownerView!=0) return {};
    const uint32_t left=nativeHandle(out.owner,0),right=nativeHandle(out.owner,1);
    if(left && left==right) return out;
    out.hand=left==out.wh ? 0u:right==out.wh ? 1u:2u;
    if(out.hand>=2) return out;
    if(!singlePlayer() && multiplayer::server()) {
        const auto authority=copyAuthority(out.owner);
        const auto live=multiplayer::authority(out.owner);
        if(live.negotiated && !(purpose==NativeZoomQuery::PreserveCrossDelete && frame->preparation &&
            frame->frozenOwner!=out.oh)) {
            if(purpose==NativeZoomQuery::PreserveCrossDelete && frame->preparation) {
                out.admitted=currentWeaponSample(live,live,out.hand);
                out.desired=out.admitted && (network::intervalZoomMask(live.pose)&(1u<<out.hand));
                return out;
            }
            out.admitted=authority.hand[out.hand]==out.wh && authority.rider==rider &&
                currentWeaponSample(authority.sample,live,out.hand);
            out.desired=out.admitted && (network::intervalZoomMask(live.pose)&(1u<<out.hand));
            return out;
        }
    }
    const auto snapshot=copySnapshot();
    out.admitted=out.owner==snapshot.player && out.oh==snapshot.playerHandle && livePlayer(snapshot) &&
        handOf(weapon,snapshot)==int(out.hand) && vrSession(snapshot) && fresh(snapshot.input) &&
        snapshot.ui.gameplay && !recenterHeld(snapshot.input) && !snapshot.selecting[out.hand] &&
        !snapshot.ui.wheel[out.hand].open && snapshot.input.handValid[out.hand] && finite(snapshot.input.hand[out.hand]);
    if(out.admitted && multiplayer::remoteClient())
        out.admitted=multiplayer::localZoomAllowed(out.owner,out.hand,snapshot.intentEpoch[out.hand],
            snapshot.input.zoomInputGeneration[out.hand],snapshot.input.sequence);
    out.desired=out.admitted && snapshot.zoom[out.hand];
    return out;
}
static bool zoomState(void *weapon,bool active) {
    uint32_t deleted=0;int state=0;
    memcpy(&deleted,static_cast<uint8_t *>(weapon)+0x10,4);
    memcpy(&state,static_cast<uint8_t *>(weapon)+0xb0,4);
    return nativeZoomStateAllowed(state,active,deleted);
}
extern "C" int __cdecl ss2vrZoomPredicate(int original,void *weapon,unsigned kind) noexcept {
    // No engine callbacks/allocations or subject dereference in a naked helper.
    auto *context=zoomContext;
    if(!context || context->weapon!=weapon) return original;
    const auto live=zoomClaim(weapon);
    if(context->operation==ZoomOperation::Step) {
        const int result=nativeZoomStepPredicate(kind,original,context->claim,live,
                                                 context->revoked,context->sourceRevoked);
        if(kind==4 && result && context->claim.serial && context->claim.serial==live.serial &&
            context->claim.revision==live.revision && live.effect && live.hand==0) {
            AcquireSRWLockExclusive(&zoomLock);
            zoomClaims.audioStarted(live.serial,live.revision);
            ReleaseSRWLockExclusive(&zoomLock);
        }
        return result;
    }
    ZoomClaim claim;
    if(!zoomToken(*context,claim)) return original;
    const bool cleanup=context->operation==ZoomOperation::Deactivate;
    if(kind==0) return context->operation==ZoomOperation::Activate && claim.effect ? 1:original;
    if(kind==1) return cleanup && claim.effect ? 0:original;
    if(kind==2) return cleanup && claim.effect && claim.hand==0 ? 1:original;
    return original;
}
SS2VR_X86_PREDICATE_ENTRY(zoomActivatePredicate,ss2vrZoomPredicate,28,4,0)
SS2VR_X86_PREDICATE_ENTRY(zoomOwnerPredicate,ss2vrZoomPredicate,28,4,1)
SS2VR_X86_PREDICATE_ENTRY(zoomFovPredicate,ss2vrZoomPredicate,28,4,2)
SS2VR_X86_PREDICATE_ENTRY(zoomInterpolatePredicate,ss2vrZoomPredicate,28,4,3)
SS2VR_X86_PREDICATE_ENTRY(zoomSoundStartPredicate,ss2vrZoomPredicate,24,4,4)
SS2VR_X86_PREDICATE_ENTRY(zoomSoundStopPredicate,ss2vrZoomPredicate,28,4,5)
static void zoomNative(ZoomContext &parent,bool activate) {
    ZoomContext operation{parent.weapon,activate ? ZoomOperation::Activate:ZoomOperation::Deactivate,
                          parent.claim,zoomContext};
    zoomContext=&operation;
    withNativeFinally([&] {
        if(activate) originalZoomActivate(parent.weapon);
        else originalZoomDeactivate(parent.weapon);
    },[&](bool aborted) noexcept {
        zoomContext=operation.previous;
        if(aborted) {
            zoomChange(operation.claim.serial,false,false,true);
            zoomRevokeFrames(operation.weapon);
            nativeInputFailed();
        }
    });
}
static void zoomReconcile(ZoomContext &context,bool activation) {
    if(context.revoked) return;
    ZoomClaim claim;
    if(context.claim.serial && !zoomBorrowed(context,claim)) return;
    const auto intent=zoomIntent(context);
    if(context.revoked) return;
    const bool active=nativeZoomFlag(context.weapon)!=0;
    const bool sameOwner=!context.claim.serial || (claim.owner==intent.owner && claim.ownerHandle==intent.oh &&
        claim.weaponHandle==intent.wh && claim.hand==intent.hand && !claim.blocked && !claim.teardown);
    const bool desired=sameOwner && intent.admitted && intent.desired && zoomState(context.weapon,active);
    if(!desired) {
        if(context.claim.serial && claim.effect) {
            nativeBaseAlternativeRelease(context.weapon);
            if(context.revoked || !zoomBorrowed(context,claim)) return;
            if(active) zoomNative(context,false);
        }
        return;
    }
    if(!activation && !context.claim.serial) return;
    if(!context.claim.serial) {
        if(active) return; // No unmanaged adoption.
        if(context.revoked || resolve(intent.wh)!=context.weapon || resolve(intent.oh)!=intent.owner) return;
        const auto live=zoomIntent(context);
        if(!live.admitted || !live.desired || live.wh!=intent.wh || live.oh!=intent.oh || live.hand!=intent.hand) return;
        context.claim=zoomReserve(context.weapon,intent.owner,intent.wh,intent.oh,intent.hand);
        if(!context.claim.serial) return;
        claim=context.claim;
    }
    if(!zoomBorrowed(context,claim) || claim.blocked || !claim.effect) return;
    nativeBaseAlternativePress(context.weapon);
    if(context.revoked || !zoomBorrowed(context,claim)) return;
    if(!active && activation) zoomNative(context,true);
}
static void __fastcall sniperStep(void *weapon,void *) {
    ZoomContext context{weapon,ZoomOperation::Step,zoomClaim(weapon),zoomContext};
    for(auto *parent=context.previous;parent;parent=parent->previous)
        if(parent->weapon==weapon && parent->revoked) {
            context.revoked=true;context.sourceRevoked|=parent->sourceRevoked;
        }
    zoomContext=&context;
    withNativeFinally([&] {
        zoomReconcile(context,true);
        originalSniperStep(weapon);
    },[&](bool aborted) noexcept {
        zoomContext=context.previous;
        if(aborted) { zoomChange(context.claim.serial,false,false,true); zoomRevokeFrames(weapon);nativeInputFailed(); }
    });
}
static void __fastcall baseWeaponStep(void *weapon,void *) {
    const auto caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    auto *parent=zoomContext;
    MeleeStepFrame step;
    step.previous = meleeStep;
    withNativeFinally([&] {
        meleeStep = &step;
        prepareMeleeStep(weapon, step);
        originalBaseWeaponStep(weapon);
        if (step.observe && observeMeleeLevel(step.observation, step.level) && step.level &&
            step.entry && step.entry->observer)
            multiplayer::finishObserverGesture(step.entry->observerSample, step.hand);
        if(caller==sniperBaseStepReturn && parent && parent==zoomContext &&
            parent->operation==ZoomOperation::Step && parent->weapon==weapon && !parent->revoked)
            zoomReconcile(*parent,false); // Never reserve/restart activation after native base.
    },[&](bool aborted) noexcept {
        meleeStep = step.previous;
        if(aborted) {
            if (step.observe) abortMeleeTicket(step.observation);
            if(parent) parent->revoked=true;
            nativeInputFailed();
        }
    });
}
static int __fastcall sniperFire(void *weapon,void *,float interval) {
    int result=0;
    ZoomContext context{weapon,ZoomOperation::Fire,zoomClaim(weapon),zoomContext};
    for(auto *parent=context.previous;parent;parent=parent->previous)
        if(parent->weapon==weapon && parent->revoked) {
            context.revoked=true;context.sourceRevoked|=parent->sourceRevoked;
        }
    zoomContext=&context;
    withNativeFinally([&] {
        zoomReconcile(context,true); // Includes frozen historical OFF, before native damage use.
        result=originalSniperFire(weapon,interval);
    },[&](bool aborted) noexcept {
        zoomContext=context.previous;
        if(aborted) { zoomChange(context.claim.serial,false,false,true);zoomRevokeFrames(weapon);nativeInputFailed(); }
    });
    return result;
}
static void __fastcall zoomActivated(void *weapon,void *) {
    const auto claim=zoomClaim(weapon);
    zoomChange(claim.serial,false,true,false);
    zoomRevokeFrames(weapon);
    ZoomContext context{weapon,ZoomOperation::ExternalActivate,zoomClaim(weapon),zoomContext};
    zoomContext=&context;
    withNativeFinally([&] { originalZoomActivate(weapon); },[&](bool aborted) noexcept {
        zoomContext=context.previous;
        if(aborted) { zoomChange(context.claim.serial,false,false,true);nativeInputFailed(); }
    });
}
static void __fastcall zoomDeactivated(void *weapon,void *) {
    const auto caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const auto claim=zoomClaim(weapon);
    auto *parent=zoomContext;
    if(caller==sniperCrossDeleteReturn && parent && parent->operation==ZoomOperation::Delete &&
        nativeZoomPreserveDelete(parent->deleteRouting,claim)) {
        bool suppress=false;
        ZoomContext borrowed{weapon,ZoomOperation::Deactivate,claim,zoomContext};
        zoomContext=&borrowed;
        withNativeFinally([&] {
            const auto intent=zoomIntent(borrowed,NativeZoomQuery::PreserveCrossDelete);
            ZoomClaim currentClaim;
            suppress=intent.admitted && intent.desired && intent.owner==claim.owner &&
                intent.oh==claim.ownerHandle && intent.wh==claim.weaponHandle && intent.hand==0 &&
                zoomBorrowed(borrowed,currentClaim) && currentClaim.effect && !currentClaim.blocked &&
                !currentClaim.teardown && !currentClaim.deleting;
        },[&](bool aborted) noexcept {
            zoomContext=borrowed.previous;
            if(aborted) { suppress=false;nativeInputFailed(); }
        });
        if(suppress) return;
    }
    ZoomContext context{weapon,ZoomOperation::Deactivate,claim,zoomContext};
    zoomContext=&context;
    withNativeFinally([&] { originalZoomDeactivate(weapon); },[&](bool aborted) noexcept {
        zoomContext=context.previous;
        if(aborted) { zoomChange(context.claim.serial,false,false,true);zoomRevokeFrames(weapon);nativeInputFailed(); }
    });
}
static void __fastcall sniperPutDown(void *weapon,void *,int immediate) {
    ZoomContext context{weapon,ZoomOperation::PutDown,zoomClaim(weapon),zoomContext};
    zoomRevokeFrames(weapon);
    zoomContext=&context;
    withNativeFinally([&] { originalSniperPutDown(weapon,immediate); },[&](bool aborted) noexcept {
        zoomContext=context.previous;
        if(aborted) { zoomChange(context.claim.serial,false,false,true);nativeInputFailed(); }
    });
}
static void __fastcall step(void *p, void *) {
    if (!hooksReady.load(std::memory_order_acquire)) {
        originalStep(p);
        return;
    }
    withNativeFinally([&] {
        const uint32_t handle = pointerHandle(p);
        bool prepared = false;
        if (simulationInterval && handle && resolve(handle) == p)
            for (auto *interval = simulationInterval; interval && !prepared; interval = interval->previous) {
                if (!interval->managerPrepared) continue;
                for (const auto &candidate : interval->preparedPlayers)
                    if (candidate.handle == handle && candidate.subject == p) {
                        prepared = true;
                        break;
                    }
            }
        // A late avatar may use the existing OnStep update fallback, but cannot
        // acquire high after interval preparation. Reserve neutral before it.
        if (!prepared && simulationInterval && !preparedPrimary(p))
            for (auto &entry : simulationInterval->preparedPlayers)
                if (!entry.subject) {
                    const auto snapshot = copySnapshot();
                    entry.subject = p; entry.handle = handle;
                    entry.recognized = primaryLocalRecognized(snapshot, p, handle) ||
                                      multiplayer::knownVrAvatar(handle);
                    if (entry.recognized) entry.primary = NativePrimaryValue::neutral();
                    break;
                }
        if (!prepared) {
            { LARGE_INTEGER sectionStart{}; QueryPerformanceCounter(&sectionStart); update(p); notePerfSection(0, sectionStart); }
            if (auto *entry = preparedPrimary(p)) {
                entry->recognized |= primaryLocalRecognized(copySnapshot(), p, handle);
                if (entry->recognized) entry->primary = NativePrimaryValue::neutral();
            }
        }
        // A reserved manager entry owns preparation even during native reentry.
        // In particular, a failed/unopened network interval cannot freeze here.
        if (!prepared)
            authorityStep(p);
    }, [&](bool aborted) noexcept {
        if (aborted)
            nativeInputFailed();
    });
    originalStep(p);
    withNativeFinally([&] {
        authorityAfterStep(p);
        remote_render::observePlayer(p);
        // Native equip/holster transitions can finish during OnStep, after the request.
        // Refresh ownership without resampling input or advancing wheel/trigger state twice.
        auto s = copySnapshot();
        if (channel.shared && p == s.player && local(p) && livePlayer(s)) {
            refreshOwnership(s, p);
            int currentHealth = health(p);
            if (s.ui.gameplay && currentHealth < s.ui.health)
                damageFeedback.fetch_add(1, std::memory_order_relaxed);
            s.ui.health = currentHealth;
            s.ui.armor = armor(p);
            publishSnapshot(s);
        }
    }, [&](bool aborted) noexcept {
        if (aborted)
            nativeInputFailed();
    });
}
static void __fastcall deleted(void *p, void *) {
    retireLocalMelee(p);
    revokePrimary(p);
    zoomOwnerDeleting(p);
    remote_render::invalidatePlayer(p);
    multiplayer::invalidatePlayer(p);
    Snapshot invalid;
    bool changed = false;
    AcquireSRWLockExclusive(&snapshotLock);
    if (current.player == p) {
        uint32_t epoch = advanceEpochUnlocked();
        const auto interruption = current.interruption;
        current = {};
        current.rigRevision = rigPublication.current();
        current.interruption = interruption;
        current.generation = epoch;
        current.ui.trackingGeneration = epoch;
        current.ui.tickMs = GetTickCount64();
        invalid = current;
        changed = true;
        for (auto &c : calibration)
            c = {};
        for (auto &g : gates)
            g = {};
        for (auto &w : wheels)
            w = {};
    }
    ReleaseSRWLockExclusive(&snapshotLock);
    if (changed)
        publishSnapshot(invalid);
    originalDelete(p);
}
void invalidateRenderer() {
    AcquireSRWLockExclusive(&snapshotLock);
    uint32_t epoch = advanceEpochUnlocked();
    const auto interruption = current.interruption;
    current = {};
    current.rigRevision = rigPublication.current();
    current.interruption = interruption;
    current.generation = epoch;
    current.ui.trackingGeneration = epoch;
    current.ui.tickMs = GetTickCount64();
    for (auto &c : calibration)
        c = {};
    Snapshot invalid = current;
    ReleaseSRWLockExclusive(&snapshotLock);
    publishSnapshot(invalid);
}
static void invalidWeapon(void *w) {
    uint32_t owner;
    memcpy(&owner, static_cast<uint8_t *>(w) + 0x28, 4);
    if (auto player = resolve(owner)) {
        remote_render::invalidatePlayer(player);
        auto value = copyAuthority(player);
        if (value.sample.negotiated) {
            uint8_t changed = 0;
            uint32_t weapon = pointerHandle(w);
            for (unsigned hand = 0; hand < 2; ++hand)
                if (value.hand[hand] == weapon)
                    changed |= uint8_t(1u << hand);
            multiplayer::invalidateWeapons(player, ++value.weaponGeneration, changed);
            network::invalidateWeaponIntents(value.sample.pose, changed);
            saveAuthority(value);
        }
    }
    uint32_t id = pointerHandle(w);
    Snapshot invalid;
    bool changed = false;
    AcquireSRWLockExclusive(&snapshotLock);
    for (int h = 0; h < 2; h++)
        if (current.handle[h] == id) {
            if (current.rider.handheld()) {
                current.generation = advanceEpochUnlocked();
                current.fire[h] = false;
                current.zoom[h] = false;
            }
            current.handle[h] = 0;
            calibration[h] = {};
            current.ui.trackingGeneration = current.generation;
            current.ui.currentWeapon[h] = -1;
            current.ui.currentAmmo[h] = 0;
            current.ui.tickMs = GetTickCount64();
            changed = true;
        }
    invalid = current;
    ReleaseSRWLockExclusive(&snapshotLock);
    if (changed)
        publishSnapshot(invalid);
}
static void __fastcall weaponDeleted(void *w, void *) {
    retireLocalMelee(w);
    revokePrimary(w);
    invalidWeapon(w);
    originalWeaponDelete(w);
}
static void __fastcall sniperDeleted(void *w, void *) {
    revokePrimary(w);
    ZoomContext context{w,ZoomOperation::Delete,zoomClaim(w),zoomContext};
    zoomRevokeFrames(w,true);
    zoomMarkDeleting(context.claim.serial);
    context.claim=zoomClaim(w);
    uint32_t owner=0,hand=0;
    memcpy(&owner,static_cast<uint8_t *>(w)+0x28,4);
    memcpy(&hand,static_cast<uint8_t *>(w)+0xbc,4);
    context.deleteRouting={owner ? resolve(owner):nullptr,owner,hand ? 1u:0u};
    zoomContext=&context;
    withNativeFinally([&] {
        invalidWeapon(w);
        originalSniperDelete(w); // Own deactivation, base teardown and sound source deletion once.
    },[&](bool aborted) noexcept {
        zoomContext=context.previous;
        if(aborted) { zoomChange(context.claim.serial,false,false,true);nativeInputFailed(); }
        else zoomRetire(context.claim.serial);
    });
}
static int __fastcall sniperAlternativePressed(void *w, void *) {
#ifdef _MSC_VER
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    // In the native no-left-weapon branch, plcmdAltFire toggles the right
    // sniper's zoom. VR uses that command for LEFT primary fire. Block only
    // this demonstrated dispatch origin, before it changes native damage/time.
    if (hooksReady.load(std::memory_order_acquire) && caller == playerAlternativePressReturn &&
        nativeMainThread && nativeMainThread()) {
        uint32_t ownerHandle = 0;
        std::memcpy(&ownerHandle, static_cast<uint8_t *>(w) + 0x28, sizeof(ownerHandle));
        if (void *owner = resolve(ownerHandle)) {
            const auto s = copySnapshot();
            const bool localRoute = owner == s.player && ownerHandle == s.playerHandle &&
                                    livePlayer(s) && vrSession(s);
            const auto peer = localRoute ? multiplayer::Sample{} : multiplayer::authority(owner);
            const bool remoteRoute = multiplayer::server() && peer.negotiated &&
                                     peer.avatar == ownerHandle && peer.incarnation;
            if ((localRoute || remoteRoute) && resolve(nativeHandle(owner, 1)) == w &&
                !resolve(nativeHandle(owner, 0)))
                return 1; // Preserve sniper's native handled-event return ABI.
        }
    }
    return originalSniperAlternativePress(w);
}
thread_local void *eyePlayer = nullptr;
thread_local Request eyeRequest;
thread_local Snapshot eyeSnapshot;
thread_local Pose eyeAnchor, eyeNativeCamera;
static thread_local bool eyeAnchorReady = false; // eyeAnchor refreshed for this stereo pair.
struct PhysicalWeaponInvocation {
    void *weapon = nullptr;
    int hand = -1;
    WeaponViewPass pass;
    Matrix44 entryProjection{};
    float entryNear = 0, entryFar = 1;
    ScopePoseObservation scopePose;
    bool scopePoseAmbiguous = false;
    bool scopeGeometryRejected = false;
    bool ordinaryCommand = false;
    IdleWeaponTrace *idle = nullptr; // Borrowed from this original gun-call stack only.
};
thread_local PhysicalWeaponInvocation *physicalWeapon = nullptr;
thread_local void *executingView = nullptr;
thread_local WeaponWorldView preparedWeaponWorld, executedWeaponWorld;
thread_local Matrix44 executedUiProjection;
thread_local bool executedUiProjectionValid = false;
thread_local uint64_t executedRootGeneration = 0;
thread_local bool rootCaptureArmed = false, rootCaptureAttempted = false;
thread_local bool weaponPairFault = false;
// Source line of the first pair fault in the current eye (diagnostics only).
thread_local unsigned weaponPairFaultLine = 0;
#define SS2VR_PAIR_FAULT() (weaponPairFaultLine = weaponPairFaultLine ? weaponPairFaultLine : __LINE__, weaponPairFault = true)
thread_local ScopeObservationBank eyeScopePoses;
static thread_local float eyeNativeBaseFov = 0;
static thread_local bool scopePreview = false;
struct NativeScopeSource {
    ScopeSourceView view;
    ScopeCaptureCallback callback = nullptr;
    ScopeCaptureCurrent current = nullptr;
    void *context = nullptr;
    bool active = false, prepared = false, attempted = false, queued = false, executed = false, fault = false;
};
static thread_local NativeScopeSource scopeSource;
static ScopeCaptureCommands scopeCaptureCommands;
static uintptr_t scopeInjectionReturn = 0;
struct NativeProjectionObservation {
    void *player = nullptr;
    Matrix44 *output = nullptr;
    float baseFov = 0;
    bool seen = false, rejected = false;
};
static thread_local NativeProjectionObservation *nativeProjectionObservation = nullptr;
thread_local void *rootView = nullptr;
thread_local LaserAim eyeLasers[2];
static thread_local float eyeWorldVisibility = 1;
float frozenWorldVisibility() {
    return eyeWorldVisibility;
}
static void freezeLasers(const Request &request);
static void drawLasers();
static void drawVoiceIndicators();
static void drawSteeringWheel();
static void drawWeaponWheels();
static void drawHeldGrenade();
// Diagnostic: time spent executing the root view's collected commands, per eye
// (the rest of an eye render is scene collection, sorting and setup).
static double rootExecuteSeconds[2]{};
void rootExecuteStats(double &left, double &right) noexcept {
    left = 1000 * std::exchange(rootExecuteSeconds[0], 0.);
    right = 1000 * std::exchange(rootExecuteSeconds[1], 0.);
}
static void __fastcall viewExecute(void *command, void *) {
    const auto generation = graphicsResourceGeneration();
    auto *previousExecution = executingView;
    const bool root = command == rootView && eyePlayer && eyeIndex >= 0;
    executingView = command;
    if (root && !rootCaptureAttempted) {
        rootCaptureAttempted = true;
        rootCaptureArmed = true;
    }
    const bool sourceRoot = command == rootView && scopeSource.active && eyePlayer && eyeIndex == -1;
    LARGE_INTEGER executeStart{};
    if (root) QueryPerformanceCounter(&executeStart);
    withNativeFinally([&] {
        originalViewExecute(command);
        if (root) {
            LARGE_INTEGER executeEnd{}, frequency{};
            QueryPerformanceCounter(&executeEnd); QueryPerformanceFrequency(&frequency);
            rootExecuteSeconds[eyeIndex & 1] += double(executeEnd.QuadPart - executeStart.QuadPart) / double(frequency.QuadPart);
        }
        if (generation != graphicsResourceGeneration()) return;
        if (sourceRoot) scopeSource.executed = true;
        if (root) {
            rootCaptureArmed = false;
            if (!executedWeaponWorld.valid) SS2VR_PAIR_FAULT();
            LARGE_INTEGER sectionStart{}; QueryPerformanceCounter(&sectionStart);
            drawLasers();
            drawVoiceIndicators();
            notePerfSection(1, sectionStart);
            drawSteeringWheel();
            QueryPerformanceCounter(&sectionStart);
            drawWeaponWheels();
            drawHeldGrenade();
            notePerfSection(2, sectionStart);
        }
    }, [&](bool aborted) noexcept {
        executingView = !aborted && generation == graphicsResourceGeneration() ? previousExecution : nullptr;
        if (aborted) { SS2VR_PAIR_FAULT(); rootCaptureArmed = false; }
    });
}
static void __fastcall viewPrepare(void *command, void *, const Matrix34 &view, const Matrix44 &projection,
                                   const Box1 &depthRange, uint32_t identifier) {
#ifdef _MSC_VER
    auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    // Only renPrepareRender's main-root call. Native nested/shadow/reflection
    // preparation keeps its own identifiers and inherits parent IDs normally.
    if (scopeSource.active && eyePlayer && eyeIndex == -1 && caller == rootPrepareReturn) {
        if (rootView) scopeSource.fault = true;
        else {
            rootView = command;
            scopeSource.prepared = finiteMatrix(view) && finiteProjection(projection) &&
                validDepthRange(depthRange.min,depthRange.max) &&
                sameWeaponView(view,matrix(inverse(scopeSource.view.camera))) &&
                sameProjectionXY(projection,ss2vr::projection(scopeSource.view.fov));
            if (!scopeSource.prepared) scopeSource.fault = true;
            identifier ^= scopeSource.view.hand == 0 ? 0xa0000000u : 0xe0000000u;
        }
    }
    if (eyeIndex >= 0 && eyePlayer && caller == rootPrepareReturn) {
        if (rootView) {
            static unsigned duplicateDiagnostics=0;
            if(duplicateDiagnostics<4) { ++duplicateDiagnostics; log("Native root duplicate rejected eye=%d",eyeIndex); }
            SS2VR_PAIR_FAULT(); // Another root cannot inherit this eye's execution capture.
            preparedWeaponWorld = {};
            executedWeaponWorld = {};
            executedUiProjectionValid = false;
            executedRootGeneration = 0;
            rootCaptureArmed = false;
            originalViewPrepare(command, view, projection, depthRange, identifier);
            return;
        }
        rootView = command;
        single_pass::setMainView(view.m);
        const Pose eye = worldEyeTracking(eyeAnchor, eyeSnapshot.origin, eyeSnapshot.turn,
                                         eyeRequest.input.head, eyeRequest.eye[eyeIndex]);
        const Matrix34 expectedView = matrix(inverse(eye));
        preparedWeaponWorld = {view, projection, depthRange.min, depthRange.max,
            finiteMatrix(view) && finiteProjection(projection) &&
            validDepthRange(depthRange.min, depthRange.max) && shakenWeaponView(view, expectedView) &&
            sameProjectionXY(projection, ss2vr::projection(eyeRequest.fov[eyeIndex]))};
        {
            // Bounded trace of shaken root views the pair now keeps.
            static unsigned shaken = 0;
            if (preparedWeaponWorld.valid && !sameWeaponView(view, expectedView) && (++shaken <= 8 || shaken % 2000 == 0)) {
                const Vec3 moved = viewCamera(view) - viewCamera(expectedView);
                log("Native root view shaken eye=%d (%u) camera=%.4f,%.4f,%.4f", eyeIndex, shaken, moved.x, moved.y, moved.z);
            }
        }
        {
            // Bounded trace of rejected root views: what differed from the eye.
            static unsigned rejected = 0;
            if (!preparedWeaponWorld.valid && (++rejected <= 24 || rejected % 500 == 0)) {
                const auto &a = view.m, &b = expectedView.m;
                float rotation = 0;
                for (unsigned i : {0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u}) rotation = std::max(rotation, std::abs(a[i] - b[i]));
                const auto expectedProjection = ss2vr::projection(eyeRequest.fov[eyeIndex]);
                const Vec3 moved = viewCamera(view) - viewCamera(expectedView);
                log("Native root view rejected eye=%d (%u) view=%u xy=%u depth=%u camera=%.4f,%.4f,%.4f rot=%.4f P %.5f/%.5f %.5f/%.5f %.5f/%.5f",
                    eyeIndex, rejected, sameWeaponView(view, expectedView), sameProjectionXY(projection, expectedProjection),
                    validDepthRange(depthRange.min, depthRange.max), moved.x, moved.y, moved.z, rotation,
                    projection.m[0], expectedProjection.m[0], projection.m[5], expectedProjection.m[5], projection.m[2],
                    expectedProjection.m[2]);
            }
        }
        static unsigned prepareDiagnostics=0;
        if(prepareDiagnostics<4) {
            ++prepareDiagnostics;
            log("Native root prepared eye=%d valid=%u matrix=%u projection=%u depth=%u viewMatch=%u xyMatch=%u depthRange=%.9g,%.9g",
                eyeIndex,preparedWeaponWorld.valid,finiteMatrix(view),finiteProjection(projection),
                validDepthRange(depthRange.min,depthRange.max),sameWeaponView(view,expectedView),
                sameProjectionXY(projection,ss2vr::projection(eyeRequest.fov[eyeIndex])),depthRange.min,depthRange.max);
        }
        identifier ^= scopePreview ? 0x40000000u : (eyeIndex == 0 ? 0x80000000u : 0xc0000000u);
    }
    originalViewPrepare(command, view, projection, depthRange, identifier);
}
bool headFramePrepared(void* player,const Request& request) noexcept {
    const auto snapshot=copySnapshot();
    if(!settings.headFade||(!snapshot.rider.handheld()&&!snapshot.rider.seated()))return true;
    if(player!=snapshot.player||simulationThread.load()!=GetCurrentThreadId())return false;
    AcquireSRWLockShared(&laserLock);
    const auto& h=headVolumeObservation;
    const auto now=GetTickCount64();
    const bool attempted=h.owner==snapshot.playerHandle&&h.generation==request.trackingGeneration&&
        h.session==request.session&&h.reference==request.reference&&h.requestSequence==request.sequence&&
        h.inputSequence==request.input.sequence&&h.simulationRevision==simulationRevision&&
        h.rigRevision==snapshot.rigRevision&&now>=h.tickMs&&now-h.tickMs<=100;
    ReleaseSRWLockShared(&laserLock);
    return attempted;
}
bool beginStereo(void *p, const Request &request, uint32_t &remoteOwner) {
    remoteOwner=0;
    weaponPairFault = false;
    weaponPairFaultLine = 0;
    eyeWorldVisibility = 1;
    eyeHeadClearance={};headVolumeDrawSafe=true;pairHeadNearBits=0;
    eyeSnapshot = copySnapshot();
    if (!nativeFrameIdentity(request, eyeSnapshot.input, eyeSnapshot.generation, eyeSnapshot.initialized,
                             eyeSnapshot.ui.gameplay, p == eyeSnapshot.player && livePlayer(eyeSnapshot)) ||
        !vrSession(eyeSnapshot))
        return false;
    originalCamera(p, &eyeNativeCamera);
    eyeAnchorReady = false;
    bool ready = nativeRiderCurrent(p, eyeSnapshot.rider) &&
                 nativeTrackingAnchor(p, eyeAnchor, &eyeSnapshot.rider) && finite(eyeNativeCamera);
    eyeAnchorReady = ready;
    if (ready && eyeSnapshot.rider.seated()) {
        // Lab (SS2VR_LAB_ANCHOR_TRACE=1): the seated anchor per stereo pair, raw and
        // steadied, with the simulation tick, to measure judder.
        static const bool trace = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_ANCHOR_TRACE", v, 2) == 1 && v[0] == L'1'; }();
        static unsigned traced = 0;
        if (trace && traced < 3000) {
            ++traced;
            Pose raw{};
            steadyBypass = true;
            nativeTrackingAnchor(p, raw, &eyeSnapshot.rider);
            steadyBypass = false;
            LARGE_INTEGER t{}, f{};
            QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
            AcquireSRWLockShared(&steadyLock);
            const SteadyState st = steadyState;
            ReleaseSRWLockShared(&steadyLock);
            t.QuadPart = LONGLONG(fineSeconds() * 1e6); f.QuadPart = 1000000;
            log("Lab anchor t=%.3f sim=%llu steady=%.4f,%.4f,%.4f h=%.4f raw=%.4f,%.4f,%.4f h=%.4f valid=%d ride=%08x/%08x age=%.4f v=%.2f,%.2f,%.2f",
                double(t.QuadPart) * 1000.0 / double(f.QuadPart),
                static_cast<unsigned long long>(simulationRevision), eyeAnchor.p.x, eyeAnchor.p.y, eyeAnchor.p.z,
                horizontalHeading(eyeAnchor.q), raw.p.x, raw.p.y, raw.p.z, horizontalHeading(raw.q), int(st.valid), st.ride,
                eyeSnapshot.rider.ride, fineSeconds() - st.stamp, st.v.x, st.v.y, st.v.z);
        }
    }
    if (ready) {
        freezeLasers(request);
        if (settings.headFade && (eyeSnapshot.rider.handheld()||eyeSnapshot.rider.seated())) {
            const Pose head = worldHeadTracking(eyeAnchor,eyeSnapshot.origin,eyeSnapshot.turn,request.input.head);
            HeadVolumeObservation diagnosticSample;
            HeadQueryDiagnostic diagnosticQuery;
            HeadVolumeVisibilityReason visibilityReason=HeadVolumeVisibilityReason::Unsampled;
            uint64_t checkedAt=0;
            unsigned renderGate=0; // 1 native fault, 2 thread, 3 visibility predicate.
            bool diagnostic=false;
            AcquireSRWLockShared(&laserLock);
            bool clear=false;
            if(nativeRayFaulted.load(std::memory_order_acquire))renderGate=1;
            else if(simulationThread.load()!=GetCurrentThreadId())renderGate=2;
            else {
                renderGate=3;checkedAt=GetTickCount64();
                visibilityReason=headVolumeVisibilityReason(headVolumeObservation,eyeAnchor,head,
                    eyeSnapshot.playerHandle,request,simulationRevision,eyeSnapshot.rigRevision,checkedAt);
                clear=visibilityReason==HeadVolumeVisibilityReason::Clear;
            }
            // Darken only on evidence: a recent probe for this player that found the
            // head inside geometry. A missing or stale probe (it is matched to an exact
            // request, which pacing and late-latching often miss) leaves the world
            // visible and the pair unprotected, as with HeadComfort off; treating that
            // as a hit blacked out ordinary play.
            const bool recentProbe=renderGate==3 && headVolumeObservation.sampled &&
                headVolumeObservation.owner==eyeSnapshot.playerHandle && checkedAt>=headVolumeObservation.tickMs &&
                checkedAt-headVolumeObservation.tickMs<=150;
            const bool blocked=recentProbe && !headVolumeObservation.clear;
            eyeHeadClearance.mode=blocked ? HeadClearanceMode::Opaque : HeadClearanceMode::Disabled;
            if(clear) eyeHeadClearance={headVolumeObservation.tickMs,request.input.head.p,
                headVolumeObservation.radius-headVolumeObservation.numericalGuard,settings.headRadius,headVolumeObservation.nearZ,
                HeadClearanceMode::Clear,0};
            protectedWorldHead=headVolumeObservation.head;protectedWorldRadius=headVolumeObservation.radius;
            if(headVolumeObservation.requestSequence==request.sequence &&
               headVolumeObservation.inputSequence==request.input.sequence) {
                diagnosticSample=headVolumeObservation;diagnosticQuery=headQueryDiagnostic;diagnostic=true;
            }
            ReleaseSRWLockShared(&laserLock);
            eyeWorldVisibility = blocked && !clear ? 0.f : 1.f;
            // Environment lookup, formatting and I/O occur outside the lock.
            if(diagnostic&&labDualTrace()&&headDiagnosticReceipts.load(std::memory_order_relaxed)<8) {
                const unsigned ordinal=headDiagnosticReceipts.fetch_add(1,std::memory_order_relaxed);
                if(ordinal<8) {
                    const auto& d=diagnosticQuery;const auto& s=diagnosticSample;
                    log("Head clearance receipt=%u request=%llu input=%llu gate=%u reason=%u stage=%u prepared=%u queryClear=%u failed=%u mathReturned=%u cover=%u categories=%u computedRadius=%.17g computedGuard=%.17g near=%.9g radius=%.9g guard=%.9g mode=%u visibility=%.9g checkedAt=%llu sampleTick=%llu",
                        ordinal,(unsigned long long)request.sequence,(unsigned long long)request.input.sequence,
                        renderGate,unsigned(visibilityReason),unsigned(d.stage),unsigned(d.prepared),unsigned(d.clear),
                        unsigned(d.failed),unsigned(d.mathReturned),unsigned(d.coverValid),d.categoryCount,
                        d.computedRadius,d.computedGuard,s.nearZ,s.radius,s.numericalGuard,unsigned(eyeHeadClearance.mode),
                        eyeWorldVisibility,(unsigned long long)checkedAt,(unsigned long long)s.tickMs);
                    log("Head clearance identity receipt=%u owner=%u/%u generation=%u/%u session=%u/%u reference=%u/%u simulation=%llu/%llu rig=%llu/%llu sampleBody=(%.9g,%.9g,%.9g;%.9g,%.9g,%.9g,%.9g) body=(%.9g,%.9g,%.9g;%.9g,%.9g,%.9g,%.9g) sampleHead=(%.9g,%.9g,%.9g) head=(%.9g,%.9g,%.9g)",
                        ordinal,s.owner,eyeSnapshot.playerHandle,s.generation,request.trackingGeneration,
                        s.session,request.session,s.reference,request.reference,
                        (unsigned long long)s.simulationRevision,(unsigned long long)simulationRevision,
                        (unsigned long long)s.rigRevision,(unsigned long long)eyeSnapshot.rigRevision,
                        s.body.p.x,s.body.p.y,s.body.p.z,s.body.q.x,s.body.q.y,s.body.q.z,s.body.q.w,
                        eyeAnchor.p.x,eyeAnchor.p.y,eyeAnchor.p.z,eyeAnchor.q.x,eyeAnchor.q.y,eyeAnchor.q.z,eyeAnchor.q.w,
                        s.head.p.x,s.head.p.y,s.head.p.z,head.p.x,head.p.y,head.p.z);
                    const auto& q=d.sphere;const auto& k=d.kernel;
                    log("Head clearance owner receipt=%u ownerFailure=%u phaseFailure=%u cancelGate=%u cancelKind=%u cancelResource=%08x engineBase=%08x cancelFlags=%u cancelHandle=%u cancelType=%u data=%08x bytes=%u locks=%u format=%u requested=%d offset=%d count=%u",
                        ordinal,d.ownerFailure,d.phaseFailure,d.cancel.gate,d.cancel.kind,d.cancel.resource,
                        d.cancel.engineBase,d.cancel.flags,d.cancel.handle,d.cancel.type,d.cancel.data,d.cancel.bytes,
                        d.cancel.locks,d.cancel.format,int(d.cancel.requested),int(d.cancel.offset),d.cancel.count);
                    log("Head clearance kernel receipt=%u sphereStage=%u completed=%u hit=%d scopeCompleted=%u scopeFailed=%u ownerLost=%u kind=%u calls=%u origin=(%.9g,%.9g,%.9g) direction=(%.9g,%.9g,%.9g) a=(%.9g,%.9g,%.9g) b=(%.9g,%.9g,%.9g) c=(%.9g,%.9g,%.9g) normal=(%.9g,%.9g,%.9g) shape=%u,%.9g,%.9g,%.9g radius=%.9g result=%.9g entry=%.9g exit=%.9g maxT=%.17g budget=%.17g whole=%u reject=%u",
                        ordinal,q.stage,q.completed,q.hit,unsigned(q.scopeCompleted),unsigned(q.scopeFailed),unsigned(q.ownerLost),
                        k.kind,k.calls,k.ray.origin.x,k.ray.origin.y,k.ray.origin.z,
                        k.ray.direction.x,k.ray.direction.y,k.ray.direction.z,k.a.x,k.a.y,k.a.z,k.b.x,k.b.y,k.b.z,
                        k.c.x,k.c.y,k.c.z,k.normal.x,k.normal.y,k.normal.z,k.shape.kind,k.shape.width,k.shape.height,
                        k.shape.depth,k.radius,k.result,k.entry,k.exit,k.maxRayParameter,k.contactDepthBudget,
                        unsigned(k.requireWholePathClear),unsigned(k.rejectInitialContact));
                }
            }
        }
        remoteOwner=remote_render::freezePair(eyeSnapshot.playerHandle);
        ready=remoteOwner!=0;
    }
    return ready && rigPublication.usable(eyeSnapshot.rigRevision);
}
bool commitStereo(void *p,const Request &request,Slot &slot,uint32_t remoteOwner) {
    // Same existing snapshot lock, now explicitly retired on native unwind too.
    bool committed=false,held=false,counted=false;
    withNativeFinally([&] {
        ++snapshotNativeReadDepth;counted=true;
        AcquireSRWLockShared(&snapshotLock);held=true;
        const bool eligible=nativeFrameIdentity(request,current.input,current.generation,current.initialized,
            current.ui.gameplay,p==current.player && livePlayer(current)) && vrSession(current) &&
            eyeSnapshot.rigRevision==current.rigRevision &&
            fresh(current.input) && trackingEligible(p) && nativeRiderCurrent(p,current.rider) && !weaponPairFault && headVolumeDrawSafe;
        if(eligible)slot.headClearance=eyeHeadClearance;
        const RideGripRig rig{eyeSnapshot.rider,eyeSnapshot.inputProducer,eyeSnapshot.generation,
            eyeSnapshot.rigRevision,graphicsResourceGeneration(),eyeAnchor,eyeSnapshot.origin,eyeSnapshot.turn};
        const bool gripCurrent=eligible && eyeWorldVisibility>0 && eyeHeadClearance.mode!=HeadClearanceMode::Opaque &&
            current.rider==rig.rider && current.inputProducer==rig.producer &&
            !std::memcmp(&current.origin,&rig.origin,sizeof(Pose)) && current.turn==rig.turn;
        committed=remote_render::commitPair(slot,request,eligible,remoteOwner,gripCurrent?&rig:nullptr);
    },[&](bool aborted) noexcept {
        if(aborted) nativeUiFault();
        if(held) ReleaseSRWLockShared(&snapshotLock);
        if(counted) --snapshotNativeReadDepth;
    });
    return committed;
}
void beginEye(void *p, const Request &r, int i) {
    scopePreview = false;
    scopeSource = {};
    remote_render::useFrozenPair(true);
    rootView = nullptr;
    preparedWeaponWorld = {};
    executedWeaponWorld = {};
    executedUiProjectionValid = false;
    executedRootGeneration = 0;
    rootCaptureArmed = rootCaptureAttempted = false;
    eyePlayer = p;
    eyeRequest = r;
    eyeSnapshot.input = r.input;
    eyeIndex = i;
    eyeScopePoses = {};
    eyeNativeBaseFov = 0;
}
void endEye() {
    executedRootGeneration = 0;
    scopePreview = false;
    scopeSource = {};
    remote_render::useFrozenPair(false);
    eyePlayer = nullptr;
    eyeIndex = -1;
    physicalWeapon = nullptr;
    executingView = nullptr;
    rootCaptureArmed = false;
    rootView = nullptr;
    eyeScopePoses = {};
    eyeNativeBaseFov = 0;
}
// True while the single-pass world renders from the centre camera (its per-eye
// lab camera receipts come from singlePassAdoptEye instead).
static bool singlePassWorld = false, singlePassRealValid = false;
static Request singlePassReal{}; // The real stereo request while the centre pass renders.
void setSinglePassWorld(bool on, const Request *real) noexcept {
    singlePassWorld = on;
    singlePassRealValid = on && real;
    if (singlePassRealValid) singlePassReal = *real;
}
static uintptr_t viewRayCameraReturn = 0; // CPlayerBrainEntity's view ray search (usable objects, auto-aim).
static Pose *__fastcall camera(void *p, void *, Pose *out) {
#ifdef _MSC_VER
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    originalCamera(p, out);
    if (caller && caller == viewRayCameraReturn) {
        // The brain finds what Use acts on (switches, buttons, doors, the "Press
        // Use" prompt) with a ray along the camera placement: the flat body view at
        // eye height, which never met things above or below it (the prison's
        // detonator switch). In VR the ray goes where the head looks.
        const auto v = copySnapshot();
        Pose body;
        if (p == v.player && v.rider.handheld() && vrSession(v) && fresh(v.input) && v.input.headValid &&
            nativeMainThread && nativeMainThread() && trackingAnchor(p, body)) {
            const Pose head = worldHeadTracking(body, v.origin, v.turn, v.input.head);
            if (finite(head)) *out = head;
            // Lab (SS2VR_LAB_USE_TRACE=1): what the ray found last (brain +0x1f4).
            static const bool trace = [] { wchar_t t[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_USE_TRACE", t, 2) == 1 && t[0] == L'1'; }();
            static uint32_t lastUsable = ~0u;
            static uint64_t lastRay = 0;
            if (trace && GetTickCount64() - lastRay > 3000) {
                lastRay = GetTickCount64();
                log("Lab use ray from head at %.2f,%.2f,%.2f", head.p.x, head.p.y, head.p.z);
            }
            void *brain = resolve(primaryField(p, 0x38c));
            const uint32_t usable = brain ? primaryField(brain, 0x1f4) : 0;
            static uint32_t lastHit = ~0u;
            const uint32_t hitHandle = brain ? primaryField(brain, 0x1ec) : 0; // What the ray hit.
            if (trace && hitHandle != lastHit) {
                lastHit = hitHandle;
                void *hitEntity = hitHandle ? resolve(hitHandle) : nullptr;
                const uintptr_t hitTable = hitEntity ? primaryField(hitEntity, 0) : 0;
                const Vec3 f = rotate(normalize(head.q), Vec3{0, 0, -1});
                log("Lab use ray hit=%u class=game+%lx pitch=%.2f heading=%.2f", hitHandle,
                    static_cast<unsigned long>(hitTable ? hitTable - reinterpret_cast<uintptr_t>(GetModuleHandleW(L"Sam2Game.dll")) : 0),
                    std::asin(std::clamp(f.y, -1.f, 1.f)), std::atan2(-f.x, -f.z));
            }
            if (trace && usable != lastUsable) {
                lastUsable = usable;
                void *entity = usable ? resolve(usable) : nullptr;
                const uintptr_t table = entity ? primaryField(entity, 0) : 0;
                const Vec3 f = rotate(normalize(head.q), Vec3{0, 0, -1});
                log("Lab use target=%u class=game+%lx head pitch=%.2f", usable,
                    static_cast<unsigned long>(table ? table - reinterpret_cast<uintptr_t>(GetModuleHandleW(L"Sam2Game.dll")) : 0),
                    std::asin(std::clamp(f.y, -1.f, 1.f)));
            }
        }
        return out;
    }
    auto s = copySnapshot();
    if (p == eyePlayer && scopeSource.active && eyeSnapshot.initialized)
        *out = scopeSource.view.camera;
    else if (p == eyePlayer && eyeIndex >= 0 && eyeSnapshot.initialized) {
        *out = worldEyeTracking(eyeAnchor, eyeSnapshot.origin, eyeSnapshot.turn, eyeRequest.input.head,
                                eyeRequest.eye[eyeIndex]);
        // Opt-in lab observation only: record the actual hooked native camera,
        // without overriding runtime poses or adding test input to gameplay.
        static const bool labTrace = [] { wchar_t value[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_TRACE",value,2)==1 && value[0]==L'1'; }();
        static uint64_t observedRequest[2]{};
        static uint32_t observedSession[2]{},observedReference[2]{},observedTracking[2]{};
        static unsigned cameraDiagnostics = 0;
        const auto &head = eyeRequest.input.head;
        if (labTrace && !singlePassWorld && cameraDiagnostics < 16384 && (observedRequest[eyeIndex]!=eyeRequest.sequence || observedSession[eyeIndex]!=eyeRequest.session ||
            observedReference[eyeIndex]!=eyeRequest.reference || observedTracking[eyeIndex]!=eyeRequest.trackingGeneration)) {
            ++cameraDiagnostics; observedRequest[eyeIndex]=eyeRequest.sequence;
            observedSession[eyeIndex]=eyeRequest.session;observedReference[eyeIndex]=eyeRequest.reference;observedTracking[eyeIndex]=eyeRequest.trackingGeneration;
            log("Lab native camera eye=%d request=%llu session=%u reference=%u tracking=%u head=%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f camera=%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f",
                eyeIndex,static_cast<unsigned long long>(eyeRequest.sequence),eyeRequest.session,eyeRequest.reference,eyeRequest.trackingGeneration,
                head.p.x,head.p.y,head.p.z,head.q.x,head.q.y,head.q.z,head.q.w,
                out->p.x,out->p.y,out->p.z,out->q.x,out->q.y,out->q.z,out->q.w);
        }
        if(eyeHeadClearance.mode==HeadClearanceMode::Clear) {
            const auto f=eyeRequest.fov[eyeIndex];
            const double x=std::max(std::abs(std::tan(double(f.left))),std::abs(std::tan(double(f.right))));
            const double y=std::max(std::abs(std::tan(double(f.down))),std::abs(std::tan(double(f.up))));
            const double distance=std::hypot(double(out->p.x)-protectedWorldHead.p.x,
                double(out->p.y)-protectedWorldHead.p.y,double(out->p.z)-protectedWorldHead.p.z);
            const double extent=distance+double(eyeHeadClearance.nearZ)*std::sqrt(1+x*x+y*y)+.0005;
            if(!finite(*out)||!std::isfinite(extent)||extent>protectedWorldRadius)headVolumeDrawSafe=false;
        }
    }
    else if (p == s.player && s.rider.handheld() && nativeRiderCurrent(p, s.rider) &&
             vrSession(s) && fresh(s.input)) {
        Pose body;
        if (trackingAnchor(p, body))
            *out = worldHeadTracking(body, s.origin, s.turn, s.input.head);
    }
    return out;
}
// Ride boarding (TryFindBestRideAndSeat, and OnUse's re-check) picks the seat
// inside a cone around CPlayerPuppetEntity::GetViewOrigin, the native operator
// look. In VR that look does not follow the head (turning is the mod's own), so
// Use near a turret found no seat. Those two callers see the tracked head.
using ViewOrigin = Pose *(__thiscall *)(void *, Pose *, int);
static ViewOrigin originalViewOrigin = nullptr;
static uintptr_t boardingSeatReturn = 0, useSeatReturn = 0, grenadeViewReturn = 0;
// Lab (SS2VR_LAB_TURRET=1): boarding sees a view origin just in front of the
// world's first turret, looking back at it, so Use boards it from anywhere
// (the lab cannot walk to one). The turret is found in the world info's puppet
// array (+0x58) by its vtable; its camera comes from CPuppetEntity::GetCameraPlacement.
// Lab (SS2VR_LAB_RIDE=hover|ball): the same for the world's first hovercraft
// or rolling ball, with the view 3 m beside it and Use aimed at it; the player
// is teleported there 3 s into play (labRideTeleport).
static int labRideKind() {
    static const int kind = [] {
        wchar_t v[8]{};
        if (GetEnvironmentVariableW(L"SS2VR_LAB_RIDE", v, 8))
            return !wcscmp(v, L"hover") ? 2 : !wcscmp(v, L"ball") ? 3 : !wcscmp(v, L"dino") ? 4 : 0;
        return GetEnvironmentVariableW(L"SS2VR_LAB_TURRET", v, 2) == 1 && v[0] == L'1' ? 1 : 0;
    }();
    return kind;
}
static bool labTurretMode() { return labRideKind() != 0; }
// Lab rides (hovercraft, rolling ball) are not in the world info's turret list:
// their own native steps record the latest instance (handle) instead.
static std::atomic<uint32_t> labSeenRide{0};
using EntityStep = void(__thiscall *)(void *);
using EntityCreate = void(__thiscall *)(void *, void *);
static EntityStep originalHoverStep = nullptr, originalBallStep = nullptr, originalBallReset = nullptr;
static EntityCreate originalHoverCreate = nullptr;
static void labNoteRide(void *ride) {
    if (ride && pointerHandle && !labSeenRide.load(std::memory_order_relaxed)) {
        labSeenRide = pointerHandle(ride);
        log("Lab ride seen at %p handle=%08x", ride, labSeenRide.load());
    }
}
static void __fastcall hoverStep(void *ride, void *) { labNoteRide(ride); originalHoverStep(ride); }
static void __fastcall ballStep(void *ride, void *) { labNoteRide(ride); originalBallStep(ride); }
static void __fastcall ballReset(void *ride, void *) { originalBallReset(ride); labNoteRide(ride); }
static void __fastcall hoverCreate(void *ride, void *, void *properties) { originalHoverCreate(ride, properties); labNoteRide(ride); }
// Lab dinosaurs: legged puppets (not the player) that can be ridden.
static EntityStep originalLeggedStep = nullptr;
static void __fastcall leggedStep(void *ride, void *) {
    if (!labSeenRide.load(std::memory_order_relaxed) && ride) {
        const uintptr_t table = primaryField(ride, 0);
        static void *listed[16]{};
        static unsigned count = 0;
        if (count < 16 && std::find(listed, listed + count, ride) == listed + count) {
            listed[count++] = ride;
            using Ridable = int(__thiscall *)(void *);
            log("Lab legged %p vtable rva=%lx ridable=%d", ride, static_cast<unsigned long>(table - dinoVtables[0] + 0x29e180),
                reinterpret_cast<Ridable>((*static_cast<uintptr_t **>(ride))[0x1fc / 4])(ride));
        }
        if (table && (table == dinoVtables[0] || table == dinoVtables[1])) {
            using Ridable = int(__thiscall *)(void *);
            if (reinterpret_cast<Ridable>((*static_cast<uintptr_t **>(ride))[0x1fc / 4])(ride)) labNoteRide(ride);
        }
    }
    originalLeggedStep(ride);
}
static void *labFindTurret() {
    if (labRideKind() >= 2) {
        const uint32_t handle = labSeenRide.load(std::memory_order_relaxed);
        void *ride = handle && resolve ? resolve(handle) : nullptr;
        if (handle && !ride) labSeenRide = 0; // Gone (a load): take the next one.
        return ride;
    }
    void *info = nativeWorldInfo ? nativeWorldInfo() : nullptr;
    if (!info || !laserTurretVtable) return nullptr;
    uint32_t raw[4]{};
    if (!readableMemory(static_cast<uint8_t *>(info) + 0x58, sizeof(raw))) return nullptr;
    std::memcpy(raw, static_cast<uint8_t *>(info) + 0x58, sizeof(raw));
    static unsigned probes = 0;
    if (probes++ < 2) log("Lab turret probe info+0x58=%08x %08x %08x %08x", raw[0], raw[1], raw[2], raw[3]);
    // CStaticStackArray-like: capacity, data, count.
    const uint32_t data = raw[1], count = raw[2];
    if (!count || count > 4096 || !readableMemory(reinterpret_cast<void *>(uintptr_t(data)), count * 4)) {
        static unsigned unreadable = 0;
        if (unreadable++ < 2) log("Lab turret puppet array unreadable data=%08x count=%u", data, count);
        return nullptr;
    }
    void *turret = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t entry = 0;
        std::memcpy(&entry, reinterpret_cast<void *>(uintptr_t(data) + i * 4), 4);
        // Entries may be entity pointers or handles.
        void *candidate = reinterpret_cast<void *>(uintptr_t(entry));
        if (!candidate || !readableMemory(candidate, 4)) candidate = resolve ? resolve(entry) : nullptr;
        uintptr_t table = 0;
        static unsigned listed = 0;
        if (!candidate || !readableMemory(candidate, 4)) {
            if (listed++ < 24) log("Lab turret puppet %u/%u entry=%08x unresolved", i, count, entry);
            continue;
        }
        std::memcpy(&table, candidate, 4);
        if (listed++ < 24) log("Lab turret puppet %u/%u entry=%08x vtable rva=%lx", i, count, entry,
                               static_cast<unsigned long>(table - laserTurretVtable + 0x2b39e8));
        // CTurretPuppetEntity or CTurretCharacterEntity (Sam2Game 0x2b9fe0); the
        // hovercraft puppet/character (0x2a8558, 0x2b8420); the rolling ball
        // puppet/character (0x2b9240, 0x2b98f0).
        const uintptr_t rva = table - laserTurretVtable + 0x2b39e8;
        const bool match = labRideKind() == 2 ? rva == 0x2a8558 || rva == 0x2b8420 :
                           labRideKind() == 3 ? rva == 0x2b9240 || rva == 0x2b98f0 : rva == 0x2b39e8 || rva == 0x2b9fe0;
        if (match && !turret) turret = candidate;
    }
    static unsigned found = 0;
    if (turret && found++ < 2) log("Lab turret found at %p", turret);
    return turret;
}
// A puppet's placement (CPuppetEntity::GetAbsPlacement, vtable +0x54).
static bool labPlacement(void *puppet, Pose &out) {
    using AbsPlacement = Pose *(__thiscall *)(void *, Pose *);
    const auto table = *static_cast<uintptr_t **>(puppet);
    reinterpret_cast<AbsPlacement>(table[0x54 / 4])(puppet, &out);
    return finite(out);
}
static bool labTurretView(Pose &out) {
    void *turret = labFindTurret();
    if (!turret) return false;
    using CameraPlacement = Pose *(__thiscall *)(void *, Pose *);
    const auto table = *static_cast<uintptr_t **>(turret);
    if (labRideKind() >= 2) {
        Pose ride{};
        if (!labPlacement(turret, ride)) return false;
        out.p = ride.p + Vec3{3.f, 1.2f, 0};
        out.q = yaw(Pi / 2); // Facing -x, at the ride.
        static unsigned logged = 0;
        if (logged++ < 4) log("Lab ride view ride=%.2f,%.2f,%.2f eye=%.2f,%.2f,%.2f", ride.p.x, ride.p.y, ride.p.z, out.p.x, out.p.y, out.p.z);
        return true;
    }
    Pose camera{};
    reinterpret_cast<CameraPlacement>(table[0x5f4 / 4])(turret, &camera);
    if (!finite(camera)) return false;
    const Vec3 forward = rotate(camera.q, Vec3{0, 0, -1});
    out.p = camera.p + forward * 1.0f;
    out.q = normalize(multiply(camera.q, yaw(Pi))); // Facing back at the turret.
    static unsigned logged = 0;
    if (logged++ < 4) log("Lab turret view camera=%.2f,%.2f,%.2f eye=%.2f,%.2f,%.2f", camera.p.x, camera.p.y, camera.p.z, out.p.x, out.p.y, out.p.z);
    return true;
}
// Lab (SS2VR_LAB_RIDE): Use's target (brain +0x1f4, set by
// CPlayerBrainEntity::ProcessViewRayCasting from the view) is the found ride
// while the player is on foot.
using BrainStep = void(__thiscall *)(void *);
static BrainStep originalViewRayCasting = nullptr;
static void __fastcall viewRayCasting(void *brain, void *) {
    originalViewRayCasting(brain);
    if (labRideKind() < 2 || !nativeMainThread || !nativeMainThread()) return;
    void *ride = labFindTurret();
    const uint32_t playerHandle = primaryField(brain, 0x28);
    void *player = playerHandle ? resolve(playerHandle) : nullptr;
    if (!ride || !player || primaryField(player, 0x544)) return;
    const uint32_t handle = pointerHandle(ride);
    std::memcpy(static_cast<uint8_t *>(brain) + 0x1f4, &handle, 4);
}
static Pose *__fastcall playerViewOrigin(void *p, void *, Pose *out, int purpose) {
#ifdef _MSC_VER
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    originalViewOrigin(p, out, purpose);
    if (caller == grenadeViewReturn) {
        // The hand-grenade throw (after its cooldown and ammo checks) takes its
        // start and direction from here: start at the grenade in the right hand,
        // aimed at the end of the left hand's laser (its aim if it has none).
        // The game then adds its throw pitch and picks the speed from the pitch.
        if (!grenadeLaunching.load() || !nativeMainThread || !nativeMainThread()) return out;
        const auto s = copySnapshot();
        Pose body;
        const unsigned g = grenadeLaunchHand.load() & 1u, aim = 1u - g;
        if (p != s.player || !s.rider.handheld() || !vrSession(s) || !fresh(s.input) || !trackingAnchor(p, body) ||
            !controllerTracked(s.input, g))
            return out;
        const Vec3 from = worldHandTracking(body, s.origin, s.turn, s.input.head, weaponTracking(s.input, g)).p;
        Vec3 target{};
        bool targetValid = false;
        for (unsigned n = 0; n < 2; ++n)
            if (s.handle[n] && mountHand(s, n) == aim) {
                AcquireSRWLockShared(&laserLock);
                const LaserAim beam = laserAim[n];
                ReleaseSRWLockShared(&laserLock);
                const uint64_t now = GetTickCount64();
                if (beam.valid && beam.owner == s.playerHandle && now >= beam.tickMs && now - beam.tickMs < 250) {
                    target = beam.end;
                    targetValid = true;
                }
            }
        const Pose aimPose = worldHandTracking(body, s.origin, s.turn, s.input.head, s.input.hand[aim]);
        const Vec3 fallback = controllerTracked(s.input, aim) ? rotate(normalize(aimPose.q), Vec3{0, 0, -1})
                                                              : rotate(normalize(out->q), Vec3{0, 0, -1});
        const Pose launch = grenadeLaunchPose(from, target, targetValid, fallback);
        if (finite(launch)) *out = launch;
        grenadeTaken = true;
        const Vec3 d = rotate(launch.q, Vec3{0, 0, -1});
        log("Grenade thrown from %.2f,%.2f,%.2f dir=%.2f,%.2f,%.2f target=%d", from.x, from.y, from.z, d.x, d.y, d.z,
            int(targetValid));
        return out;
    }
    if (caller != boardingSeatReturn && caller != useSeatReturn) return out;
    if (labTurretMode() && nativeMainThread && nativeMainThread() && labTurretView(*out)) return out;
    const auto s = copySnapshot();
    if (p == s.player && s.rider.handheld() && nativeRiderCurrent(p, s.rider) && vrSession(s) && fresh(s.input) &&
        nativeMainThread && nativeMainThread()) {
        Pose body;
        if (trackingAnchor(p, body)) {
            const Pose head = worldHeadTracking(body, s.origin, s.turn, s.input.head);
            if (finite(head)) *out = head;
        }
    }
    return out;
}
static int __fastcall renderThirdPerson(void *p, void *) {
#ifdef _MSC_VER
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    if (caller == scopeInjectionReturn && p == eyePlayer && scopeSource.active && eyeIndex == -1) {
        if (scopeSource.attempted) scopeSource.fault = true;
        else {
            scopeSource.attempted = true;
            if (scopeSource.prepared && !scopeSource.fault)
                scopeSource.queued = scopeCaptureCommands.queue(rootView,caller,scopeSource.callback,
                                                                scopeSource.current,scopeSource.context);
            if (!scopeSource.queued) scopeSource.fault = true;
        }
        return 1; // Only native source collection's existing no-handheld path.
    }
    if (caller == mountedAvatarReturn && p == eyePlayer && eyeIndex >= 0 &&
        eyeSnapshot.rider.seated() && eyeSnapshot.initialized &&
        nativeRiderCurrent(p, eyeSnapshot.rider) && vrSession(eyeSnapshot))
        return 0; // Native root selects this player as the first-person avatar.
    return originalThirdPerson(p);
}
static Matrix44 *__fastcall project(void *p, void *, Matrix44 *out) {
    const auto generation = graphicsResourceGeneration();
#ifdef _MSC_VER
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    NativeProjectionObservation observation{p,out};
    auto *previous = nativeProjectionObservation;
    if (previous) previous->rejected = true; // Nested projection cannot certify the outer sample.
    const bool observe = caller == rootProjectionReturn && p == eyePlayer && eyeIndex >= 0 &&
                         eyeSnapshot.initialized && !previous;
    if (observe) eyeNativeBaseFov = 0;
    nativeProjectionObservation = observe ? &observation : nullptr;
    bool completed = false;
    // The observation lives above the native finally frame. Restore its TLS
    // pointer on native unwind too; never retain a pointer into an unwound hook.
    withNativeFinally([&] {
        const auto result = originalProjection(p, out);
        completed = true;
        if (generation != graphicsResourceGeneration()) return;
        if (observe && result == out && observation.seen && !observation.rejected &&
            p == eyePlayer && eyeIndex >= 0 && !weaponPairFault)
            eyeNativeBaseFov = observation.baseFov;
    },[&](bool aborted) noexcept {
        nativeProjectionObservation = !aborted && generation == graphicsResourceGeneration() ? previous : nullptr;
        if (aborted && observe) eyeNativeBaseFov = 0;
    });
    if (!completed) { SS2VR_PAIR_FAULT(); return out; }
    if (generation != graphicsResourceGeneration()) return out;
    if (p == eyePlayer && scopeSource.active) {
        const float nz = out->m[11]/(out->m[10]-1), fz = out->m[11]/(out->m[10]+1);
        if (!(std::isfinite(nz) && nz > 0 && std::isfinite(fz) && fz > nz)) scopeSource.fault = true;
        else *out = projection(scopeSource.view.fov,nz,fz);
        return out;
    }
    if (p == eyePlayer && eyeIndex >= 0) {
        float nz = out->m[11] / (out->m[10] - 1), fz = out->m[11] / (out->m[10] + 1);
        if (!(nz > 0 && fz > nz && std::isfinite(fz))) {
            nz = .05f;
            fz = 10000;
        }
        if(settings.headFade && (eyeSnapshot.rider.handheld()||eyeSnapshot.rider.seated())) {
            const uint32_t bits=std::bit_cast<uint32_t>(nz);
            if(caller==rootProjectionReturn) {
                pairHeadNearBits=std::max(pairHeadNearBits,bits);
                headNearBits.store(pairHeadNearBits,std::memory_order_relaxed);
            }
            if(eyeHeadClearance.mode==HeadClearanceMode::Clear&&nz>eyeHeadClearance.nearZ)
                headVolumeDrawSafe=false; // Query a matching volume on a later frame.
        }
        *out = projection(eyeRequest.fov[eyeIndex], nz, fz);
    }
    return out;
}
// The flat game draws first-person weapons with their own (narrower) FOV, which
// magnifies them against the world by tan(worldFov/2) / tan(weaponFov/2) (from
// the native frustum arguments). At full magnification hand-held guns overlap;
// automatic VR scale uses the geometric mean of true and flat apparent size.
static std::atomic<float> nativeWorldFov{0}, nativeWeaponFov[64]{};
static void noteWorldFov(float fov) {
    constexpr float degree = Pi / 180;
    if (std::isfinite(fov) && fov >= 45 * degree && fov <= 135 * degree && fov > nativeWorldFov.load())
        nativeWorldFov = fov; // The widest observation is the unzoomed camera.
}
static void noteWeaponFov(uint32_t id, float fov) {
    constexpr float degree = Pi / 180;
    if (id < 64 && std::isfinite(fov) && fov >= 20 * degree && fov <= 135 * degree && !nativeWeaponFov[id].load()) {
        nativeWeaponFov[id] = fov;
        log("Weapon native fov id=%u weapon=%.1f world=%.1f degrees", id, fov / degree, nativeWorldFov.load() / degree);
    }
}
static float weaponModelScale(const VrSettings &s, int id) {
    // [Weapons] Scale multiplies the matched per-weapon sizes (0 or 1: as matched).
    const float base = (s.weaponScale > 0 ? s.weaponScale : 1.f) * weaponDisplayScale(uint32_t(id));
    return id >= 0 && id < int(WeaponCount) ? base * s.weaponScaleById[id] : base;
}
static Matrix44 *__cdecl weaponFrustum(Matrix44 *out, float fov, float aspect, float nz, float fz) {
#ifdef _MSC_VER
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    if (caller == nativeFrustumReturn) noteWorldFov(fov);
    else if (caller == weaponFrustumReturn && physicalWeapon && physicalWeapon->weapon)
        noteWeaponFov(primaryField(physicalWeapon->weapon, 0xb4), fov);
    if (auto *sample = nativeProjectionObservation; sample && caller == nativeFrustumReturn) {
        if (sample->seen || out != sample->output || sample->player != eyePlayer || eyeIndex < 0)
            sample->rejected = true;
        sample->seen = true;
        // This exact native call follows the side-effect-free Player getter
        // with no intervening call. Verify its actual virtual slot before reading
        // the field it returned; subclasses/foreign getters are not equivalent.
        if (!sample->rejected && readableMemory(sample->player,0x85c)) {
            uintptr_t table = 0, getter = 0;
            float multiplier = 0;
            std::memcpy(&table,sample->player,4);
            if (table <= UINTPTR_MAX-0x5fc && readableMemory(reinterpret_cast<void *>(table+0x5f8),4))
                std::memcpy(&getter,reinterpret_cast<void *>(table+0x5f8),4);
            if (getter == playerFovGetter) {
                std::memcpy(&multiplier,static_cast<uint8_t *>(sample->player)+0x858,4);
                sample->baseFov = scopeNativeBaseFov(fov,multiplier);
            }
        }
    }
    if (physicalWeapon && caller == weaponFrustumReturn) {
        if (physicalWeapon->pass.projection(*out))
            return out;
        SS2VR_PAIR_FAULT();
    }
    return originalFrustum(out, fov, aspect, nz, fz);
}
static Matrix34 *__cdecl weaponMatrixInverse(Matrix34 *out, const Matrix34 &incomingCamera) {
#ifdef _MSC_VER
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    if (physicalWeapon && caller == weaponInverseReturn) {
        if (physicalWeapon->pass.view(*out))
            return out;
        SS2VR_PAIR_FAULT();
    }
    return originalMatrixInverse(out, incomingCamera);
}
static bool physicalCallbacksAvailable() {
    return nativeDepthRange && *nativeDepthRange == expectedDepthRange &&
           nativeProjectionSet && *nativeProjectionSet == expectedProjectionSet;
}
static void __cdecl weaponDepthRange(float nearDepth, float farDepth) {
    const auto generation = graphicsResourceGeneration();
#ifdef _MSC_VER
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    const bool capture = caller == rootDepthReturn && rootCaptureArmed &&
                         executingView == rootView && eyePlayer && eyeIndex >= 0;
    if (physicalWeapon && caller == weaponDepthReturn) {
        physicalWeapon->pass.graphicsSetup(); // Callback mismatch still reaches native graphics setup.
        if (!physicalCallbacksAvailable() || !physicalWeapon->pass.depth(nearDepth, farDepth)) {
            physicalWeapon->pass.failed = true;
            SS2VR_PAIR_FAULT();
        }
    }
    static unsigned depthDiagnostics=0;
    const bool traceDepth=eyePlayer && eyeIndex>=0 && depthDiagnostics<8;
    if(traceDepth) {
        ++depthDiagnostics;
        log("Native root depth call eye=%d caller=%p expected=%p armed=%u root=%u capture=%u callback=%u supplied=%.9g,%.9g",
            eyeIndex,reinterpret_cast<void *>(caller),reinterpret_cast<void *>(rootDepthReturn),rootCaptureArmed,
            executingView==rootView,capture,physicalCallbacksAvailable(),nearDepth,farDepth);
    }
    originalDepthRange(nearDepth, farDepth);
    if (generation != graphicsResourceGeneration()) return;
    if (physicalWeapon && caller == weaponRestoreReturn) {
        if (!physicalWeapon->pass.restored() || !physicalCallbacksAvailable() ||
            std::memcmp(nativeCurrentProjection, &physicalWeapon->entryProjection, sizeof(Matrix44)) ||
            *nativeDepthNear != physicalWeapon->entryNear || *nativeDepthFar != physicalWeapon->entryFar) {
            physicalWeapon->pass.failed = true;
            SS2VR_PAIR_FAULT();
        }
    }
    if (capture) {
        rootCaptureArmed = false; // Parent reactivation cannot overwrite this eye snapshot.
        if (physicalCallbacksAvailable())
            executedWeaponWorld = executedRootWeaponView(preparedWeaponWorld, *nativeCurrentView,
                                                      *nativeCurrentProjection, *nativeDepthNear,
                                                      *nativeDepthFar);
        if(traceDepth && physicalCallbacksAvailable()) {
            log("Native root executed eye=%d prepared=%u matrix=%u projection=%u depth=%u viewMatch=%u xyMatch=%u nearMatch=%u farMatch=%u actualDepth=%.9g,%.9g",
                eyeIndex,preparedWeaponWorld.valid,finiteMatrix(*nativeCurrentView),finiteProjection(*nativeCurrentProjection),
                validDepthRange(*nativeDepthNear,*nativeDepthFar),sameWeaponView(preparedWeaponWorld.view,*nativeCurrentView),
                sameProjectionXY(preparedWeaponWorld.projection,*nativeCurrentProjection),
                *nativeDepthNear==preparedWeaponWorld.nearDepth,*nativeDepthFar==preparedWeaponWorld.farDepth,
                *nativeDepthNear,*nativeDepthFar);
        }
        if (!executedWeaponWorld.valid && preparedWeaponWorld.valid && physicalCallbacksAvailable() &&
            finiteMatrix(*nativeCurrentView) && finiteProjection(*nativeCurrentProjection) &&
            validDepthRange(*nativeDepthNear, *nativeDepthFar)) {
            // The root executed with other matrices than prepared (camera shake,
            // a native FOV or depth change). The world was drawn with them, so the
            // HUD, weapons and right eye follow them rather than dropping the pair.
            const unsigned reason = unsigned(!sameWeaponView(preparedWeaponWorld.view, *nativeCurrentView)) |
                unsigned(!sameProjectionXY(preparedWeaponWorld.projection, *nativeCurrentProjection)) << 1 |
                unsigned(*nativeDepthNear != preparedWeaponWorld.nearDepth || *nativeDepthFar != preparedWeaponWorld.farDepth) << 2;
            static unsigned logged = 0, lastReason = 0;
            if (logged < 24 && reason != lastReason) {
                ++logged; lastReason = reason;
                const auto &pv = preparedWeaponWorld.view.m; const auto &nv = nativeCurrentView->m;
                const auto &pp = preparedWeaponWorld.projection.m; const auto &np = nativeCurrentProjection->m;
                log("Native root executed other matrices eye=%d view=%u projection=%u depth=%u dt=%.3f,%.3f,%.3f P %.4f/%.4f %.4f/%.4f %.4f/%.4f "
                    "depth %.3f,%.3f/%.3f,%.3f", eyeIndex, reason & 1, (reason >> 1) & 1, (reason >> 2) & 1,
                    nv[3] - pv[3], nv[7] - pv[7], nv[11] - pv[11], pp[0], np[0], pp[5], np[5], pp[2], np[2],
                    preparedWeaponWorld.nearDepth, preparedWeaponWorld.farDepth, *nativeDepthNear, *nativeDepthFar);
            }
            executedWeaponWorld = {*nativeCurrentView, *nativeCurrentProjection, *nativeDepthNear, *nativeDepthFar, true};
        }
        executedUiProjectionValid = executedWeaponWorld.valid && nativeAdjustedProjection &&
                                    finiteProjection(*nativeAdjustedProjection);
        if (executedUiProjectionValid)
            executedUiProjection = *nativeAdjustedProjection;
        executedRootGeneration = executedUiProjectionValid ? generation : 0;
        if (!executedWeaponWorld.valid)
            SS2VR_PAIR_FAULT();
    }
}
// Copy while this eye still owns its completed root, before marker ortho setup
// and endEye retire native context. Never read a later UI projection as world P.
bool copyExecutedUiProjection(void *player, const Request &request, int index, Matrix44 &out) {
    const bool mainThread=nativeMainThread && nativeMainThread();
    const bool current=mainThread && player==eyePlayer && index==eyeIndex && index>=0 && index<=1 &&
        sameRequest(request,eyeRequest);
    const bool admitted=current && rootCaptureAttempted && executedWeaponWorld.valid &&
        executedUiProjectionValid && !weaponPairFault;
    static unsigned diagnostics=0, lastLine=0;
    if (!admitted && (diagnostics<4 || (weaponPairFaultLine!=lastLine && diagnostics<32))) {
        ++diagnostics; lastLine=weaponPairFaultLine;
        log("Native eye projection rejected eye=%d current=%u rootAttempted=%u worldValid=%u uiProjection=%u weaponFault=%u at=%u",
            index,current,rootCaptureAttempted,executedWeaponWorld.valid,executedUiProjectionValid,weaponPairFault,weaponPairFaultLine);
    }
    if (!admitted) return false;
    out = executedUiProjection;
    return true;
}
bool copyExecutedRideCamera(uint32_t player,uint64_t generation,int index,RideCameraCopy &out) {
    out={};
    if(!remote_render::ownsNativeThread() || !hooksReady.load(std::memory_order_acquire) ||
       !generation || generation!=graphicsResourceGeneration() || generation!=executedRootGeneration ||
       index<0 || index>1 || index!=eyeIndex || !eyePlayer || eyePlayer!=eyeSnapshot.player ||
       player!=eyeSnapshot.playerHandle || !livePlayer(eyeSnapshot) ||
       !rootView || executingView!=rootView || scopePreview || scopeSource.active || physicalWeapon ||
       !rootCaptureAttempted || rootCaptureArmed || weaponPairFault ||
       !executedWeaponWorld.valid || !executedUiProjectionValid ||
       !rigPublication.usable(eyeSnapshot.rigRevision) || !channel.shared ||
       trackingEpoch(*channel.shared)!=eyeSnapshot.generation ||
       eyeRequest.trackingGeneration!=eyeSnapshot.generation || !physicalCallbacksAvailable() ||
       !nativeCurrentView || !nativeCurrentProjection || !nativeAdjustedProjection ||
       std::memcmp(nativeCurrentView,&executedWeaponWorld.view,sizeof(Matrix34)) ||
       std::memcmp(nativeCurrentProjection,&executedWeaponWorld.projection,sizeof(Matrix44)) ||
       std::memcmp(nativeAdjustedProjection,&executedUiProjection,sizeof(Matrix44)))return false;
    out.view=executedWeaponWorld.view;
    out.projection=executedUiProjection;
    return true;
}
static void finishPhysicalWeapon(PhysicalWeaponInvocation &invocation, uint64_t generation) noexcept {
    if (generation != graphicsResourceGeneration() || invocation.pass.complete()) return;
    SS2VR_PAIR_FAULT();
    if (!invocation.pass.cleanupRequired() || !physicalCallbacksAvailable()) return;
    auto *previous = physicalWeapon;
    physicalWeapon = nullptr;
    withNativeFinally([&] {
        (*nativeProjectionSet)(invocation.entryProjection);
        if (generation != graphicsResourceGeneration()) return;
        *nativeCachedMatrices &= ~6u;
        originalDepthRange(invocation.entryNear, invocation.entryFar);
    }, [&](bool aborted) noexcept {
        physicalWeapon = !aborted && generation == graphicsResourceGeneration() ? previous : nullptr;
    });
}
struct MuzzleContext {
    bool accepted = false, authoritative = false;
    RiderIdentity rider;
    void *player = nullptr;
    uint32_t handle = 0, owner = 0;
    unsigned hand = 2;
    multiplayer::Sample sample;
};
static MuzzleContext muzzleContext(void *weapon, const Snapshot &s, bool explicitLocal) {
    MuzzleContext context;
    if (!hooksReady.load(std::memory_order_acquire))
        return context;
    // Actual shots give negotiated native authority precedence. Laser queries
    // explicitly supply their presentation-correlated local snapshot instead.
    if (!explicitLocal && !singlePlayer() && multiplayer::server()) {
        uint32_t owner = 0;
        memcpy(&owner, static_cast<uint8_t *>(weapon) + 0x28, sizeof(owner));
        void *player = owner ? resolve(owner) : nullptr;
        const auto peer = player ? multiplayer::authority(player) : multiplayer::Sample{};
        if (peer.negotiated) {
            context.authoritative = true;
            context.player = player;
            context.owner = owner;
            context.handle = pointerHandle(weapon);
            const auto value = copyAuthority(player);
            context.sample = value.sample;
            context.rider = value.rider;
            if (!context.rider.handheld() || !nativeRiderCurrent(player, context.rider))
                return context;
            for (unsigned h = 0; h < 2; ++h) {
                MuzzleBinding binding{owner, value.sample.avatar, peer.avatar,
                                      value.sample.incarnation, peer.incarnation,
                                      context.handle, value.hand[h], nativeHandle(player, int(h)),
                                      value.hand[1 - h],
                                      *reinterpret_cast<int *>(static_cast<uint8_t *>(weapon) + 0xb4), h,
                                      peer.negotiated, peer.valid,
                                      currentWeaponSample(value.sample, peer, h),
                                      isAlive(player) != 0,
                                      GetTickCount64(), value.sample.receivedMs};
                if (eligibleAuthorityMuzzle(value.sample.pose, binding)) {
                    context.accepted = true;
                    context.hand = h;
                    break;
                }
            }
            return context; // Rejected authority cannot fall through to local tracking.
        }
    }
    const int hand = handOf(weapon, s);
    if (eligibleLocalMuzzle(s.input, hand < 0 ? 2u : mountHand(s, unsigned(hand)), hand >= 0, vrSession(s),
                            hand >= 0 && trackingEligible(s.player), fresh(s.input))) {
        context.accepted = true;
        context.player = s.player;
        context.rider = s.rider;
        context.owner = s.playerHandle;
        context.hand = unsigned(hand);
        context.handle = s.handle[hand];
        uint32_t owner = 0;
        memcpy(&owner, static_cast<uint8_t *>(weapon) + 0x28, sizeof(owner));
        context.accepted = owner == context.owner;
    }
    return context;
}
// Muzzle of a weapon from its grip (weapon frame: -Z forward, +Y up) at scale 1,
// for weapons whose native shot origin is not at the displayed barrel (lab side
// views with grip and 0.5 m scale marks, SS2VR_LAB_MUZZLE_MARK=1, V451-V461).
// Zap gun: between the prongs (native origin 0.4 m beside, 0.5 m above).
// Double shotgun: barrel tips (native origin at the receiver). Uzi: barrel nub
// (native origin above the rear of the receiver). Sniper: muzzle brake (native
// origin 1.2 m behind it, level with the scope). The others start at or near
// their barrels already.
static const Vec3 *measuredEmitter(uint32_t nativeId) {
    static const Vec3 zap{0, .014f, -.52f}, doubleShotgun{0, .087f, -.80f}, uzi{0, .16f, -.67f},
        sniper{0, .30f, -1.85f};
    switch (nativeId) {
    case 1: return &zap;
    case 3: return &doubleShotgun;
    case 4: return &uzi;
    case 13: return &sniper;
    default: return nullptr;
    }
}
// Emitter offsets were measured at the matched display size; they scale with the
// model whenever [Weapons] Scale or a per-weapon Scale changes it.
static float emitterScale(uint32_t nativeId) {
    return WeaponSizeFactor * (settings.weaponScale > 0 ? settings.weaponScale : 1.f) *
           (nativeId < WeaponCount ? settings.weaponScaleById[nativeId] : 1.f);
}
static Pose *shooting(void *w, Pose *out, PoseGet original, const Snapshot *snapshot = nullptr,
                      bool *calibrated = nullptr) {
    const unsigned previousDepth = nativeShotDepth;
    const bool nested = previousDepth != 0;
    if (calibrated) *calibrated = false;
    if (previousDepth != UINT_MAX) ++nativeShotDepth;
    AttachmentObservation attachmentReceipt;
    AttachmentObservation *previousAttachment=nullptr;
    bool attachmentPublished=false;
    // Context acquisition and native retarget getters share this extent. A
    // foreign unwind must not strand TLS; reentry during adaptation stays native.
    withNativeFinally([&] {
        const auto s = nested ? Snapshot{} : (snapshot ? *snapshot : copySnapshot());
        const auto context = nested ? MuzzleContext{} : muzzleContext(w, s, snapshot != nullptr);
        if(nested && activeAttachmentObservation)activeAttachmentObservation->nested=true;
        const bool observeAttachment=snapshot && context.accepted && !nested && context.hand==1 &&
            labSniperConfigured && labPreparation->sniperZoom && labOnlineIsolationInstalled() &&
            primaryField(w,0xb4)==13 && primaryField(w,0xb0)==1 && !primaryField(w,0xd4) &&
            !s.fire[0] && !s.fire[1] && s.input.trigger[0]==0 && s.input.trigger[1]==0 &&
            !s.input.zoomDownMask;
        const auto originalWithObservation=[&](PoseGet getter) {
            if(observeAttachment) {
                previousAttachment=activeAttachmentObservation;
                activeAttachmentObservation=&attachmentReceipt;attachmentPublished=true;
            }
            getter(w,out);
            if(attachmentPublished) {
                activeAttachmentObservation=previousAttachment;attachmentPublished=false;
            }
        };
        unsigned diagnosticRejection=10;
        // Zoomed native sniper placement (Sam+172A70) is the owner's view
        // origin, not its gun attachment. Keep native zoom/damage, but obtain
        // the base weapon's attachment once for positively identified VR aim.
        // This shared boundary also serves collision lasers and server shots.
        invokeNativeMuzzle(
            original == originalSniperShot, context.accepted, nested,
            [&] { originalWithObservation(original); },
            [&] { originalWithObservation(originalShot); },
            [&] {
                const Pose nativeReturned=*out; // Existing original return, before adaptation/getters.
                diagnosticRejection=11;
                if (!finite(*out) || resolve(context.handle) != w || resolve(context.owner) != context.player)
                    return;
                diagnosticRejection=12;
                if (!context.rider.handheld() || !nativeRiderCurrent(context.player, context.rider))
                    return;
                diagnosticRejection=13;
                uint32_t owner = 0;
                memcpy(&owner, static_cast<uint8_t *>(w) + 0x28, sizeof(owner));
                if (owner != context.owner || nativeHandle(context.player, int(context.hand)) != context.handle)
                    return;
                if (context.authoritative) {
                    // Retain the same validated hand/sample. Only lifecycle is
                    // rechecked; never select a weaker or newer placement context.
                    const auto peer = multiplayer::authority(context.player);
                    if (!currentWeaponSample(context.sample, peer, context.hand) || !isAlive(context.player))
                        return;
                    Pose body, camera, modelPose;
                    Vec3 displacement,alignment;WeaponAlignmentBinding modelEvidence;
                    if (!nativeWeaponReference(context.player, w, body, camera, modelPose,
                                                context.hand,&displacement,&alignment,&modelEvidence))
                        return;
                    if (resolve(context.owner) != context.player || resolve(context.handle) != w ||
                        nativeHandle(context.player, int(context.hand)) != context.handle ||
                        !isAlive(context.player) || !nativeRiderCurrent(context.player, context.rider))
                        return;
                    memcpy(&owner, static_cast<uint8_t *>(w) + 0x28, sizeof(owner));
                    if (owner != context.owner ||
                        !currentWeaponSample(context.sample, multiplayer::authority(context.player), context.hand))
                        return;
                    WeaponAlignmentBinding current;
                    if(!captureWeaponModelBinding(context.player,w,context.hand,false,current) ||
                       !weaponModelCalibrationMatches(modelEvidence,current,
                           weaponAlignmentSupported(modelEvidence.nativeId)))return;
                    Pose grip = compose(body, context.sample.pose.grip[context.hand]);
                    Pose target = retargetCalibratedMuzzle(
                        camera, *out, compose(inverse(camera), modelPose).p, grip,
                        displacement, alignment);
                    // Same measured emitters as the client's own shots and laser.
                    if (const Vec3 *emitter = measuredEmitter(primaryField(w, 0xb4)))
                        target = {normalize(grip.q), grip.p + rotate(normalize(grip.q), *emitter * emitterScale(primaryField(w, 0xb4)))};
                    if (finite(target))
                        *out = target;
                    return;
                }
                Pose body, nativeCamera;
                diagnosticRejection=14;
                if (!vrSession(s) || !fresh(s.input) || !trackingAnchor(s.player, body))
                    return;
                diagnosticRejection=15;
                originalCamera(s.player, &nativeCamera);
                if (!finite(nativeCamera) || !localWeaponCurrent(w, s, context.hand))
                    return;
                Pose hand = worldHandTracking(
                    body, s.origin, s.turn, s.input.head,
                    weaponGrip(s.input, mountHand(s, context.hand),
                               *reinterpret_cast<int *>(static_cast<uint8_t *>(w) + 0xb4)));
                AcquireSRWLockShared(&snapshotLock);
                auto c = calibration[context.hand];
                ReleaseSRWLockShared(&snapshotLock);
                const uint64_t now = GetTickCount64();
                // The binding check below catches model changes; age only guards a
                // render that stopped (a hitch of >100 ms made every shot native).
                bool calibrationAdmitted = c.valid && c.handle == context.handle &&
                    now >= c.tickMs && now - c.tickMs <= 500;
                const bool wantsAlignment=weaponAlignmentSupported(primaryField(w,0xb4));
                diagnosticRejection=16;
                if(calibrationAdmitted || wantsAlignment || c.alignmentApplied) {
                    WeaponAlignmentBinding binding;
                    bool attempted=false,captured=false;
                    unsigned failure=UINT_MAX;
                    calibrationAdmitted=calibrationAdmitted &&
                        ([&] { attempted=true;
                            captured=captureWeaponModelBinding(context.player,w,context.hand,false,binding,&failure);
                            return captured; }()) &&
                        weaponModelCalibrationMatches(c.modelBinding,binding,c.alignmentApplied);
                    uint64_t order;
                    if(laserTraceTicket(LaserTraceStage::Muzzle,order)) {
                        log("Lab laser muzzle schema=1 order=%llu input=%llu generation=%u owner=%u weapon=%u hand=%u now=%llu cacheValid=%u cacheWeapon=%u cacheTick=%llu alignment=%u wants=%u attempted=%u captured=%u failure=%u equal=%u admitted=%u renderDepth=%u",
                            order,s.input.sequence,s.generation,s.playerHandle,context.handle,context.hand,now,
                            unsigned(c.valid),c.handle,c.tickMs,unsigned(c.alignmentApplied),unsigned(wantsAlignment),
                            unsigned(attempted),unsigned(captured),failure,unsigned(captured&&c.modelBinding==binding),
                            unsigned(calibrationAdmitted),nativeWeaponRenderDepth);
                        const auto emitBinding=[&](const char *kind,const WeaponAlignmentBinding &b) {
                            log("Lab laser binding schema=1 order=%llu kind=%s owner=%u weapon=%u model=%u instance=%u selector=%u hand=%u cfg=%u file=%u resource=%d stretch=%08x,%08x,%08x",
                                order,kind,b.owner,b.weapon,b.model,b.instance,b.selector,b.hand,b.configuration,
                                b.file,b.resource,std::bit_cast<uint32_t>(b.baseStretch.x),
                                std::bit_cast<uint32_t>(b.baseStretch.y),std::bit_cast<uint32_t>(b.baseStretch.z));
                        };
                        emitBinding("cache",c.modelBinding);
                        if(captured)emitBinding("current",binding);
                    }
                    if(!calibrationAdmitted) {
                        static const bool shotLog = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SHOT_LOG", v, 2) == 1 && v[0] == L'1'; }();
                        static unsigned notAdmitted = 0;
                        if (shotLog && (++notAdmitted <= 80 || notAdmitted % 100 == 0))
                            log("Lab calibration not admitted hand=%u valid=%d handle=%d age=%lld attempted=%d captured=%d match=%d applied=%d handValid=%d,%d n=%u",
                                context.hand, int(c.valid), int(c.handle == context.handle), (long long)(now - c.tickMs),
                                int(attempted), int(captured), int(captured && weaponModelCalibrationMatches(c.modelBinding,binding,c.alignmentApplied)),
                                int(c.alignmentApplied), int(s.input.handValid[0]), int(s.input.handValid[1]), notAdmitted);
                        invalidateMatchingCalibration(s,context.hand,1);
                        // Never certify a cached model after a binding/reference change:
                        // the shot leaves from the tracked hand (or its measured emitter)
                        // uncalibrated instead of from the flat camera's native pose.
                    }
                }
                Pose target;
                if (calibrationAdmitted)
                    target = handAlignedMuzzle(nativeCamera, *out, c.nativeModelLocal, c.nativeDisplacement,
                                               c.nativeAlignment, hand, weaponModelScale(settings, primaryField(w, 0xb4)));
                else
                    target = {normalize(hand.q), hand.p};
                // Weapons whose native shot leaves from the camera (the zap gun's
                // bolt) mapped to a point far off the displayed gun: shots and
                // laser start at the measured emitter instead.
                if (const Vec3 *emitter = measuredEmitter(primaryField(w, 0xb4)))
                    target = {normalize(hand.q), hand.p + rotate(normalize(hand.q), *emitter * emitterScale(primaryField(w, 0xb4)))};
                diagnosticRejection=17;
                if (!finite(target) || !localWeaponCurrent(w, s, context.hand))
                    return;
                diagnosticRejection=18;
                if(calibrationAdmitted) {
                    WeaponAlignmentBinding current;
                    if(!captureWeaponModelBinding(context.player,w,context.hand,false,current) ||
                       !weaponModelCalibrationMatches(c.modelBinding,current,c.alignmentApplied)) {
                        invalidateMatchingCalibration(s,context.hand,2);
                        return;
                    }
                }
                // Exact-call attachment provenance joins only this accepted
                // binding and copied calibration. It does NOT assert that the
                // earlier draw's animation transform is unchanged.
                static std::atomic<bool> attachmentWitnessReported{false};
                if(observeAttachment && c.alignmentApplied && c.modelBinding.nativeId==13 && c.labSerial &&
                   attachmentReceipt.admitted(c.modelBinding.instance) &&
                   !attachmentWitnessReported.exchange(true,std::memory_order_relaxed)) {
                    const auto &a=attachmentReceipt;
                    log("Lab sniper attachment schema=1 input=%llu generation=%u owner=%u weapon=%u hand=%u nativeId=13 model=%u instance=%u ident=%u calls=%u result=%d calibrationSerial=%llu drawRequest=%llu drawInput=%llu drawEye=%d cacheAge=%llu cfg=%u file=%u resource=%d nativeReturn=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
                        s.input.sequence,s.generation,c.modelBinding.owner,c.modelBinding.weapon,context.hand,
                        c.modelBinding.model,c.modelBinding.instance,a.ident,a.calls,a.result,c.labSerial,c.labRequest,
                        c.labInput,c.labEye,now-c.tickMs,c.modelBinding.configuration,c.modelBinding.file,c.modelBinding.resource,
                        nativeReturned.p.x,nativeReturned.p.y,nativeReturned.p.z,nativeReturned.q.x,
                        nativeReturned.q.y,nativeReturned.q.z,nativeReturned.q.w);
                    const auto emitMatrix=[&](const char *kind,const Matrix34 &m) {
                        log("Lab sniper attachment matrix schema=1 calibrationSerial=%llu kind=%s words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
                            c.labSerial,kind,std::bit_cast<uint32_t>(m.m[0]),std::bit_cast<uint32_t>(m.m[1]),
                            std::bit_cast<uint32_t>(m.m[2]),std::bit_cast<uint32_t>(m.m[3]),std::bit_cast<uint32_t>(m.m[4]),
                            std::bit_cast<uint32_t>(m.m[5]),std::bit_cast<uint32_t>(m.m[6]),std::bit_cast<uint32_t>(m.m[7]),
                            std::bit_cast<uint32_t>(m.m[8]),std::bit_cast<uint32_t>(m.m[9]),std::bit_cast<uint32_t>(m.m[10]),
                            std::bit_cast<uint32_t>(m.m[11]));
                    };
                    emitMatrix("nativeAttachment",a.absolute);emitMatrix("calibrationNativeModel",c.labNativeModel);
                }
                // One private neutral original-return observation. No native
                // getter is added and the existing clamp/output is unchanged.
                static std::atomic<bool> muzzleWitnessReported{false};
                if(snapshot && context.hand==1 && c.alignmentApplied && c.modelBinding.nativeId==13 &&
                   labSniperConfigured && labPreparation->sniperZoom && labOnlineIsolationInstalled() &&
                   primaryField(w,0xb0)==1 && !primaryField(w,0xd4) &&
                   !s.fire[0] && !s.fire[1] && s.input.trigger[0]==0 && s.input.trigger[1]==0 &&
                   !s.input.zoomDownMask && !muzzleWitnessReported.exchange(true,std::memory_order_relaxed)) {
                    const auto local=compose(inverse(nativeCamera),nativeReturned);
                    const auto v=local.p-(c.nativeModelLocal.p-c.nativeDisplacement)+c.nativeAlignment;
                    const auto radius=std::sqrt(dot(v,v));
                    const auto &b=c.modelBinding;
                    log("Lab sniper muzzle witness schema=1 input=%llu generation=%u owner=%u weapon=%u hand=%u nativeId=%u model=%u instance=%u selector=%u cfg=%u file=%u resource=%d stretch=%.9g,%.9g,%.9g cacheAge=%llu nativeReturn=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g camera=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g modelLocal=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g handPose=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g charge=%.9g,%.9g,%.9g correction=%.9g,%.9g,%.9g vector=%.9g,%.9g,%.9g radius=%.9g bound=0.5 reachPreserved=%u target=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
                        s.input.sequence,s.generation,b.owner,b.weapon,context.hand,b.nativeId,b.model,b.instance,b.selector,
                        b.configuration,b.file,b.resource,b.baseStretch.x,b.baseStretch.y,b.baseStretch.z,now-c.tickMs,
                        nativeReturned.p.x,nativeReturned.p.y,nativeReturned.p.z,nativeReturned.q.x,nativeReturned.q.y,nativeReturned.q.z,nativeReturned.q.w,
                        nativeCamera.p.x,nativeCamera.p.y,nativeCamera.p.z,nativeCamera.q.x,nativeCamera.q.y,nativeCamera.q.z,nativeCamera.q.w,
                        c.nativeModelLocal.p.x,c.nativeModelLocal.p.y,c.nativeModelLocal.p.z,c.nativeModelLocal.q.x,c.nativeModelLocal.q.y,c.nativeModelLocal.q.z,c.nativeModelLocal.q.w,
                        hand.p.x,hand.p.y,hand.p.z,hand.q.x,hand.q.y,hand.q.z,hand.q.w,
                        c.nativeDisplacement.x,c.nativeDisplacement.y,c.nativeDisplacement.z,
                        c.nativeAlignment.x,c.nativeAlignment.y,c.nativeAlignment.z,v.x,v.y,v.z,radius,
                        unsigned(calibrationAdmitted),
                        target.p.x,target.p.y,target.p.z,target.q.x,target.q.y,target.q.z,target.q.w);
                }
                laserTraceValue(LaserTraceStage::Muzzle,"certified",s,context.hand,0,
                                calibrationAdmitted?0:1,&c);
                if (calibrated)
                    *calibrated = calibrationAdmitted;
                *out = target;
                diagnosticRejection=0;
            }, calibrated);
        if(snapshot)
            laserTraceValue(LaserTraceStage::Muzzle,"attempt-completed",s,context.hand,0,
                            diagnosticRejection);
    }, [&](bool aborted) noexcept {
        if(attachmentPublished) {
            activeAttachmentObservation=previousAttachment;attachmentPublished=false;
        }
        if(aborted)attachmentReceipt.aborted=true;
        nativeShotDepth = previousDepth;
        if (aborted) {
            if (calibrated) *calibrated = false;
            nativeInputFailed();
        }
    });
    return out;
}
static Pose *__fastcall shot(void *w, void *, Pose *out) {
    // Lab (SS2VR_LAB_SHOT_LOG=1): where each fired shot starts and points.
    static const bool shotLog = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SHOT_LOG", v, 2) == 1 && v[0] == L'1'; }();
    if (!shotLog || nativeShotDepth) return shooting(w, out, originalShot);
    bool calibrated = false;
    shooting(w, out, originalShot, nullptr, &calibrated);
    static unsigned logged = 0;
    if (++logged <= 60 || logged % 50 == 0) {
        const auto s = copySnapshot();
        const int slot = handOf(w, s);
        const Vec3 d = rotate(normalize(out->q), {0, 0, -1});
        void *pl = livePlayer(s);
        const int wid = *reinterpret_cast<int *>(static_cast<uint8_t *>(w) + 0xb4);
        log("Lab shot ammo=%d owner=%08x player=%08x", pl && ammo ? ammo(static_cast<uint8_t *>(pl) + 0x8f8, wid) : -1,
            primaryField(w, 0x28), s.playerHandle);
        if (slot < 0 && pl && readableMemory(pl, 0x1000)) {
            // Where the player still refers to this weapon (lab: weapons fired without a hand).
            char refs[256]{};
            size_t n = 0;
            for (unsigned off = 0; off + 4 <= 0x1000 && n + 12 < sizeof refs; off += 4) {
                uint32_t v;
                std::memcpy(&v, static_cast<uint8_t *>(pl) + off, 4);
                if (v && v < 0x40000000u && resolve(v) == w) n += std::snprintf(refs + n, sizeof refs - n, " %x", off);
            }
            log("Lab shot unheld weapon refs:%s native=%08x,%08x", refs, nativeHandle(pl, 0), nativeHandle(pl, 1));
        }
        log("Lab shot caller=%p slot=%d mount=%u weapon=%d handValid=%d,%d gripValid=%d,%d calibrated=%d origin=%.3f,%.3f,%.3f dir=%.3f,%.3f,%.3f",
            __builtin_return_address(0), slot, slot >= 0 ? mountHand(s, unsigned(slot)) : 9u, *reinterpret_cast<int *>(static_cast<uint8_t *>(w) + 0xb4),
            int(s.input.handValid[0]), int(s.input.handValid[1]), int(s.input.gripValid[0]), int(s.input.gripValid[1]),
            int(calibrated), out->p.x, out->p.y, out->p.z, d.x, d.y, d.z);
    }
    return out;
}
static Pose *__fastcall sniperShot(void *w, void *, Pose *out) {
    return shooting(w, out, originalSniperShot);
}
// These are the exact shared counts/reset pointer cleared by Engine DAD90.
// The child-traversal flag 2C7F9C is not an idle indicator or a lock.
static bool laserModelScratchIdle() {
    if (!laserEngineBase || !nativeMainThread || !nativeMainThread())
        return false;
    NativeModelScratch scratch;
    constexpr uint32_t counts[] = {0x2eac28, 0x2eac38, 0x2eac48, 0x2eac58, 0x2eac68,
                                   0x2eac78, 0x2eac98, 0x2eac88, 0x2eab40};
    for (unsigned i = 0; i != scratch.counts.size(); ++i)
        memcpy(&scratch.counts[i], reinterpret_cast<void *>(laserEngineBase + counts[i]), 4);
    memcpy(&scratch.evaluated, reinterpret_cast<void *>(laserEngineBase + 0x2eab68), 4);
    return scratch.idle();
}
static bool laserRead32(const void *object, size_t offset, uint32_t &out) {
    if (!readableMemory(object, offset + 4))
        return false;
    memcpy(&out, static_cast<const uint8_t *>(object) + offset, 4);
    return true;
}
// Presence validation for the original selector's unchecked pointer lookups.
// Selection remains native; no resource/action/idle policy is retained here.
static uint32_t laserFindIdent(const void *object, size_t pointerOffset, uint32_t ident) {
    uint32_t backing = 0, count = 0, capacity = 0;
    if (!laserRead32(object, pointerOffset - 4, capacity) || !laserRead32(object, pointerOffset, backing) ||
        !laserRead32(object, pointerOffset + 4, count) || !count || count > 4096 || count > capacity ||
        !readableMemory(reinterpret_cast<void *>(backing), size_t(count) * 4))
        return 0;
    for (uint32_t i = 0; i != count; ++i) {
        uint32_t pointer = 0, name = 0;
        memcpy(&pointer, reinterpret_cast<void *>(backing + i * 4), 4);
        if (!laserRead32(reinterpret_cast<void *>(pointer), 0, name))
            return 0;
        if (name == ident)
            return pointer;
    }
    return 0;
}
// Any other ride (other turret classes, vehicles with guns): a generic source,
// marked by attachment 0, whose beam follows the ride's own aim (its view
// origin and GetShootDirection) instead of a model attachment.
static bool genericVehicleLaserSource(const Snapshot &s, VehicleLaserSource &out) {
    void *ride = resolve(s.rider.ride);
    VehicleLaserSource source;
    source.rider = s.rider;
    if (!ride || !laserRead32(ride, 0x114, source.mechanism) || !source.mechanism || !resolve(source.mechanism))
        return false;
    source.model = s.rider.ride; source.instance = ~0u; source.resource = 1; source.config = 1;
    source.attachment = 0;
    if (!source.usable() || !nativeRiderCurrent(s.player, s.rider) || resolve(s.rider.ride) != ride) return false;
    out = source;
    return true;
}
static bool auditedVehicleLaserSource(const Snapshot &s, VehicleLaserSource &out);
static bool attachedVehicleLaserSource(const Snapshot &s, VehicleLaserSource &out, bool turretOnly);
static bool readVehicleLaserSource(const Snapshot &s, VehicleLaserSource &out) {
    if (!s.rider.seated() || !nativeMainThread() || !nativeRiderCurrent(s.player, s.rider))
        return false;
    // The audited turret with its laser attachment; any other ride whose model
    // has its firing attachment (the beam then leaves the gun's muzzle, where
    // the old eye-level beam was hidden by the vehicle's own dashboard); else
    // generic.
    if (auditedVehicleLaserSource(s, out)) return true;
    VehicleLaserSource attached;
    if (attachedVehicleLaserSource(s, attached, false) && laserModelScratchIdle()) {
        Matrix34 placement;
        if (laserAttachment(reinterpret_cast<void *>(attached.instance), attached.attachment, placement) != 0) {
            out = attached;
            return true;
        }
    }
    return genericVehicleLaserSource(s, out);
}
static bool auditedVehicleLaserSource(const Snapshot &s, VehicleLaserSource &out) {
    return attachedVehicleLaserSource(s, out, true);
}
// The ride's firing process blast attachment (the muzzle the model fires from):
// audited for the turret, tried for other rides by the diagnostics.
static bool attachedVehicleLaserSource(const Snapshot &s, VehicleLaserSource &out, bool turretOnly) {
    void *ride = resolve(s.rider.ride);
    uint32_t table = 0;
    VehicleLaserSource source;
    source.rider = s.rider;
    if (!laserRead32(ride, 0, table) || (turretOnly && table != laserTurretVtable) ||
        !laserRead32(ride, 0x120, source.model) || !laserRead32(ride, 0x47c, source.resource) ||
        !laserRead32(ride, 0x1bc, source.action) || !laserRead32(ride, 0x1c4, source.blast) ||
        !laserRead32(ride, 0x114, source.mechanism) || !source.mechanism || !resolve(source.mechanism))
        return false;
    void *renderable = source.model ? resolve(source.model) : nullptr;
    if (!renderable || !readableMemory(renderable, 0x60))
        return false;
    source.instance = reinterpret_cast<uintptr_t>(laserModelInstance(renderable));
    if (!laserRead32(reinterpret_cast<void *>(source.instance), 0x18, source.config))
        return false;
    source.attachment = laserIdleAttachment;
    if (source.action != *invalidSeatIdent && source.blast != *invalidSeatIdent) {
        const auto process = laserFindIdent(reinterpret_cast<void *>(source.resource), 0x298, source.action);
        void *processPointer = reinterpret_cast<void *>(process);
        if (!process || resolve(pointerHandle(processPointer)) != processPointer)
            return false;
        const auto blast = laserFindIdent(processPointer, 0x64, source.blast);
        if (!blast || !laserRead32(reinterpret_cast<void *>(blast), 4, source.attachment))
            return false;
    }
    if (!source.usable() || source.attachment == *invalidSeatIdent ||
        !nativeRiderCurrent(s.player, s.rider) || resolve(s.rider.ride) != ride ||
        resolve(source.model) != renderable)
        return false;
    out = source;
    return true;
}
static bool laserSampleCurrent(const Snapshot &s, const Request &request) {
    const auto live = copySnapshot();
    if (!vrSession(live) || s.rigRevision != live.rigRevision ||
        !livePlayer(live) || live.player != s.player ||
        !compatibleRiderInput(s.rider, live.rider, s.input, live.input, s.generation, live.generation,
                              GetTickCount64()) || !nativeRiderCurrent(s.player, s.rider) ||
        !nativeFrameIdentity(request, live.input, live.generation, live.initialized, live.ui.gameplay, true))
        return false;
    Lock lock(channel, 1);
    if (lock)
        for (const auto &slot : channel.shared->slot)
            if (slot.state == SlotState::Requested && !slot.cancelled && sameRequest(slot.request, request))
                return true;
    return false;
}
static bool vehicleLaserCurrent(const Snapshot &s, const Request &request,
                                const VehicleLaserSource &expected) {
    VehicleLaserSource live;
    return laserSampleCurrent(s, request) && laserModelScratchIdle() &&
           readVehicleLaserSource(s, live) && live == expected;
}
// Uzi laser emitter (the tube under the barrel) from the uzi's muzzle (its
// barrel nub, measuredEmitter), in the weapon's frame at scale 1: the beam starts
// 1 cm inside the tube's front face, where the game's own uzi laser starts
// (V428-V429: 12 cm below the nub, its front 3.5 cm behind it).
constexpr Vec3 UziLaserEmitter{0, -.118f, .021f};
// Colt laser emitter (the tube under its barrel, whose lens the game's own Colt
// laser starts from) from its shot origin, at scale 1 (V470 side view: lens
// 12 cm behind and 8 cm below the shot origin at the 0.85 size; V484 rolled
// views: 3.3 cm to the right of it). Mirrored for the left hand's Colt.
constexpr Vec3 ColtLaserEmitter{.039f, -.0965f, .1435f};
// Lab (SS2VR_LAB_MUZZLE_MARK=1): the shot origin of each hand's weapon, drawn as
// a green cross through any geometry.
static Vec3 labShotMuzzle[2];
static bool labShotMuzzleValid[2];
static Vec3 genericBeamOffset{};
static bool vehicleLaserMuzzle(const Snapshot &s, const Request &request, VehicleLaserSource &source,
                               Pose &muzzle, Vec3 &rayDirection) {
    if (!laserModelScratchIdle() || !readVehicleLaserSource(s, source) ||
        !vehicleLaserCurrent(s, request, source))
        return false;
    void *ride = resolve(source.rider.ride);
    auto *table = *reinterpret_cast<uintptr_t **>(ride);
    if (!source.attachment) {
        // Generic ride: from just below and ahead of the ride's eye along its aim.
        Pose origin;
        reinterpret_cast<ViewOrigin>(table[0xac / 4])(ride, &origin, 1);
        Vec3 direction;
        reinterpret_cast<ShootDirection>(table[0x5a4 / 4])(ride, &direction);
        if (!vehicleLaserCurrent(s, request, source) || !nativeLaserPose(origin, direction, muzzle, &rayDirection))
            return false;
        // The ray is cast from the eye along the aim (where shots go); the beam
        // is drawn from just below and ahead of it to that hit (sampling applies
        // genericBeamOffset), so it converges on the target.
        genericBeamOffset = rayDirection * .5f + Vec3{0, -.25f, 0};
        return finite(muzzle);
    }
    uint32_t attachment = *invalidSeatIdent;
    reinterpret_cast<AttachmentIdent>(table[0x550 / 4])(ride, &attachment);
    if (attachment != source.attachment || !vehicleLaserCurrent(s, request, source))
        return false;
    Matrix34 placement;
    const bool found = laserAttachment(reinterpret_cast<void *>(source.instance), attachment, placement) != 0;
    if (!found || !vehicleLaserCurrent(s, request, source))
        return false; // Missing native attachment must not admit the identity fallback.
    Pose origin;
    reinterpret_cast<ViewOrigin>(table[0xac / 4])(ride, &origin, 1);
    if (!vehicleLaserCurrent(s, request, source))
        return false;
    Vec3 direction;
    reinterpret_cast<ShootDirection>(table[0x5a4 / 4])(ride, &direction);
    if (!vehicleLaserCurrent(s, request, source)) return false;
    // The beam follows the barrel: from its firing attachment along its axis. The
    // eye's shot direction keeps rising past the barrel's limit at the top of a
    // turret's arc, where the old beam (cast from the eye) left the real line of
    // fire. The attachment's forward axis is found once per ride class, against
    // the shot direction while the two agree (mid-range aim).
    {
        static uintptr_t axisClass = 0;
        static int axis = -1; // 2*column + (1: negated)
        const uintptr_t rideClass = table ? reinterpret_cast<uintptr_t>(table) : 0;
        if (rideClass != axisClass) { axisClass = rideClass; axis = -1; }
        Vec3 columns[3]{{placement.m[0], placement.m[4], placement.m[8]}, {placement.m[1], placement.m[5], placement.m[9]},
                        {placement.m[2], placement.m[6], placement.m[10]}};
        const float shotLength = std::sqrt(dot(direction, direction));
        bool columnsOk = std::isfinite(shotLength) && shotLength > 1e-6f;
        for (auto &c : columns) {
            const float n = std::sqrt(dot(c, c));
            if (!(n > 1e-6f) || !std::isfinite(n)) columnsOk = false;
            else c = c * (1 / n);
        }
        if (columnsOk && axis < 0) {
            const Vec3 shot = direction * (1 / shotLength);
            int best = -1;
            float bestDot = .97f;
            for (int c = 0; c < 3; ++c) {
                const float d = dot(columns[c], shot);
                if (std::abs(d) > bestDot) { bestDot = std::abs(d); best = 2 * c + (d < 0 ? 1 : 0); }
            }
            if (best >= 0) {
                axis = best;
                log("Vehicle laser barrel axis=%d (agreement %.3f)", axis, bestDot);
            }
        }
        if (columnsOk && axis >= 0) {
            const Vec3 barrel = columns[axis / 2] * (axis & 1 ? -1.f : 1.f);
            const Pose at{origin.q, Vec3{placement.m[3], placement.m[7], placement.m[11]}};
            static unsigned traced = 0;
            if (traced++ % 200 == 0) {
                const Vec3 shot = direction * (1 / shotLength);
                log("Vehicle laser barrel=%.3f,%.3f,%.3f shot=%.3f,%.3f,%.3f apart=%.3f rad", barrel.x, barrel.y, barrel.z,
                    shot.x, shot.y, shot.z, std::acos(std::clamp(dot(barrel, shot), -1.f, 1.f)));
            }
            if (finite(at) && nativeLaserPose(at, barrel, muzzle, &rayDirection)) return true;
        }
    }
    return nativeLaserPose(origin, direction, muzzle, &rayDirection);
}
// Spatial audio occlusion: rays from the listener toward the sounds due a
// trace (spatial_audio.hpp), in the same native query lifecycle as the lasers.
static void traceSoundOcclusion() {
    if (!settings.spatialAudio || !settings.audioOcclusion) return;
    const auto s = copySnapshot();
    if (!s.initialized || !livePlayer(s) || !s.ui.gameplay) return;
    Vec3 from{};
    uint32_t ids[spatial_audio_common::TracesPerFrame];
    Vec3 ends[spatial_audio_common::TracesPerFrame];
    const unsigned n = spatial_audio::dueTraces(from, ids, ends, spatial_audio_common::TracesPerFrame);
    if (!n) return;
    void *mechanism = getMechanism ? getMechanism(s.player) : nullptr;
    laserQuerying = true;
    for (unsigned i = 0; i < n; ++i) {
        const Vec3 d = ends[i] - from;
        const float distance = std::sqrt(dot(d, d));
        if (!(distance > .05f) || !std::isfinite(distance)) { spatial_audio::traced(ids[i], false, 0, 0); continue; }
        originalRayInit();
        NativeRay ray{from, d * (1 / distance)};
        setRay(ray);
        rayMaximum(distance);
        rayMinimum(.05f);
        rayRadius(0);
        rayCategory(bulletCategory);
        thickCategory(bulletCategory);
        rayAvatar(s.player);
        rayMechanism(mechanism);
        rayFluids(0);
        checkRay();
        const bool hit = rayHit() != 0;
        const float at = hit ? hitDistance() : distance;
        originalRayInit();
        spatial_audio::traced(ids[i], hit, at, distance);
    }
    laserQuerying = false;
    static unsigned traced = 0;
    if (traced < 20 && GetTickCount64() % 1000 < 50) {
        ++traced;
        log("Spatial audio: %u occlusion rays from %.1f,%.1f,%.1f, first to %.1f,%.1f,%.1f", n, from.x, from.y, from.z,
            ends[0].x, ends[0].y, ends[0].z);
    }
}
static void trackedRayInitBody(uintptr_t caller, bool outermost) {
    // The native caller is intentionally beginning a new query. Retire its old
    // query first, use the same native lifecycle for our rays, then leave a fresh
    // baseline for that caller. Never restore dangling native hit/cleanup state.
    originalRayInit();
    if (!outermost || nativeRayFaulted.load(std::memory_order_acquire) || laserQuerying ||
        eyeIndex >= 0 || scopeSource.active || caller != allowedRayReturn ||
        simulationThread.load(std::memory_order_relaxed) != GetCurrentThreadId() || !nativeMainThread()) {
        if(caller==allowedRayReturn && outermost && !laserQuerying)
            laserTraceValue(LaserTraceStage::Query,"admission-rejected",Snapshot{},2,0,1,nullptr,caller);
        return;
    }
    traceSoundOcclusion();
    auto s = copySnapshot();
    if ((!settings.lasers && !s.zoom[0] && !s.zoom[1]) ||
        !vrSession(s) || !fresh(s.input) || !s.ui.gameplay || !livePlayer(s)) {
        laserTraceValue(LaserTraceStage::Query,"snapshot-rejected",s,2,0,2);return;
    }
    bool requested = false;
    Request queryRequest;
    {
        Lock lock(channel, 1);
        if (lock) {
            // The newest request, as the render picks it (bridge acquireRequest).
            const auto &slots = channel.shared->slot;
            const unsigned first = slots[1].request.sequence > slots[0].request.sequence ? 1u : 0u;
            for (unsigned i : {first, 1u - first}) {
                const auto &slot = slots[i];
                if (slot.state == SlotState::Requested && !slot.cancelled &&
                    nativeFrameIdentity(slot.request, s.input, s.generation, s.initialized, s.ui.gameplay,
                                        true) &&
                    fresh(slot.request.input)) {
                    s.input = slot.request.input;
                    queryRequest = slot.request;
                    requested = true;
                    break;
                }
            }
        }
    }
    if (!requested) {
        laserTraceValue(LaserTraceStage::Query,"request-missing",s,2,0,3);return;
    }
    Pose body;
    if (!trackingAnchor(s.player, body)) {
        laserTraceValue(LaserTraceStage::Query,"anchor-rejected",s,2,queryRequest.sequence,4);return;
    }
    laserTraceValue(LaserTraceStage::Query,"admitted",s,2,queryRequest.sequence,0);
    LaserAim existing[2];
    AcquireSRWLockShared(&laserLock);
    memcpy(existing, laserAim, sizeof(existing));
    ReleaseSRWLockShared(&laserLock);
    laserQuerying = true;
    LaserAim sampled[2];
    // A transient rejection (request replaced mid-sample, calibration refresh,
    // native scratch busy) keeps the previous grip-relative beam (age-bounded at
    // freeze) instead of blanking it for a frame.
    bool transient[2]{};
    for (unsigned h = 0; h != 2; ++h) {
        // Slot h's weapon is held by controller m (a lone weapon may sit on the other hand).
        const unsigned m = s.rider.handheld() ? mountHand(s, h) : h;
        // The weapon hides when its grip pose is lost; the aim pose can stay valid
        // (last pose, untracked) and drew a stale, skewed beam.
        if ((!settings.lasers && !(s.rider.handheld() && s.zoom[h])) || !s.input.handValid[m] ||
            (!s.rider.seated() && !s.input.gripValid[m]))
            { laserTraceValue(LaserTraceStage::Sample,"rejected",s,h,queryRequest.sequence,1);continue; }
        Pose muzzle, local{};
        bool relative = false;
        Vec3 direction;
        VehicleLaserSource vehicle;
        const bool mounted = s.rider.seated();
        uint32_t sourceHandle = s.handle[h];
        void *mechanism = nullptr;
        if (mounted) {
            const bool sourced = h == 1 && vehicleLaserMuzzle(s, queryRequest, vehicle, muzzle, direction);
            if (h == 1) {
                static uint64_t lastTrace = 0;
                static unsigned traced = 0;
                if (GetTickCount64() - lastTrace > 2000 && traced < 40) {
                    lastTrace = GetTickCount64(); ++traced;
                    VehicleLaserSource probe;
                    void *ride = resolve(s.rider.ride);
                    VehicleLaserSource attached;
                    const bool muzzle = attachedVehicleLaserSource(s, attached, false);
                    log("Vehicle laser sample source=%d read=%d ride=%08x vtable=%lx mechanism=%08x action=%08x blast=%08x muzzle=%d attachment=%08x",
                        int(sourced), int(readVehicleLaserSource(s, probe)), s.rider.ride,
                        static_cast<unsigned long>(ride && laserTurretVtable ? primaryField(ride, 0) - (laserTurretVtable - 0x2b39e8) : 0),
                        ride ? primaryField(ride, 0x114) : 0, ride ? primaryField(ride, 0x1bc) : 0, ride ? primaryField(ride, 0x1c4) : 0,
                        int(muzzle), attached.attachment);
                }
            }
            if (!sourced)
                { laserTraceValue(LaserTraceStage::Sample,"rejected",s,h,queryRequest.sequence,2);continue; }
            if (creatureAimRide(s.rider)) {
                // A hand-aimed dinosaur: the beam leaves the right hand along its aim.
                const Pose hand = worldHandTracking(body, s.origin, s.turn, s.input.head, s.input.hand[1]);
                if (!finite(hand)) { laserTraceValue(LaserTraceStage::Sample,"rejected",s,h,queryRequest.sequence,2);continue; }
                muzzle = {normalize(hand.q), hand.p};
                direction = rotate(muzzle.q, Vec3{0, 0, -1});
            }
            sourceHandle = s.rider.ride;
            mechanism = resolve(vehicle.mechanism);
        } else {
            if (!s.rider.handheld() || aliased(s) || s.ui.wheel[m].open || !s.handle[h] || s.selecting[h] ||
                grenadeShownHand.load(std::memory_order_relaxed) == int(m) || bombLighterSlot(s, h))
                { laserTraceValue(LaserTraceStage::Sample,"rejected",s,h,queryRequest.sequence,3);continue; }
            void *w = resolve(s.handle[h]);
            if (!w || nativeHandle(s.player, h) != s.handle[h] || !laserModelScratchIdle())
                { laserTraceValue(LaserTraceStage::Sample,"rejected",s,h,queryRequest.sequence,4);continue; }
            const int weapon = *reinterpret_cast<int *>(static_cast<uint8_t *>(w) + 0xb4);
            // The circular saw cuts what it touches and the serious bomb clears
            // everything around regardless of aim: neither has use for a laser.
            if (weapon == 0 || weapon == 11)
                { laserTraceValue(LaserTraceStage::Sample,"rejected",s,h,queryRequest.sequence,5);continue; }
            bool calibrated = false;
            shooting(w, &muzzle, weapon == 13 ? originalSniperShot : originalShot, &s, &calibrated);
            const bool usable = calibrated || measuredEmitter(uint32_t(weapon)); // An emitter needs no model calibration.
            if (!finite(muzzle) || !usable || !localWeaponCurrent(w, s, h) || !laserModelScratchIdle()) {
                const unsigned reason = !finite(muzzle) ? 50 : !usable ? 51 : !localWeaponCurrent(w, s, h) ? 52 : 53;
                transient[h] = reason == 51 || reason == 53;
                laserTraceValue(LaserTraceStage::Sample,"rejected",s,h,queryRequest.sequence,reason);continue;
            }
            mechanism = getMechanism(s.player);
            direction = rotate(normalize(muzzle.q), {0, 0, -1});
            labShotMuzzle[h] = muzzle.p; // Where shots start (before the uzi laser's tube offset).
            labShotMuzzleValid[h] = true;
            // The uzi's and the Colt's beams start at the laser emitter tube under
            // their barrels, where the game's own lasers start (V420/V421, V470).
            if (weapon == 4 || weapon == 12) {
                // Lab: SS2VR_LAB_UZI_EMITTER / SS2VR_LAB_COLT_EMITTER=x,y,z override the offsets.
                static const auto labEmitter = [](const char *name, Vec3 e) {
                    char v[64]{};
                    if (GetEnvironmentVariableA(name, v, sizeof v)) std::sscanf(v, "%f,%f,%f", &e.x, &e.y, &e.z);
                    return e;
                };
                static const Vec3 uzi = labEmitter("SS2VR_LAB_UZI_EMITTER", UziLaserEmitter),
                                  colt = labEmitter("SS2VR_LAB_COLT_EMITTER", ColtLaserEmitter);
                Vec3 emitter = weapon == 4 ? uzi : colt;
                if (weapon == 12 && h == 0) emitter.x = -emitter.x; // The left Colt is the mirrored model.
                muzzle.p = muzzle.p + rotate(normalize(muzzle.q), emitter * emitterScale(uint32_t(weapon)));
            }
            // The same grip pose shooting() and the model placement use.
            const Pose grip = worldHandTracking(body, s.origin, s.turn, s.input.head, weaponGrip(s.input, m, weapon));
            local = compose(inverse(grip), muzzle);
            relative = finite(grip) && finite(local);
        }
        if (!laserSampleCurrent(s, queryRequest) || !trackedHandCurrent(s.input, copySnapshot().input, m))
            { transient[h] = true; laserTraceValue(LaserTraceStage::Sample,"rejected",s,h,queryRequest.sequence,6);continue; }
        const auto &old = existing[h];
        const uint64_t now = GetTickCount64();
        // Moving native aim may change without an origin change. Mounted rays
        // deliberately do not borrow handheld cache reuse.
        if (!mounted && old.valid && old.kind == LaserSourceKind::Handheld &&
            old.requestSequence == queryRequest.sequence && old.sequence == s.input.sequence &&
            old.generation == s.generation && old.owner == s.playerHandle && old.weapon == sourceHandle &&
            now >= old.tickMs && now - old.tickMs < 80 && std::memcmp(&old.body, &body, sizeof(body)) == 0 &&
            std::memcmp(&old.muzzle, &muzzle, sizeof(muzzle)) == 0) {
            sampled[h] = old;
            continue;
        }
        originalRayInit();
        NativeRay ray{muzzle.p, direction};
        setRay(ray);
        rayMaximum(settings.laserDistance);
        rayMinimum(.01f);
        rayRadius(0);
        rayCategory(bulletCategory);
        thickCategory(bulletCategory);
        rayAvatar(s.player);
        rayMechanism(mechanism);
        rayFluids(0);
        checkRay();
        const bool hit = rayHit() != 0;
        const float distance = hit ? hitDistance() : settings.laserDistance;
        originalRayInit(); // Retire the native query on success and every rejection below.
        if (!std::isfinite(distance) || distance < 0 || distance > settings.laserDistance ||
            !laserSampleCurrent(s, queryRequest) || !trackedHandCurrent(s.input, copySnapshot().input, m) ||
            (mounted && !vehicleLaserCurrent(s, queryRequest, vehicle)))
            { transient[h] = !mounted; laserTraceValue(LaserTraceStage::Sample,"rejected",s,h,queryRequest.sequence,7);continue; }
        sampled[h] = {muzzle, body, ray.origin + ray.direction * distance, s.playerHandle, sourceHandle,
                      s.generation, s.input.sequence, GetTickCount64(), true, hit};
        sampled[h].kind = mounted ? LaserSourceKind::Vehicle : LaserSourceKind::Handheld;
        sampled[h].vehicle = vehicle;
        if (mounted && !vehicle.attachment && !creatureAimRide(s.rider))
            sampled[h].muzzle.p = sampled[h].muzzle.p + genericBeamOffset; // Converging beam.
        if (mounted && creatureAimRide(s.rider)) {
            static uint64_t lastTrace = 0;
            static unsigned traced = 0;
            if (GetTickCount64() - lastTrace > 1000 && traced < 60) {
                lastTrace = GetTickCount64(); ++traced;
                log("Creature beam from=%.2f,%.2f,%.2f dir=%.3f,%.3f,%.3f hit=%d distance=%.2f", muzzle.p.x, muzzle.p.y, muzzle.p.z,
                    direction.x, direction.y, direction.z, int(hit), distance);
            }
        }
        sampled[h].requestSequence = queryRequest.sequence;
        sampled[h].local = local;
        sampled[h].distance = distance;
        sampled[h].relative = !mounted && relative;
    }
    originalRayInit(); // Runs native collision cleanup and resets all native ray state.
    laserQuerying = false;
    const auto live = copySnapshot();
    if (!laserSampleCurrent(s, queryRequest)) {
        sampled[0] = sampled[1] = {};
        transient[0] = transient[1] = true;
    }
    for (unsigned h = 0; h != 2; ++h)
        if (sampled[h].valid && (!trackedHandCurrent(s.input, live.input, s.rider.handheld() ? mountHand(s, h) : h) ||
            (sampled[h].kind == LaserSourceKind::Vehicle &&
             !vehicleLaserCurrent(s, queryRequest, sampled[h].vehicle)))) {
            transient[h] = sampled[h].kind == LaserSourceKind::Handheld;
            sampled[h] = {};
        }
    for (unsigned h = 0; h != 2; ++h) {
        const auto &old = existing[h];
        if (!sampled[h].valid && transient[h] && old.valid && old.relative && old.kind == LaserSourceKind::Handheld &&
            old.owner == s.playerHandle && old.weapon == s.handle[h] && old.generation == s.generation)
            sampled[h] = old;
    }
    AcquireSRWLockExclusive(&laserLock);
    for (unsigned h = 0; h != 2; ++h)
        laserAim[h] = sampled[h];
    ReleaseSRWLockExclusive(&laserLock);
    for(unsigned h=0;h<2;++h)
        laserTraceValue(LaserTraceStage::Sample,"published",s,h,queryRequest.sequence,sampled[h].valid?0:8);
}
static void __cdecl trackedRayInit() {
#ifdef _MSC_VER
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    const bool outermost = activeNativeRay == nullptr;
    observeNativeRay([&] { trackedRayInitBody(caller, outermost); });
}
static void freezeLasers(const Request &request) {
    if (nativeRayFaulted.load(std::memory_order_acquire)) {
        eyeLasers[0] = eyeLasers[1] = {};
        return;
    }
    LaserFrame frame;
    frame.body = eyeAnchor;
    frame.owner = eyeSnapshot.playerHandle;
    frame.generation = eyeSnapshot.generation;
    frame.sequence = request.input.sequence;
    frame.now = GetTickCount64();
    frame.requestSequence = request.sequence;
    frame.kind = eyeSnapshot.rider.seated() ? LaserSourceKind::Vehicle : LaserSourceKind::Handheld;
    if (frame.kind == LaserSourceKind::Vehicle && !readVehicleLaserSource(eyeSnapshot, frame.vehicle)) {
        eyeLasers[0] = eyeLasers[1] = {};
        return;
    }
    const auto live = copySnapshot();
    for (unsigned h = 0; h != 2; ++h) {
        frame.weapon[h] = frame.kind == LaserSourceKind::Vehicle ? eyeSnapshot.rider.ride : eyeSnapshot.handle[h];
        const unsigned m = frame.kind == LaserSourceKind::Vehicle ? h : mountHand(eyeSnapshot, h);
        // Handheld beams are drawn from this request's grip pose, like the weapon
        // model, so only this request's tracking matters (a one-sample flicker of
        // the latest input no longer hides a drawn weapon's beam).
        frame.handValid[h] = frame.kind == LaserSourceKind::Vehicle ? trackedHandCurrent(request.input, live.input, m)
            : m < 2 && request.input.handValid[m] && finite(request.input.hand[m]) && request.input.gripValid[m];
        frame.wheel[h] = eyeSnapshot.ui.wheel[m].open != 0;
        frame.selecting[h] = eyeSnapshot.selecting[h];
    }
    AcquireSRWLockShared(&laserLock);
    const LaserAim observed[2]{laserAim[0],laserAim[1]};
    auto pair = freezeLaserPair(laserAim, frame);
    ReleaseSRWLockShared(&laserLock);
    // Rebuild handheld beams from this frame's grip pose: the beam leaves the
    // muzzle of the weapon exactly where weaponAbs draws it.
    for (unsigned h = 0; h != 2; ++h) {
        auto &a = pair[h];
        if (!a.valid || !a.relative || a.kind != LaserSourceKind::Handheld) continue;
        void *held = resolve(a.weapon);
        const unsigned m = mountHand(eyeSnapshot, h);
        const Pose grip = held ? worldHandTracking(eyeAnchor, eyeSnapshot.origin, eyeSnapshot.turn, request.input.head,
                                                   weaponGrip(request.input, m, int(primaryField(held, 0xb4)))) : Pose{};
        const Pose muzzle = compose(grip, a.local);
        if (!held || !finite(grip) || !finite(muzzle)) { a.valid = false; continue; }
        a.muzzle = muzzle;
        a.end = muzzle.p + rotate(normalize(muzzle.q), Vec3{0, 0, -1}) * a.distance;
    }
    // A hand-aimed dinosaur's beam starts at this frame's right hand, to the
    // sampled end (where the hand's ray hit).
    if (frame.kind == LaserSourceKind::Vehicle && creatureAimRide(eyeSnapshot.rider) && pair[1].valid) {
        auto &a = pair[1];
        const Pose hand = worldHandTracking(eyeAnchor, eyeSnapshot.origin, eyeSnapshot.turn, request.input.head, request.input.hand[1]);
        if (finite(hand) && request.input.handValid[1]) a.muzzle = {normalize(hand.q), hand.p};
        else a.valid = false;
    }
    std::copy(pair.begin(), pair.end(), eyeLasers);
    for(unsigned h=0;h<2;++h) {
        uint64_t order;
        if(!laserTraceTicket(LaserTraceStage::Freeze,order))continue;
        const auto &a=observed[h];
        const auto drift=a.body.p-frame.body.p;
        const auto alignment=a.body.q.x*frame.body.q.x+a.body.q.y*frame.body.q.y+
                             a.body.q.z*frame.body.q.z+a.body.q.w*frame.body.q.w;
        log("Lab laser freeze schema=1 order=%llu request=%llu input=%llu generation=%u owner=%u weapon=%u hand=%u eye=%d valid=%u sampleValid=%u sampleRequest=%llu sampleInput=%llu sampleGeneration=%u sampleOwner=%u sampleWeapon=%u now=%llu tick=%llu kinds=%u,%u handValid=%u wheel=%u selecting=%u finite=%u,%u,%u end=%.9g,%.9g,%.9g drift2=%.9g alignment=%.9g",
            order,frame.requestSequence,frame.sequence,frame.generation,frame.owner,frame.weapon[h],h,eyeIndex,
            unsigned(pair[h].valid),unsigned(a.valid),a.requestSequence,a.sequence,a.generation,a.owner,a.weapon,
            frame.now,a.tickMs,unsigned(a.kind),unsigned(frame.kind),unsigned(frame.handValid[h]),
            unsigned(frame.wheel[h]),unsigned(frame.selecting[h]),unsigned(finite(a.muzzle)),
            unsigned(finite(a.body)),unsigned(finite(frame.body)),a.end.x,a.end.y,a.end.z,dot(drift,drift),alignment);
    }
}
static void drawLasers() {
    if (!settings.lasers || !eyeSnapshot.ui.gameplay || !currentCanvas || !*currentCanvas) {
        laserTraceValue(LaserTraceStage::Draw,"gate-rejected",eyeSnapshot,2,eyeRequest.sequence,1);return;
    }
    // Establish identity model and this root's view/projection through native
    // helpers. Root Execute has no parent to restore its matrices.
    ortho();
    activateView(rootView);
    const bool wasDepth = *depthEnabled != 0, wasWrite = *depthWriting != 0;
    const bool wasBlend = *blending != 0, wasAlpha = *alphaTesting != 0;
    const int priorComparison = *depthComparison;
    // Translucent beams (source alpha over the frame) when the blend state can be restored.
    const bool translucent = nativeBlendType && blendSource && blendDestination && setBlendFunction && *setBlendFunction;
    const int priorSource = translucent ? *blendSource : 0, priorDestination = translucent ? *blendDestination : 0;
    (*enableDepth)();
    (*disableDepthWrite)();
    if (translucent) nativeBlendType(501); // GFU_BT_TRANSLUCENT
    else (*disableBlend)();
    (*disableAlpha)();
    (*setDepthComparison)(42); // GfxD3D table at RVA11274 maps native index42 to D3DCMP_LESSEQUAL.
    for (unsigned h = 0; h != 2; ++h) {
        const auto &a = eyeLasers[h];
        if (!a.valid) {
            laserTraceValue(LaserTraceStage::Draw,"sample-invalid",eyeSnapshot,h,eyeRequest.sequence,2);continue;
        }
        {
            static const bool shotLog = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SHOT_LOG", v, 2) == 1 && v[0] == L'1'; }();
            static unsigned drawn[2]{};
            if (shotLog && eyeIndex == 0 && (++drawn[h] % 40) == 1) {
                // Muzzle, direction and the holding controller's grip in the VR eye's frame.
                const Pose eye = worldEyeTracking(eyeAnchor, eyeSnapshot.origin, eyeSnapshot.turn, eyeRequest.input.head,
                                                  eyeRequest.eye[0]);
                const unsigned m = eyeSnapshot.rider.handheld() ? mountHand(eyeSnapshot, h) : h;
                const Pose grip = worldHandTracking(eyeAnchor, eyeSnapshot.origin, eyeSnapshot.turn, eyeRequest.input.head,
                                                    weaponTracking(eyeRequest.input, m));
                const Pose local = compose(inverse(eye), a.muzzle), hand = compose(inverse(eye), grip);
                Vec3 d = rotate(inverse(eye.q), a.end - a.muzzle.p);
                const float len = std::sqrt(dot(d, d));
                if (len > 0) d = d * (1 / len);
                const Vec3 aim = rotate(hand.q, {0, 0, -1});
                void *held = a.weapon ? resolve(a.weapon) : nullptr;
                log("Lab laser eye-space hand=%u id=%d muzzle=%.3f,%.3f,%.3f dir=%.3f,%.3f,%.3f grip=%.3f,%.3f,%.3f aim=%.3f,%.3f,%.3f handValid=%d,%d",
                    h, held ? int(primaryField(held, 0xb4)) : -1, local.p.x, local.p.y, local.p.z, d.x, d.y, d.z, hand.p.x, hand.p.y, hand.p.z, aim.x, aim.y, aim.z,
                    int(eyeSnapshot.input.handValid[0]), int(eyeSnapshot.input.handValid[1]));
            }
        }
        // A laser-pointer red beam: a 3 mm bundle of lines, visible at any distance.
        // ARGB (V109 capture: 0xff1a1aff drew blue). Blended, each line is ~30 %
        // opaque, so where lines overlap the beam is about as see-through as the
        // game's own uzi laser (~40-80 %).
        const uint32_t color = translucent ? 0x4cff1a1au : 0xffff1a1au;
        drawLine(a.muzzle.p, a.end, color, UINT32_MAX);
        for (unsigned k = 0; k != 8; ++k) {
            const float angle = k * Pi / 4;
            const Vec3 side = rotate(a.muzzle.q, {.0015f * std::cos(angle), .0015f * std::sin(angle), 0});
            drawLine(a.muzzle.p + side, a.end + side, color, UINT32_MAX);
        }
        uint64_t order;
        if(laserTraceTicket(LaserTraceStage::Draw,order))
            log("Lab laser dispatch schema=1 order=%llu request=%llu input=%llu generation=%u owner=%u weapon=%u hand=%u eye=%d hit=%u muzzle=%.9g,%.9g,%.9g end=%.9g,%.9g,%.9g linesReturned=3 visible=unproved",
                order,a.requestSequence,a.sequence,a.generation,a.owner,a.weapon,h,eyeIndex,unsigned(a.hit),
                a.muzzle.p.x,a.muzzle.p.y,a.muzzle.p.z,a.end.x,a.end.y,a.end.z);
    }
    static const bool muzzleMark = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_MUZZLE_MARK", v, 2) == 1 && v[0] == L'1'; }();
    if (muzzleMark) {
        (*disableDepth)();
        for (unsigned h = 0; h != 2; ++h)
            if (eyeLasers[h].valid && labShotMuzzleValid[h]) {
                // Green: shot origin. Blue: the weapon's grip; magenta: 0.5 m ahead of it (scale).
                const unsigned m = eyeSnapshot.rider.handheld() ? mountHand(eyeSnapshot, h) : h;
                void *held = eyeLasers[h].weapon ? resolve(eyeLasers[h].weapon) : nullptr;
                const Pose grip = worldHandTracking(eyeAnchor, eyeSnapshot.origin, eyeSnapshot.turn, eyeRequest.input.head,
                    weaponGrip(eyeRequest.input, m, held ? int(primaryField(held, 0xb4)) : -1));
                const std::pair<Vec3, uint32_t> marks[]{{labShotMuzzle[h], 0xff00ff00u}, {grip.p, 0xff0000ffu},
                    {grip.p + rotate(normalize(grip.q), Vec3{0, 0, -.5f}), 0xffff00ffu}};
                for (const auto &[c, color] : marks)
                    for (const Vec3 axis : {Vec3{.02f, 0, 0}, Vec3{0, .02f, 0}, Vec3{0, 0, .02f}})
                        for (int k = -1; k <= 1; ++k) {
                            const Vec3 o{k * .001f, k * .001f, 0};
                            drawLine(c - axis + o, c + axis + o, color, UINT32_MAX);
                        }
            }
        (*enableDepth)();
    }
    if (!wasDepth)
        (*disableDepth)();
    if (wasWrite)
        (*enableDepthWrite)();
    if (translucent) (*setBlendFunction)(priorSource, priorDestination);
    if (wasBlend)
        (*enableBlend)();
    else
        (*disableBlend)();
    if (wasAlpha)
        (*enableAlpha)();
    (*setDepthComparison)(priorComparison);
}
// Voice chat indicators: a green ring over each player who is talking, and a
// small one at your left hand while your microphone is sending.
static void drawVoiceIndicators() {
    if (!settings.voice.indicators || !eyeSnapshot.ui.gameplay || !currentCanvas || !*currentCanvas) return;
    const auto scene = voice::lastScene();
    const bool sending = voice::sending();
    bool any = sending;
    for (const auto &sp : scene.speakers) any = any || (sp.avatar && sp.positioned && voice::talking(sp.avatar));
    if (!any) return;
    const Pose head = worldHeadTracking(eyeAnchor, eyeSnapshot.origin, eyeSnapshot.turn, eyeRequest.input.head);
    if (!finite(head)) return;
    ortho();
    activateView(rootView);
    const bool wasDepth = *depthEnabled != 0, wasWrite = *depthWriting != 0;
    const bool wasBlend = *blending != 0, wasAlpha = *alphaTesting != 0;
    const int priorComparison = *depthComparison;
    (*enableDepth)();
    (*disableDepthWrite)();
    (*disableBlend)();
    (*disableAlpha)();
    (*setDepthComparison)(42); // LESSEQUAL, as for the lasers.
    const auto ring = [&](Vec3 centre, float radius, uint32_t color) {
        Vec3 n = centre - head.p;
        const float length = std::sqrt(dot(n, n));
        if (!(length > 1e-3f)) return;
        n = n * (1 / length);
        Vec3 u = cross(n, Vec3{0, 1, 0});
        const float ul = std::sqrt(dot(u, u));
        u = ul > 1e-3f ? u * (1 / ul) : Vec3{1, 0, 0};
        const Vec3 v = cross(u, n);
        constexpr unsigned Segments = 24;
        for (unsigned k = 0; k < Segments; ++k)
            for (float r : {radius, radius * .9f}) {
                const float a = 2 * Pi * k / Segments, b = 2 * Pi * (k + 1) / Segments;
                drawLine(centre + (u * std::cos(a) + v * std::sin(a)) * r, centre + (u * std::cos(b) + v * std::sin(b)) * r,
                         color, UINT32_MAX);
            }
    };
    for (const auto &sp : scene.speakers)
        if (sp.avatar && sp.positioned && voice::talking(sp.avatar)) ring(sp.position + Vec3{0, .35f, 0}, .12f, 0xff3cdc5au);
    if (sending && controllerTracked(eyeRequest.input, 0)) {
        const Pose hand = worldHandTracking(eyeAnchor, eyeSnapshot.origin, eyeSnapshot.turn, eyeRequest.input.head,
                                            eyeRequest.input.hand[0]);
        if (finite(hand)) ring(hand.p + rotate(normalize(hand.q), Vec3{0, .03f, .04f}), .015f, 0xff3cdc5au);
    }
    if (!wasDepth) (*disableDepth)();
    if (wasWrite) (*enableDepthWrite)();
    if (wasBlend) (*enableBlend)();
    if (wasAlpha) (*enableAlpha)();
    (*setDepthComparison)(priorComparison);
}
struct LabWeaponReceipt {
    bool active=false;
    uint32_t owner=0,weaponHint=0,receiver=0,id=0,hand=0,state=0,session=0,reference=0,tracking=0;
    uint64_t sequence=0;
    uint64_t inputTick=0;
    uint32_t actionGeneration[2]{};
    float trigger[2]{};
};
static LabWeaponReceipt labWeaponReceipt(void *weapon,const Snapshot &s,int hand) {
    if(!labDualTrace() || hand<0 || hand>1 || !vrSession(s) || !fresh(s.input) || !s.ui.gameplay)return {};
    // The incoming native method owns its receiver here. Copy scalar fields
    // before calling the original; no receiver/pointee is retained for logging
    // after native callbacks, deletion or foreign unwinding.
    const uint32_t owner=primaryField(weapon,0x28);
    if(owner!=s.playerHandle)return {};
    return {true,owner,s.handle[hand],uint32_t(reinterpret_cast<uintptr_t>(weapon)),primaryField(weapon,0xb4),
        primaryField(weapon,0xbc),primaryField(weapon,0xb0),s.input.session,s.input.reference,s.generation,s.input.sequence,
        s.input.tickMs,{s.input.primaryInputGeneration[0],s.input.primaryInputGeneration[1]},{s.input.trigger[0],s.input.trigger[1]}};
}
static void logWeaponReceipt(const char *event,const LabWeaponReceipt &r,bool returned,bool aborted,int result) {
    const auto completionTick=GetTickCount64(); // After original return; never called in cleanup.
    log("Lab native weapon event=%s input=%llu session=%u reference=%u tracking=%u owner=%u weaponHint=%u receiver=%u id=%u hand=%u stateBefore=%u returned=%u aborted=%u result=%d tick=%llu actions=%u,%u trigger=%.9g,%.9g completion=%llu",
        event,static_cast<unsigned long long>(r.sequence),r.session,r.reference,r.tracking,r.owner,r.weaponHint,r.receiver,r.id,r.hand,r.state,
        returned,aborted,result,static_cast<unsigned long long>(r.inputTick),r.actionGeneration[0],r.actionGeneration[1],r.trigger[0],r.trigger[1],
        static_cast<unsigned long long>(completionTick));
}
static VoidThis originalLabFireRelease=nullptr;
struct MeleeStopReceipts {
    std::array<MeleeTicket, 38> ticket{}; // Two local plus the existing eighteen two-hand owners.
    unsigned count = 0;
};
static MeleeStopReceipts captureMeleeBindings(void *weapon) noexcept {
    MeleeStopReceipts result;
    if (!meleeConfigured || !settings.physicalMelee) return result;
    AcquireSRWLockShared(&snapshotLock);
    for (const auto &owner : localSnapshotOwner.melee)
        if (owner.weapon == weapon && owner.epoch && owner.consumption.matches(owner.key))
            result.ticket[result.count++] = {owner.key, owner.epoch, owner.consumption.prepare(owner.key)};
    ReleaseSRWLockShared(&snapshotLock);
    AcquireSRWLockShared(&authorityLock);
    for (unsigned slot = 0; slot < authorities.size(); ++slot)
        for (const auto &owner : authorities[slot].melee)
            if (owner.weapon == weapon && owner.epoch && owner.consumption.matches(owner.key))
                result.ticket[result.count++] = {owner.key, owner.epoch, owner.consumption.prepare(owner.key), true, slot};
    ReleaseSRWLockShared(&authorityLock);
    return result;
}
static MeleeStopReceipts captureMeleeStop(void *weapon, uintptr_t caller) noexcept {
    return caller == sawReleaseReturn ? captureMeleeBindings(weapon) : MeleeStopReceipts{};
}
static void __fastcall labFireRelease(void *weapon,void *) {
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const auto nativeReceipt = captureMeleeStop(weapon, caller);
    // No added native getter or handle lookup on this formerly unhooked path.
    // Receiver ownership comes from the incoming native method, not the snapshot.
    LabWeaponReceipt receipt;
    if (labDualTrace()) receipt = labWeaponReceipt(weapon,copySnapshot(),int(primaryField(weapon,0xbc)));
    static std::atomic<unsigned> releaseReceipts{0};
    if(receipt.active) {
        const auto ordinal=releaseReceipts.fetch_add(1,std::memory_order_relaxed);
        if(ordinal==256)log("Lab native weapon trace saturated event=release");
        receipt.active=ordinal<256;
    }
    bool returned=false,aborted=false;
    withNativeFinally([&] {
        originalLabFireRelease(weapon);
        returned=true;
        // Native165498 is before saw sound processing, not its completion.
        for (unsigned i = 0; i < nativeReceipt.count; ++i) {
            const auto &ticket = nativeReceipt.ticket[i];
            if (completeMeleeTicket(ticket, false))
                for (auto *frame = meleeDispatch; frame; frame = frame->previous)
                    if (frame->release && frame->weapon == weapon && frame->ticket.key == ticket.key &&
                        frame->ticket.epoch == ticket.epoch && frame->ticket.authoritative == ticket.authoritative &&
                        frame->ticket.slot == ticket.slot) {
                        frame->committed = true; break;
                    }
        }
    },[&](bool unwind) noexcept {
        aborted=unwind;
        if (unwind) {
            for (unsigned i = 0; i < nativeReceipt.count; ++i) abortMeleeTicket(nativeReceipt.ticket[i]);
            nativeInputFailed();
        }
    });
    if(receipt.active)logWeaponReceipt("release",receipt,returned,aborted,0);
}
static void __fastcall sawDeleted(void *weapon, void *) {
    retireLocalMelee(weapon); // Derived cleanup has native callbacks before base OnDelete.
    withNativeFinally([&] { originalSawDelete(weapon); },
        [](bool aborted) noexcept { if (aborted) nativeInputFailed(); });
}
static void __fastcall sawWeaponPutDown(void *weapon, void *) {
    retireLocalMelee(weapon);
    withNativeFinally([&] { originalSawWeaponPutDown(weapon); },
        [](bool aborted) noexcept { if (aborted) nativeInputFailed(); });
}
static void *__fastcall sawCopied(void *destination, void *, const void *source) {
    // The assignment entry is folded with MiniGun. Unmatched receivers retain
    // exact native forwarding; only an existing matching adapter owner retires.
    retireLocalMelee(destination);
    void *result = nullptr;
    withNativeFinally([&] { result = originalSawCopy(destination, source); },
        [](bool aborted) noexcept { if (aborted) nativeInputFailed(); });
    return result; // A copied active weapon starts unknown on later binding.
}
static void *__fastcall sawAssigned(void *destination, void *, const void *source) {
    retireLocalMelee(destination);
    void *result = nullptr;
    withNativeFinally([&] { result = originalSawAssign(destination, source); },
        [](bool aborted) noexcept { if (aborted) nativeInputFailed(); });
    return result;
}
static void __fastcall sawPutDown(void *weapon, void *, int reason) {
    const auto bindings = captureMeleeBindings(weapon);
    std::array<unsigned, 38> depths{};
    for (unsigned i = 0; i < bindings.count; ++i) {
        const auto &ticket = bindings.ticket[i];
        auto *lock = meleeRecordLock(ticket.authoritative);
        AcquireSRWLockExclusive(lock);
        auto *owner = meleeRecord(ticket.authoritative, ticket.slot, ticket.key.hand);
        if (owner && owner->epoch == ticket.epoch && owner->key == ticket.key) {
            depths[i] = owner->teardownDepth;
            if (depths[i] == UINT_MAX) owner->blocked = true;
            else owner->teardownDepth = depths[i] + 1;
        }
        ReleaseSRWLockExclusive(lock);
    }
    withNativeFinally([&] {
        originalSawPutDown(weapon, reason);
        // Refusal preserves each role's history. Only a reacquired same binding
        // with native state2 permits actual teardown retirement, never a low receipt.
        for (unsigned i = 0; i < bindings.count; ++i) {
            const auto &ticket = bindings.ticket[i];
            auto *lock = meleeRecordLock(ticket.authoritative);
            AcquireSRWLockShared(lock);
            const auto *owner = meleeRecord(ticket.authoritative, ticket.slot, ticket.key.hand);
            const bool currentOwner = owner && owner->epoch == ticket.epoch && owner->key == ticket.key &&
                owner->weapon == weapon;
            ReleaseSRWLockShared(lock);
            if (currentOwner && resolve(ticket.key.weapon) == weapon && primaryField(weapon, 0xb0) == 2) {
                retireLocalMelee(weapon); break;
            }
        }
    }, [&](bool aborted) noexcept {
        for (unsigned i = 0; i < bindings.count; ++i) {
            const auto &ticket = bindings.ticket[i];
            auto *lock = meleeRecordLock(ticket.authoritative);
            AcquireSRWLockExclusive(lock);
            auto *owner = meleeRecord(ticket.authoritative, ticket.slot, ticket.key.hand);
            if (owner && owner->epoch == ticket.epoch && owner->key == ticket.key)
                owner->teardownDepth = depths[i];
            ReleaseSRWLockExclusive(lock);
            if (aborted) abortMeleeTicket(ticket);
        }
        if (aborted) nativeInputFailed();
    });
}
static int __fastcall nativeFire(void *w, void *, float amount) {
    auto s = copySnapshot();
    int hand = handOf(w, s);
    auto receipt=labWeaponReceipt(w,s,hand);
    static std::atomic<unsigned> fireReceipts{0};
    if(receipt.active) {
        const auto ordinal=fireReceipts.fetch_add(1,std::memory_order_relaxed);
        if(ordinal==256)log("Lab native weapon trace saturated event=fire");
        receipt.active=ordinal<256;
    }
    int result = originalNativeFire(w, amount);
    if(receipt.active)logWeaponReceipt("fire",receipt,true,false,result);
    static std::atomic<unsigned> traced{0};
    if (hand >= 0 && traced.fetch_add(1, std::memory_order_relaxed) < 24)
        log("Input native DoTheFiring hand=%d result=%d", hand, result);
    if (result && hand >= 0 && vrSession(s) && fresh(s.input) && s.ui.gameplay)
        fireFeedback[hand].fetch_add(1, std::memory_order_relaxed);
    return result; // Feedback observes the native event; it never initiates damage/fire.
}
static bool trackedWeaponSession(const Snapshot &s) {
    if (!s.rider.handheld() || !nativeRiderCurrent(s.player, s.rider))
        return false;
    if (eyeIndex < 0)
        return vrSession(s);
    // Pair age is checked at admission/commit. Presentation never switches
    // back to a native camera between its frozen left and right eyes.
    return hooksReady.load(std::memory_order_acquire) && s.initialized &&
           rigPublication.usable(s.rigRevision) && eyePlayer == s.player &&
           validTrackingEpoch(s.generation) && channel.shared &&
           trackingEpoch(*channel.shared) == s.generation && eyeRequest.input.session;
}
static bool currentWeaponDraw(ScopeDrawBinding &out,int wantedId) {
    out = {};
    if (!remote_render::ownsNativeThread() || weaponPairFault ||
        !physicalWeapon || physicalWeapon->pass.failed || physicalWeapon->pass.stage != 4 ||
        eyeIndex < 0 || !trackedWeaponSession(eyeSnapshot) ||
        executingView != rootView || !rootView || !livePlayer(eyeSnapshot))
        return false;
    const unsigned hand = unsigned(physicalWeapon->hand);
    if (hand >= 2 || !eyeRequest.input.handValid[mountHand(eyeSnapshot, hand)] ||
        !finite(weaponTracking(eyeRequest.input, mountHand(eyeSnapshot, hand))))
        return false;
    const uint32_t handle = eyeSnapshot.handle[hand];
    void *weapon = handle ? resolve(handle) : nullptr;
    if (!weapon || weapon != physicalWeapon->weapon ||
        nativeHandle(eyeSnapshot.player, int(hand)) != handle ||
        nativeHandle(eyeSnapshot.player, int(1 - hand)) == handle)
        return false;
    uint32_t ownerHandle = 0, modelHandle = 0;
    int nativeId = -1;
    std::memcpy(&ownerHandle, static_cast<uint8_t *>(weapon) + 0x28, 4);
    std::memcpy(&modelHandle, static_cast<uint8_t *>(weapon) + 0x24, 4);
    std::memcpy(&nativeId, static_cast<uint8_t *>(weapon) + 0xb4, 4);
    if (ownerHandle != eyeSnapshot.playerHandle || nativeId != wantedId)
        return false;
    void *instance = modelHandle ? resolve(modelHandle) : nullptr;
    if (!instance)
        return false;
    out = {instance, eyeRequest.sequence, eyeRequest.input.sequence,
           eyeSnapshot.playerHandle, handle, modelHandle, eyeSnapshot.generation, hand};
    return true;
}
bool currentScopeDraw(ScopeDrawBinding &out) {return currentWeaponDraw(out,13);}
// Cheap pre-check for the per-draw probe: scope and idle observations need the
// tracked weapon's own model draw in progress (currentWeaponDraw's first gate).
bool weaponModelDrawActive() noexcept {
    return physicalWeapon && !physicalWeapon->pass.failed && physicalWeapon->pass.stage == 4 && eyeIndex >= 0;
}
IdleWeaponTrace *idleProjectionOwner() noexcept {
    if(!physicalWeapon || !physicalWeapon->ordinaryCommand || !physicalWeapon->idle ||
       physicalWeapon->hand<0 || physicalWeapon->hand>1 || eyeIndex<0 || eyeIndex>1)
        return nullptr;
    auto *trace=physicalWeapon->idle;
    // ID2's count-one affine rejection must not hide later actual producers.
    const bool stage=trace->stage==IdleWeaponTrace::Stage::Palette ||
        (trace->nativeId==2 && trace->stage==IdleWeaponTrace::Stage::Rejected &&
         trace->precedingStage==IdleWeaponTrace::Stage::Palette);
    return trace->admitted && stage && trace->poseCopied ? trace : nullptr;
}
IdleWeaponTrace *idleRawProjectionOwner() noexcept {
    if(!physicalWeapon || !physicalWeapon->ordinaryCommand || !physicalWeapon->idle ||
       physicalWeapon->hand<0 || physicalWeapon->hand>1 || eyeIndex<0 || eyeIndex>1)
        return nullptr;
    auto *invocation=physicalWeapon;
    auto *trace=invocation->idle;
    if(!trace->admitted || trace->nativeId!=2)return nullptr;
    bool exact=false,aborted=false;
    withNativeFinally([&] {
        ScopeDrawBinding binding;IdleDrawIdentity identity;IdleWeaponTrace *same=nullptr;
        exact=currentIdleDraw(binding,identity,same) && same==trace && identity==trace->binding;
    },[&](bool interrupted) noexcept {aborted=interrupted;});
    if(aborted || physicalWeapon!=invocation || invocation->idle!=trace || !exact ||
       weaponPairFault || invocation->pass.failed || invocation->pass.stage!=4) {
        trace->projectionProbe.invalidate();return nullptr;
    }
    return trace;
}
void observeIdleProjectionOpportunity(unsigned kind,unsigned source) noexcept {
    if(!physicalWeapon || !physicalWeapon->ordinaryCommand || !physicalWeapon->idle ||
       physicalWeapon->hand<0 || physicalWeapon->hand>1 || eyeIndex<0 || eyeIndex>1)return;
    auto *trace=physicalWeapon->idle;
    if(!trace->admitted || trace->nativeId!=2)return;
    auto *invocation=physicalWeapon;
    bool exact=false,aborted=false;
    withNativeFinally([&] {
        ScopeDrawBinding binding;IdleDrawIdentity identity;IdleWeaponTrace *same=nullptr;
        exact=currentIdleDraw(binding,identity,same) && same==trace && identity==trace->binding;
    },[&](bool interrupted) noexcept {aborted=interrupted;});
    if(aborted || physicalWeapon!=invocation || invocation->idle!=trace) {
        trace->projectionOpportunities.overflow=true;return;
    }
    trace->projectionOpportunities.observe(kind,source,invocation->pass.stage,
        unsigned(trace->stage),trace->poseCopied,exact,idleProjectionOwner()==trace);
}
int selectedIdleProbeWeapon() noexcept {
    static const int selected=[] {wchar_t value[3]{};
        const auto n=GetEnvironmentVariableW(L"SS2VR_LAB_IDLE_WEAPON",value,3);
        return n<3?idleProbeWeaponId(std::wstring_view(value,n)):-1;}();
    return selected;
}
bool currentIdleDraw(ScopeDrawBinding &out,IdleDrawIdentity &identity,IdleWeaponTrace *&trace) {
    out={};identity={};trace=nullptr;
    if (!physicalWeapon || !physicalWeapon->idle || !physicalWeapon->ordinaryCommand ||
        !idleProbeWeaponSupported(physicalWeapon->idle->nativeId) ||
        physicalWeapon->idle->nativeId!=selectedIdleProbeWeapon() ||
        !currentWeaponDraw(out,physicalWeapon->idle->nativeId) ||
        primaryField(physicalWeapon->weapon,0xb0)!=1 ||
        (physicalWeapon->idle->nativeId==13 && primaryField(physicalWeapon->weapon,0xd4)))
        return false;
    identity={out.requestSequence,out.inputSequence,out.ownerHandle,out.weaponHandle,
              out.modelHandle,out.generation,out.hand,unsigned(eyeIndex)};
    trace=physicalWeapon->idle;
    return true;
}
static bool copyBoundIdleRaster(const ScopeDrawBinding &binding,const IdleDrawIdentity &identity,
                                IdleWeaponTrace *trace,IdleRasterCopy &out) {
    if(!remote_render::copyIdleRaster(binding.modelInstance,out) || out.rootConfig!=trace->config) {
        trace->reject(IdleWeaponTrace::Rejection::RasterMapping);return false;
    }
    out.binding=identity;
    out.clipValid=executedUiProjectionValid && scopeCapClip(executedUiProjection,physicalWeapon->pass.world.view,out.affine,out.clip);
    if(!out.clipValid){trace->reject(IdleWeaponTrace::Rejection::RasterClip);return false;}
    out.factors.view=physicalWeapon->pass.world.view;out.factors.projection=executedUiProjection;
    out.factors.cameraCopied=true;
    // Optional producer evidence never replaces the reference or admission.
    if(!trace->projectionProbe.blocked) {
        for(unsigned n=trace->projectionProbe.count;n>0;--n) {
            const auto &p=trace->projectionProbe.pairs[n-1];
            if(!p.qualified() || p.after.modelRecord!=out.projectionModelAddress ||
               p.after.drawRecord!=out.projectionDrawAddress)continue;
            // ID2 SOURCE2 is raw diagnostic evidence only. Do not skip it to
            // choose an older matching producer; existing ID13 behavior stays.
            if((trace->nativeId!=2 || p.source==1) && out.matchesProjection(p))out.projectionSequence=p.sequence;
            break; // Only the latest corresponding producer; no passing-choice search.
        }
    }
    return true;
}
IdleWeaponTrace *idleSubmissionOwner() noexcept {
    IdleWeaponTrace *owned=nullptr;
    withNativeFinally([&] {
        ScopeDrawBinding binding;IdleDrawIdentity identity;IdleWeaponTrace *trace=nullptr;
        if(currentIdleDraw(binding,identity,trace) && trace && trace->admitted && trace->binding==identity)
            owned=trace;
    },[&](bool aborted) noexcept {if(aborted)owned=nullptr;});
    return owned;
}
bool copyIdleSubmissionMetadata(const IdleWeaponTrace *wanted,IdleSubmissionMetadata &out) noexcept {
    out={};bool copied=false;
    withNativeFinally([&] {
        ScopeDrawBinding binding;IdleDrawIdentity identity;IdleWeaponTrace *trace=nullptr;
        if(!currentIdleDraw(binding,identity,trace) || trace!=wanted || !trace ||
           !trace->admitted || trace->binding!=identity)return;
        IdleRasterCopy raster;
        if(!remote_render::copyIdleRaster(binding.modelInstance,raster))return;
        ScopeDrawBinding after;IdleDrawIdentity current;IdleWeaponTrace *same=nullptr;
        if(!currentIdleDraw(after,current,same) || same!=trace || current!=identity ||
           after.modelInstance!=binding.modelInstance)return;
        out=IdleSubmissionMetadata::copy(raster);copied=true;
    },[&](bool aborted) noexcept {if(aborted){out={};copied=false;}});
    return copied; // No trace rejection, clip qualification, or retained native borrow.
}
bool copyIdleSubmissionPalette(const IdleWeaponTrace *wanted,IdlePaletteCopy &out,IdlePaletteBoundaryCopy *diagnostic) noexcept {
    out={};bool copied=false;
    if(diagnostic)*diagnostic={};
    const auto failed=[&](unsigned step) noexcept {if(diagnostic && diagnostic->wrapper!=99)diagnostic->wrapper=step;};
    withNativeFinally([&] {
        ScopeDrawBinding binding;IdleDrawIdentity identity;IdleWeaponTrace *trace=nullptr;
        if(!currentIdleDraw(binding,identity,trace)) {failed(1);return;}
        if(trace!=wanted || !trace || trace->nativeId!=2 || !trace->admitted || trace->binding!=identity) {failed(2);return;}
        // A cached native draw need not repeat the Event observer. Its own two
        // current native copies supply diagnostic identity; never seed Event history.
        if(!trace->config.configuration && (trace->config.file || trace->config.resource!=-1)) {failed(3);return;}
        IdlePaletteCopy before,after;
        if(diagnostic)diagnostic->leg=1;
        if(!remote_render::copyIdlePalette(binding.modelInstance,before,diagnostic)) {failed(4);return;}
        if(trace->config.configuration && before.metadata.rootConfig!=trace->config) {failed(5);return;}
        const uint32_t projection=remote_render::idlePaletteProjectionSequence(trace->projectionProbe,before);
        ScopeDrawBinding now;IdleDrawIdentity current;IdleWeaponTrace *same=nullptr;
        if(!currentIdleDraw(now,current,same)) {failed(6);return;}
        if(same!=trace || current!=identity || now.modelInstance!=binding.modelInstance) {failed(7);return;}
        if(diagnostic)diagnostic->leg=2;
        if(!remote_render::copyIdlePalette(now.modelInstance,after,diagnostic)) {failed(8);return;}
        if(before!=after) {failed(9);return;}
        ScopeDrawBinding end;IdleDrawIdentity finalIdentity;IdleWeaponTrace *finalTrace=nullptr;
        if(!currentIdleDraw(end,finalIdentity,finalTrace)) {failed(10);return;}
        if(finalTrace!=trace || finalIdentity!=identity || end.modelInstance!=binding.modelInstance) {failed(11);return;}
        out=before;
        const uint32_t finalProjection=remote_render::idlePaletteProjectionSequence(trace->projectionProbe,after);
        if(projection && projection==finalProjection)out.projectionSequence=projection;
        copied=true;
    },[&](bool aborted) noexcept {if(aborted){out={};copied=false;if(diagnostic){*diagnostic={};diagnostic->wrapper=99;}}});
    return copied;
}
bool currentIdleRaster(IdleRasterCopy &out,IdleWeaponTrace *&trace) {
    out={};trace=nullptr;ScopeDrawBinding binding;IdleDrawIdentity identity;
    if(!currentIdleDraw(binding,identity,trace))return false;
    trace->callbacks|=IdleWeaponTrace::RasterSeen;
    if(trace->stage!=IdleWeaponTrace::Stage::Palette || !trace->poseCopied) {
        trace->noteRejection(IdleWeaponTrace::Rejection::RasterPrerequisites,IdleWeaponTrace::checks({trace->stage==IdleWeaponTrace::Stage::Palette,trace->poseCopied}));return false;
    }
    return copyBoundIdleRaster(binding,identity,trace,out);
}
bool idleRejectedRasterCurrent(const IdleRasterCopy &expected,const IdleWeaponTrace *wanted) {
    ScopeDrawBinding binding;IdleDrawIdentity identity;IdleWeaponTrace *trace=nullptr;
    if(!currentIdleDraw(binding,identity,trace) || trace!=wanted || identity!=expected.binding ||
       trace->stage!=IdleWeaponTrace::Stage::Rejected || trace->rejection!=IdleWeaponTrace::Rejection::CollectInputs ||
       !trace->poseCopied || !IdleWeaponTrace::observedStreamFamily(trace->inputFailure))return false;
    IdleRasterCopy now;
    return copyBoundIdleRaster(binding,identity,trace,now) && now==expected;
}
static void emitIdleProjectionOpportunities(const IdleWeaponTrace &trace) {
    if(!trace.admitted || !trace.submissions.outerCompleted)return;
    const auto &b=trace.binding;
    if(trace.nativeId==2) {
        const auto &op=trace.projectionOpportunities;
        log("Lab idle projectionOpportunitySummary request=%llu eye=%u hand=%u count=%u overflow=%u ordinal=%u rootQuery=%u palette=%u submission=%u diagnostic=1",
            b.request,b.eye,b.hand,op.count,unsigned(op.overflow),op.ordinal,op.rootQuery,op.palette,op.submission);
        for(unsigned i=0;i<op.count && i<IdleProjectionOpportunities::MaxRows;++i) {
            const auto &row=op.rows[i];
            log("Lab idle projectionOpportunity request=%llu eye=%u hand=%u index=%u ordinal=%u kind=%u source=%u pass=%u stage=%u pose=%u exact=%u oldGate=%u",
                b.request,b.eye,b.hand,i,row.ordinal,row.kind,row.source,row.passStage,row.traceStage,
                row.poseCopied,row.exactDraw,row.oldGate);
        }
    }
}
static void emitIdlePaletteBoundaries(const IdleWeaponTrace &trace) {
    if(!trace.admitted || trace.nativeId!=2 || !trace.submissions.outerCompleted)return;
    const auto &b=trace.binding;
    unsigned count=0;
    for(unsigned n=0;n<trace.submissions.count && n<IdleSubmissionTrace::MaxAttempts;++n)
        count+=trace.paletteBoundaryPublishable(n);
    log("Lab idle paletteBoundarySummary schema=1 source=%.*s request=%llu input=%llu generation=%u owner=%u weapon=%u model=%u eye=%u hand=%u count=%u diagnostic=1",
        64,ss2vrBuildContract.sourceFingerprint.data(),b.request,b.input,b.generation,b.owner,b.weapon,b.model,b.eye,b.hand,count);
    for(unsigned n=0;n<trace.submissions.count && n<IdleSubmissionTrace::MaxAttempts;++n) {
        if(!trace.paletteBoundaryPublishable(n))continue;
        const auto &row=trace.submissions.rows[n];const auto &p=trace.paletteApiPayloads[row.paletteApiSlot].boundary;
        log("Lab idle paletteBoundary request=%llu input=%llu generation=%u owner=%u weapon=%u model=%u eye=%u hand=%u index=%u ordinal=%u api=%u phase=%u kind=%u wrapper=%u leg=%u reader=%u valid=%u root=%u render=%u evaluated=%u linked=%u cacheOwner=%u canonicalCount=%u matrices=%u apiStatus=%u apiStep=%u apiIndex=%u apiResult=%d software=%d",
            b.request,b.input,b.generation,b.owner,b.weapon,b.model,b.eye,b.hand,n,row.ordinal,row.paletteApiSlot,
            p.phase,p.kind,p.wrapper,p.leg,p.reader,p.valid,p.root,p.render,p.evaluated,p.linked,p.cacheOwner,
            p.canonicalCount,p.matrices,p.apiStatus,p.apiStep,p.apiIndex,p.apiResult,p.software);
    }
}
static void emitIdlePaletteCompanions(const IdleWeaponTrace &trace) {
    if(trace.nativeId!=2)return;
    const auto &b=trace.binding;
    log("Lab idle paletteSummary schema=3 request=%llu eye=%u hand=%u source=%.*s payloads=%u overflow=%u outerCompleted=%u alignment=0 grasp=0",
        b.request,b.eye,b.hand,64,ss2vrBuildContract.sourceFingerprint.data(),trace.paletteApiCount,
        unsigned(trace.paletteApiOverflow),unsigned(trace.submissions.outerCompleted));
    for(unsigned n=0;n<trace.submissions.count;++n) {
        if(!trace.paletteApiPublishable(n))continue;
        const auto &row=trace.submissions.rows[n];const auto &p=row.palette;
        const auto &api=trace.paletteApiPayloads[row.paletteApiSlot];const auto &s=api.before;
        const auto &m=p.metadata;
        log("Lab idle paletteRow schema=3 request=%llu input=%llu generation=%u owner=%u weapon=%u model=%u eye=%u hand=%u index=%u ordinal=%u api=%u modelAddress=%u drawAddress=%u modelRecord=%d drawRecord=%d instance=%u surface=%u root=%u,%u,%d render=%u,%u,%d evaluated=%u matrices=%u mapping=%u palette=%u first=%u count=%u mapCount=%u paletteCount=%u canonicalCount=%u modelCount=%u canonicalEqual=%u words=%u constants=%u declaration=%u objects=%u,%u,%u contentCopied=%u projectionSequence=%u cleanupCertified=1 outerCurrent=1",
            b.request,b.input,b.generation,b.owner,b.weapon,b.model,b.eye,b.hand,n,row.ordinal,row.paletteApiSlot,
            m.modelAddress,m.drawAddress,m.modelRecord,m.drawRecord,m.instance,m.surface,
            m.rootConfig.configuration,m.rootConfig.file,m.rootConfig.resource,
            m.renderConfig.configuration,m.renderConfig.file,m.renderConfig.resource,
            p.evaluated,p.matrices,p.mappingAddress,p.paletteAddress,p.first,p.count,p.mapCount,p.paletteCount,
            p.canonicalCount,p.modelCount,unsigned(p.paletteCopiesCanonical()),api.words,s.caps,s.declarationCount,
            s.declarationObject,s.indexObject,s.shaderObject,unsigned(trace.paletteContentPublishable(n)),
            trace.paletteProjectionSequence(n));
        auto words=[&](const char *kind,unsigned item,unsigned chunk,std::span<const uint32_t> values) {
            char text[32*9]{};unsigned cursor=0;
            for(auto value:values)cursor+=unsigned(std::snprintf(text+cursor,sizeof(text)-cursor,"%s%08x",cursor?",":"",value));
            log("Lab idle paletteData request=%llu eye=%u hand=%u index=%u ordinal=%u kind=%s item=%u chunk=%u values=%s",
                b.request,b.eye,b.hand,n,row.ordinal,kind,item,chunk,text);
        };
        if(trace.paletteContentPublishable(n)) {
            const auto &v=api.content;const auto &l=v.surface;
            const std::array<uint32_t,14> layout{uint32_t(l.vertices),uint32_t(l.triangles),
                l.channels[0].offset,l.channels[0].format,l.channels[0].buffer,
                l.channels[1].offset,l.channels[1].format,l.channels[1].buffer,
                l.channels[2].offset,l.channels[2].format,l.channels[2].buffer,
                l.channels[3].offset,l.channels[3].format,l.channels[3].buffer};
            words("contentSurface",0,0,layout);
            const std::array<uint32_t,10> buffers{v.vertex.size,v.vertex.usage,v.vertex.pool,v.vertex.format,v.vertex.fvf,
                v.index.size,v.index.usage,v.index.pool,v.index.format,v.index.fvf};
            words("contentBuffers",0,0,buffers);
            const std::array<uint32_t,18> streams{uint32_t(v.positions.object),v.positions.offset,v.positions.stride,v.positions.frequency,
                uint32_t(v.localIndices.object),v.localIndices.offset,v.localIndices.stride,v.localIndices.frequency,
                uint32_t(v.weights.object),v.weights.offset,v.weights.stride,v.weights.frequency,
                uint32_t(v.uv.object),v.uv.offset,v.uv.stride,v.uv.frequency,uint32_t(v.indexObject),uint32_t(v.softwarePositions)};
            words("contentStreams",0,0,streams);
            for(unsigned i=0;i<api.contentHashes.size();++i) {
                std::array<uint32_t,8> hash{};std::memcpy(hash.data(),api.contentHashes[i].data(),32);
                words("contentHash",i,0,hash);
            }
        }
        words("world",0,0,p.world);
        const auto &d=row.draw;
        const std::array<uint32_t,6> draw{d.topology,uint32_t(d.base),d.minimum,d.vertices,d.start,d.primitives};
        words("draw",0,0,draw);
        for(unsigned i=0;i<p.count;++i) {
            const auto &e=p.entries[i];
            const std::array<uint32_t,5> mapping{uint32_t(e.draw),uint32_t(e.bone),uint32_t(e.owner),e.definition,e.name};
            words("mapping",i,0,mapping);words("canonical",i,0,e.canonical);words("palette",i,0,e.palette);
        }
        for(unsigned i=0;i<s.streams.size();++i) {
            const auto &v=s.streams[i];
            const std::array<uint32_t,4> values{uint32_t(v.object),v.offset,v.stride,v.frequency};words("stream",i,0,values);
        }
        for(unsigned i=0;i<s.declarationCount;++i) {
            const auto &e=s.declaration[i];
            const std::array<uint32_t,6> values{e.stream,e.offset,e.type,e.method,e.usage,e.usageIndex};words("declaration",i,0,values);
        }
        for(unsigned i=0;i<s.caps;++i)words("constant",i,0,s.constants[i]);
        for(unsigned i=0;i<api.words;i+=32)words("program",0,i,std::span(api.program).subspan(i,std::min(32u,api.words-i)));
    }
}
static void emitIdleWeaponTrace(const IdleWeaponTrace &trace) {
    const auto &b=trace.binding;
    log("Lab idle draw schema=4 nativeId=%d copyLayout=%u rawGripValid=%u draws=%u source=%.*s ipc=%u wire=%u request=%llu input=%llu owner=%u weapon=%u model=%u generation=%u hand=%u eye=%u stage=%u cfg=%u file=%u resource=%d contributors=%u matrices=%u historicalBytes=0 grasp=0",
        trace.nativeId,IdleWeaponTrace::CopyLayout,unsigned(trace.rawGripValid),trace.draws,64,ss2vrBuildContract.sourceFingerprint.data(),ss2vrBuildContract.ipcAbi,ss2vrBuildContract.wireVersion,
        b.request,b.input,b.owner,b.weapon,b.model,b.generation,b.hand,b.eye,unsigned(trace.stage),
        trace.config.configuration,trace.config.file,trace.config.resource,trace.contributors,trace.matrixCount);
    if(trace.rejection!=IdleWeaponTrace::Rejection::None)
        log("Lab idle rejection request=%llu eye=%u hand=%u reason=%u preceding=%u checks=%u state=%u callbacks=%u",
            b.request,b.eye,b.hand,unsigned(trace.rejection),unsigned(trace.precedingStage),trace.rejectionChecks,trace.rejectionState,trace.callbacks);
    const auto &submitted=trace.submissions;
    log("Lab idle submissionSummary request=%llu eye=%u hand=%u attempts=%u count=%u overflow=%u outerReturned=%u limit=%u",
        b.request,b.eye,b.hand,submitted.attempts,submitted.count,unsigned(submitted.overflow),
        unsigned(submitted.outerReturned),IdleSubmissionTrace::MaxAttempts);
    for(unsigned i=0;i<submitted.count;++i) {
        const auto &r=submitted.rows[i];const auto &d=r.draw;
        log("Lab idle submissionRow request=%llu eye=%u hand=%u index=%u ordinal=%u status=%u hr=%d flags=%u draw=%u,%d,%u,%u,%u,%u",
            b.request,b.eye,b.hand,i,r.ordinal,unsigned(r.status),r.hresult,
            IdleWeaponTrace::checks({r.preCopied,r.postCopied,r.returned,r.matched}),
            d.topology,d.base,d.minimum,d.vertices,d.start,d.primitives);
        if(r.status!=IdleSubmissionTrace::Status::Qualified)continue;
        const auto &m=r.metadata;const auto &l=m.layout;
        log("Lab idle submissionMetadata request=%llu eye=%u hand=%u index=%u keys=%u,%u,%u,%u,%u,%u,%u,%u,%d root=%u,%u,%d render=%u,%u,%d layout=%d,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
            b.request,b.eye,b.hand,i,m.modelAddress,m.drawAddress,m.modelRecord,m.drawRecord,m.surface,m.instance,
            m.surfaceName,m.boneName,m.bone,m.rootConfig.configuration,m.rootConfig.file,m.rootConfig.resource,
            m.renderConfig.configuration,m.renderConfig.file,m.renderConfig.resource,l.vertices,l.triangles,
            l.channels[0].offset,l.channels[0].format,l.channels[0].buffer,
            l.channels[1].offset,l.channels[1].format,l.channels[1].buffer,
            l.channels[2].offset,l.channels[2].format,l.channels[2].buffer,
            l.channels[3].offset,l.channels[3].format,l.channels[3].buffer);
    }
    if(trace.rejection==IdleWeaponTrace::Rejection::Palette && trace.paletteOwnershipFailure.copied) {
        const auto &p=trace.paletteOwnershipFailure;
        log("Lab idle paletteOwnershipFailure schema=1 source=%.*s request=%llu input=%llu generation=%u owner=%u weapon=%u model=%u eye=%u hand=%u cfg=%u file=%u resource=%d instance=%u evaluated=%u linked=%u cacheOwner=%u count=%d evaluatedMatches=%u ownerMatches=%u",
            64,ss2vrBuildContract.sourceFingerprint.data(),b.request,b.input,b.generation,b.owner,b.weapon,b.model,
            b.eye,b.hand,trace.config.configuration,trace.config.file,trace.config.resource,
            p.instance,p.evaluated,p.linked,p.owner,p.count,unsigned(p.evaluated==p.linked),unsigned(p.owner==p.instance));
    }
    if(trace.rejection==IdleWeaponTrace::Rejection::QueryAnimationName && trace.animationNameFailure.copied) {
        const auto &n=trace.animationNameFailure;
        log("Lab idle animationNameFailure request=%llu eye=%u hand=%u index=%u expected=%08x header=%08x,%08x,%08x,%08x",
            b.request,b.eye,b.hand,n.index,n.expected,n.header[0],n.header[1],n.header[2],n.header[3]);
    }
    if(trace.rejection==IdleWeaponTrace::Rejection::CollectInputs && trace.inputFailure.step) {
        const auto &d=trace.inputFailure;const auto &v=d.inputs;
        log("Lab idle inputFailure request=%llu eye=%u hand=%u step=%u index=%u hr=%d valid=%u caps=%u declaration=%u rangeChecks=%u layout=%u",
            b.request,b.eye,b.hand,d.step,d.index,d.hresult,d.valid,d.caps,d.declarationCount,d.rangeChecks,unsigned(d.layout));
        if(d.valid&2)for(unsigned i=0;i<d.declarationCount;++i) {
            const auto &e=d.declaration[i];
            log("Lab idle inputDeclaration request=%llu eye=%u hand=%u index=%u values=%u,%u,%u,%u,%u,%u",
                b.request,b.eye,b.hand,i,unsigned(e.stream),unsigned(e.offset),unsigned(e.type),unsigned(e.method),unsigned(e.usage),unsigned(e.usageIndex));
        }
        if(d.valid&4) {
            log("Lab idle inputBinding request=%llu eye=%u hand=%u vertex=%u,%u,%u,%u,%u index=%u,%u,%u,%u,%u draw=%u,%d,%u,%u,%u,%u software=%u",
                b.request,b.eye,b.hand,v.vertex.size,v.vertex.usage,v.vertex.pool,v.vertex.format,v.vertex.fvf,
                v.index.size,v.index.usage,v.index.pool,v.index.format,v.index.fvf,
                v.draw.topology,v.draw.base,v.draw.minimum,v.draw.vertices,v.draw.start,v.draw.primitives,unsigned(v.softwarePositions));
            const ScopeStreamInput streams[]{v.positions,v.localIndices,v.uv,v.weights};
            for(unsigned i=0;i<4;++i)log("Lab idle inputStream request=%llu eye=%u hand=%u index=%u object=%u offset=%u stride=%u frequency=%u",
                b.request,b.eye,b.hand,i,unsigned(streams[i].object),streams[i].offset,streams[i].stride,streams[i].frequency);
            log("Lab idle inputSurface request=%llu eye=%u hand=%u vertices=%d triangles=%d",
                b.request,b.eye,b.hand,v.surface.vertices,v.surface.triangles);
            for(unsigned i=0;i<4;++i)log("Lab idle inputChannel request=%llu eye=%u hand=%u index=%u offset=%u format=%u buffer=%u",
                b.request,b.eye,b.hand,i,v.surface.channels[i].offset,v.surface.channels[i].format,v.surface.channels[i].buffer);
        }
    }
    if(trace.rejection==IdleWeaponTrace::Rejection::CollectInputs && trace.streamProbe.selected) {
        const auto &p=trace.streamProbe;
        log("Lab idle streamProbe request=%llu eye=%u hand=%u attempts=%u flags=%u words=%u invalidations=%u forwardResult=%d",
            b.request,b.eye,b.hand,p.attempts,p.flags,p.words,p.invalidations,p.forwardResult);
        for(unsigned phase=0;phase<p.attempts && phase<2;++phase) {
            const auto &s=p.snapshots[phase];
            log("Lab idle streamSnapshot request=%llu eye=%u hand=%u phase=%u status=%u step=%u index=%u hr=%d",
                b.request,b.eye,b.hand,phase,s.status,s.step,s.index,s.hresult);
            if(s.status!=IdleWeaponTrace::StreamSnapshot::Copied)continue;
            log("Lab idle streamBinding request=%llu eye=%u hand=%u phase=%u caps=%u declaration=%u declarationObject=%u indexObject=%u shaderObject=%u",
                b.request,b.eye,b.hand,phase,s.caps,s.declarationCount,s.declarationObject,s.indexObject,s.shaderObject);
            const auto numbers=IdleWeaponTrace::passiveStreamNumbers(trace.inputFailure);
            for(unsigned i=0;i<3;++i) {
                const auto &v=s.streams[i];
                log("Lab idle streamInput request=%llu eye=%u hand=%u phase=%u index=%u object=%u offset=%u stride=%u frequency=%u",
                    b.request,b.eye,b.hand,phase,numbers[i],unsigned(v.object),v.offset,v.stride,v.frequency);
            }
            for(unsigned i=0;i<s.declarationCount && i<65;++i) {
                const auto &e=s.declaration[i];
                log("Lab idle streamDeclaration request=%llu eye=%u hand=%u phase=%u index=%u values=%u,%u,%u,%u,%u,%u",
                    b.request,b.eye,b.hand,phase,i,unsigned(e.stream),unsigned(e.offset),unsigned(e.type),unsigned(e.method),unsigned(e.usage),unsigned(e.usageIndex));
            }
            for(unsigned i=0;i<s.caps && i<256;++i) {
                const auto &v=s.constants[i];
                log("Lab idle streamConstant request=%llu eye=%u hand=%u phase=%u index=%u values=%08x,%08x,%08x,%08x",
                    b.request,b.eye,b.hand,phase,i,v[0],v[1],v[2],v[3]);
            }
        }
        for(unsigned chunk=0;chunk<p.words && chunk<IdleGeometryCopy::MaxProgramWords;chunk+=32) {
            char values[32*9]{};size_t used=0;
            for(unsigned i=chunk;i<p.words && i<chunk+32 && i<IdleGeometryCopy::MaxProgramWords;++i)
                used+=size_t(std::snprintf(values+used,sizeof(values)-used,"%s%08x",i==chunk?"":",",p.program[i]));
            log("Lab idle streamProgram request=%llu eye=%u hand=%u chunk=%u values=%s",b.request,b.eye,b.hand,chunk,values);
        }
    }
    log("Lab idle projectionSummary request=%llu eye=%u hand=%u configured=%u count=%u invalidations=%u blocked=%u pending=%u",
        b.request,b.eye,b.hand,unsigned(remote_render::idleProjectionConfigured()),trace.projectionProbe.count,
        trace.projectionProbe.invalidations,unsigned(trace.projectionProbe.blocked),unsigned(trace.projectionProbe.pending));
    for(unsigned n=0;n<trace.projectionProbe.count && n<IdleProjectionProbe::MaxPairs;++n) {
        const auto &p=trace.projectionProbe.pairs[n];
        log("Lab idle projectionPair request=%llu eye=%u hand=%u sequence=%u source=%u complete=%u controlBefore=%u controlAfter=%u flagsBefore=%u flagsAfter=%u modelBefore=%u modelAfter=%u drawBefore=%u drawAfter=%u cleanupCertified=0 outerCurrent=0",
            b.request,b.eye,b.hand,p.sequence,p.source,unsigned(p.complete),p.before.control,p.after.control,
            p.before.flags,p.after.flags,p.before.modelRecord,p.after.modelRecord,p.before.drawRecord,p.after.drawRecord);
        auto raw=[&](unsigned phase,const char *kind,std::span<const uint32_t> values) {
            char text[16*9]{};unsigned cursor=0;
            for(auto value:values)cursor+=unsigned(std::snprintf(text+cursor,sizeof(text)-cursor,"%s%08x",cursor?",":"",value));
            log("Lab idle projectionData request=%llu eye=%u hand=%u sequence=%u phase=%u kind=%s values=%s",
                b.request,b.eye,b.hand,p.sequence,phase,kind,text);
        };
        const IdleProjectionSnapshot *phases[]{&p.before,&p.after};
        for(unsigned phase=0;phase<2;++phase) {
            const auto &s=*phases[phase];
            raw(phase,"model",s.model);raw(phase,"view",s.view);raw(phase,"projection",s.projection);
            raw(phase,"cachedVP",s.cachedVP);raw(phase,"cachedMVP",s.cachedMVP);
        }
    }
    const bool retained=trace.retainedDrawCopiesAvailable();
    if(trace.stage!=IdleWeaponTrace::Stage::Complete && !retained)return;
    if(retained)log("Lab idle retainedCopies request=%llu eye=%u hand=%u count=%u postOriginal=1 cleanupCertified=0 outerCurrent=0",
        b.request,b.eye,b.hand,trace.draws);
    if(!retained) {
        for(unsigned i=0;i<trace.contributors;++i) {
            const auto &a=trace.animations[i];
            log("Lab idle animation request=%llu eye=%u hand=%u index=%u raw=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x header=%08x,%08x,%08x,%08x",
                b.request,b.eye,b.hand,i,a.contribution[0],a.contribution[1],a.contribution[2],a.contribution[3],
                a.contribution[4],a.contribution[5],a.contribution[6],a.contribution[7],
                a.header[0],a.header[1],a.header[2],a.header[3]);
        }
        auto emitMatrix=[&](const char *kind,unsigned index,const Matrix34 &m) {
            log("Lab idle matrix request=%llu eye=%u hand=%u kind=%s index=%u values=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
                b.request,b.eye,b.hand,kind,index,m.m[0],m.m[1],m.m[2],m.m[3],m.m[4],m.m[5],
                m.m[6],m.m[7],m.m[8],m.m[9],m.m[10],m.m[11]);
        };
        emitMatrix("world",0,trace.world);
        emitMatrix("nativePlacement",0,trace.nativePlacement);
        emitMatrix("trackedPlacement",0,trace.trackedPlacement);
        emitMatrix("controller",0,trace.controller);
        emitMatrix("rawAim",0,trace.rawAim);
        if(trace.rawGripValid)emitMatrix("rawGrip",0,trace.rawGrip);
        for(unsigned i=0;i<trace.matrixCount;++i)emitMatrix("canonical",i,trace.matrices[i]);
        log("Lab idle stretch request=%llu eye=%u hand=%u values=%.9g,%.9g,%.9g",b.request,b.eye,b.hand,
            trace.stretch.x,trace.stretch.y,trace.stretch.z);
    }
    for(unsigned n=0;n<trace.draws;++n) {
        const auto &g=trace.geometry[n];const auto &r=g.raster;const auto &v=g.inputs;
        log("Lab idle %s request=%llu eye=%u hand=%u index=%u modelRecord=%u drawRecord=%u surface=%u instance=%u surfaceName=%u boneName=%u bone=%d cfg=%u file=%u resource=%d words=%u constants=%u declaration=%u",
            retained?"retainedGeometry":"geometry",b.request,b.eye,b.hand,n,r.modelRecord,r.drawRecord,r.surface,r.instance,r.surfaceName,r.boneName,r.bone,
            r.renderConfig.configuration,r.renderConfig.file,r.renderConfig.resource,g.words,g.constantCount,g.declarationCount);
        auto words=[&](const char *kind,unsigned chunk,std::span<const uint32_t> values) {
            char text[32*9]{};unsigned cursor=0;
            for(auto value:values)cursor+=unsigned(std::snprintf(text+cursor,sizeof(text)-cursor,"%s%08x",cursor?",":"",value));
            log("Lab idle %s request=%llu eye=%u hand=%u index=%u kind=%s chunk=%u values=%s",
                retained?"retainedGeometryData":"geometryData",b.request,b.eye,b.hand,n,kind,chunk,text);
        };
        std::array<uint32_t,12> affine{};std::memcpy(affine.data(),r.affine.m,sizeof(affine));words("affine",0,affine);
        std::array<uint32_t,16> clip{};std::memcpy(clip.data(),r.clip.m,sizeof(clip));words("clip",0,clip);
        if(g.factorsAvailable()) {
            log("Lab idle %s request=%llu eye=%u hand=%u index=%u palette=%u bookends=3 postOriginal=1 cleanupCertified=0 outerCurrent=0",
                retained?"retainedGeometryFactors":"geometryFactors",b.request,b.eye,b.hand,n,r.factors.paletteIndex);
            r.factors.emitMatrices([&](const char *kind,std::span<const float> values) {
                std::array<uint32_t,16> raw{};
                std::memcpy(raw.data(),values.data(),values.size_bytes());
                words(kind,0,std::span(raw).first(values.size()));
            });
        }
        if(r.projectionSequence && r.projectionSequence<=trace.projectionProbe.count &&
           g.projectionAvailable(trace.projectionProbe.pairs[r.projectionSequence-1])) {
            log("Lab idle %s request=%llu eye=%u hand=%u index=%u sequence=%u modelAddress=%u drawAddress=%u bookends=3 postOriginal=1 cleanupCertified=0 outerCurrent=0",
                retained?"retainedGeometryProjection":"geometryProjection",b.request,b.eye,b.hand,n,
                r.projectionSequence,r.projectionModelAddress,r.projectionDrawAddress);
        }
        const std::array<uint32_t,14> layout{uint32_t(r.layout.vertices),uint32_t(r.layout.triangles),
            r.layout.channels[0].offset,r.layout.channels[0].format,r.layout.channels[0].buffer,
            r.layout.channels[1].offset,r.layout.channels[1].format,r.layout.channels[1].buffer,
            r.layout.channels[2].offset,r.layout.channels[2].format,r.layout.channels[2].buffer,
            r.layout.channels[3].offset,r.layout.channels[3].format,r.layout.channels[3].buffer};words("layout",0,layout);
        const std::array<uint32_t,10> descriptions{v.vertex.size,v.vertex.usage,v.vertex.pool,v.vertex.format,v.vertex.fvf,
            v.index.size,v.index.usage,v.index.pool,v.index.format,v.index.fvf};words("buffers",0,descriptions);
        const std::array<uint32_t,6> draw{v.draw.topology,uint32_t(v.draw.base),v.draw.minimum,v.draw.vertices,v.draw.start,v.draw.primitives};words("draw",0,draw);
        const std::array<uint32_t,18> streams{uint32_t(v.positions.object),v.positions.offset,v.positions.stride,v.positions.frequency,
            uint32_t(v.localIndices.object),v.localIndices.offset,v.localIndices.stride,v.localIndices.frequency,
            uint32_t(v.weights.object),v.weights.offset,v.weights.stride,v.weights.frequency,
            uint32_t(v.uv.object),v.uv.offset,v.uv.stride,v.uv.frequency,uint32_t(v.indexObject),uint32_t(v.softwarePositions)};words("streams",0,streams);
        for(unsigned i=0;i<5;++i) {std::array<uint32_t,8> hash{};
            std::memcpy(hash.data(),g.hashes[i].data(),32);words("hash",i,hash);}
        for(unsigned i=0;i<g.declarationCount;++i) {const auto &e=g.declaration[i];
            const std::array<uint32_t,6> fields{e.stream,e.offset,e.type,e.method,e.usage,e.usageIndex};words("declaration",i,fields);}
        for(unsigned i=0;i<g.words;i+=32)words("program",i,std::span(g.program).subspan(i,std::min(32u,g.words-i)));
        for(unsigned i=0;i<g.constantCount;++i) {std::array<uint32_t,4> values{};
            std::memcpy(values.data(),g.constants[i].data(),16);words("constant",i,values);}
    }

}
void recordScopeObservation(const ScopeDrawBinding &binding, const Matrix34 &affine,
                            const ScopeSurfaceLayout &layout) {
    ScopeDrawBinding currentBinding;
    if (!finiteMatrix(affine) || !currentScopeDraw(currentBinding) ||
        currentBinding.modelInstance != binding.modelInstance ||
        currentBinding.requestSequence != binding.requestSequence ||
        currentBinding.inputSequence != binding.inputSequence ||
        currentBinding.ownerHandle != binding.ownerHandle ||
        currentBinding.weaponHandle != binding.weaponHandle ||
        currentBinding.modelHandle != binding.modelHandle ||
        currentBinding.generation != binding.generation || currentBinding.hand != binding.hand)
        return;
    if (physicalWeapon->scopePose.valid || physicalWeapon->scopePoseAmbiguous) {
        physicalWeapon->scopePose = {};
        physicalWeapon->scopePoseAmbiguous = true;
        return;
    }
    physicalWeapon->scopePose = {affine, binding.requestSequence, binding.inputSequence,
                                binding.ownerHandle, binding.weaponHandle, binding.modelHandle, binding.generation,
                                true, false, layout};
    // CurrentScopeDraw has just revalidated the exact live native weapon and
    // owner. Copy only this sniper's existing interpolation fields; do not use
    // the player's shared FOV multiplier, advance time, or invoke zoom callbacks.
    // The native Step formula is F0 + (EC - F0) * F4 (1728B3..1728D9).
    physicalWeapon->scopePose.nativeBaseFovRadians = eyeNativeBaseFov;
    void *weapon = physicalWeapon->weapon;
    if (nativeSniper(weapon)) {
        uint32_t active = 0, held = 0;
        float start = 0, end = 0, progress = 0;
        const auto *bytes = static_cast<const uint8_t *>(weapon);
        std::memcpy(&active, bytes + 0xd4, 4);
        std::memcpy(&held, bytes + 0x2c, 4);
        std::memcpy(&start, bytes + 0xf0, 4);
        std::memcpy(&end, bytes + 0xec, 4);
        std::memcpy(&progress, bytes + 0xf4, 4);
        physicalWeapon->scopePose.zoom = scopeNativeZoom(active, held, start, end, progress);
    }
}
bool currentScopeQueryBoundary() {
    ScopeDrawBinding binding;
    if (!currentScopeDraw(binding) || !physicalWeapon->ordinaryCommand || !laserEngineBase)
        return false;
    int32_t query=0;
    std::memcpy(&query,reinterpret_cast<void *>(laserEngineBase+0x2c3f34),4);
    return query==-1;
}
static unsigned scopeOpaqueDetail = 0xffu; // Diagnostic only: why the last recorded geometry had no color candidate.
static uint32_t scopeOpaqueGlobals[3]{};
bool currentScopeRaster(ScopeRasterObservation &out) {
    out = {};
    ScopeDrawBinding binding;
    if (!currentScopeDraw(binding) || physicalWeapon->scopePoseAmbiguous ||
        physicalWeapon->scopeGeometryRejected) return false;
    const auto pose = physicalWeapon->scopePose;
    if (!pose.valid || pose.requestSequence != binding.requestSequence || pose.inputSequence != binding.inputSequence ||
        pose.ownerHandle != binding.ownerHandle || pose.weaponHandle != binding.weaponHandle ||
        pose.modelHandle != binding.modelHandle || pose.generation != binding.generation) return false;
    Matrix34 affine;
    ScopeSurfaceLayout layout;
    const auto status=remote_render::copyScopeRaster(binding.modelInstance,affine,layout);
    applyScopeRasterStatus(status,physicalWeapon->scopePose.geometry,physicalWeapon->scopeGeometryRejected);
    if (status == ScopeRasterStatus::Rejected) {
        physicalWeapon->scopePose.opticalFrame = {};
        physicalWeapon->scopePose.imageCoordinates = {};
        physicalWeapon->scopePose.opaqueColorCandidate = false;
    }
    if (status != ScopeRasterStatus::Observed) return false;
    if (layout != pose.layout || std::memcmp(&affine,&pose.affine,sizeof(affine))) {
        retireScopeGeometry(); return false;
    }
    out = {pose,binding.hand};
    out.nearDepth=physicalWeapon->pass.world.nearDepth;
    out.farDepth=physicalWeapon->pass.world.farDepth;
    out.capClipValid = executedUiProjectionValid && scopeCapClip(executedUiProjection,
        physicalWeapon->pass.world.view,pose.affine,out.capClip);
    if (laserEngineBase) {
        uint32_t forced=1,mode=1;
        int32_t special=0;
        std::memcpy(&forced,reinterpret_cast<void *>(laserEngineBase+0x2e49f0),4);
        std::memcpy(&special,reinterpret_cast<void *>(laserEngineBase+0x2c36ec),4);
        std::memcpy(&mode,reinterpret_cast<void *>(laserEngineBase+0x2e494c),4);
        out.opaqueMode=!forced && special<0 && !mode;
        scopeOpaqueGlobals[0]=forced; scopeOpaqueGlobals[1]=uint32_t(special); scopeOpaqueGlobals[2]=mode;
    }
    return true;
}
void retireScopeGeometry(bool fatal) noexcept {
    if (physicalWeapon) {
        rejectScopeGeometry(physicalWeapon->scopePose.geometry,physicalWeapon->scopeGeometryRejected);
        physicalWeapon->scopePose.opticalFrame = {};
        physicalWeapon->scopePose.imageCoordinates = {};
        physicalWeapon->scopePose.opaqueColorCandidate = false;
    }
    if (fatal) SS2VR_PAIR_FAULT();
}
bool recordScopeGeometry(const ScopeRasterObservation &sample, const ScopeCapGeometry &cap,
                         const ScopeUvTransform &nativeUv, bool opaqueColorCandidate) {
    ScopeRasterObservation now;
    if (!cap.valid || !currentScopeRaster(now) || sample.hand != now.hand ||
        sample.pose.requestSequence != now.pose.requestSequence || sample.pose.inputSequence != now.pose.inputSequence ||
        sample.pose.ownerHandle != now.pose.ownerHandle || sample.pose.weaponHandle != now.pose.weaponHandle ||
        sample.pose.modelHandle != now.pose.modelHandle || sample.pose.generation != now.pose.generation ||
        sample.pose.layout != now.pose.layout || std::memcmp(&sample.pose.affine,&now.pose.affine,sizeof(Matrix34))) {
        retireScopeGeometry(); return false;
    }
    // contentVerified remains false: no material/skeleton proof here.
    ScopeOpticalFrame optical;
    ScopeImageCoordinates coordinates;
    if (!scopeOpticalFrame(cap,now.pose.affine,optical) ||
        !scopeImageCoordinates(cap,now.pose.affine,nativeUv,coordinates) ||
        (physicalWeapon->scopePose.imageCoordinates.valid &&
         physicalWeapon->scopePose.imageCoordinates.rows != coordinates.rows) ||
        !acceptScopeGeometry(physicalWeapon->scopePose.geometry,physicalWeapon->scopeGeometryRejected,cap)) {
        retireScopeGeometry(); return false;
    }
    physicalWeapon->scopePose.opticalFrame = optical;
    physicalWeapon->scopePose.imageCoordinates = coordinates;
    // Color failure never retires independently admitted geometry. This value
    // describes matching pre-draw color observations, not uninterrupted device
    // state through this DIP, image permission, source resource or transfer.
    physicalWeapon->scopePose.opaqueColorCandidate=opaqueColorCandidate && sample.opaqueMode && now.opaqueMode &&
        sample.nearDepth==now.nearDepth && sample.farDepth==now.farDepth;
    scopeOpaqueDetail = (opaqueColorCandidate ? 0u : 1u) | (sample.opaqueMode ? 0u : 2u) | (now.opaqueMode ? 0u : 4u) |
        (sample.nearDepth==now.nearDepth && sample.farDepth==now.farDepth ? 0u : 8u);
    return true;
}
bool scopeImageTarget(unsigned hand,Vec3 &out) noexcept {
    out = {};
    if (hand >= 2 || !eyePlayer || eyeIndex < 0 || eyeIndex > 1 || scopePreview || weaponPairFault) return false;
    const auto &aim = eyeLasers[hand];
    if (!aim.valid || aim.kind != LaserSourceKind::Handheld || aim.owner != eyeSnapshot.playerHandle ||
        aim.weapon != eyeSnapshot.handle[hand] || aim.generation != eyeSnapshot.generation ||
        aim.requestSequence != eyeRequest.sequence || aim.sequence != eyeRequest.input.sequence) return false;
    out = aim.end;
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}
// Single pass: both eyes' positions (the pass renders from the centre camera).
bool scopeImageEyes(Vec3 (&out)[2]) noexcept {
    if (!eyePlayer || eyeIndex != 0 || scopePreview || weaponPairFault || !singlePassWorld || !singlePassRealValid ||
        singlePassReal.sequence != eyeRequest.sequence || singlePassReal.input.sequence != eyeRequest.input.sequence) return false;
    for (unsigned k = 0; k < 2; ++k) {
        out[k] = worldEyeTracking(eyeAnchor,eyeSnapshot.origin,eyeSnapshot.turn,eyeRequest.input.head,singlePassReal.eye[k]).p;
        if (!std::isfinite(out[k].x) || !std::isfinite(out[k].y) || !std::isfinite(out[k].z)) return false;
    }
    return true;
}
bool scopeImageEye(Vec3 &out) noexcept {
    if (!eyePlayer || eyeIndex < 0 || eyeIndex > 1 || scopePreview || weaponPairFault) return false;
    out = worldEyeTracking(eyeAnchor,eyeSnapshot.origin,eyeSnapshot.turn,eyeRequest.input.head,
                           eyeRequest.eye[eyeIndex]).p;
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}
bool scopePreviewNeeded(void *player, const Request &request) {
    if (!scopeInjectionReturn || player != eyeSnapshot.player || !eyeSnapshot.rider.handheld() ||
        !nativeUiFrameCurrent(player,request)) return false;
    for (unsigned hand = 0; hand < 2; ++hand)
        if ((eyeSnapshot.zoom[hand] || settings.scopeAlwaysLive) && eyeSnapshot.handle[hand] &&
            nativeSniper(resolve(eyeSnapshot.handle[hand]))) return true;
    return false;
}
static bool copyScopeSourceViewChecked(unsigned hand, ScopeSourceView &out, unsigned &reason) {
    out = {};
    reason = 1; if (eyeIndex != 0) return false; // Observed in the ordinary left-eye render.
    reason = 2; if (!observedScopePose(hand,out.pose)) return false;
    reason = 3; if (!out.pose.opaqueColorCandidate) return false;
    reason = 4; if (!out.pose.imageCoordinates.valid) return false;
    reason = 5; if (!scopeOpticalFrameValid(out.pose.opticalFrame)) return false;
    float magnification = 0;
    // Not zoomed: the always-live scope's own magnification.
    reason = 6; if (!scopeNativeMagnification(out.pose.nativeBaseFovRadians,out.pose.zoom,magnification) &&
                    !(settings.scopeAlwaysLive && !out.pose.zoom.valid && (magnification = settings.scopeIdleMagnification) >= 1))
        return false;
    reason = 7; if (!scopeOpticalProjection(out.pose.opticalFrame,settings.scopeEyeRelief,magnification,out.opticalProjection) ||
        !scopeOpticalFov(out.opticalProjection,out.fov)) return false;
    matrixPose(&out.camera,out.pose.opticalFrame.camera);
    reason = 8; if (!finite(out.camera) || !sameWeaponView(matrix(out.camera),out.pose.opticalFrame.camera)) return false;
    out.hand = hand;
    out.alwaysLive = settings.scopeAlwaysLive;
    reason = 0;
    return true;
}
bool copyScopeSourceView(unsigned hand, ScopeSourceView &out) {
    unsigned reason = 0;
    const bool ok = copyScopeSourceViewChecked(hand, out, reason);
    // Diagnostics: the first failing condition, rate-limited (1 preview/eye, 2 no
    // observed pose, 3 colour, 4 image coords, 5 optical frame, 6 magnification,
    // 7 projection, 8 camera).
    static uint64_t lastLog[2]{};
    static unsigned lastReason[2]{~0u,~0u};
    const uint64_t now = GetTickCount64();
    if (hand < 2 && (reason != lastReason[hand] || now - lastLog[hand] > 5000) && (reason != 2 || hand == 1)) {
        lastReason[hand] = reason; lastLog[hand] = now;
        log("Scope view hand=%u reason=%u zoomValid=%u zoomEnd=%.3f baseFov=%.3f colorFailure=%u capError=%.3g opaque=%x "
            "globals=%x/%x/%x", hand, reason, unsigned(out.pose.zoom.valid), double(out.pose.zoom.endMultiplier),
            double(out.pose.nativeBaseFovRadians), scopeColorFailure(), scopeCapError(), scopeOpaqueDetail, scopeOpaqueGlobals[0],
            scopeOpaqueGlobals[1], scopeOpaqueGlobals[2]);
        const uint32_t *st = scopeColorStates();
        if (scopeColorFailure() == 9) log("Scope color states z=%x zw=%x zf=%x ab=%x at=%x st=%x fog=%x cw=%x fill=%x cull=%x sc=%x clip=%x "
            "bias=%x slope=%x clipping=%x srgb=%x", st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7], st[8],
            st[9], st[10], st[11], st[12], st[13], st[14], st[15]);
    }
    return ok;
}
static unsigned scopeBeginFailure = 0; // Diagnostic: 1 view/player, 2 sequence, 3 identity/frame.
unsigned scopeSourceBeginFailure() noexcept { return scopeBeginFailure; }
bool beginScopeSource(void *player, const Request &request, const ScopeSourceView &view,
                      ScopeCaptureCallback callback, ScopeCaptureCurrent currentSource, void *context) {
    scopeBeginFailure = 1;
    if (!callback || !currentSource || !context || view.hand >= 2 || !view.pose.valid ||
        !finite(view.camera) || !scopeOpticalProjectionValid(view.opticalProjection) ||
        player != eyeSnapshot.player || !eyeSnapshot.rider.handheld()) return false;
    // The view was observed in the previous frame's left-eye render (no
    // preview pass); the lens reprojects it into the current optic frame.
    // Late-latched requests advance the sequence by several per frame.
    scopeBeginFailure = 2;
    if (!view.pose.requestSequence || view.pose.requestSequence >= request.sequence ||
        request.sequence - view.pose.requestSequence > ScopeSourceMaxRequestAge) return false;
    scopeBeginFailure = 3;
    if (view.pose.generation != eyeSnapshot.generation || view.pose.ownerHandle != eyeSnapshot.playerHandle ||
        view.pose.weaponHandle != eyeSnapshot.handle[view.hand] || !nativeUiFrameCurrent(player,request)) return false;
    beginEye(player,request,-1); // Explicit source purpose, never a synthetic eye index.
    scopeSource.view = view;
    scopeSource.callback = callback; scopeSource.current = currentSource; scopeSource.context = context;
    scopeSource.active = true;
    scopeBeginFailure = 0;
    return true;
}
bool scopeSourceExecuting() noexcept {
    return scopeSource.active && scopeSource.prepared && scopeSource.queued && !scopeSource.fault &&
        !scopeSource.executed && eyePlayer && eyeIndex == -1 && rootView && executingView == rootView &&
        !weaponPairFault && nativeCurrentView && nativeCurrentProjection &&
        sameWeaponView(*nativeCurrentView,matrix(inverse(scopeSource.view.camera))) &&
        sameProjectionXY(*nativeCurrentProjection,ss2vr::projection(scopeSource.view.fov));
}
bool endScopeSource(bool normalCompletion) noexcept {
    const bool completed = normalCompletion && scopeSource.active && scopeSource.prepared &&
        scopeSource.queued && scopeSource.executed && !scopeSource.fault && !weaponPairFault;
    endEye();
    return completed;
}
bool observedScopePose(unsigned hand, ScopePoseObservation &out) {
    out = {};
    if (!remote_render::ownsNativeThread() || weaponPairFault || hand >= 2 || eyeIndex < 0 ||
        !trackedWeaponSession(eyeSnapshot) || !livePlayer(eyeSnapshot))
        return false;
    const uint32_t weaponHandle = eyeSnapshot.handle[hand];
    if (!weaponHandle || nativeHandle(eyeSnapshot.player, int(hand)) != weaponHandle ||
        nativeHandle(eyeSnapshot.player, int(1 - hand)) == weaponHandle)
        return false;
    void *weapon = resolve(weaponHandle);
    uint32_t modelHandle = 0, ownerHandle = 0;
    if (!weapon)
        return false;
    std::memcpy(&modelHandle, static_cast<uint8_t *>(weapon) + 0x24, 4);
    std::memcpy(&ownerHandle, static_cast<uint8_t *>(weapon) + 0x28, 4);
    if (!modelHandle || !resolve(modelHandle) || ownerHandle != eyeSnapshot.playerHandle)
        return false;
    return eyeScopePoses.copy(hand, eyeRequest.sequence, eyeRequest.input.sequence,
                              ownerHandle, weaponHandle, modelHandle, eyeSnapshot.generation,
                              weaponPairFault, out);
}
// One-time evidence per weapon: native model extent and the applied scale.
struct Box3 { Vec3 min, max; }; // Native Box3f.
static Box3 *(__cdecl *modelBounds)(Box3 *, const void *) = nullptr;
// Private-lab weapon audit: each weapon's native first-person pose relative to
// the camera and its model-space hand/palm bones, logged once per weapon.
static void auditWeaponAnchor(void *w, int nativeId, const Pose &cameraRelative) {
    static bool traced[64]{};
    if (!settings.labWeaponAudit || nativeId < 0 || nativeId >= 64 || traced[nativeId]) return;
    traced[nativeId] = true;
    void *instance = resolve(primaryField(w, 0x24));
    Box3 box{};
    if (instance && modelBounds) modelBounds(&box, instance);
    char bones[256] = "";
    for (const auto &[name, ident] : {std::pair{"Hand", boneHandIdent}, std::pair{"Palm", bonePalmIdent},
                                      std::pair{"Arm", boneArmIdent}}) {
        Matrix34 m{};
        if (instance && boneAbsolute && ident && boneAbsolute(instance, ident, m)) {
            const size_t used = std::strlen(bones);
            std::snprintf(bones + used, sizeof(bones) - used, " %s=%.4f,%.4f,%.4f", name, m.m[3], m.m[7], m.m[11]);
        }
    }
    if (instance && bonesAbsolute && identToString) {
        // Each BonePlacement: 4-byte IDENT then a 3x4 matrix (translation at 3, 7, 11).
        BonePlacementArray all;
        bonesAbsolute(instance, all);
        for (int32_t i = 0; i < all.count && all.data; ++i) {
            const uint8_t *entry = all.data + size_t(i) * 0x34;
            uint32_t ident = 0;
            float m[12]{};
            std::memcpy(&ident, entry, 4);
            std::memcpy(m, entry + 4, sizeof(m));
            const char *name = identToString(ident);
            log("Weapon bone id=%d index=%d name=%s at=%.4f,%.4f,%.4f", nativeId, i, name ? name : "?", m[3], m[7], m[11]);
        }
    }
    const auto &q = cameraRelative.q;
    const auto &p = cameraRelative.p;
    log("Weapon audit id=%d camera=%.4f,%.4f,%.4f q=%.4f,%.4f,%.4f,%.4f box=%.3f,%.3f,%.3f..%.3f,%.3f,%.3f%s",
        nativeId, p.x, p.y, p.z, q.x, q.y, q.z, q.w, box.min.x, box.min.y, box.min.z, box.max.x, box.max.y,
        box.max.z, bones);
}
static void traceWeaponModel(void *w, int nativeId, float scale) {
    static bool traced[64]{};
    if (nativeId < 0 || nativeId >= 64 || traced[nativeId] || !modelBounds) return;
    traced[nativeId] = true;
    void *instance = resolve(primaryField(w, 0x24));
    Box3 box{};
    if (!instance || !modelBounds(&box, instance)) return;
    log("Weapon model id=%d extent=%.3f,%.3f,%.3f scale=%.2f", nativeId, box.max.x - box.min.x,
        box.max.y - box.min.y, box.max.z - box.min.z, scale);
}
static int __fastcall weaponAbs(void *w, void *, const Matrix34 &view, Matrix34 &out) {
#ifdef _MSC_VER
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    const bool physical = physicalWeapon && physicalWeapon->weapon == w &&
                          caller == weaponPlacementReturn;
    Vec3 charge;
    bool invalidCharge = false;
    const int result = nativePlacementWithCharge(w, view, out, charge, invalidCharge);
    if(nativeDesktopSniperPlacement(weaponRenderRoute,reinterpret_cast<uintptr_t>(w),eyeIndex,
                                   weaponPlacementReturn && caller==weaponPlacementReturn))
        return result; // Unmanaged original desktop delegation owns this placement.
    auto reject = [&]() {
        if (physical) {
            physicalWeapon->pass.placed(false);
            SS2VR_PAIR_FAULT();
            return 0; // Native Render skips model draw, then adapter repairs missing restore.
        }
        return result;
    };
    if (!result) {
        if (physical && physicalWeapon && physicalWeapon->hand >= 0)
            invalidateMatchingCalibration(eyeSnapshot, unsigned(physicalWeapon->hand),8,caller);
        return reject();
    }
    const auto s = eyeIndex >= 0 ? eyeSnapshot : copySnapshot();
    const int h = handOf(w, s);
    const Input &tracking = eyeIndex >= 0 ? eyeRequest.input : s.input;
    if (h < 0 || !trackedWeaponSession(s)) return reject();
    const bool align=weaponAlignmentSupported(primaryField(w,0xb4));
    WeaponAlignmentBinding modelBefore,modelAfter;
    // Other native callers (e.g. Sam2Game 22BF20, an attachment query that runs
    // while the other controller is off) get the native placement; they are no
    // evidence against the render's calibration, and invalidating it there left
    // the remaining hand's shots and laser uncalibrated. Shots recheck the binding.
    if(caller!=weaponPlacementReturn) return reject();
    if(!captureWeaponModelBinding(s.player,w,unsigned(h),true,modelBefore)) {
        invalidateMatchingCalibration(s,unsigned(h),3,caller);
        return reject();
    }
    if (invalidCharge || !finiteMatrix(out)) {
        invalidateMatchingCalibration(s, unsigned(h),4,caller);
        return reject(); // Unmanaged native callers retain result/out above.
    }
    if ((!physical && !fresh(s.input)) || !tracking.handValid[mountHand(s, unsigned(h))] ||
        !finite(weaponTracking(tracking, mountHand(s, unsigned(h)))) ||
        (physical && (physicalWeapon->hand != h || physicalWeapon->pass.stage != 3)))
        return reject();
    Pose native, suppliedCamera;
    matrixPose(&native, out);
    matrixPose(&suppliedCamera, view);
    if (!localWeaponCurrent(w, s, unsigned(h)))
        return reject();
    Pose body, nativeCamera;
    if (eyeIndex >= 0) {
        body = eyeAnchor;
        nativeCamera = eyeNativeCamera;
    } else {
        if (!trackingAnchor(s.player, body))
            return reject();
        originalCamera(s.player, &nativeCamera);
    }
    if (!finite(nativeCamera) || !finite(native) || !finite(suppliedCamera) ||
        !localWeaponCurrent(w, s, unsigned(h)))
        return reject();
    const Pose hand = worldHandTracking(
        body, s.origin, s.turn, tracking.head,
        weaponGrip(tracking, mountHand(s, unsigned(h)), *reinterpret_cast<int *>(static_cast<uint8_t *>(w) + 0xb4)));
    // The weapon is held along the controller's aim axis: the flat viewmodel's
    // tilt toward the screen centre is dropped so barrel, laser and shots agree,
    // but a model authored turned (e.g. the auto shotgun, a half turn) keeps it.
    const Quat offset = snapToAxes(multiply(inverse(suppliedCamera.q), native.q));
    const Vec3 displacement = rotate(offset, charge);
    Vec3 alignment;
    if(align && !alignmentDisplacement(modelBefore,offset,alignment)) {
        invalidateMatchingCalibration(s,unsigned(h),5,caller);return reject();
    }
    const int nativeId = *reinterpret_cast<int *>(static_cast<uint8_t *>(w) + 0xb4);
    const float scale = weaponModelScale(settings, nativeId);
    traceWeaponModel(w, nativeId, scale);
    const Pose target{normalize(multiply(hand.q, offset)), hand.p + rotate(hand.q, (displacement+alignment) * scale)};
    if (!finite(target))
        return reject();
    const Matrix34 staged = scaledModel(matrix(target), scale); // Enlarged about the hand.
    Matrix34 flatModel;
    const auto flatCamera = matrix(nativeCamera);
    Pose flatPose;
    Vec3 flatCharge;
    bool invalidFlatCharge = false;
    const bool calibrated = nativePlacementWithCharge(w, flatCamera, flatModel, flatCharge,
                                                      invalidFlatCharge) != 0;
    if (!localWeaponCurrent(w, s, unsigned(h)))
        return reject();
    if (calibrated)
        matrixPose(&flatPose, flatModel);
    if (calibrated && h == 1 && finite(flatPose))
        auditWeaponAnchor(w, nativeId, compose(inverse(nativeCamera), flatPose));
    if (!calibrated || invalidFlatCharge || !finite(flatPose)) {
        invalidateMatchingCalibration(s, unsigned(h),6,caller);
        return reject();
    }
    if (!localWeaponCurrent(w, s, unsigned(h)))
        return reject();
    if(!captureWeaponModelBinding(s.player,w,unsigned(h),true,modelAfter) ||
       modelBefore!=modelAfter) {
        invalidateMatchingCalibration(s,unsigned(h),7,caller);return reject();
    }
    AcquireSRWLockExclusive(&snapshotLock);
    const bool compatible = sameHandheldRig(s.rider, current.rider, s.generation, current.generation) &&
        s.rigRevision==current.rigRevision && rigPublication.usable(s.rigRevision) &&
        current.playerHandle == s.playerHandle && current.handle[h] == s.handle[h] &&
        channel.shared && trackingEpoch(*channel.shared) == s.generation;
    const bool placed = compatible && (!physical || physicalWeapon->pass.placed(finiteMatrix(staged)));
    if (placed) {
        const auto relative = calibrated ? compose(inverse(nativeCamera), flatPose) : Pose{};
        calibration[h] = {calibrated && finite(flatPose), s.handle[h], relative,
                          GetTickCount64(), calibrated ? rotate(relative.q, flatCharge) : Vec3{},
                          alignment,modelAfter,align};
        if(labSniperConfigured && labPreparation->sniperZoom && eyeIndex>=0 && eyeIndex<2 && labCalibrationSerial!=UINT64_MAX) {
            auto &c=calibration[h];c.labSerial=++labCalibrationSerial;
            c.labRequest=eyeRequest.sequence;c.labInput=eyeRequest.input.sequence;c.labEye=eyeIndex;
            c.labNativeModel=flatModel;
        }
        if(physical && physicalWeapon->idle) {
            auto &trace=*physicalWeapon->idle;
            const IdleDrawIdentity identity{eyeRequest.sequence,eyeRequest.input.sequence,s.playerHandle,s.handle[h],
                primaryField(w,0x24),s.generation,unsigned(h),unsigned(eyeIndex)};
            trace.placement(identity,out,staged,matrix(hand));
            const unsigned m=mountHand(s,unsigned(h));
            const bool gripValid=tracking.gripValid[m] && finite(tracking.grip[m]);
            const auto rawGrip=gripValid?matrix(worldHandTracking(body,s.origin,s.turn,tracking.head,tracking.grip[m])):Matrix34{};
            const auto rawAim=matrix(worldHandTracking(body,s.origin,s.turn,tracking.head,tracking.hand[m]));
            trace.references(identity,rawGrip,gripValid,rawAim);
        }
        out = staged;
        // The Serious Bomb model is both arms: its left arm follows the left controller.
        if (nativeId == 11 && controllerTracked(tracking, 0) && mountHand(s, unsigned(h)) == 1)
            remote_render::setSplitModel(uintptr_t(modelAfter.instance),
                worldHandTracking(body, s.origin, s.turn, tracking.head, weaponTracking(tracking, 0)), hand.q, true);
        else if (nativeId == 11 || mountHand(s, unsigned(h)) == 1)
            remote_render::setSplitModel(0, {}, {0, 0, 0, 1}, false);
    }
    const auto published=calibration[h];
    ReleaseSRWLockExclusive(&snapshotLock);
    laserTraceValue(LaserTraceStage::Cache,placed?"published":"placement-rejected",s,unsigned(h),
                    eyeIndex>=0?eyeRequest.sequence:0,0,&published,caller);
    if (!placed)
        return reject();
    return result;
}
static bool heldUnderOpenWheel(void *w) {
    if (eyeIndex < 0) return false;
    const int h = handOf(w, eyeSnapshot);
    if (h < 0) return false;
    const unsigned m = mountHand(eyeSnapshot, unsigned(h));
    return eyeSnapshot.ui.wheel[m].open || grenadeShownHand.load(std::memory_order_relaxed) == int(m) ||
           bombLighterSlot(eyeSnapshot, unsigned(h));
}
static void renderTrackedWeapon(void *w, Matrix34 m, bool sniper, uintptr_t caller) {
    // A hand with its weapon wheel open shows the wheel, not the held gun (nor
    // does the bomb's lighter hand).
    if (heldUnderOpenWheel(w)) return;
    const unsigned previousRenderDepth=nativeWeaponRenderDepth;
    if(previousRenderDepth!=UINT_MAX)++nativeWeaponRenderDepth;
    const WeaponRenderRoute *previousRoute=nullptr;
    WeaponRenderRoute route{reinterpret_cast<uintptr_t>(w),nativeWeaponRenderDepth,sniper,eyeIndex==-1,false,nullptr};
    bool routePublished=false;
    const auto generation = graphicsResourceGeneration();
    auto *previous = physicalWeapon;
    if (previous) { previous->pass.failed = true; SS2VR_PAIR_FAULT();
        if(previous->idle)previous->idle->reject(IdleWeaponTrace::Rejection::NestedGun); }
    physicalWeapon = nullptr;
    PhysicalWeaponInvocation invocation;
    std::optional<IdleWeaponTrace> idleStorage;
    bool physical = false,originalReturned=false;
    const bool returned=withNativeFinally([&] {
    previousRoute=weaponRenderRoute;
    route.parent=previousRoute;
    weaponRenderRoute=&route;
    routePublished=true;
    const auto s = eyeIndex >= 0 ? eyeSnapshot : copySnapshot();
    const int hand = handOf(w, s);
    if (eyeIndex >= 0 && hand >= 0)
        eyeScopePoses.beginDraw(unsigned(hand)); // Supersedes earlier evidence even on early return/unwind.
    if (eyeIndex >= 0 && hand >= 0 &&
        (!eyeRequest.input.handValid[mountHand(s, unsigned(hand))] ||
         !finite(weaponTracking(eyeRequest.input, mountHand(s, unsigned(hand))))))
        return;
    physical = eyeIndex >= 0 && eyePlayer == s.player && hand >= 0 &&
                          executingView == rootView && rootView;
    if (physical) {
        if (!trackedWeaponSession(s) || !executedWeaponWorld.valid || !physicalCallbacksAvailable() ||
            !finiteProjection(*nativeCurrentProjection) ||
            !validDepthRange(*nativeDepthNear, *nativeDepthFar)) {
            SS2VR_PAIR_FAULT();
            return;
        }
        invocation = {w, hand, {executedWeaponWorld}, *nativeCurrentProjection,
                      *nativeDepthNear, *nativeDepthFar, {}, false};
        invocation.ordinaryCommand=ordinaryWeaponRenderReturn && caller==ordinaryWeaponRenderReturn;
        const int selectedId=selectedIdleProbeWeapon();
        if(invocation.ordinaryCommand && idleProbeWeaponSupported(selectedId) &&
           primaryField(w,0xb4)==unsigned(selectedId) && primaryField(w,0xb0)==1 &&
           (selectedId!=13 || !primaryField(w,0xd4))) {
            static std::atomic<unsigned> attempts{0};
            if(attempts.fetch_add(1,std::memory_order_relaxed)<32) {
                idleStorage.emplace();invocation.idle=&*idleStorage;
                idleStorage->admit({eyeRequest.sequence,eyeRequest.input.sequence,s.playerHandle,
                    s.handle[hand],primaryField(w,0x24),s.generation,unsigned(hand),unsigned(eyeIndex)},selectedId);
            }
        }
    }
    // Establish a suppression barrier even for unowned/nested/desktop calls.
    physicalWeapon = physical ? &invocation : nullptr;
    route.originalActive=true;
    if (sniper && !(eyeIndex >= 0 && hand >= 0))
        originalSniperRender(w, m);
    else
        originalWeaponRender(w, m);
    originalReturned=true;
    route.originalActive=false;
    if (physical && generation == graphicsResourceGeneration())
        eyeScopePoses.finishDraw(unsigned(hand), invocation.scopePose,
                                 invocation.pass.complete() && !invocation.scopePoseAmbiguous);
    if(idleStorage) {
        retireIdleSubmissionOwner(&*idleStorage);
        idleStorage->submissions.outerReturned=true;
        idleStorage->projectionProbe.retire();
        idleStorage->finish(invocation.pass.complete() && !weaponPairFault,generation==graphicsResourceGeneration());
        emitIdleWeaponTrace(*idleStorage);
    }
    }, [&](bool aborted) noexcept {
        if(routePublished)weaponRenderRoute=previousRoute;
        nativeWeaponRenderDepth=previousRenderDepth;
        retireIdleSubmissionOwner(invocation.idle);
        if(invocation.idle)invocation.idle->projectionProbe.retire();
        if (!aborted && physical && generation == graphicsResourceGeneration())
            finishPhysicalWeapon(invocation, generation);
        physicalWeapon = !aborted && generation == graphicsResourceGeneration() ? previous : nullptr;
        if (aborted) {SS2VR_PAIR_FAULT();if(invocation.idle)invocation.idle->reject(IdleWeaponTrace::Rejection::GunAbort);}
        if(invocation.idle)invocation.idle->submissions.completeOuter(originalReturned,
            physical && !aborted && invocation.pass.complete(),
            !aborted && generation==graphicsResourceGeneration(),!weaponPairFault,!aborted);
    });
    // The joined companion emits only here, after normal original and
    // cleanup completion. Legacy diagnostics keep their existing schema/order.
    if(!returned && idleStorage)idleStorage->submissions.outerCompleted=false;
    if(returned && idleStorage)withNativeFinally([&] {
        emitIdlePaletteCompanions(*idleStorage);
        emitIdlePaletteBoundaries(*idleStorage);
        emitIdleProjectionOpportunities(*idleStorage);
    },[](bool) noexcept {});
}
// While a hand's wheel is open its held weapon is not drawn in the eyes, so it
// cannot hide the floating weapon models it is choosing between.
static void __fastcall weaponRender(void *w, void *, Matrix34 m) {
    renderTrackedWeapon(w, m, false,reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
}
static void __fastcall sniperRender(void *w, void *, Matrix34 m) {
    // Native zoom overlay stays on desktop; XR retains the native gun/hand mesh.
    renderTrackedWeapon(w, m, true,reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
}
static bool markerFrameCurrent(void *player, const Request &request, int index) {
    if (index < 0 || index > 1 || eyeIndex != index || player != eyePlayer || weaponPairFault ||
        !rootCaptureAttempted || !executedWeaponWorld.valid || !nativeMainThread || !nativeMainThread() ||
        request.sequence != eyeRequest.sequence || request.session != eyeRequest.session ||
        request.reference != eyeRequest.reference || request.input.sequence != eyeRequest.input.sequence)
        return false;
    const auto live = copySnapshot();
    return live.rigRevision==eyeSnapshot.rigRevision &&
           live.rider == eyeSnapshot.rider && nativeRiderCurrent(player, eyeSnapshot.rider) &&
           nativeFrameIdentity(request, live.input, live.generation, live.initialized, live.ui.gameplay,
                               live.player == player && livePlayer(live)) && vrSession(live) && fresh(live.input);
}
static bool markerDrawPortCurrent(void *drawPort, const Request &request, WorldMarkerDimensions &out) {
    if (!drawPort || nativeDrawPort() != drawPort)
        return false;
    int32_t rectangle[4]{};
    memcpy(rectangle, static_cast<uint8_t *>(drawPort) + 4, sizeof(rectangle));
    return worldMarkerDimensions(rectangle, request.width, request.height, out);
}
static bool nativeMarkerPostlude(void *player, const Request &request, int index) {
    const auto reject = [] { SS2VR_PAIR_FAULT(); return false; };
    if (!markerFrameCurrent(player, request, index))
        return reject();
    // Native renFinishRender has destroyed rootView. These are value copies;
    // no command activation, pointer dereference or second world pass belongs here.
    const auto world = executedWeaponWorld;
    void *drawPort = nativeDrawPort(), *info = nativeWorldInfo();
    const uint32_t infoHandle = info ? pointerHandle(info) : 0;
    WorldMarkerDimensions dimensions;
    const Pose eye = worldEyeTracking(eyeAnchor, eyeSnapshot.origin, eyeSnapshot.turn,
                                     request.input.head, request.eye[index]);
    if (!infoHandle || resolve(infoHandle) != info ||
        !markerDrawPortCurrent(drawPort, request, dimensions) ||
        !worldMarkerViewValid(world, eye, request.fov[index], dimensions, request.width, request.height))
        return reject();
    const auto compatible = [&] {
        WorldMarkerDimensions currentDimensions;
        return markerFrameCurrent(player, request, index) && resolve(infoHandle) == info &&
               nativeWorldInfo() == info && markerDrawPortCurrent(drawPort, request, currentDimensions) &&
               markerFrameCurrent(player, request, index);
    };
    if (!compatible())
        return reject();
    auto *vtable = *static_cast<uintptr_t **>(player);
    const float fade = reinterpret_cast<MarkerFade>(vtable[0x60c / 4])(player);
    if (!std::isfinite(fade) || !compatible())
        return reject();
    ortho();
    nativeBlendType(501);
    (*disableDepthWrite)();
    (*disableDepth)();
    (*disableAlpha)();
    if (!compatible())
        return reject();
    if (fade > 0) {
        nativeNavigation(player, world.view, world.projection, dimensions, fade);
        if (!compatible())
            return reject();
        nativeObjectives(player, world.view, world.projection, dimensions, fade);
    }
    return compatible() ? true : reject();
}
// Bound by the installed build fingerprint. Native simple programs are
// assembled from the verified c1..c4 position operation; both device objects
// occupy offset0 in their12-byte records. Borrow nothing past this draw.
bool nativeUiProgramsCurrent(IDirect3DVertexShader9 *vertex, IDirect3DPixelShader9 *pixel) {
    if (!nativeMainThread || !nativeMainThread() || !vertex || !pixel ||
        !readableMemory(uiPrograms,12) || !readableMemory(uiSimplePrograms,48) ||
        !readableMemory(uiVertexHandle,4) || !readableMemory(uiPixelHandle,4)) return false;
    const uint32_t data = uiPrograms[1], count = uiPrograms[2];
    const int32_t vh = *uiVertexHandle, ph = *uiPixelHandle;
    if (!count || count > 65536 || vh <= 0 || ph <= 0 || uint32_t(vh) > count || uint32_t(ph) > count ||
        !readableMemory(reinterpret_cast<void *>(data),size_t(count)*12)) return false;
    bool builtin = false;
    for (unsigned i=0;i<6;++i)
        builtin |= uiSimplePrograms[2*i] == vh && uiSimplePrograms[2*i+1] == ph;
    if (!builtin) return false;
    const auto *records = reinterpret_cast<const uint32_t *>(data);
    return records[3*(vh-1)] == reinterpret_cast<uintptr_t>(vertex) &&
           records[3*(ph-1)] == reinterpret_cast<uintptr_t>(pixel);
}
using OverlayRender = void(__thiscall *)(void *,int);
using NativeFill = void(__cdecl *)(uint32_t);
static VoidThis originalBrainRender = nullptr;
static OverlayRender originalOverlayRender = nullptr;
static NativeFill originalNativeFill = nullptr;
static uintptr_t overlayParentReturn = 0, uiFadeReturn[2]{};
struct NativeUiOwner {
    void *brain = nullptr, *player = nullptr;
    uint32_t brainHandle = 0, playerHandle = 0;
};
static thread_local NativeUiOwner uiOwner;
static thread_local unsigned uiOwnerDepth = 0, uiOverlayDepth = 0;
static thread_local uint64_t uiOwnerGeneration = 0;
static bool labRideControlConfigured=false;
static uintptr_t controlsGameBase=0, rideEngineModelRenderable=0;
// A ride's model renderable: the game's puppet renderable (Sam2Game 0x2a4648),
// or the engine's own CModelRenderable (Engine 0x209840), which the HoverFighter
// bike in "Road to Ursul" uses (its handles were never admitted before).
static bool rideRenderableTable(uintptr_t table) {
    return table && ((controlsGameBase && table==controlsGameBase+0x2a4648) ||
                     (rideEngineModelRenderable && table==rideEngineModelRenderable));
}
static constinit thread_local uint32_t nativeWorldRenderDepth=0;
bool nativeRenderExtentCurrent() noexcept {
    return !uiOwnerDepth || uiOwnerGeneration == graphicsResourceGeneration();
}
bool nativeUiOwnerCurrent(void *player) {
    if(uiOwnerDepth!=1 || !nativeRenderExtentCurrent() || !player || player!=uiOwner.player || !uiOwner.brain ||
        !nativeMainThread || !nativeMainThread() || !uiOwner.brainHandle || !uiOwner.playerHandle ||
        resolve(uiOwner.brainHandle)!=uiOwner.brain || resolve(uiOwner.playerHandle)!=player ||
        !readableMemory(static_cast<uint8_t *>(uiOwner.brain)+0x28,4)) return false;
    uint32_t handle=0;
    memcpy(&handle,static_cast<uint8_t *>(uiOwner.brain)+0x28,4);
    return handle==uiOwner.playerHandle && resolve(handle)==player;
}
int nativeWorldEye() noexcept {return eyeIndex;}
// Bounded trace: the first time each reason declines a ride's render identity.
static bool rideIdentityFail(unsigned reason) {
    static uint32_t seen=0;
    if(reason<32 && !(seen&(1u<<reason))) {seen|=1u<<reason;log("Ride render identity declined reason=%u",reason);}
    return false;
}
bool copyNativeRideRenderIdentity(uint32_t expectedPlayer,RideRenderIdentity &out) {
    out={};
    if((!labRideControlConfigured && !settings.immersiveVehicleGrips) || nativeWorldRenderDepth!=1 || !expectedPlayer ||
       expectedPlayer!=uiOwner.playerHandle || !hooksReady.load(std::memory_order_acquire) ||
       !laserModelInstance || !isAlive || !isLocal || !resolve)return rideIdentityFail(1);
    void *player=resolve(expectedPlayer);
    if(!nativeUiOwnerCurrent(player) || !isLocal(player) || !isAlive(player) ||
       !readableMemory(player,0x550))return rideIdentityFail(2);
    RiderIdentity rider;
    if(!readNativeRider(player,rider) || !rider.seated())return rideIdentityFail(3);
    // Re-resolve in dependency order. readNativeRider's equality token is not
    // dereferenced; this is a new, reviewed render-extent owner route.
    void *ride=resolve(rider.ride);
    if(!readableMemory(ride,0x480))return rideIdentityFail(4);
    uint32_t table=0,operatorBrain=0,renderableHandle=0,parameter=0;
    std::memcpy(&table,ride,4);
    std::memcpy(&operatorBrain,static_cast<uint8_t*>(ride)+0x38c,4);
    if(!controlsGameBase || (table!=controlsGameBase+0x2a8558 && table!=controlsGameBase+0x2b8420) ||
       operatorBrain!=uiOwner.brainHandle || resolve(operatorBrain)!=uiOwner.brain)return rideIdentityFail(5);
    std::memcpy(&renderableHandle,static_cast<uint8_t*>(ride)+0x120,4);
    std::memcpy(&parameter,static_cast<uint8_t*>(ride)+0x47c,4);
    void *renderable=renderableHandle?resolve(renderableHandle):nullptr;
    if(!readableMemory(renderable,0x60))return rideIdentityFail(6);
    uint32_t renderableTable=0,owner=0,instanceToken=0;
    std::memcpy(&renderableTable,renderable,4);
    std::memcpy(&owner,static_cast<uint8_t*>(renderable)+0x48,4);
    std::memcpy(&instanceToken,static_cast<uint8_t*>(renderable)+0x5c,4);
    const auto gameBase=controlsGameBase;
    if(!gameBase || !rideRenderableTable(renderableTable) || owner!=reinterpret_cast<uintptr_t>(ride) ||
       !instanceToken || laserModelInstance(renderable)!=reinterpret_cast<void*>(instanceToken)) {
        static unsigned traced=0;
        if(traced++<3)log("Ride renderable table rva=%lx owner=%08x ride=%p instance=%08x model=%p",
            static_cast<unsigned long>(renderableTable-gameBase),owner,ride,instanceToken,laserModelInstance(renderable));
        return rideIdentityFail(7);
    }
    if(!nativeUiOwnerCurrent(player) || resolve(rider.ride)!=ride ||
       !nativeRiderCurrent(player,rider) || resolve(renderableHandle)!=renderable)return rideIdentityFail(8);
    // Bookend every owner edge, including mutations preserving handles.
    uint32_t afterTable=0,afterOperator=0,afterHandle=0,afterOwner=0,afterInstance=0,afterParameter=0;
    std::memcpy(&afterTable,ride,4);
    std::memcpy(&afterOperator,static_cast<uint8_t*>(ride)+0x38c,4);
    std::memcpy(&afterHandle,static_cast<uint8_t*>(ride)+0x120,4);
    std::memcpy(&afterOwner,static_cast<uint8_t*>(renderable)+0x48,4);
    std::memcpy(&afterInstance,static_cast<uint8_t*>(renderable)+0x5c,4);
    std::memcpy(&afterParameter,static_cast<uint8_t*>(ride)+0x47c,4);
    if(afterTable!=table || afterOperator!=operatorBrain || afterHandle!=renderableHandle ||
       afterOwner!=owner || afterInstance!=instanceToken || afterParameter!=parameter)return rideIdentityFail(9);
    out={expectedPlayer,operatorBrain,rider.ride,rider.seat,uint32_t(table-gameBase),renderableHandle,
         uint32_t(reinterpret_cast<uintptr_t>(renderable)),instanceToken,parameter};
    return out.valid();
}
bool nativeUiFrameCurrent(void *player,const Request &request) {
    if(!nativeUiOwnerCurrent(player)) return false;
    const auto live=copySnapshot();
    return live.rider==eyeSnapshot.rider && nativeRiderCurrent(player,eyeSnapshot.rider) &&
        nativeFrameIdentity(request,live.input,live.generation,live.initialized,live.ui.gameplay,
                            live.player==player && livePlayer(live)) &&
        vrSession(live) && fresh(live.input) && trackingEligible(player) && !weaponPairFault;
}
static void __fastcall brainRender(void *brain,void *) {
    if(uiOwnerDepth) {
        nativeUiFault();
        ++uiOwnerDepth;
        withNativeFinally([&]{ originalBrainRender(brain); },[&](bool aborted) noexcept {
            if(aborted) nativeUiFault();
            --uiOwnerDepth;
        });
        return;
    }
    uiOwnerDepth=1;
    uiOwnerGeneration=graphicsResourceGeneration();
    withNativeFinally([&] {
        if(nativeMainThread && nativeMainThread() && readableMemory(static_cast<uint8_t *>(brain)+0x28,4)) {
            uint32_t handle=0;
            memcpy(&handle,static_cast<uint8_t *>(brain)+0x28,4);
            void *player=handle ? resolve(handle) : nullptr;
            const auto live=copySnapshot();
            if(player && player==live.player && handle==live.playerHandle && livePlayer(live) &&
                trackingEligible(player) && vrSession(live) && fresh(live.input) && live.ui.gameplay)
                uiOwner={brain,player,pointerHandle(brain),handle};
        }
        originalBrainRender(brain); // Native player index/listener/HUD timing owner once.
        if (nativeRenderExtentCurrent()) nativeUiFinishOwner();
    },[&](bool aborted) noexcept {
        nativeUiEndOwner(aborted);
        uiOwner={}; uiOwnerDepth=0; uiOverlayDepth=0;
        uiOwnerGeneration=0;
    });
}
static void __fastcall overlayRender(void *player,void *,int flag) {
    const auto caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const bool outer=uiOverlayDepth==0;
    ++uiOverlayDepth;
    bool active=false;
    withNativeFinally([&] {
        const bool admitted=caller==overlayParentReturn && flag==0 && outer && nativeUiOwnerCurrent(player);
        static unsigned overlayDiagnostics=0;
        if(uiOwnerDepth==1 && player==uiOwner.player && overlayDiagnostics<4) {
            ++overlayDiagnostics;
            log("Native UI overlay callback caller=%p expected=%p flag=%d outer=%u admitted=%u",
                reinterpret_cast<void *>(caller),reinterpret_cast<void *>(overlayParentReturn),flag,outer,admitted);
        }
        active=nativeUiBeginOverlay(player,admitted);
        originalOverlayRender(player,flag);
    },[&](bool aborted) noexcept {
        if(active) nativeUiEndOverlay(!aborted);
        else if(aborted) nativeUiFault();
        --uiOverlayDepth;
    });
}
static void __cdecl nativeFill(uint32_t color) {
    const auto caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    bool tagged=false;
    withNativeFinally([&] {
        if(caller==uiFadeReturn[0] || caller==uiFadeReturn[1]) tagged=nativeUiBeginFade();
        originalNativeFill(color); // Native gradient/color/blend and timing unchanged.
    },[&](bool aborted) noexcept {
        if(tagged) nativeUiEndFade(!aborted);
        else if(aborted) nativeUiFault();
    });
}
bool nativeWorldRenderActive() noexcept {return nativeWorldRenderDepth!=0;}
static void __thiscall presentedRender(void *p) {
    uint32_t owner=0;
    withNativeFinally([&] {
        if(eyeIndex<0 && nativeRenderExtentCurrent())
            owner=remote_render::beginMonoPresentation(uiOwner.player==p?uiOwner.playerHandle:0);
        originalRender(p);
        // No publication if the original unwound. A diagnostic failure still
        // retires the exact owner in the same existing finally.
        remote_render::completeMonoPresentation(owner,true);
    },[&](bool) noexcept {remote_render::retirePresentation(owner);});
}
// VR frames skip the native desktop render (the window mirrors the left eye),
// but its multiplayer presentation bookkeeping still runs once per frame.
void monoPresentationWithoutRender(void *p) {
    uint32_t owner=0;
    withNativeFinally([&] {
        if(nativeRenderExtentCurrent())
            owner=remote_render::beginMonoPresentation(uiOwner.player==p?uiOwner.playerHandle:0);
        remote_render::completeMonoPresentation(owner,true);
    },[&](bool) noexcept {remote_render::retirePresentation(owner);});
}
static void __fastcall render(void *p,void *) {
    const auto caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const auto previousDepth=nativeWorldRenderDepth;
    nativeWorldRenderDepth=previousDepth==UINT32_MAX ? previousDepth : previousDepth+1;
    const bool previousSuppression=remote_render::presentationSuppressionCurrent();
    withNativeFinally([&] {
        if(previousDepth) {
            remote_render::suppressNestedPresentation();
            originalRender(p); // Nested draws remain native; never begin another bank/eye pair.
            return;
        }
        auto s=copySnapshot();
        // Preserve the original short-circuit order: retired render extents must
        // never acquire a native player/rider getter merely for diagnostics.
        const bool extent = nativeRenderExtentCurrent(), samePlayer = extent && p == s.player;
        const bool eligible = samePlayer && trackingEligible(p);
        const bool session = eligible && vrSession(s), currentInput = session && fresh(s.input);
        static unsigned renderDiagnostics = 0;
        if (s.ui.gameplay && renderDiagnostics < 4) {
            ++renderDiagnostics;
            log("Native world render gate extent=%u player=%u eligible=%u session=%u fresh=%u gameplay=%u marker=%u",
                extent,samePlayer,eligible,session,currentInput,s.ui.gameplay,caller==markerParentReturn);
        }
        if(extent && samePlayer && eligible && session && currentInput && s.ui.gameplay)
            stereo(p,presentedRender,caller==markerParentReturn ? nativeMarkerPostlude : nullptr);
        else presentedRender(p);
    },[&](bool aborted) noexcept {
        if(aborted) {
            if(nativeRenderOwnsOuterScope(previousDepth))nativeUiEndOwner(true);
            else nativeUiFault(); // Nested unwind never tears down its enclosing owner.
        }
        // Invalidation/reset stays latched through every nested exit. Only the
        // outer draw owns restoration of this whole presentation scope.
        if(nativeRenderOwnsOuterScope(previousDepth))
            remote_render::restorePresentationSuppression(previousSuppression);
        nativeWorldRenderDepth=previousDepth;
    });
}
// Native command query adapter: no fabricated CInputDevice object or command-data writes.
using CommandFloat = float(__thiscall *)(void *, uint32_t);
using CommandInt = int(__thiscall *)(void *, uint32_t);
using DataGet = void *(__thiscall *)(void *);
static CommandFloat originalValue = nullptr;
static CommandInt originalDown = nullptr, originalPressed = nullptr, originalReleased = nullptr,
                  originalRepeated = nullptr;
static VoidThis originalPoll = nullptr;
static DataGet getData = nullptr;
static uint32_t *(__cdecl *toId)(uint32_t *, const char *) = nullptr;
constexpr int CommandCount = 19;
// Quick save/load are queried with IsCommandPressed on these same player
// bindings (Sam2Game player-controls vtable 2C3424 slot 7, RVA F2880, called
// from slot 3 before plcmdFire). Only wheel pulses drive them; otherwise the
// native keyboard binding passes through.
constexpr int FirstPassThroughCommand = 16;
static uint32_t ids[CommandCount]{};
static const char *commands[] = {"plcmdX+",   "plcmdX-",        "plcmdZ+",  "plcmdZ-",     "plcmdY+",
                                 "plcmdFire", "plcmdAltFire",   "plcmdUse", "plcmdSprint", "plcmdH+",
                                 "plcmdH-",   "plcmdMouseLook", "plcmdP+",  "plcmdP-",     "plcmdReloadWeapon",
                                 "plcmdDisplayNetricsa", "plccmdQuickSave", "plccmdQuickLoad", "plcmdTertFire"};
static_assert(sizeof(commands) / sizeof(commands[0]) == CommandCount);
static void *playerBindings = nullptr;
static float values[CommandCount]{}, priorValues[CommandCount]{};
// A dead local player has no tracked gameplay, but native respawn/reload prompts
// still wait for Fire/Use. Forward only those presses while dead (main thread).
static float respawnValues[CommandCount]{}, respawnPrior[CommandCount]{};
static uint64_t respawnTick = 0;
// Provenance of the existing cached command values, not an admission policy.
struct ControlSample {
    RiderIdentity rider;
    uint32_t generation = 0, intentEpoch[2]{};
    Input input;
    Pose origin;
    float turn = 0;
    bool client = false, enabled = false;
    uint32_t producer=0;
    uint64_t rigRevision=0;
    uint32_t gripPoseGeneration[2]{};
};
static ControlSample sampledControls;
static SRWLOCK controlsLock = SRWLOCK_INIT;
static RideGripFrames rideGripFrames;
static RideGrasp rideGrasp;
// These are this mod's own x86 metadata fields inspected by the finite cleanup
// verifier. Changing layout requires a new compiled-byte proof, never guessing.
static_assert(offsetof(RideGrasp,mask)==0x134 && offsetof(RideGrasp,keyed)==0x150);
struct RideGripBorrow {
    const Snapshot *snapshot=nullptr;
    const ControlSample *controls=nullptr;
    const RideGripFrame *frame=nullptr;
    void *brain=nullptr;
    uint32_t brainHandle=0,continuity=0;
    bool eligible=false,headingIntent=false;
};
static thread_local RideGripBorrow *activeRideGrip=nullptr;
static uint32_t handleFrameRide=0;
static uint64_t handleFrameTick=0;
void recordRideGripFrame(const RideGripFrame &frame) {
    AcquireSRWLockExclusive(&controlsLock);
    rideGripFrames.add(frame);
    if(frame.valid) {handleFrameRide=frame.rig.rider.ride;handleFrameTick=GetTickCount64();}
    ReleaseSRWLockExclusive(&controlsLock);
    static unsigned traced=0;
    static uint64_t lastTrace=0;
    if(frame.valid && traced<30 && GetTickCount64()-lastTrace>2000) {
        ++traced;lastTrace=GetTickCount64();
        // Each handle's centre in the seated anchor's frame (metres; x right, y up, -z ahead).
        Vec3 c[2]{};
        for(unsigned h=0;h<2;++h) {
            const auto &sf=frame.surfaces[h];
            for(unsigned v=0;v<sf.vertices;++v)c[h]=c[h]+sf.positions[v];
            if(sf.vertices)c[h]=c[h]*(1.f/float(sf.vertices));
        }
        log("Ride grip frame profile=%u handles anchor-relative left=%.3f,%.3f,%.3f right=%.3f,%.3f,%.3f",unsigned(frame.profile),
            c[0].x,c[0].y,c[0].z,c[1].x,c[1].y,c[1].z);
    }
}
using LookClamp = void(__thiscall *)(void *, Vec3 &);
using QuaternionEuler = Vec3 *(__cdecl *)(Vec3 *, const Quat &);
static LookClamp originalLookClamp = nullptr,originalRideLookClamp=nullptr;
static thread_local RideControlObservation *activeRideControlObservation=nullptr;
static std::atomic<unsigned> labRideControlRows=0;
using RideModelGetter=void *(__thiscall *)(void *);
static RideModelGetter originalRideModelInstance=nullptr,originalRideRenderable=nullptr;
static uintptr_t rideModelRenderableReturn=0;
static thread_local RideModelJoin *activeRideModelJoin=nullptr;
static std::atomic<unsigned> labRideModelInvocations=0;
static bool borrowedRideModelScalars(void *ride,uint32_t &classRva,uint32_t &handle) {
    classRva=handle=0;
    if(!readableMemory(ride,0x124))return false;
    uint32_t table=0;std::memcpy(&table,ride,4);
    if(table<controlsGameBase || (table-controlsGameBase!=0x2a8558 && table-controlsGameBase!=0x2b8420))return false;
    classRva=uint32_t(table-controlsGameBase);
    std::memcpy(&handle,static_cast<uint8_t*>(ride)+0x120,4);
    return true;
}
static void *__fastcall observedRideRenderable(void *ride,void *) {
#ifdef _MSC_VER
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    auto *join=activeRideModelJoin;
    if(!join)return originalRideRenderable(ride);
    const bool entered=join->enter(reinterpret_cast<uintptr_t>(ride),caller,GetCurrentThreadId());
    void *result=nullptr;
    withNativeFinally([&] {
        result=originalRideRenderable(ride);
        if(entered)join->copyRenderable(reinterpret_cast<uintptr_t>(result));
    },[&](bool aborted) noexcept {
        if(entered)join->finishInner(aborted);
        else if(aborted) {join->aborted=true;join->invalidate();}
    });
    return result; // Preserve the native EAX pointer, including null/rejected calls.
}
static void *__fastcall observedRideModelInstance(void *ride,void *) {
    auto *previous=activeRideModelJoin;
    if(previous)previous->invalidate();
    uint32_t classRva=0,handle=0;
    if(!labRideControlConfigured || previous || !nativePresentationThreadCurrent() ||
       !borrowedRideModelScalars(ride,classRva,handle))return originalRideModelInstance(ride);
    auto invocation=labRideModelInvocations.load(std::memory_order_relaxed);
    while(invocation<64 && !labRideModelInvocations.compare_exchange_weak(invocation,invocation+1,std::memory_order_relaxed)) {}
    if(invocation>=64)return originalRideModelInstance(ride);
    RideModelJoin join;
    join.receiver=reinterpret_cast<uintptr_t>(ride);join.expectedCaller=rideModelRenderableReturn;
    join.thread=GetCurrentThreadId();join.classRva=classRva;join.handle=handle;join.invocation=invocation+1;
    void *result=nullptr;
    withNativeFinally([&] {
        activeRideModelJoin=&join;
        result=originalRideModelInstance(ride); // This body already resolves renderable and tail-calls its instance getter.
        uint32_t liveClass=0,liveHandle=0;
        if(borrowedRideModelScalars(ride,liveClass,liveHandle))
            join.copyInstance(reinterpret_cast<uintptr_t>(result),liveClass,liveHandle);
        else join.invalidate();
    },[&](bool aborted) noexcept {
        activeRideModelJoin=previous;
        join.finishOuter(aborted);
    });
    if(join.publishable())
        log("Lab rideModelJoin schema=1 source=%.*s invocation=%u receiver=%u class=%u handle=%u thread=%u renderableToken=%u instanceToken=%u innerCalls=%u innerReturned=1 borrowedJoin=1 localRiderAssociated=0 operatedSeatAssociated=0 resourceAssociated=0 frameAssociated=0 steeringApplied=0",
            64,ss2vrBuildContract.sourceFingerprint.data(),join.invocation,unsigned(join.receiver),join.classRva,
            join.handle,join.thread,unsigned(join.renderable),unsigned(join.instance),join.calls);
    return result;
}
static void applyRideGrip(void *,Vec3 &,const RideControlScalars &);
static void __fastcall observedRideLookClamp(void *ride,void *,Vec3 &look) {
    auto *observation=activeRideControlObservation;
    if(!observation) {originalRideLookClamp(ride,look);return;}
    const bool entered=observation->enter(reinterpret_cast<uintptr_t>(ride),
        reinterpret_cast<uintptr_t>(&look),GetCurrentThreadId());
    withNativeFinally([&] {
        if(entered) {
            RideControlScalars row{};
            // Only this original callback's borrowed receiver may provide bytes.
            // The prior resolve token is used solely for equality above.
            if(readableMemory(ride,0x4d0)) {
                uint32_t table=0;std::memcpy(&table,ride,4);
                if(table>=controlsGameBase && (table-controlsGameBase==0x2a8558 ||
                                               table-controlsGameBase==0x2b8420)) {
                    row.classRva=uint32_t(table-controlsGameBase);
                    const auto *bytes=static_cast<const uint8_t*>(ride);
                    std::memcpy(&row.mode,bytes+0x4c4,4);
                    std::memcpy(&row.executionAbilities,bytes+0x2d8,4);
                    std::memcpy(&row.movementAbilities,bytes+0x4cc,4);
                    std::memcpy(&row.parameterToken,bytes+0x47c,4);
                    std::memcpy(&row.renderableToken,bytes+0x120,4);
                }
            }
            observation->copy(row);
            if(observation->copied && !observation->declined)applyRideGrip(ride,look,row);
        }
        originalRideLookClamp(ride,look); // Exactly one original call, even for rejected sampling.
    },[&](bool aborted) noexcept {
        if(entered)observation->finishCallback(aborted);
        else if(aborted)observation->finishOuter(true);
    });
}
static QuaternionEuler quaternionEuler = nullptr;
using PlayerControls = void(__thiscall *)(void *, uint8_t, Vec3, Vec3);
using OperatorMoveDir = Vec3 *(__thiscall *)(void *, Vec3 *, Vec3, Vec3);
static PlayerControls originalPlayerControls = nullptr;
static OperatorMoveDir nativeOperatorMoveDir = nullptr;
static uintptr_t playerControlsReturn = 0, swimmingPlayerVtable = 0;
static uintptr_t inactivePlayerControlsReturn = 0;
static uintptr_t controlsExeBase = 0;
static SwimmingStrokes swimmingStrokes;
static bool currentControls(int, const Snapshot &, const ControlSample &, bool);

// A bounded local producer experiment, not a native exclusivity override.
// Disabled in ordinary installations until source/runtime acceptance is complete.
static bool labBackgroundMovement() {
    static const bool enabled=[] { wchar_t v[2]{};return GetEnvironmentVariableW(L"SS2VR_LAB_BACKGROUND_MOVE",v,2)==1 && v[0]==L'1'; }();
    return enabled && labDualTrace();
}
static bool nativeInactiveInputReady() {
    auto read=[](uintptr_t base, unsigned offset, uint32_t &value) {
        if (!base || base > UINTPTR_MAX-offset || !readableMemory(reinterpret_cast<void *>(base+offset),4)) return false;
        memcpy(&value,reinterpret_cast<void *>(base+offset),4);return true;
    };
    uint32_t instance=0,sam=0,exe=0,table=0,enabled=0,running=0,sim=0,
             dispatch=0,q=0,blocked=1,worldBlocked=1,registeredSim=0,exclusive=1;
    return read(roomscaleEngineBase,0x2f19b0,instance) && instance &&
        read(controlsGameBase,0x403350,sam) && sam==instance &&
        read(controlsExeBase,0x849c,exe) && exe==instance &&
        read(instance,0,table) && table==controlsGameBase+0x29cf10 &&
        read(instance,0x10,enabled) && enabled==1 && read(instance,0x0c,running) && running==1 &&
        read(instance,0x24,dispatch) && dispatch==1 && read(instance,0x08,sim) && sim &&
        read(instance,0x30,q) && read(q,0xfc,blocked) && blocked==0 &&
        read(roomscaleEngineBase,0x2f1a70,registeredSim) && registeredSim==sim &&
        read(sim,0x48,worldBlocked) && worldBlocked==0 &&
        read(roomscaleEngineBase,0x2e9fd4,exclusive) && exclusive==0;
}
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static bool inactiveJoystick(void *brain, uint8_t fire, Vec3 look, Vec3 nativeMove, Vec3 &out) {
    // Preserve the audited native inactive call, including native look/zero
    // fire; unknown original arguments cannot be silently replaced.
    if (!labBackgroundMovement() || fire || nativeMove.x || nativeMove.y || nativeMove.z ||
        !std::isfinite(look.x) || !std::isfinite(look.y) || !std::isfinite(look.z) ||
        !nativeInputHealthy() || !nativeInactiveInputReady()) return false;
    const auto s=copySnapshot();const auto now=GetTickCount64();
    if (!vrSession(s) || !livePlayer(s) || !trackingEligible(s.player) || !fresh(s.input) ||
        !s.rider.handheld() || recenterHeld(s.input) || !s.ui.gameplay || s.ui.tickMs>now ||
        now-s.ui.tickMs>=200 || s.ui.wheel[0].open || s.ui.wheel[1].open ||
        !validNativeBodyPose(s.origin) || !std::isfinite(s.turn)) return false;
    const auto brainHandle=pointerHandle(brain);
    if (!brainHandle || resolve(brainHandle)!=brain ||
        !readableMemory(static_cast<uint8_t *>(brain)+0x28,4) ||
        !readableMemory(static_cast<uint8_t *>(s.player)+0x38c,4) ||
        primaryField(brain,0x28)!=s.playerHandle || primaryField(s.player,0x38c)!=brainHandle)
        return false;
    // This first proof is walking only. Existing swimming/mounted producers
    // keep their current native look/turn and all other control behavior.
    uint32_t table=0;memcpy(&table,s.player,4);
    const auto flags=primaryField(s.player,0x4cc);
    if (table!=swimmingPlayerVtable || (flags&2u) || nativeWaterInputMode(flags,
                                                        primaryField(s.player,0x610))) return false;
    const ControlSample captured{s.rider,s.generation,{s.intentEpoch[0],s.intentEpoch[1]},
                                s.input,s.origin,s.turn,multiplayer::remoteClient(),true};
    const auto direction=horizontalStickMovement(s.input,false,moveHeading(s));
    const auto latest=copySnapshot();
    if (!vrSession(latest) || !fresh(latest.input) || !latest.ui.gameplay ||
        latest.ui.wheel[0].open || latest.ui.wheel[1].open ||
        !currentControls(0,latest,captured,true) || !nativeInactiveInputReady() ||
        resolve(brainHandle)!=brain || primaryField(brain,0x28)!=s.playerHandle ||
        primaryField(s.player,0x38c)!=brainHandle) return false;
    // Handle lookups above can wait on the native resource lock. Recheck time
    // and the copied admission after the last native getter, not before it.
    const auto submit=copySnapshot();const auto submitNow=GetTickCount64();
    if (!nativeInputHealthy() || !vrSession(submit) || !fresh(submit.input) ||
        submit.player!=s.player || submit.playerHandle!=s.playerHandle || submit.rider!=s.rider ||
        submit.generation!=s.generation || submit.rigRevision!=s.rigRevision ||
        submit.input.session!=s.input.session || submit.input.reference!=s.input.reference ||
        submit.input.sequence!=s.input.sequence || submit.turn!=s.turn ||
        std::memcmp(&submit.origin,&s.origin,sizeof(Pose)) || !submit.ui.gameplay ||
        submit.ui.tickMs>submitNow || submitNow-submit.ui.tickMs>=200 ||
        submit.ui.wheel[0].open || submit.ui.wheel[1].open || recenterHeld(submit.input)) return false;
    out=direction;return true;
}

// Private-lab observation only. Neither caller admission nor native arguments
// change. In particular, observing the inactive route does not enable device
// polling, desktop cursor movement, fire or an alternate movement producer.
struct LabControlObservation {
    bool enabled=false,binding=false,bodyValid=false,saturated=false;
    unsigned ordinal=0;
    uint32_t brain=0,player=0,mechanism=0,root=0,session=0,reference=0,generation=0,focused=0,headValid=0;
    uint64_t tick=0,sequence=0,rig=0;
    Pose origin{},body{};
    float turn=0;
};
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void collectLocalControls(void *brain, uintptr_t caller, LabControlObservation &observation) {
    if (!labDualTrace() || !hooksReady.load(std::memory_order_acquire) ||
        (caller != playerControlsReturn && caller != inactivePlayerControlsReturn) ||
        !nativeMainThread || !nativeMainThread()) return;
    const auto s = copySnapshot();
    if (!s.initialized || !s.ui.gameplay || !s.player || !s.playerHandle) return;
    static unsigned ordinary = 0, inactive = 0;
    auto &count = caller == playerControlsReturn ? ordinary : inactive;
    if (count >= 120) {
        if(count==120){++count;observation.saturated=true;}
        return;
    }
    ++count;
    // Capture the bidirectional binding before the original native call; no
    // borrowed receiver is accessed after that call or retained by this trace.
    const auto brainHandle = pointerHandle(brain);
    uint32_t playerHandle = 0, ownedBrain = 0;
    const bool recognized = brainHandle && resolve(brainHandle) == brain &&
        resolve(s.playerHandle) == s.player &&
        readableMemory(static_cast<uint8_t *>(brain) + 0x28, 4) &&
        readableMemory(static_cast<uint8_t *>(s.player) + 0x38c, 4);
    if (recognized) {
        memcpy(&playerHandle, static_cast<uint8_t *>(brain) + 0x28, 4);
        memcpy(&ownedBrain, static_cast<uint8_t *>(s.player) + 0x38c, 4);
    }
    Pose body{};bool bodyValid=false;uint32_t mechanism=0,root=0;
    if (recognized && playerHandle==s.playerHandle && ownedBrain==brainHandle &&
        nativeBodyPlacement && nativeRiderCurrent(s.player,s.rider) &&
        readableMemory(static_cast<uint8_t *>(s.player)+0x114,0x10)) {
        mechanism=primaryField(s.player,0x114);
        void *source=mechanism?resolve(mechanism):nullptr;
        if (source && readableMemory(static_cast<uint8_t *>(source)+0x38,4)) root=primaryField(source,0x38);
        const bool hasSource=source && root && resolve(root);
        if (hasSource && nativeBodyPlacement(s.player,&body)==&body && validNativeBodyPose(body) &&
            resolve(brainHandle)==brain && nativeRiderCurrent(s.player,s.rider) &&
            primaryField(brain,0x28)==s.playerHandle && primaryField(s.player,0x38c)==brainHandle &&
            primaryField(s.player,0x114)==mechanism && resolve(mechanism)==source &&
            primaryField(source,0x38)==root && resolve(root))
            bodyValid=true;
    }
    observation={true,recognized && playerHandle==s.playerHandle && ownedBrain==brainHandle,
        bodyValid,false,count,brainHandle,s.playerHandle,mechanism,root,s.input.session,s.input.reference,
        s.generation,s.input.focused,s.input.headValid,GetTickCount64(),s.input.sequence,s.rigRevision,
        s.origin,body,s.turn};
}
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void logLocalControls(const LabControlObservation &o, uintptr_t caller, uint8_t fire, Vec3 look, Vec3 move) {
    // Called only after normal original completion. All native observations
    // precede movement admission; only copied scalar/value fields remain here.
    if(o.saturated)log("Lab controls observation saturated route=%s",caller==playerControlsReturn?"ordinary":"inactive");
    if(!o.enabled)return;
    log("Lab controls observation route=%s ordinal=%u brain=%u player=%u mechanism=%u root=%u binding=%u tick=%llu sequence=%llu session=%llu reference=%llu generation=%u focused=%u headValid=%u rig=%llu origin=%.9g,%.9g,%.9g turn=%.9g fire=%u look=%.9g,%.9g,%.9g move=%.9g,%.9g,%.9g bodyValid=%u body=%.9g,%.9g,%.9g completed=1",
        caller==playerControlsReturn?"ordinary":"inactive",o.ordinal,o.brain,o.player,o.mechanism,o.root,o.binding,
        static_cast<unsigned long long>(o.tick),static_cast<unsigned long long>(o.sequence),
        static_cast<unsigned long long>(o.session),static_cast<unsigned long long>(o.reference),o.generation,
        o.focused,o.headValid,static_cast<unsigned long long>(o.rig),o.origin.p.x,o.origin.p.y,o.origin.p.z,o.turn,
        fire,look.x,look.y,look.z,move.x,move.y,move.z,o.bodyValid,o.body.p.x,o.body.p.y,o.body.p.z);
}

static bool currentControls(int index, const Snapshot &snapshot, const ControlSample &captured,
                            bool active = true) {
    if (index < 0)
        return true;
    if (!captured.enabled || !compatibleRiderInput(captured.rider, snapshot.rider,
        captured.input, snapshot.input, captured.generation, snapshot.generation, GetTickCount64()) ||
        recenterHeld(captured.input) || recenterHeld(snapshot.input) ||
        !nativeRiderCurrent(snapshot.player, captured.rider))
        return false;
    if (index != 5 && index != 6)
        return true;
    unsigned hand = index == 5 ? 1u : 0u;
    // Coupled dual weapons: the left trigger also drives native Fire (see poll).
    if (index == 5 && active && !snapshot.fire[1] && snapshot.fire[0] && captured.rider.handheld() &&
        coupledNativeFire.load(std::memory_order_relaxed))
        hand = 0;
    // fire[] is native-slot indexed; the trigger is on the controller holding that slot's weapon.
    const unsigned controller = captured.rider.handheld() ? mountHand(snapshot, hand) : hand;
    if ((active && !snapshot.fire[hand]) || !primaryActionEligible(captured.input, controller) ||
        !primaryActionEligible(snapshot.input, controller) || !trackedHandCurrent(captured.input, snapshot.input, controller) ||
        captured.input.primaryInputGeneration[controller] != snapshot.input.primaryInputGeneration[controller])
        return false;
    if (captured.rider.seated())
        return true; // Native vehicle operator fire is outside handheld ACK admission.
    if (!captured.rider.handheld())
        return false;
    if (!captured.client && !multiplayer::remoteClient())
        return true;
    return captured.input.sequence == snapshot.input.sequence &&
        multiplayer::localPrimaryAllowed(snapshot.player, hand, captured.intentEpoch[hand],
            captured.input.primaryInputGeneration[hand], captured.input.sequence, active);
}
static void rideGripFail(unsigned reason) {
    static uint32_t seen=0;
    static unsigned counts[32]{},reports=0;
    static uint64_t reported=0;
    if(reason<32) ++counts[reason];
    if(reason<32 && !(seen&(1u<<reason))) {seen|=1u<<reason;log("Ride grip declined reason=%u",reason);}
    const auto now=GetTickCount64();
    if(now-reported>=3000 && reports<40) {
        reported=now;++reports;
        log("Ride grip declines r0=%u r1=%u r2..16=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",counts[0],counts[1],counts[2],counts[3],
            counts[4],counts[5],counts[6],counts[7],counts[8],counts[9],counts[10],counts[11],counts[12],counts[13],counts[14],
            counts[15],counts[16]);
        std::fill(std::begin(counts),std::end(counts),0u);
    }
}
// While a squeeze is held without a grasp: each hand's admission and its
// body-space distance to the nearest handle surface (lab diagnosis).
static void rideGripDiagnose(const RideGripFrame &frame,const Input &input,const uint32_t poseGeneration[2],uint64_t now) {
    static uint64_t traced=0;static unsigned count=0;
    if(!((input.buttons[0]|input.buttons[1])&Wheel) || now-traced<1000 || count>=40)return;
    traced=now;++count;
    for(unsigned h=0;h<2;++h) {
        const auto grip=bodyHandTracking(frame.rig.origin,frame.rig.turn,input.head,input.grip[h]);
        double nearest=INFINITY;Vec3 centre{};
        for(unsigned handle=0;handle<2;++handle) {
            const auto &surface=frame.surfaces[handle];
            if(!surface.vertices||surface.vertices>35)continue;
            if(handle==h) {for(unsigned v=0;v<surface.vertices;++v)centre=centre+surface.positions[v];centre=centre*(1.f/surface.vertices);}
            for(unsigned i=0;i<90;i+=3) {
                const auto a=surface.indices[i],b=surface.indices[i+1],c=surface.indices[i+2];
                if(a>=surface.vertices||b>=surface.vertices||c>=surface.vertices)break;
                nearest=std::min(nearest,ridePointTriangle(grip.p,surface.positions[a],surface.positions[b],surface.positions[c]));
            }
        }
        const auto &hand=rideGrasp.hands[h];
        if(!h)log("Ride grasp rekeys=%u mismatch=0x%x continuity=%u",rideGrasp.rekeys,rideGrasp.mismatch,rideGrasp.continuity);
        log("Ride grasp hand=%u down=%u pose=%u eligible=%u admitted=%u epoch=%u armed=%u held=%u handle=%d grip=%.3f,%.3f,%.3f handle%u-centre=%.3f,%.3f,%.3f nearest=%.3f",
            h,(input.buttons[h]&Wheel)?1u:0u,poseGeneration[h],rideGripPoseEligible(input,h)?1u:0u,
            (input.wheelAdmissionMask>>h)&1u,input.wheelInputEpoch[h],hand.armed?1u:0u,hand.held?1u:0u,hand.handle,
            grip.p.x,grip.p.y,grip.p.z,h,centre.x,centre.y,centre.z,std::sqrt(nearest));
    }
}
static void applyRideGrip(void *ride,Vec3 &look,const RideControlScalars &row) {
    auto *borrow=activeRideGrip;
    if(!borrow || !borrow->eligible) {rideGripFail(borrow?1:0);return;}
    const auto *observation=activeRideControlObservation;
    const auto &s=*borrow->snapshot;
    const auto &c=*borrow->controls;
    const auto &frame=*borrow->frame;
    const auto now=GetTickCount64();
    auto fail=[&](unsigned reason) {rideGripFail(reason);borrow->eligible=false;rideGrasp.cancel(c.input,c.producer);};
    if(!settings.immersiveVehicleGrips || row.mode!=2 || !(row.movementAbilities&8u)) {fail(2);return;}
    if(row.classRva!=frame.render.classRva || row.parameterToken!=frame.parameter ||
       row.renderableToken!=frame.render.renderableHandle) {fail(3);return;}
    if(!nativeMainThread || !nativeMainThread() || !vrSession(s) || !livePlayer(s)) {fail(4);return;}
    if(!s.ui.gameplay || s.ui.wheel[0].open || s.ui.wheel[1].open) {fail(5);return;}
    if(frame.render.brain!=borrow->brainHandle) {fail(6);return;}
    if(c.producer!=s.inputProducer || c.rigRevision!=s.rigRevision || !currentControls(0,s,c)) {fail(7);return;}
    if(resolve(c.rider.ride)!=ride || resolve(borrow->brainHandle)!=borrow->brain ||
       primaryField(s.player,0x38c)!=borrow->brainHandle || primaryField(borrow->brain,0x28)!=s.playerHandle ||
       primaryField(ride,0x38c)!=borrow->brainHandle) {fail(8);return;}
    uint32_t handle=0;void *renderable=nullptr;
    if(!laserRead32(ride,0x120,handle) || handle!=frame.render.renderableHandle ||
       !(renderable=resolve(handle)) || !readableMemory(renderable,0x60) ||
       !rideRenderableTable(primaryField(renderable,0)) ||
       primaryField(renderable,0x48)!=reinterpret_cast<uintptr_t>(ride) ||
       primaryField(renderable,0x5c)!=frame.render.instance) {fail(9);return;}
    IdleConfigIdentity config;Vec3 stretch;
    void *instance=laserModelInstance(renderable);
    if(instance!=reinterpret_cast<void*>(frame.render.instance) ||
       !remote_render::copyModelConfigurationStretch(instance,laserEngineBase+0x2095b4,config,stretch) ||
       config.configuration!=frame.configuration || config.file!=frame.file || uint32_t(config.resource)!=frame.resource ||
       std::memcmp(&stretch,frame.stretch.data(),12)) {fail(10);return;}
    const RideGripRig rig{s.rider,s.inputProducer,s.generation,s.rigRevision,graphicsResourceGeneration(),
        {},c.origin,c.turn};
    const auto latest=copySnapshot();
    if(!rideGripFrameCurrent(frame,rig,c.input,now)) {fail(11);return;}
    if(!currentControls(0,latest,c)) {fail(12);return;}
    if(!submittedWorldMatches(latest.submittedWorld,c.producer,frame.request,GetTickCount64()) ||
       latest.submittedWorld.continuity!=borrow->continuity) {fail(13);return;}
    if(!latest.ui.gameplay || latest.ui.wheel[0].open || latest.ui.wheel[1].open ||
       std::memcmp(&latest.origin,&c.origin,sizeof(Pose)) || latest.turn!=c.turn ||
       !submittedWorldMatches(latest.submittedWorld,c.producer,frame.request,GetTickCount64()) ||
       latest.submittedWorld.continuity!=borrow->continuity ||
       latest.rigRevision!=c.rigRevision || latest.inputProducer!=c.producer ||
       resolve(borrow->brainHandle)!=borrow->brain ||
       primaryField(latest.player,0x38c)!=borrow->brainHandle ||
       primaryField(borrow->brain,0x28)!=latest.playerHandle ||
       resolve(c.rider.ride)!=ride || resolve(handle)!=renderable ||
       primaryField(ride,0)!=controlsGameBase+row.classRva ||
       primaryField(ride,0x38c)!=borrow->brainHandle || primaryField(ride,0x120)!=handle ||
       primaryField(ride,0x47c)!=row.parameterToken || primaryField(ride,0x4c4)!=row.mode ||
       primaryField(ride,0x2d8)!=row.executionAbilities || primaryField(ride,0x4cc)!=row.movementAbilities ||
       primaryField(renderable,0x48)!=reinterpret_cast<uintptr_t>(ride) ||
       !rideRenderableTable(primaryField(renderable,0)) ||
       primaryField(renderable,0x5c)!=frame.render.instance ||
       primaryField(instance,0x18)!=frame.configuration ||
       primaryField(reinterpret_cast<void*>(frame.configuration),0xc)!=frame.file ||
       primaryField(reinterpret_cast<void*>(frame.configuration),0x10)!=frame.resource ||
       std::memcmp(instance,frame.stretch.data(),12) || !nativeInputHealthy()) {fail(14);return;}
    const auto stillAdmitted=[&] {
        return activeRideGrip==borrow && borrow->eligible && observation &&
            activeRideControlObservation==observation &&
            activeRideControlObservation->rideToken==reinterpret_cast<uintptr_t>(ride) &&
            activeRideControlObservation->lookToken==reinterpret_cast<uintptr_t>(&look) &&
            activeRideControlObservation->copied && !activeRideControlObservation->declined &&
            !activeRideControlObservation->aborted && activeRideControlObservation->callbackBusy;
    };
    if(!stillAdmitted()) {fail(15);return;}
    uint32_t poseGeneration[2]{};
    for(unsigned h=0;h<2;++h)
        if(c.gripPoseGeneration[h]==latest.gripPoseGeneration[h] &&
           c.input.wheelInputEpoch[h]==latest.input.wheelInputEpoch[h] &&
           (latest.input.wheelAdmissionMask&(1u<<h)) && latest.input.gripValid[h])
            poseGeneration[h]=c.gripPoseGeneration[h];
    float delta=0,base=0;
    if(rideGrasp.sample(frame,c.input,poseGeneration,borrow->continuity,true,look.x,delta,base)) {
        // A control stick: the hands' turn from where they took hold sets a turn
        // rate (dead zone, full rate at 0.4 rad); the heading builds from the
        // grasp's base. GripTurnRateDegrees 0 keeps handlebars 1:1.
        static float travel=0;static uint32_t heldMask=0;static uint64_t clock=0;static float heldBase=0;
        if(rideGrasp.mask!=heldMask || base!=heldBase) {travel=0;heldMask=rideGrasp.mask;heldBase=base;clock=now;}
        if(settings.gripTurnRateDegrees>0) {
            const float dt=now>clock ? std::min(.1f,float(now-clock)/1000.f) : 0.f;
            clock=now;
            const float deflection=std::clamp((std::abs(delta)-.05f)/.35f,0.f,1.f);
            travel+=std::copysign(deflection*deflection,delta)*settings.gripTurnRateDegrees*Pi/180*dt;
            travel=std::remainder(travel,2*Pi);
            delta=travel;
        }
        if(commitRideHeading(look.x,base,delta,[&](float radians,float &heading) {
            Vec3 converted;
            if(quaternionEuler(&converted,yaw(radians))!=&converted)return false;
            heading=converted.x;return true;
        },stillAdmitted)) {
            borrow->headingIntent=true;
            static uint64_t traced=0;
            if(now-traced>=1000) {traced=now;log("Ride grasp steering mask=%u delta=%.3f base=%.3f heading=%.3f",rideGrasp.mask,delta,base,look.x);}
        } else fail(16);
    } else rideGripDiagnose(frame,c.input,poseGeneration,now);
}
// Adapt only the local native control producer. The native ClientAction RPC
// transports the resulting full Vec3 unchanged, including on stock servers.
// Do not hook the authority's movement consumer or alter its physics state.
static void __fastcall waterPlayerControls(void *brain, void *, uint8_t fire, Vec3 look, Vec3 move) {
#ifdef _MSC_VER
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    LabControlObservation observation;
    collectLocalControls(brain,caller,observation);
    {
        // Which native producer ran (ordinary polled bindings, or the inactive
        // route that passes zero fire/move) and the fire bits it submitted.
        static unsigned traced = 0, previous = ~0u;
        const unsigned route = caller == playerControlsReturn ? 1u : caller == inactivePlayerControlsReturn ? 2u : 3u;
        const unsigned state = route | unsigned(fire) << 2;
        if (state != previous && traced < 160) {
            previous = state;
            ++traced;
            log("Input native controls route=%s fire=%u move=%.2f,%.2f,%.2f", route == 1 ? "ordinary" :
                route == 2 ? "inactive" : "other", fire, move.x, move.y, move.z);
        }
    }
    // Inactive native submissions are kept separate from ordinary polling and
    // swimming history. No cached command values are borrowed for this route.
    if (hooksReady.load(std::memory_order_acquire) && caller==inactivePlayerControlsReturn &&
        nativeMainThread && nativeMainThread()) {
        Vec3 adapted{};
        if (inactiveJoystick(brain,fire,look,move,adapted)) move=adapted;
        // Continue to the existing native forwarding path below. This route
        // does not enter or reset the ordinary swimming sample history.
    }
    // This state belongs solely to the audited main-thread input producer.
    // Other callers pass through without reading or resetting stroke history.
    if (!hooksReady.load(std::memory_order_acquire) || caller != playerControlsReturn ||
        !nativeMainThread || !nativeMainThread()) {
        originalPlayerControls(brain, fire, look, move);
        logLocalControls(observation,caller,fire,look,move);
        return;
    }
    ControlSample captured;
    AcquireSRWLockShared(&controlsLock);
    captured = sampledControls;
    ReleaseSRWLockShared(&controlsLock);
    const auto snapshot = copySnapshot();
    const auto waterState = [&](uint32_t &flags, uint32_t &pose) {
        if (!snapshot.player || !nativeRiderCurrent(snapshot.player, captured.rider)) return false;
        uintptr_t table = 0;
        memcpy(&table, snapshot.player, sizeof(table));
        if (table != swimmingPlayerVtable) return false;
        memcpy(&flags, static_cast<uint8_t *>(snapshot.player) + 0x4cc, 4);
        memcpy(&pose, static_cast<uint8_t *>(snapshot.player) + 0x610, 4);
        return nativeWaterInputMode(flags, pose);
    };
    const bool finiteControls = std::isfinite(look.x) && std::isfinite(look.y) && std::isfinite(look.z) &&
        std::isfinite(move.x) && std::isfinite(move.y) && std::isfinite(move.z) &&
        std::abs(move.x) <= 2.f && std::abs(move.y) <= 2.f && std::abs(move.z) <= 2.f;
    bool waterAdapted = false;
    uint32_t flags = 0, pose = 0;
    if (finiteControls && hooksReady.load(std::memory_order_acquire) && caller == playerControlsReturn &&
        nativeMainThread && nativeMainThread() && captured.rider.handheld() &&
        nativeOperatorMoveDir && quaternionEuler && livePlayer(snapshot) && snapshot.ui.gameplay &&
        snapshot.ui.tickMs <= GetTickCount64() && GetTickCount64() - snapshot.ui.tickMs < 200 &&
        currentControls(0, snapshot, captured) &&
        validNativeBodyPose(captured.input.head) && validNativeBodyPose(captured.origin) &&
        std::isfinite(captured.turn) && waterState(flags, pose)) {
        const auto brainHandle = pointerHandle(brain);
        uint32_t playerHandle = 0, ownedBrain = 0;
        if (brainHandle && resolve(brainHandle) == brain) {
            memcpy(&playerHandle, static_cast<uint8_t *>(brain) + 0x28, 4);
            memcpy(&ownedBrain, static_cast<uint8_t *>(snapshot.player) + 0x38c, 4);
        }
        Pose anchor;
        if (playerHandle == captured.rider.player && ownedBrain == brainHandle &&
            nativeTrackingAnchor(snapshot.player, anchor, &captured.rider)) {
            const Pose head = worldHeadTracking(anchor, captured.origin, captured.turn, captured.input.head);
            Vec3 headLook;
            if (validNativeBodyPose(head) && quaternionEuler(&headLook, head.q) == &headLook &&
                std::isfinite(headLook.x) && std::isfinite(headLook.y)) {
                headLook.z = 0; // Head roll must not turn a lateral stroke into climbing.
                // Poll already applied snap/smooth turn to the control vector.
                // Remove it once; worldHeadTracking includes that same turn.
                Vec3 local = rotate(yaw(-moveHeading(captured.input, captured.origin, captured.turn)), move);
                const bool joystickActive = !swimmingJoystickIdle(local);
                const bool currentHands = snapshot.input.gripValid[0] && snapshot.input.gripValid[1] &&
                    finite(snapshot.input.grip[0]) && finite(snapshot.input.grip[1]) &&
                    swimmingGripStreamsMatch(captured.gripPoseGeneration, snapshot.gripPoseGeneration);
                const float stroke = swimmingStrokes.sample(captured.input, captured.gripPoseGeneration, captured.rider.player,
                    captured.generation, pose, GetTickCount64(), settings.immersiveSwimming &&
                    !joystickActive && currentHands && !snapshot.ui.wheel[0].open && !snapshot.ui.wheel[1].open);
                local.z -= stroke;
                std::array<Vec3, 3> basis{};
                Vec3 desired{}, mapped{};
                const bool returned =
                    nativeOperatorMoveDir(snapshot.player, &basis[0], look, {1,0,0}) == &basis[0] &&
                    nativeOperatorMoveDir(snapshot.player, &basis[1], look, {0,1,0}) == &basis[1] &&
                    nativeOperatorMoveDir(snapshot.player, &basis[2], look, {0,0,1}) == &basis[2] &&
                    nativeOperatorMoveDir(snapshot.player, &desired, headLook, local) == &desired;
                uint32_t finalFlags = 0, finalPose = 0;
                const auto latest = copySnapshot();
                const bool strokeStillValid = stroke == 0 ||
                    (latest.input.gripValid[0] && latest.input.gripValid[1] &&
                     finite(latest.input.grip[0]) && finite(latest.input.grip[1]) &&
                     swimmingGripStreamsMatch(captured.gripPoseGeneration, latest.gripPoseGeneration) &&
                     !latest.ui.wheel[0].open && !latest.ui.wheel[1].open);
                if (livePlayer(latest) && latest.ui.gameplay && strokeStillValid && returned && swimmingInputInBasis(basis, desired, mapped) &&
                    waterState(finalFlags, finalPose) && finalFlags == flags && finalPose == pose &&
                    resolve(brainHandle) == brain && currentControls(0, latest, captured)) {
                    move = mapped;
                    waterAdapted = true;
                }
            }
        }
    }
    if (!waterAdapted) swimmingStrokes.reset();
    if (captured.rider.seated()) {
        // Bounded trace of what a ride is driven with.
        static uint64_t lastTrace = 0;
        static unsigned traced = 0;
        if (GetTickCount64() - lastTrace > 1000 && traced < 120) {
            lastTrace = GetTickCount64(); ++traced;
            log("Ride controls move=%.2f,%.2f,%.2f look=%.3f,%.3f,%.3f ball=%d", move.x, move.y, move.z, look.x, look.y, look.z,
                ballRide(captured.rider));
        }
    }
    originalPlayerControls(brain, fire, look, move);
    logLocalControls(observation,caller,fire,look,move);
}
// Virtual steering wheel for driven rides without authored controls. Main
// thread only: input sampling updates the grasp, the mounted look clamp turns
// heading-steered rides, and the root view draws the wheel in each eye.
enum class WheelSteering : uint32_t { None, Axis, Heading };
struct SteeringWheelRuntime {
    SteeringWheelGrasp grasp;
    uint32_t ride=0;
    uint64_t rideSince=0,updated=0,clamped=0;
    WheelSteering steering=WheelSteering::None;
    bool visible=false,preview=false;
    void *texture=nullptr;
    bool textureTried=false;
};
static SteeringWheelRuntime steeringWheel;
static_assert(SteeringWheelRim==SteeringWheelRimRadius,"Grasp rim must match the generated mesh");
static WheelSteering steeringWheelClass(void *ride) {
    if(!ride || !controlsGameBase || !readableMemory(ride,4))return WheelSteering::None;
    uintptr_t table=0;std::memcpy(&table,ride,4);
    if(table<controlsGameBase)return WheelSteering::None;
    switch(table-controlsGameBase) {
    // EnforcePuppetMoveLook 1563D0 with wheel-joint steering from move X.
    case 0x2a7ea0: case 0x2ba688: case 0x2aedd8: case 0x2af898: return WheelSteering::Axis;
    // Aircraft, rolling ball, air bike and hovercraft turn toward look heading.
    case 0x2b79b0: case 0x2b7348: case 0x2b9240: case 0x2b0760: case 0x2a8558: case 0x2b8420:
        return WheelSteering::Heading;
    default: return WheelSteering::None; // Turrets, mounts and unknown classes.
    }
}
static bool steeringWheelTextureReady(void *texture) {
    // Only a loaded CStaticTexture may reach gfuCurrentTexture.
    if(!texture || !staticTextureVtable || !readableMemory(texture,8))return false;
    uintptr_t table=0;std::memcpy(&table,texture,4);
    return table==staticTextureVtable && !(static_cast<const uint8_t*>(texture)[4]&1u);
}
// Main thread only. The resource Ptr is owned at &resource for the process.
static bool obtainNativeResource(void *&resource,void *dataType,const char *path) {
    if(!resourceObtain || !dataType || !smartAddRef || !smartRemRef)return false;
    // Mirror the game's own immediate resource wrappers (sniper mask texture,
    // Sam2Game model loader at RVA 475B0): method 0, null name, then replace a
    // pending proxy by its loaded object. The Ptr owner address is &resource
    // throughout, as for the native AddRef/RemRef pairs.
    const bool completed=withNativeFinally([&] {
        resourceObtain(&resource,dataType,0,1,1,path,nullptr);
        void *proxy=resource;
        if(proxy && readableMemory(proxy,8) && (static_cast<const uint8_t*>(proxy)[4]&1u)) {
            using Replacement=void *(__thiscall *)(void *);
            auto **table=*static_cast<Replacement **>(proxy);
            void *loaded=table[3](proxy);
            if(loaded)smartAddRef(loaded,&resource);
            resource=loaded;
            smartRemRef(proxy,&resource);
        }
    },[](bool) noexcept {});
    if(!completed)resource=nullptr;
    return completed;
}
static bool obtainStaticTexture(void *&texture,const char *path) {
    return textureDataType && *textureDataType && obtainNativeResource(texture,*textureDataType,path) &&
           steeringWheelTextureReady(texture);
}
static void loadSteeringWheelTexture() {
    auto &w=steeringWheel;
    if(w.textureTried || !resourceObtain || !textureDataType || !*textureDataType || !smartAddRef || !smartRemRef)return;
    w.textureTried=true;
    const bool ready=obtainStaticTexture(w.texture,
        "Content/SeriousSam2/Models/Vehicles/HoverFighter/Textures/Fighter.tex");
    log("Steering wheel texture %s",ready ? "loaded" : "unavailable; using plain wheel");
}
static void updateSteeringWheel(const Snapshot &s) {
    auto &w=steeringWheel;
    if(!nativeMainThread || !nativeMainThread())return;
    const auto &config=settings.steeringWheel;
    const auto now=GetTickCount64();
    const float dt=w.updated && now>w.updated ? float(now-w.updated)/1000.f : 0.f;
    w.updated=now;
    WheelSteering steering=WheelSteering::None;
    if(config.enabled && s.rider.seated() && livePlayer(s) && s.ui.gameplay) {
        steering=steeringWheelClass(resolve(s.rider.ride));
        if(w.ride!=s.rider.ride) {w.ride=s.rider.ride;w.rideSince=now;w.grasp.reset();}
        // Rides with admitted authored handles keep them; wait for that admission first.
        const bool hover=steering==WheelSteering::Heading && controlsGameBase && [&] {
            void *ride=resolve(s.rider.ride);uintptr_t table=0;
            if(ride && readableMemory(ride,4))std::memcpy(&table,ride,4);
            return table==controlsGameBase+0x2a8558 || table==controlsGameBase+0x2b8420;
        }();
        if(hover && settings.immersiveVehicleGrips) {
            AcquireSRWLockShared(&controlsLock);
            const bool handles=handleFrameRide==s.rider.ride && now-handleFrameTick<1500;
            ReleaseSRWLockShared(&controlsLock);
            if(handles || now-w.rideSince<1500)steering=WheelSteering::None;
        }
    } else w.ride=0;
    w.steering=steering;
    w.preview=steering==WheelSteering::None && config.enabled && config.previewOnFoot && s.rider.handheld() &&
        livePlayer(s) && s.ui.gameplay;
    w.visible=steering!=WheelSteering::None || w.preview;
    if(w.visible)loadSteeringWheelTexture();
    const bool eligible=steering!=WheelSteering::None && fresh(s.input) && !recenterHeld(s.input) &&
        s.ui.tickMs<=now && now-s.ui.tickMs<200 &&
        !s.ui.wheel[0].open && !s.ui.wheel[1].open && nativeRiderCurrent(s.player,s.rider);
    w.grasp.update(config,s.input,s.gripPoseGeneration,s.origin,s.turn,s.inputProducer,eligible,dt);
}
// Heading-steered rides: while the wheel is held, the wheel's steering replaces
// hand-aim heading with a turn rate; pitch keeps the native current value.
static bool steeringWheelHeading(Vec3 &look) {
    auto &w=steeringWheel;
    const auto now=GetTickCount64();
    const float dt=w.clamped && now>w.clamped ? std::min(.1f,float(now-w.clamped)/1000.f) : 0.f;
    w.clamped=now;
    if(w.steering!=WheelSteering::Heading || !w.grasp.steering() || rideGrasp.mask || !quaternionEuler ||
       now-w.updated>200)return false;
    const float delta=-w.grasp.steer(settings.steeringWheel)*settings.steeringWheel.turnRateDegrees*Pi/180*dt;
    Vec3 converted;
    if(quaternionEuler(&converted,yaw(delta))!=&converted || !std::isfinite(converted.x))return true;
    if(std::isfinite(look.x+converted.x))look.x+=converted.x;
    return true;
}
static void drawSteeringWheel() {
    auto &w=steeringWheel;
    if(!w.visible || !eyeAnchorReady || !nativeMainThread || !nativeMainThread() ||
       !eyeSnapshot.ui.gameplay || !currentCanvas || !*currentCanvas || !unbindVertexBuffers ||
       !*unbindVertexBuffers || !bindVertexArray || !*bindVertexArray || !drawVertices || !*drawVertices ||
       !useSimpleShader || !constantColor || !currentTexture)return;
    if(w.preview ? !eyeSnapshot.rider.handheld() :
       (!eyeSnapshot.rider.seated() || eyeSnapshot.rider.ride!=w.ride))return;
    const auto &config=settings.steeringWheel;
    const Pose wheel=compose(eyeAnchor,steeringWheelSpin(config,w.preview?0.f:w.grasp.angle));
    if(!finite(wheel))return;
    constexpr size_t Count=SteeringWheelVertexCount;
    // Both windings, so native face culling cannot hide the far side.
    static Vec3 position[Count*2];
    static float uv[Count*2][2];
    static uint32_t color[Count*2];
    const Vec3 light=rotate(eyeAnchor.q,Vec3{.3f,.85f,.45f}*(1/std::sqrt(.3f*.3f+.85f*.85f+.45f*.45f)));
    for(size_t i=0;i<Count;i+=3)
        for(unsigned k=0;k<3;++k) {
            const auto &v=SteeringWheelMesh[i+k];
            const Vec3 p=wheel.p+rotate(wheel.q,Vec3{v.x,v.y,v.z}*config.scale);
            const Vec3 n=rotate(wheel.q,Vec3{v.nx,v.ny,v.nz});
            const float lit=dot(n,light);
            const auto shade=[](float x) {
                const uint32_t c=uint32_t(std::clamp(x,0.f,1.f)*255.f+.5f);
                return c|c<<8|c<<16|0xff000000u;
            };
            const size_t front=i+k,back=Count+i+(2-k);
            position[front]=position[back]=p;
            uv[front][0]=uv[back][0]=v.u;uv[front][1]=uv[back][1]=v.v;
            color[front]=shade(.62f+.5f*std::max(0.f,lit));
            color[back]=shade(.62f+.5f*std::max(0.f,-lit));
        }
    ortho();
    activateView(rootView);
    const bool wasDepth=*depthEnabled!=0,wasWrite=*depthWriting!=0;
    const bool wasBlend=*blending!=0,wasAlpha=*alphaTesting!=0;
    const int priorComparison=*depthComparison;
    (*enableDepth)();(*enableDepthWrite)();(*disableBlend)();(*disableAlpha)();
    (*setDepthComparison)(42); // LESSEQUAL, as for the lasers.
    const bool textured=steeringWheelTextureReady(w.texture);
    if(textured)currentTexture(0,w.texture,0);
    (*unbindVertexBuffers)();
    useSimpleShader(textured?3:0);
    if(!textured)constantColor(0x8a8f96ffu);
    // Bounded batches of whole triangles keep each client array small.
    constexpr size_t Batch=900;
    for(size_t first=0;first<Count*2;first+=Batch) {
        const size_t n=std::min(Batch,Count*2-first);
        (*bindVertexArray)(0,position+first,0x87,int(n*12),12);
        if(textured) {
            (*bindVertexArray)(2,uv+first,0x86,int(n*8),8);
            (*bindVertexArray)(3,color+first,0x82,int(n*4),4);
        }
        (*drawVertices)(0x99,int(n/3),0);
    }
    (*unbindVertexBuffers)(); // Leave no client arrays bound to later native draws.
    if(!wasDepth)(*disableDepth)();
    if(!wasWrite)(*disableDepthWrite)();
    if(wasBlend)(*enableBlend)();
    if(wasAlpha)(*enableAlpha)();
    (*setDepthComparison)(priorComparison);
}
// Hand weapon wheels, drawn in each eye after the root world like the lasers.
// Translucent bubbles face the head and carry the stock weapon display meshes,
// slowly spinning; a weapon whose mesh is unavailable shows the game's own icon
// (samGetWeaponIcon). Colours are native constant RGBA; icon tint is only grey
// and alpha. Bubbles ignore depth so an open wheel stays readable over the held
// weapon; meshes are depth tested like world models. Main thread only; gfx
// state is restored after.
struct WeaponWheelTextures {
    void *netricsa=nullptr;
    bool tried=false;
};
static WeaponWheelTextures weaponWheelTextures;
static void loadWeaponWheelTextures(const Snapshot &s) {
    auto &t=weaponWheelTextures;
    if(t.tried || !nativeMainThread || !nativeMainThread() || (!s.ui.wheel[0].open && !s.ui.wheel[1].open))return;
    t.tried=true;
    const bool ready=obtainStaticTexture(t.netricsa,"Content/SeriousSam2/Textures/Interface/Hud/netricsa.tex");
    log("Weapon wheel Netricsa icon %s",ready ? "loaded" : "unavailable; using a drawn symbol");
}
// Stock weapon display meshes: the per-weapon pickups the combo-weapons item
// shows (ComboWeapons_Item.mdl children). The flamer has no pickup model and
// uses its plain weapon model. Indexed by native WeaponIndex.
static const char *const wheelModelPaths[WeaponCount]={
    "Content/SeriousSam2/Models/Weapons/CircularSaw/CircularSaw_Pickup.mdl",
    "Content/SeriousSam2/Models/Weapons/ZapGun/ZapGun_Pickup.mdl",
    "Content/SeriousSam2/Models/Weapons/AutoShotgun/AutoShotgun_Pickup.mdl",
    "Content/SeriousSam2/Models/Weapons/DoubleShotgun/DoubleShotgun_Pickup.mdl",
    "Content/SeriousSam2/Models/Weapons/Uzi/Uzi_Pickup.mdl",
    "Content/SeriousSam2/Models/Weapons/Minigun/Minigun_Pickup.mdl",
    "Content/SeriousSam2/Models/Weapons/RocketLauncher/RocketLauncher_Pickup.mdl",
    "Content/SeriousSam2/Models/Weapons/GrenadeLauncher/GrenadeLauncher_Pickup.mdl",
    "Content/SeriousSam2/Models/Weapons/PlasmaRifle/PlasmaRifle_Pickup.mdl",
    "Content/SeriousSam2/Models/Weapons/Klodovik/Klodovik_Pickup.mdl",
    "Content/SeriousSam2/Models/Weapons/Cannon/Cannon_Pickup.mdl",
    "Content/SeriousSam2/Models/Weapons/SeriousBomb/SeriousBomb_Pickup.mdl",
    "Content/SeriousSam2/Models/Weapons/Colt/Colt_Pickup.mdl",
    "Content/SeriousSam2/Models/Weapons/Sniper/Sniper_Pickup.mdl",
    nullptr, // Unused index.
    "Content/SeriousSam2/Models/Weapons/BeamGun/BeamGun_Pickup.mdl",
    "Content/SeriousSam2/Models/Weapons/Flamer/Flamer.mdl"};
// Process-lifetime display instances: the configuration Ptr is owned at
// &configuration and the bare CModelInstance (no renderable, mechanism or
// animation) only by this table. The union of its render meshes' boxes, at
// unit stretch, normalises the size; the stored configuration box of a stock
// pickup does not match its mesh (sniper 2.2 m box, rocket launcher too small).
struct WheelModel {
    void *configuration=nullptr,*instance=nullptr;
    Vec3 centre{};
    float extent=0;
    bool tried=false,ready=false;
};
static WheelModel wheelModels[WeaponCount];
static bool wheelModelConfigurationReady(void *configuration) {
    if(!configuration || !modelConfigurationVtable || !readableMemory(configuration,8))return false;
    uintptr_t table=0;std::memcpy(&table,configuration,4);
    return table==modelConfigurationVtable && !(static_cast<const uint8_t*>(configuration)[4]&1u);
}
static WheelModel grenadeModel;
// Loads one display model (process-lifetime instance) and normalises its size.
static void loadDisplayModel(WheelModel &m,const char *path,const char *name) {
    m.tried=true;
    constexpr uint32_t MaximumMeshes=16;
    WheelBox configured{},meshes[MaximumMeshes]{};
    uint32_t meshCount=0;
    bool boxed=false;
    if(obtainNativeResource(m.configuration,*modelDataType,path) &&
       wheelModelConfigurationReady(m.configuration))
        withNativeFinally([&] {
            m.instance=createModelInstance();
            if(!m.instance)return;
            configureModelInstance(m.instance,m.configuration);
            boxed=modelInstanceBox(&configured,m.instance)==&configured; // Unit stretch: the stored box.
            // A render mesh without LODs yields the native +-0.5 default box; skip it.
            const int32_t count=modelMeshCount(m.configuration);
            for(int32_t i=0;i<count && meshCount<MaximumMeshes;++i) {
                void *mesh=modelMesh(m.configuration,i);
                void *render=mesh ? meshRenderMesh(mesh) : nullptr;
                if(render && renderMeshLodCount(render)>0 && renderMeshBox(&meshes[meshCount],render)==&meshes[meshCount])
                    ++meshCount;
            }
        },[](bool) noexcept {});
    const auto bounds=wheelModelBounds(meshes,meshCount,boxed ? configured : WheelBox{});
    m.extent=bounds.extent;
    m.centre=bounds.centre;
    m.ready=m.instance && m.extent>0;
    if(m.instance && !m.ready) {
        void *instance=m.instance;
        m.instance=nullptr;
        withNativeFinally([&] {deleteModelInstance(instance);},[](bool) noexcept {});
    }
    const Vec3 stored=configured.max-configured.min;
    log("%s %s extent=%.3f source=%s meshes=%u configurationExtent=%.3f",name,
        m.ready ? "loaded" : "unavailable; using its icon",m.extent,bounds.fromMeshes ? "meshes" : "configuration",
        meshCount,std::max({stored.x,stored.y,stored.z}));
}
// One model per input poll once gameplay runs, so loading never stalls one frame
// for long; a weapon keeps its icon until (or unless) its mesh is ready.
static void loadWeaponWheelModels(const Snapshot &s) {
    if(!s.ui.gameplay || !nativeMainThread || !nativeMainThread() || !modelDataType || !*modelDataType ||
       !createModelInstance || !deleteModelInstance || !configureModelInstance || !modelInstanceBox ||
       !renderModelInstance || !stretchModelInstance || !modelMeshCount || !modelMesh || !meshRenderMesh ||
       !renderMeshLodCount || !renderMeshBox)return;
    for(uint32_t id=0;id!=WeaponCount;++id) {
        auto &m=wheelModels[id];
        if(m.tried || !wheelModelPaths[id])continue;
        char name[24];
        std::snprintf(name,sizeof name,"Weapon wheel model %u",id);
        loadDisplayModel(m,wheelModelPaths[id],name);
        return;
    }
    // The hand grenade held before a throw (after the wheel meshes).
    if(!grenadeModel.tried)
        loadDisplayModel(grenadeModel,"Content/SeriousSam2/Models/Projectiles/HandGrenade/HandGrenade.mdl","Hand grenade model");
}
struct WheelBillboard {
    Vec3 centre,right,up;
    Vec3 at(float u,float v) const {return centre+right*u+up*v;}
};
// Whole triangles in both windings, so native face culling cannot hide them;
// one bounded client array per constant colour.
struct WheelBatch {
    static constexpr size_t Capacity=960;
    Vec3 position[Capacity];
    size_t count=0;
    uint32_t color=0;
    void flush() {
        if(!count)return;
        constantColor(color);
        (*bindVertexArray)(0,position,0x87,int(count*12),12);
        (*drawVertices)(0x99,int(count/3),0);
        count=0;
    }
    void triangle(uint32_t rgba,Vec3 a,Vec3 b,Vec3 c) {
        if(rgba!=color || count+6>Capacity) {flush();color=rgba;}
        const Vec3 corners[6]{a,b,c,a,c,b};
        std::copy_n(corners,6,position+count);
        count+=6;
    }
    void quad(uint32_t rgba,const WheelBillboard &b,float u0,float v0,float u1,float v1) {
        const Vec3 p00=b.at(u0,v0),p10=b.at(u1,v0),p11=b.at(u1,v1),p01=b.at(u0,v1);
        triangle(rgba,p00,p10,p11);triangle(rgba,p00,p11,p01);
    }
    // A disc when inner is zero, otherwise an annulus.
    void ring(uint32_t rgba,const WheelBillboard &b,float inner,float outer,unsigned segments=28) {
        for(unsigned i=0;i!=segments;++i) {
            const float a0=2*Pi*float(i)/float(segments),a1=2*Pi*float(i+1)/float(segments);
            const float c0=std::cos(a0),s0=std::sin(a0),c1=std::cos(a1),s1=std::sin(a1);
            const Vec3 o0=b.at(c0*outer,s0*outer),o1=b.at(c1*outer,s1*outer);
            if(inner<=0) {triangle(rgba,b.centre,o0,o1);continue;}
            const Vec3 i0=b.at(c0*inner,s0*inner),i1=b.at(c1*inner,s1*inner);
            triangle(rgba,i0,o0,o1);triangle(rgba,i0,o1,i1);
        }
    }
};
static WheelBatch wheelBatch;
static uint32_t wheelTint(uint8_t grey,uint8_t alpha) {
    return grey|uint32_t(grey)<<8|uint32_t(grey)<<16|uint32_t(alpha)<<24;
}
// Fit a loaded CStaticTexture into a box. CStaticTexture GetWidth/GetHeight
// (Engine vtable slots 14/15, RVA 81130/81140) return the fields at +30/+34.
static bool wheelIconExtent(void *texture,float width,float height,float &halfWidth,float &halfHeight) {
    if(!steeringWheelTextureReady(texture) || !readableMemory(texture,0x38))return false;
    int32_t size[2]{};
    std::memcpy(size,static_cast<const uint8_t*>(texture)+0x30,sizeof(size));
    if(size[0]<1 || size[1]<1 || size[0]>8192 || size[1]>8192)return false;
    const float scale=std::min(width/float(size[0]),height/float(size[1]));
    halfWidth=.5f*scale*float(size[0]);halfHeight=.5f*scale*float(size[1]);
    return std::isfinite(halfWidth) && std::isfinite(halfHeight);
}
static void drawWheelIcon(void *texture,const WheelBillboard &b,float halfWidth,float halfHeight,uint32_t tint) {
    wheelBatch.flush();
    static Vec3 position[12];
    static float uv[12][2];
    static uint32_t color[12];
    const Vec3 corner[4]{b.at(-halfWidth,halfHeight),b.at(halfWidth,halfHeight),
                         b.at(halfWidth,-halfHeight),b.at(-halfWidth,-halfHeight)};
    constexpr float cornerUv[4][2]{{0,0},{1,0},{1,1},{0,1}}; // Engine/D3D top-left origin.
    constexpr unsigned order[12]{0,1,2,0,2,3,0,2,1,0,3,2};
    for(unsigned i=0;i!=12;++i) {
        position[i]=corner[order[i]];
        uv[i][0]=cornerUv[order[i]][0];uv[i][1]=cornerUv[order[i]][1];
        color[i]=tint;
    }
    useSimpleShader(3); // GFU_SHD_POS_TEX_COL
    currentTexture(0,texture,0);
    (*bindVertexArray)(0,position,0x87,12*12,12);
    (*bindVertexArray)(2,uv,0x86,12*8,8);
    (*bindVertexArray)(3,color,0x82,12*4,4);
    (*drawVertices)(0x99,4,0);
    (*unbindVertexBuffers)();
    useSimpleShader(0); // GFU_SHD_POS with gfuConstantColor
}
// Bubble symbols in units of the symbol half-extent: menu bars, save/load
// arrows into/out of a tray and an information mark for Netricsa.
static void drawWheelSymbol(WheelBubble bubble,const WheelBillboard &b,float s) {
    constexpr uint32_t ink=0xf5f7faffu;
    const auto box=[&](float u0,float v0,float u1,float v1) {wheelBatch.quad(ink,b,u0*s,v0*s,u1*s,v1*s);};
    const auto tray=[&] {box(-.6f,-.62f,.6f,-.48f);box(-.6f,-.62f,-.46f,-.2f);box(.46f,-.62f,.6f,-.2f);};
    switch(bubble) {
    case WheelBubble::Menu:
        box(-.55f,.28f,.55f,.46f);box(-.55f,-.09f,.55f,.09f);box(-.55f,-.46f,.55f,-.28f);
        break;
    case WheelBubble::QuickSave:
        tray();box(-.11f,-.05f,.11f,.62f);
        wheelBatch.triangle(ink,b.at(-.38f*s,-.05f*s),b.at(.38f*s,-.05f*s),b.at(0,-.42f*s));
        break;
    case WheelBubble::QuickLoad:
        tray();box(-.11f,-.36f,.11f,.2f);
        wheelBatch.triangle(ink,b.at(-.38f*s,.2f*s),b.at(.38f*s,.2f*s),b.at(0,.62f*s));
        break;
    case WheelBubble::Netricsa:
        box(-.12f,.36f,.12f,.6f);box(-.12f,-.6f,.12f,.2f);
        break;
    case WheelBubble::VrOptions: // Three sliders.
        for(const auto [v,knob]:{std::pair{.36f,.22f},std::pair{0.f,-.28f},std::pair{-.36f,.08f}}) {
            box(-.58f,v-.05f,.58f,v+.05f);box(knob-.12f,v-.16f,knob+.12f,v+.16f);
        }
        break;
    default:
        break;
    }
}
// Both eyes of one request draw the same copy of the published wheels.
static struct {
    uint64_t sequence=0,tick=0;
    uint32_t session=0;
    WheelPresentation wheel[2];
} eyeWheels;
static bool wheelModelReady(int32_t weapon) {
    return weapon>=0 && uint32_t(weapon)<WeaponCount && wheelModels[weapon].ready;
}
static void drawWeaponWheel(unsigned hand,const Pose &head) {
    const auto &view=eyeWheels.wheel[hand];
    const auto &ui=eyeSnapshot.ui.wheel[hand];
    const auto &ring=view.ring;
    if(!ui.open || !view.open || !ring.anchored || !ring.count || ring.count>WheelSlotCount ||
       !std::isfinite(ring.radius) || !std::isfinite(ring.itemSize))return;
    // The ring lives in tracking space like a hand: it follows the rig, not the world.
    const Pose frame=worldHandTracking(eyeAnchor,eyeSnapshot.origin,eyeSnapshot.turn,eyeRequest.input.head,ring.frame);
    if(!finite(frame))return;
    const Vec3 ringRight=rotate(frame.q,Vec3{1,0,0}),ringUp=rotate(frame.q,Vec3{0,1,0});
    const auto billboard=[&](Vec3 centre) {
        // Face the head centre (shared by both eyes) and keep the ring's up.
        Vec3 forward=head.p-centre;
        const float distance=std::sqrt(dot(forward,forward));
        Vec3 right=distance>.01f ? cross(ringUp,forward*(1/distance)) : Vec3{};
        const float width=std::sqrt(dot(right,right));
        if(!(width>.05f))return WheelBillboard{centre,ringRight,ringUp};
        right=right*(1/width);
        return WheelBillboard{centre,right,cross(forward*(1/distance),right)};
    };
    // Faint outline of the centre: releasing inside it cancels.
    const float cancel=WheelDeadZone*ring.radius;
    wheelBatch.ring(0xffffff38u,WheelBillboard{frame.p,ringRight,ringUp},cancel-.0015f,cancel+.0015f,40);
    constexpr uint32_t gold=0xffc23affu;
    const uint32_t handColour=hand ? 0x40dfffffu : 0x60ff40ffu; // Laser colours.
    constexpr uint32_t bubbleFill[WheelBubbleCount]{0x1d7891d8u,0x6a44a8d8u,0x2f8a46d8u,0xb37a1cd8u,0x4a5566d8u};
    const auto drawItem=[&](uint32_t i) {
        const auto &slot=ring.slot[i];
        const bool hovered=int(i)==view.hoverSlot;
        const float radius=ring.slotSize(i)*(hovered ? .65f : .5f);
        const auto b=billboard(frame.p+rotate(frame.q,ring.slotPosition(i)));
        if(hovered)
            for(unsigned g=0;g!=3;++g)
                wheelBatch.ring((gold&~0xffu)|(0x70u>>g),b,radius*(1+.15f*float(g)),radius*(1.15f+.15f*float(g)));
        if(slot.bubble!=WheelBubble::None) {
            const uint32_t kind=uint32_t(slot.bubble);
            wheelBatch.ring(bubbleFill[kind<WheelBubbleCount ? kind : 0],b,0,radius);
            wheelBatch.ring(hovered ? gold : 0xe8eef4c0u,b,radius*.9f,radius);
            float halfWidth=0,halfHeight=0;
            if(slot.bubble==WheelBubble::Netricsa &&
               wheelIconExtent(weaponWheelTextures.netricsa,radius*1.3f,radius*1.3f,halfWidth,halfHeight))
                drawWheelIcon(weaponWheelTextures.netricsa,b,halfWidth,halfHeight,wheelTint(0xff,0xff));
            else
                drawWheelSymbol(slot.bubble,b,radius*.62f);
            return;
        }
        int32_t rounds=-1; // Native: -1 for weapons without ammunition, 0 when empty.
        if(slot.index>=0 && uint32_t(slot.index)<ui.count && uint32_t(slot.index)<WeaponCount &&
           ui.weapon[slot.index]==slot.weapon)
            rounds=ui.ammo[slot.index];
        const bool empty=rounds==0,held=eyeSnapshot.ui.currentWeapon[hand]==slot.weapon;
        const bool mesh=wheelModelReady(slot.weapon); // Drawn after this pass.
        wheelBatch.ring(hovered ? (mesh ? 0x3a2c1490u : 0x3a2c14e8u) : (mesh ? 0x161d2770u : 0x161d27d0u),b,0,radius);
        wheelBatch.ring(hovered ? gold : held ? handColour : empty ? 0xb83c3cffu : 0x8c99a8e0u,b,radius*.9f,radius);
        if(mesh)return;
        void *icon=nativeWeaponIcon ? nativeWeaponIcon(slot.weapon) : nullptr;
        float halfWidth=0,halfHeight=0;
        if(wheelIconExtent(icon,radius*1.8f,radius*1.2f,halfWidth,halfHeight))
            drawWheelIcon(icon,b,halfWidth,halfHeight,empty ? wheelTint(0x80,0xa0) : wheelTint(0xff,0xff));
    };
    for(uint32_t i=0;i!=ring.count;++i)
        if(int(i)!=view.hoverSlot)drawItem(i);
    if(view.hoverSlot>=0 && uint32_t(view.hoverSlot)<ring.count)drawItem(uint32_t(view.hoverSlot));
}
// Second pass: the floating meshes, each scaled by its unit-stretch box about
// the slot centre and spinning about the ring's (upright) axis. Stretch is set
// per draw, as CBaseWeaponEntity::Render does for mirrored weapons; LOD
// distance 0 and no shadows also match that native weapon draw.
static void drawWeaponWheelModels(unsigned hand) {
    const auto &view=eyeWheels.wheel[hand];
    const auto &ring=view.ring;
    if(!eyeSnapshot.ui.wheel[hand].open || !view.open || !ring.anchored || !ring.count ||
       ring.count>WheelSlotCount || !std::isfinite(ring.itemSize))return;
    const Pose frame=worldHandTracking(eyeAnchor,eyeSnapshot.origin,eyeSnapshot.turn,eyeRequest.input.head,ring.frame);
    if(!finite(frame))return;
    for(uint32_t i=0;i!=ring.count;++i) {
        const auto &slot=ring.slot[i];
        if(slot.bubble!=WheelBubble::None || !wheelModelReady(slot.weapon))continue;
        const auto &model=wheelModels[slot.weapon];
        const float spin=2*Pi*float(eyeWheels.tick%6000)/6000.f+.9f*float(i); // One turn per 6 s.
        const auto placed=wheelModelPlacement(frame,ring.slotPosition(i),model.centre,model.extent,
            wheelModelSize(ring.itemSize,int(i)==view.hoverSlot),spin);
        if(!(placed.scale>0))continue;
        stretchModelInstance(model.instance,Vec3{placed.scale,placed.scale,placed.scale});
        renderModelInstance(model.instance,matrix(placed.pose),0.f,0);
    }
}
static void drawWeaponWheels() {
    if(!eyeAnchorReady || !nativeMainThread || !nativeMainThread() || !eyeSnapshot.ui.gameplay ||
       !eyeSnapshot.rider.handheld() || !rootView || !currentCanvas || !*currentCanvas || !unbindVertexBuffers ||
       !*unbindVertexBuffers || !bindVertexArray || !*bindVertexArray || !drawVertices || !*drawVertices ||
       !useSimpleShader || !constantColor || !currentTexture || !nativeBlendType || !blendSource ||
       !blendDestination || !setBlendFunction || !*setBlendFunction)return;
    if(!eyeSnapshot.ui.wheel[0].open && !eyeSnapshot.ui.wheel[1].open)return;
    if(eyeWheels.sequence!=eyeRequest.sequence || eyeWheels.session!=eyeRequest.session) {
        AcquireSRWLockShared(&wheelPresentationLock);
        std::copy_n(wheelPresentation,2,eyeWheels.wheel);
        ReleaseSRWLockShared(&wheelPresentationLock);
        eyeWheels.sequence=eyeRequest.sequence;eyeWheels.session=eyeRequest.session;
        eyeWheels.tick=GetTickCount64();
    }
    if(!eyeWheels.wheel[0].open && !eyeWheels.wheel[1].open)return;
    const Pose head=worldHeadTracking(eyeAnchor,eyeSnapshot.origin,eyeSnapshot.turn,eyeRequest.input.head);
    if(!finite(head))return;
    ortho();
    activateView(rootView);
    const bool wasDepth=*depthEnabled!=0,wasWrite=*depthWriting!=0;
    const bool wasBlend=*blending!=0,wasAlpha=*alphaTesting!=0;
    const bool fogControl=fogging && enableFog && disableFog && *enableFog && *disableFog, wasFog=fogControl && *fogging!=0;
    const int priorSource=*blendSource,priorDestination=*blendDestination,priorComparison=*depthComparison;
    (*disableDepth)();(*disableDepthWrite)();(*disableAlpha)();
    nativeBlendType(501); // GFU_BT_TRANSLUCENT: source alpha over the frame.
    (*unbindVertexBuffers)();
    useSimpleShader(0);
    for(unsigned h=0;h!=2;++h)drawWeaponWheel(h,head);
    wheelBatch.flush();
    // The opening hand's pointer laser, from its aim to where it meets the ring.
    for(unsigned h=0;h!=2;++h) {
        const auto &view=eyeWheels.wheel[h];
        if(!eyeSnapshot.ui.wheel[h].open || !view.open || !view.ring.anchored || !eyeRequest.input.handValid[h] ||
           !finite(eyeRequest.input.hand[h]))continue;
        const Pose aim=worldHandTracking(eyeAnchor,eyeSnapshot.origin,eyeSnapshot.turn,eyeRequest.input.head,eyeRequest.input.hand[h]);
        const Pose frame=worldHandTracking(eyeAnchor,eyeSnapshot.origin,eyeSnapshot.turn,eyeRequest.input.head,view.ring.frame);
        if(!finite(aim) || !finite(frame))continue;
        const Vec3 dir=rotate(normalize(aim.q),Vec3{0,0,-1}),normal=rotate(normalize(frame.q),Vec3{0,0,1});
        const float facing=dot(dir,normal);
        float length=.6f;
        if(facing<-.1f) {
            const float t=dot(frame.p-aim.p,normal)/facing;
            if(std::isfinite(t) && t>0)length=std::min(t,WheelPointReach);
        }
        const Vec3 end=aim.p+dir*length;
        const uint32_t colour=h ? 0x9040dfffu : 0x9060ff40u; // ARGB, the hand's wheel colour.
        drawLine(aim.p,end,colour,UINT32_MAX);
        for(unsigned k=0;k!=6;++k) {
            const float a=k*Pi/3;
            const Vec3 side=rotate(normalize(aim.q),Vec3{.001f*std::cos(a),.001f*std::sin(a),0});
            drawLine(aim.p+side,end+side,colour,UINT32_MAX);
        }
    }
    (*unbindVertexBuffers)(); // Leave no client arrays bound to later native draws.
    if(renderModelInstance && stretchModelInstance) {
        // World-like opaque state for the meshes; their materials set the rest.
        (*enableDepth)();(*enableDepthWrite)();(*disableBlend)();
        (*setDepthComparison)(42); // LESSEQUAL, as for the lasers.
        for(unsigned h=0;h!=2;++h)drawWeaponWheelModels(h);
        (*unbindVertexBuffers)();
        useSimpleShader(0);
        ortho(); // Identity model again, then this root's view/projection.
        activateView(rootView);
    }
    (*setBlendFunction)(priorSource,priorDestination);
    if(wasBlend)(*enableBlend)();
    else (*disableBlend)();
    if(wasDepth)(*enableDepth)();
    else (*disableDepth)();
    if(wasWrite)(*enableDepthWrite)();
    else (*disableDepthWrite)();
    if(wasAlpha)(*enableAlpha)();
    else (*disableAlpha)();
    (*setDepthComparison)(priorComparison);
    // Mesh materials turn fog on; the HUD drawn after this eye must find it as
    // the world left it (a fogged HUD draw is rejected and drops the pair).
    if(fogControl) {
        if(wasFog)(*enableFog)();
        else (*disableFog)();
    }
}
// The grenade waiting in the hand: the model's own size, its centre in front of
// the grip point (in the palm), drawn like the wheel meshes.
static void drawHeldGrenade() {
    const int hand=grenadeShownHand.load(std::memory_order_relaxed);
    if(hand<0 || hand>1 || !grenadeModel.ready || !eyeAnchorReady || !nativeMainThread || !nativeMainThread() ||
       !eyeSnapshot.ui.gameplay || !eyeSnapshot.rider.handheld() || !rootView || !renderModelInstance ||
       !stretchModelInstance || !unbindVertexBuffers || !*unbindVertexBuffers || !useSimpleShader ||
       !controllerTracked(eyeRequest.input,unsigned(hand)))return;
    const Pose grip=worldHandTracking(eyeAnchor,eyeSnapshot.origin,eyeSnapshot.turn,eyeRequest.input.head,
                                      weaponTracking(eyeRequest.input,unsigned(hand)));
    if(!finite(grip))return;
    // The model's own size, as the thrown grenade flies (V636: 9 cm in the hand
    // looked far smaller than the same grenade in flight); held ahead of the palm.
    const float scale=1.f;
    const Quat q=normalize(grip.q);
    const Vec3 centre=grip.p+rotate(q,Vec3{0,-.01f,-(.02f+.3f*grenadeModel.extent)});
    const Pose placed{q,centre-rotate(q,grenadeModel.centre*scale)};
    ortho();
    activateView(rootView);
    const bool wasDepth=*depthEnabled!=0,wasWrite=*depthWriting!=0,wasBlend=*blending!=0;
    const bool fogControl=fogging && enableFog && disableFog && *enableFog && *disableFog, wasFog=fogControl && *fogging!=0;
    const int priorComparison=*depthComparison;
    (*enableDepth)();(*enableDepthWrite)();(*disableBlend)();
    (*setDepthComparison)(42); // LESSEQUAL, as for the lasers.
    stretchModelInstance(grenadeModel.instance,Vec3{scale,scale,scale});
    renderModelInstance(grenadeModel.instance,matrix(placed),0.f,0);
    (*unbindVertexBuffers)();
    useSimpleShader(0);
    ortho();
    activateView(rootView);
    if(wasBlend)(*enableBlend)();
    else (*disableBlend)();
    if(wasDepth)(*enableDepth)();
    else (*disableDepth)();
    if(wasWrite)(*enableDepthWrite)();
    else (*disableDepthWrite)();
    (*setDepthComparison)(priorComparison);
    if(fogControl) {
        if(wasFog)(*enableFog)();
        else (*disableFog)();
    }
}
// Core's activity test selects the inactive frame cap and input routing. A live
// VR session owns the game regardless of which desktop window has focus.
// CDamageHudElement's draw (Sam2Game 0x176dc0, its vtable slot after
// Initialize): the red hit-direction arrows and blood around the screen edge.
// The HUD is a small floating panel in VR, where those marks only clutter it;
// while VR input is live they are not drawn at all (user request).
using DamageHudRender = void(__thiscall *)(void *, void *, void *);
static DamageHudRender originalDamageHudRender = nullptr;
static void __fastcall damageHudRender(void *element, void *, void *a, void *b) {
    if (vrInputLive()) return;
    originalDamageHudRender(element, a, b);
}
// Mounting and dismounting rides (turrets, vehicles, creatures) without the
// enter/exit animation. The rider (the player puppet) plays a ride-and-rider
// sequence (CLeggedPuppetEntity::PlayRideAndRiderSequenceState, Sam2Game
// 0x64360): animation steps while the body lerps onto or off the seat and the
// camera flies between the eye and the ride's camera, shown flat in VR like a
// cutscene. For the local VR player the first step is skipped to the end
// (step index past the list), so the game's own completion runs at once: the
// seat snap and DoStartRiding on entry; on exit the rider is first put at the
// ride's enter point, where the native lerp ends. Only where the game is the
// authority (single player, host).
using RideSequenceState = void(__thiscall *)(void *);
using RideEnterPoint = Pose *(__thiscall *)(void *ride, Pose *out, uint32_t seat, int32_t way); // ride vt+0x220
using RiderDetach = void(__thiscall *)(void *rider, Pose placement);                            // rider vt+0x648
using RideRiderNote = void(__thiscall *)(void *ride, uint32_t seat);                             // ride vt+0x254/0x258
static RideSequenceState originalRideSequence = nullptr;
static void __fastcall rideSequence(void *p, void *) {
    const auto field = [p](unsigned offset) { uint32_t v; std::memcpy(&v, static_cast<uint8_t *>(p) + offset, 4); return v; };
    const auto s = copySnapshot();
    // Lab (SS2VR_LAB_NO_INSTANT=1): the native animated transitions.
    static const bool labNoInstant = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_NO_INSTANT", v, 2) == 1 && v[0] == L'1'; }();
    if (!settings.instantRides || labNoInstant || !vrInputLive() || p != s.player || !(singlePlayer() || multiplayer::server()) ||
        field(0x7d0) != 2 || field(0x7d4) != 0) {
        originalRideSequence(p);
        return;
    }
    void *ride = field(0x204) ? resolve(field(0x204)) : nullptr;
    if (!ride || !field(0x544) || !resolve(field(0x544)) || !field(0x114) || !resolve(field(0x114))) {
        originalRideSequence(p); // It finishes by itself without them.
        return;
    }
    const uint32_t seat = field(0x208);
    const int32_t way = int32_t(field(0x20c));
    const bool exiting = field(0x728) == 1;
    const auto *rideTable = *reinterpret_cast<uintptr_t *const *>(ride);
    if (field(0x54c) == seat) // The ride's own enter/exit notice (native: on lerp steps).
        reinterpret_cast<RideRiderNote>(rideTable[(exiting ? 0x258 : 0x254) / 4])(ride, seat);
    if (exiting) {
        Pose out{};
        reinterpret_cast<RideEnterPoint>(rideTable[0x220 / 4])(ride, &out, seat, way);
        if (finite(out) && validNativeBodyPose(out))
            reinterpret_cast<RiderDetach>((*reinterpret_cast<uintptr_t *const *>(p))[0x648 / 4])(p, out);
    }
    const int32_t done = INT32_MAX; // Native: index >= count -> SetState(1), the sequence's end.
    std::memcpy(static_cast<uint8_t *>(p) + 0x7d4, &done, 4);
    static unsigned logged = 0;
    if (logged++ < 32) log("Ride %s without its animation seat=%u way=%d", exiting ? "exit" : "entry", seat, way);
    originalRideSequence(p);
}
// Bounded traces of the game's weapon changes with their callers (Sam2Game
// RVAs): which native path brings a weapon back after a pick.
using WeaponSet4 = void(__thiscall *)(void *, int, int, int, int);
using WeaponPutDownTrace = void(__thiscall *)(void *, uint32_t);
static SetWeapon originalSetCurrentWeapon = nullptr;
static WeaponSet4 originalSetNewWeapon = nullptr, originalSetToHand = nullptr;
static WeaponPutDownTrace originalWeaponPutDown = nullptr;
static uintptr_t weaponTraceBase = 0;
static void weaponTrace(const char *what, void *p, int a, int b, int c, int d, uintptr_t caller) {
    static std::atomic<unsigned> traced{0};
    if (traced.fetch_add(1, std::memory_order_relaxed) >= 400) return;
    const bool game = caller >= weaponTraceBase && caller < weaponTraceBase + 0x400000;
    log("Weapon call %s %d,%d,%d,%d right=%d left=%d choices=%d,%d pending=%d,%d caller=%s%lx", what, a, b, c, d,
        resolve(nativeHandle(p, 1)) ? int(primaryField(resolve(nativeHandle(p, 1)), 0xb4)) : -1,
        resolve(nativeHandle(p, 0)) ? int(primaryField(resolve(nativeHandle(p, 0)), 0xb4)) : -1,
        int(primaryField(p, 0x988)), int(primaryField(p, 0x98c)), int(primaryField(p, 0x998)), int(primaryField(p, 0x994)),
        game ? "game+" : "", static_cast<unsigned long>(game ? caller - weaponTraceBase : caller));
}
static void __fastcall setCurrentWeaponTraced(void *p, void *, int id, int hand, int flag) {
    weaponTrace("SetCurrentWeapon", p, id, hand, flag, 0, reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    originalSetCurrentWeapon(p, id, hand, flag);
}
static void __fastcall setNewWeaponTraced(void *p, void *, int id, int a, int b, int c) {
    weaponTrace("SetNewWeapon", p, id, a, b, c, reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    originalSetNewWeapon(p, id, a, b, c);
}
static void __fastcall setToHandTraced(void *p, void *, int id, int a, int hand, int c) {
    weaponTrace("SetNewWeaponToPlayerHand", p, id, a, hand, c, reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    originalSetToHand(p, id, a, hand, c);
}
static void __fastcall weaponPutDownTraced(void *p, void *, uint32_t weapon) {
    void *w = weapon ? resolve(weapon) : nullptr;
    weaponTrace("OnWeaponPutDown", p, w ? int(primaryField(w, 0xb4)) : -1, weapon == nativeHandle(p, 1) ? 1 : weapon == nativeHandle(p, 0) ? 0 : -1,
                0, 0, reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    originalWeaponPutDown(p, weapon);
}
// A weapon this adapter replaced in a hand (SetCurrentWeapon leaves the old
// entity alive, and saved games keep it) still steps. Out of ammo (state 6) it
// reports itself unusable every tick (CBaseWeaponEntity::OnStep ->
// CPlayerPuppetEntity::CantBeUsed, Sam2Game 0xfa900), and for a weapon in
// neither hand the game then brings up its favorite weapon in the right hand:
// every pick switched straight back to it. Only held weapons are heard.
static WeaponPutDownTrace originalCantBeUsed = nullptr;
static void __fastcall cantBeUsed(void *p, void *, uint32_t weapon) {
    if (weapon && weapon != nativeHandle(p, 1) && weapon != nativeHandle(p, 0)) {
        static unsigned traced = 0;
        if (traced++ < 8) {
            void *w = resolve(weapon);
            log("Weapon in neither hand reported unusable id=%d state=%d (ignored)", w ? int(primaryField(w, 0xb4)) : -1,
                w ? int(primaryField(w, 0xb0)) : -1);
        }
        return;
    }
    originalCantBeUsed(p, weapon);
}
// Bounded trace of leaving a ride: who unregisters the rider (Sam2Game RVA),
// with its health, to tell Use, the exit sequence and the game's own ejections apart.
using RiderUnregister = void(__thiscall *)(void *);
static RiderUnregister originalUnregisterFromRide = nullptr;
static void __fastcall unregisterFromRide(void *p, void *) {
    static unsigned traced = 0;
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (p == copySnapshot().player && traced++ < 32)
        log("Ride left: unregistered by game+%lx health=%d ride=%08x", static_cast<unsigned long>(caller - weaponTraceBase),
            health ? health(p) : -1, primaryField(p, 0x544));
    originalUnregisterFromRide(p);
}
static int(__cdecl *originalAppForeground)() = nullptr;
// Spatial audio (spatial_audio.hpp): the sound API the options menu restarts
// (its startup is hooked at Direct3DCreate9) and the listener the game places
// each frame (the tracked head instead).
using SfxApiCall = void(__cdecl *)(int, long);
static SfxApiCall originalChangeApi = nullptr;
static void __cdecl changeSoundApi(int api, long frequency) {
    originalChangeApi(spatial_audio::chooseApi(api), frequency);
}
using SoundListener = void(__thiscall *)(void *, float, float, const Pose &, const Vec3 &);
static SoundListener originalSoundListener = nullptr;
static void __fastcall soundListener(void *list, void *, float a, float b, const Pose &placement, const Vec3 &velocity) {
    if (settings.spatialAudio) {
        const auto s = copySnapshot();
        Pose body;
        if (s.initialized && fresh(s.input) && s.input.headValid && livePlayer(s) && trackingAnchor(s.player, body)) {
            const Pose head = worldHeadTracking(body, s.origin, s.turn, s.input.head);
            if (finite(head)) spatial_audio::noteHead(head, GetTickCount64());
        }
        Pose listener;
        if (spatial_audio::listener(placement, listener)) { originalSoundListener(list, a, b, listener, velocity); return; }
    }
    originalSoundListener(list, a, b, placement, velocity);
}
static void maintainSpatialAudio() {
    spatial_audio::configure({settings.spatialAudio, settings.audioOcclusion});
    hud_wrist::configure(settings.hudMode == 1, settings.hudWristScale);
    spatial_audio::frame();
}
static int __cdecl appForeground() {
    maintainSpatialAudio();
    if (labLoadPending.exchange(false) && labLoadGame) {
        log("Lab load of Temp/SS2VR/user.sav issued");
        labLoadGame("Temp/SS2VR/user.sav");
        labSeenRide = 0;
        labLoadIssued = true;
    }
    if (vrInputLive())
        return 1;
    return originalAppForeground ? originalAppForeground() : 0;
}
// Native prompts ("Press {plcmdUse} to enter") name the bound keyboard key.
// While VR input is live the commands the controllers drive name their VR
// button instead; CInputBindings::SubstituteCommands copies the text first, so
// the rewritten copy only lives for the call.
using SubstituteCommands = void *(__thiscall *)(void *, void *, const char *);
static SubstituteCommands originalSubstitute = nullptr;
static void *__fastcall substituteCommands(void *bindings, void *, void *result, const char *text) {
    if (!text || !vrInputLive() || !std::strchr(text, '{')) return originalSubstitute(bindings, result, text);
    // Profiles with a jump button (Index): right A jumps, right B uses.
    const auto latest = copySnapshot().input;
    const bool jumpButton = ((latest.buttons[0] | latest.buttons[1]) & Button::JumpBound) != 0;
    const std::pair<const char *, const char *> names[] = {
        {"{plcmdUse}", jumpButton ? "B" : "A"}, {"{plcmdFire}", "the right trigger"}, {"{plcmdAltFire}", "the left trigger"},
        {"{plcmdReloadWeapon}", "left A"}, {"{plcmdDisplayNetricsa}", "the wheel's Netricsa bubble"},
        {"{plcmdTertFire}", "the right stick click"}, {"{plcmdY+}", jumpButton ? "A" : "B"}};
    std::string out = text;
    for (const auto &[from, to] : names)
        for (size_t at = out.find(from); at != std::string::npos; at = out.find(from, at + std::strlen(to)))
            out.replace(at, std::strlen(from), to);
    return originalSubstitute(bindings, result, out.c_str());
}
// Mouse look turns the native body under the tracked head: a desk mouse nudged
// mid-session, or the cursor the menu pointer moves, would pitch or turn the
// whole world. While VR input is live the mouse axes read as still; keys and
// mouse buttons are unchanged.
using ControlValue = float(__thiscall *)(void *, long);
static ControlValue originalMouseControl = nullptr;
static float __fastcall mouseControlValue(void *device, void *, long control) {
    if (control >= 0x78 && control <= 0x7b && vrInputLive()) return 0.f; // Mouse X+, X-, Y+, Y-
    return originalMouseControl(device, control);
}
static void __fastcall mountedLookClamp(void *brain, void *, Vec3 &look) {
#ifdef _MSC_VER
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    auto *previousObservation=activeRideControlObservation;
    auto *previousGrip=activeRideGrip;
    if(previousGrip)previousGrip->eligible=false;
    if(previousObservation) {previousObservation->declined=true;previousObservation->copied=false;}
    void *rideToken=nullptr;
    ControlSample captured;
    AcquireSRWLockShared(&controlsLock);
    captured = sampledControls;
    ReleaseSRWLockShared(&controlsLock);
    const auto snapshot = copySnapshot();
    uint32_t brainHandle = 0;
    if (hooksReady.load(std::memory_order_acquire) && caller == mountedClampReturn &&
        nativeMainThread && nativeMainThread() && captured.rider.seated() && quaternionEuler &&
        currentControls(0, snapshot, captured) &&
        finite(captured.origin)) {
        brainHandle = pointerHandle(brain);
        uint32_t playerHandle = 0, ownedBrain = 0;
        if (brainHandle && resolve(brainHandle) == brain) {
            memcpy(&playerHandle, static_cast<uint8_t *>(brain) + 0x28, 4);
            memcpy(&ownedBrain, static_cast<uint8_t *>(snapshot.player) + 0x38c, 4);
        }
        Pose anchor;
        if (playerHandle == captured.rider.player && ownedBrain == brainHandle &&
            nativeTrackingAnchor(snapshot.player, anchor, &captured.rider)) {
            if (seatedInTurret(captured.rider) && fresh(captured.input)) {
                // Turrets aim where the head looks. The right stick turns the view (the
                // turret's fixed view heading, updateRideSteadying), so the head and the
                // turret it drives turn together, until the turret reaches its limit and
                // stops following. Pitch is the head's alone. The native look is the
                // commanded aim the turret lerps toward, within its limits.
                static uint64_t turretClock = 0;
                const uint64_t now = GetTickCount64();
                const float dt = turretClock && now > turretClock ? std::min(.1f, float(now - turretClock) / 1000.f) : 0.f;
                turretClock = now;
                const Pose head = worldHeadTracking(anchor, captured.origin, captured.turn, captured.input.head);
                const Vec3 forward = rotate(normalize(head.q), Vec3{0, 0, -1});
                const float pitch = std::asin(std::clamp(forward.y, -1.f, 1.f)), heading = horizontalHeading(head.q);
                const auto nativeHeading = [&](float h, float &out) {
                    Vec3 converted;
                    if (quaternionEuler(&converted, yaw(h)) != &converted || !std::isfinite(converted.x)) return false;
                    out = converted.x;
                    return true;
                };
                float desired = 0;
                void *candidateRideToken = nullptr;
                if (finite(head) && std::isfinite(pitch) && std::isfinite(heading) && nativeHeading(heading, desired) &&
                    nativeRiderCurrent(snapshot.player, captured.rider, &candidateRideToken) && resolve(brainHandle) == brain) {
                    const float stick = captured.input.axis[1][0];
                    if (std::abs(stick) > .2f && dt > 0 && turretViewRide.load() == captured.rider.ride) {
                        // Push right turns right (heading decreases), at the smooth-turn rate.
                        const float delta = -std::copysign((std::abs(stick) - .2f) / .8f, stick) * settings.smoothTurnRate * dt;
                        float next = 0;
                        const float lag = std::abs(std::remainder(desired - look.x, 2 * Pi));
                        const bool stops = nativeHeading(heading + delta, next) &&
                            std::abs(std::remainder(next - look.x, 2 * Pi)) > std::max(lag, .35f);
                        if (!stops) turretViewTurn = std::remainder(turretViewTurn.load() + delta, 2 * Pi);
                    }
                    look.x = desired;
                    look.y = std::clamp(pitch, -1.4f, 1.4f);
                    rideToken = candidateRideToken;
                    static unsigned calls = 0;
                    if (calls++ % 50 == 0)
                        log("Turret head aim heading=%.3f pitch=%.3f view turn=%.3f", look.x, look.y, turretViewTurn.load());
                }
                if (!rideToken) nativeRiderCurrent(snapshot.player, captured.rider, &rideToken);
            } else if (steeringWheelHeading(look)) {
                nativeRiderCurrent(snapshot.player, captured.rider, &rideToken);
            } else if (creatureAimRide(captured.rider) && fresh(captured.input) && controllerTracked(captured.input, 1)) {
                // Aim at the hand laser's end from the dinosaur's eye (where its shots
                // start), so shots land on the dot; the hand's direction until the
                // laser has sampled. Native aim: heading as ours, pitch up positive
                // (CLeggedPuppetEntity::GetFrontDirection, Sam2Game 0x61060).
                const Pose hand = worldHandTracking(anchor, captured.origin, captured.turn, captured.input.head,
                                                    captured.input.hand[1]);
                Vec3 dir = rotate(normalize(hand.q), Vec3{0, 0, -1});
                AcquireSRWLockShared(&laserLock);
                const LaserAim beam = laserAim[1];
                ReleaseSRWLockShared(&laserLock);
                const uint64_t now = GetTickCount64();
                void *ride = resolve(captured.rider.ride);
                if (ride && beam.valid && beam.kind == LaserSourceKind::Vehicle && beam.weapon == captured.rider.ride &&
                    beam.owner == snapshot.playerHandle && now >= beam.tickMs && now - beam.tickMs < 250) {
                    Pose eye{};
                    reinterpret_cast<ViewOrigin>((*static_cast<uintptr_t **>(ride))[0xac / 4])(ride, &eye, 1);
                    // Only a dot well ahead along the hand's ray: one on something close
                    // (the dinosaur's own head) swung the aim from the eye's side of it.
                    const Vec3 d = beam.end - eye.p;
                    const float n = std::sqrt(dot(d, d));
                    if (finite(eye) && n > 3.f && std::isfinite(n) && dot(d * (1 / n), dir) > std::cos(.3f)) dir = d * (1 / n);
                }
                const float heading = std::atan2(-dir.x, -dir.z), pitch = std::asin(std::clamp(dir.y, -1.f, 1.f));
                Vec3 converted;
                if (finite(hand) && std::isfinite(heading) && std::isfinite(pitch) &&
                    quaternionEuler(&converted, yaw(heading)) == &converted && std::isfinite(converted.x) &&
                    nativeRiderCurrent(snapshot.player, captured.rider, &rideToken)) {
                    look.x = converted.x;
                    look.y = std::clamp(pitch, -1.4f, 1.4f);
                    static unsigned traced = 0;
                    if (traced++ % 200 == 0) log("Creature aim heading=%.3f pitch=%.3f", look.x, look.y);
                }
            } else if (ballRide(captured.rider) && fresh(captured.input) && ![] {
                           wchar_t v[2]{}; // Lab (SS2VR_LAB_BALL_NATIVE_LOOK=1): leave the ball's look alone.
                           return GetEnvironmentVariableW(L"SS2VR_LAB_BALL_NATIVE_LOOK", v, 2) == 1 && v[0] == L'1'; }()) {
                // The rolling ball rolls along its look (the stick's forward push is
                // relative to it): the look is where the head faces. The view keeps
                // its own heading (updateRideSteadying), so this never turns it.
                const Pose head = worldHeadTracking(anchor, captured.origin, captured.turn, captured.input.head);
                Vec3 converted;
                if (finite(head) && quaternionEuler(&converted, yaw(horizontalHeading(head.q))) == &converted &&
                    std::isfinite(converted.x) && nativeRiderCurrent(snapshot.player, captured.rider, &rideToken)) {
                    look.x = converted.x;
                    static unsigned traced = 0;
                    if (traced++ % 200 == 0) log("Ball look heading=%.3f", look.x);
                }
            } else if (fresh(captured.input)) {
                // Other rides (dinosaurs, hovercraft, cars without the wheel): the
                // right stick turns the ride's look while pushed, the right hand's
                // aim sets its pitch. Hand heading used to drive the look, and the
                // view (the ride's heading) turned with it: forward kept turning.
                // Added to the native look as mouse deltas are (see turrets): the
                // ride turns only while the stick is pushed, it never drifts.
                static uint64_t lookClock = 0;
                const uint64_t now = GetTickCount64();
                const float dt = lookClock && now > lookClock ? std::min(.1f, float(now - lookClock) / 1000.f) : 0.f;
                lookClock = now;
                // A ride with its own controls in the model (Vehicles ImmersiveGrips)
                // is turned by them alone; the right stick turns the others.
                const bool modelControls = settings.immersiveVehicleGrips && handleFrameRide == captured.rider.ride &&
                                           now - handleFrameTick < 1500;
                Vec3 rideLook = advanceTurretLook(look, modelControls ? 0.f : captured.input.axis[1][0],
                                                  settings.rideHeadPitch ? 0.f : captured.input.axis[1][1],
                                                  settings.smoothTurnRate, dt);

                // Guns pitch where the head looks (pitch up positive, as the native aim).
                if (settings.rideHeadPitch) {
                    const Pose head = worldHeadTracking(anchor, captured.origin, captured.turn, captured.input.head);
                    const Vec3 forward = rotate(normalize(head.q), Vec3{0, 0, -1});
                    const float pitch = std::asin(std::clamp(forward.y, -1.f, 1.f));
                    if (finite(head) && std::isfinite(pitch)) rideLook.y = std::clamp(pitch, -1.4f, 1.4f);
                }
                rideLook.z = look.z;
                {
                    static unsigned traced = 0;
                    if (traced++ % 200 == 0)
                        log("Ride aim heading=%.3f pitch=%.3f modelControls=%d", rideLook.x, rideLook.y, modelControls);
                }
                void *candidateRideToken=nullptr;
                if (std::isfinite(rideLook.x) && std::isfinite(rideLook.y) &&
                    nativeRiderCurrent(snapshot.player, captured.rider,&candidateRideToken) && resolve(brainHandle) == brain) {
                    look = rideLook;
                    rideToken=candidateRideToken;
                }
            }
            if(!rideToken)nativeRiderCurrent(snapshot.player,captured.rider,&rideToken);
        }
    }
    if((!labRideControlConfigured && !settings.immersiveVehicleGrips) || !rideToken || previousObservation ||
       primaryField(snapshot.player,0)!=swimmingPlayerVtable) {
        if(caller==mountedClampReturn && nativeMainThread && nativeMainThread())rideGrasp.interrupt();
        originalLookClamp(brain,look);return;
    }
    RideControlObservation observation;
    observation.rideToken=reinterpret_cast<uintptr_t>(rideToken); // Never dereferenced here.
    observation.lookToken=reinterpret_cast<uintptr_t>(&look);
    observation.thread=GetCurrentThreadId();
    RideGripFrame frame;
    AcquireSRWLockExclusive(&controlsLock);
    const bool submitted=rideGripFrames.submitted(snapshot.submittedWorld,snapshot.inputProducer,GetTickCount64(),frame);
    {
        static unsigned yes=0,no=0,reports=0;static uint64_t reported=0;
        ++(submitted?yes:no);
        if(GetTickCount64()-reported>=3000 && reports<40) {
            reported=GetTickCount64();++reports;
            log("Ride grip frames submitted=%u missing=%u active=%u pending=%u,%u receipt=%u,%llu",yes,no,rideGripFrames.active.valid,
                rideGripFrames.pending[0].valid,rideGripFrames.pending[1].valid,snapshot.submittedWorld.active,
                (unsigned long long)snapshot.submittedWorld.requestSequence);
            yes=no=0;
        }
    }
    ReleaseSRWLockExclusive(&controlsLock);
    RideGripBorrow grip{&snapshot,&captured,&frame,brain,brainHandle,snapshot.submittedWorld.continuity,submitted,false};
    const float fallbackHeading=look.x;
    withNativeFinally([&] {
        activeRideControlObservation=&observation;
        activeRideGrip=&grip;
        originalLookClamp(brain,look); // Preserve native clamp/ClientAction exactly once.
        if(grip.eligible && observation.publishable())rideGrasp.accept(look.x);
        finishRideHeading(look.x,fallbackHeading,grip.headingIntent,grip.eligible && observation.publishable());
    },[&](bool aborted) noexcept {
        activeRideControlObservation=previousObservation;
        activeRideGrip=previousGrip;
        // A tick without a matching submitted frame (render cadence, source age)
        // holds the grasp without steering; only a lasting gap releases it. Every
        // failed check after admission already cancelled it.
        static uint64_t lastFrameMs=0;
        const uint64_t tickMs=GetTickCount64();
        if(submitted)lastFrameMs=tickMs;
        if(aborted || observation.declined || !observation.copied || (!submitted && tickMs-lastFrameMs>250))
            rideGrasp.interrupt();
        observation.finishOuter(aborted);
    });
    if(labRideControlConfigured && observation.publishable() && !grip.headingIntent) {
        auto ordinal=labRideControlRows.load(std::memory_order_relaxed);
        while(ordinal<64 && !labRideControlRows.compare_exchange_weak(ordinal,ordinal+1,std::memory_order_relaxed)) {}
        if(ordinal<64) {
            const auto &row=observation.values;
            log("Lab rideControl schema=1 source=%.*s ordinal=%u input=%llu generation=%u session=%u reference=%u player=%u ride=%u seat=%u brain=%u thread=%u class=%u mode=%u executionAbilities=%u movementAbilities=%u parameterToken=%u renderableToken=%u callbackCalls=%u callbackReturned=1 resourceAssociated=0 frameAssociated=0 steeringApplied=0",
                64,ss2vrBuildContract.sourceFingerprint.data(),ordinal+1,captured.input.sequence,captured.generation,
                captured.input.session,captured.input.reference,captured.rider.player,captured.rider.ride,
                captured.rider.seat,brainHandle,observation.thread,row.classRva,row.mode,
                row.executionAbilities,row.movementAbilities,row.parameterToken,row.renderableToken,observation.calls);
        }
    }
}

static bool respawnRoute(const Snapshot &s);
static bool netricsaOpen(const Snapshot &s);
static void __fastcall poll(void *b, void *) {
    pollSettingsFile();
    originalPoll(b);
    if (!ids[0])
        for (int i = 0; i < CommandCount; i++)
            toId(&ids[i], commands[i]);
    void *data = getData(b);
    if (!data)
        return;
    auto entries = *reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(data) + 4);
    int count = *reinterpret_cast<int *>(static_cast<uint8_t *>(data) + 8);
    if (!entries || count < 1 || count > 1024)
        return;
    bool isPlayer = false;
    for (int i = 0; i < count; i++)
        if (*reinterpret_cast<uint32_t *>(entries + i * 0x40) == ids[5]) {
            isPlayer = true;
            break;
        }
    if (!isPlayer)
        return;
    auto s = copySnapshot();
    bool enabled = livePlayer(s) && s.initialized && fresh(s.input) && !recenterHeld(s.input) &&
                   s.ui.gameplay && GetTickCount64() - s.ui.tickMs < 200 && nativeRiderCurrent(s.player, s.rider);
    bool fire[2]{s.fire[0], s.fire[1]};
    const bool client = multiplayer::remoteClient();
    if (enabled && client && s.rider.handheld())
        for (unsigned h = 0; h < 2; ++h)
            fire[h] = fire[h] && multiplayer::localPrimaryAllowed(s.player, h, s.intentEpoch[h],
                s.input.primaryInputGeneration[h], s.input.sequence);
    updateSteeringWheel(s); // Before controlsLock: may load a native texture.
    loadWeaponWheelTextures(s);
    loadWeaponWheelModels(s);
    const bool respawn = respawnRoute(s), netricsa = netricsaOpen(s);
    AcquireSRWLockExclusive(&controlsLock);
    playerBindings = b;
    std::copy(std::begin(respawnValues), std::end(respawnValues), std::begin(respawnPrior));
    std::fill(std::begin(respawnValues), std::end(respawnValues), 0);
    if (respawn || netricsa) {
        if (respawn) {
            respawnValues[5] = primaryActionEligible(s.input, 1) && s.input.trigger[1] > .5f ? 1 : 0;
            respawnValues[6] = primaryActionEligible(s.input, 0) && s.input.trigger[0] > .5f ? 1 : 0;
            respawnValues[7] = ((s.input.buttons[0] | s.input.buttons[1]) & Use) ? 1 : 0;
        }
        if (netricsa)
            // The wheel-open button (Netricsa's old Touch Y / Index trackpad) also closes it.
            respawnValues[15] = ((s.input.buttons[0] | s.input.buttons[1]) & (Netricsa | WheelOpen)) ? 1 : 0;
        respawnTick = GetTickCount64();
    } else {
        std::fill(std::begin(respawnPrior), std::end(respawnPrior), 0);
        respawnTick = 0;
    }
    const bool sameContext = sampledControls.rider == s.rider && sampledControls.generation == s.generation &&
        sampledControls.input.session == s.input.session && sampledControls.input.reference == s.input.reference;
    if (sameContext)
        memcpy(priorValues, values, sizeof(values));
    else
        std::fill(std::begin(priorValues), std::end(priorValues), 0);
    for (unsigned hand = 0; hand < 2; ++hand)
        if (!primaryCommandHistoryCompatible(sampledControls.input, s.input, hand,
                sampledControls.intentEpoch[hand], s.intentEpoch[hand], s.rider.handheld()))
            priorValues[hand == 1 ? 5 : 6] = 0;
    sampledControls = {s.rider, s.generation, {s.intentEpoch[0], s.intentEpoch[1]},
        s.input, s.origin, s.turn, client, enabled};
    sampledControls.producer=s.inputProducer;sampledControls.rigRevision=s.rigRevision;
    std::copy_n(s.gripPoseGeneration,2,sampledControls.gripPoseGeneration);
    std::fill(std::begin(values), std::end(values), 0);
    if (enabled) {
        // A hand-aimed dinosaur moves relative to its aim (the right hand): the
        // stick is turned by the head's heading from it, so forward is where you face.
        const bool handAim = creatureAimRide(s.rider) && s.input.headValid && controllerTracked(s.input, 1);
        const float creatureOffset = handAim ? std::remainder(yawAngle(s.input.head.q) - yawAngle(s.input.hand[1].q), 2 * Pi) : 0.f;
        auto direction = horizontalStickMovement(s.input, s.ui.wheel[0].open,
                                                s.rider.seated() ? (std::isfinite(creatureOffset) ? creatureOffset : 0.f)
                                                                 : moveHeading(s));
        if (steeringWheel.steering == WheelSteering::Axis && steeringWheel.grasp.steering())
            direction.x = steeringWheel.grasp.steer(settings.steeringWheel); // Wheel replaces stick steering.
        // Verified native labels: X+ right, X- left, Z- forward, Z+ backward.
        values[0] = std::max(0.f, direction.x);
        values[1] = std::max(0.f, -direction.x);
        values[2] = std::max(0.f, direction.z);
        values[3] = std::max(0.f, -direction.z);
        values[4] = s.jump ? 1 : 0;
        const bool coupled = coupledNativeFire.load(std::memory_order_relaxed) && s.handle[0] && s.handle[1] &&
            s.rider.handheld();
        // A grip-held object is thrown by releasing the grip, never by the native Fire charge.
        const bool carrying = gripCarrying.load(std::memory_order_relaxed);
        values[5] = (!carrying && (fire[1] || (coupled && fire[0]))) ? 1 : 0;
        values[6] = (!carrying && fire[0] && !coupled) ? 1 : 0;
        values[7] = s.use ? 1 : 0;
        values[8] = s.sprint ? 1 : 0;
        const uint32_t buttons = s.input.buttons[0] | s.input.buttons[1];
        values[14] = (buttons & Reload) ? 1 : 0;
        // Wheel bubbles press their native command for exactly one poll.
        const uint64_t now = GetTickCount64();
        values[15] = ((buttons & Netricsa) || takeWheelRequest(WheelBubble::Netricsa, now)) ? 1 : 0;
        values[16] = takeWheelRequest(WheelBubble::QuickSave, now) ? 1 : 0;
        values[17] = takeWheelRequest(WheelBubble::QuickLoad, now) ? 1 : 0;
        values[18] = s.grenade ? 1 : 0; // Throw hand grenade.
    }
    const unsigned traceState = unsigned(enabled) | unsigned(values[5] > 0) << 1 | unsigned(values[6] > 0) << 2 |
        unsigned(values[7] > 0) << 3 | unsigned(values[4] > 0) << 4 | unsigned(respawn) << 5 | unsigned(netricsa) << 6 |
        unsigned(s.rider.seated()) << 7;
    ReleaseSRWLockExclusive(&controlsLock);
    static unsigned traced = 0, previousTrace = ~0u;
    static bool previousSeated = false;
    if (s.rider.seated() != previousSeated) { previousSeated = s.rider.seated(); traced = std::min(traced, 120u); }
    if (traceState != previousTrace && traced < 160) {
        previousTrace = traceState;
        ++traced;
        log("Input poll enabled=%d live=%d init=%d fresh=%d recenter=%d gameplay=%d uiAge=%llu rider=%d seated=%d state=%u "
            "fire=%d,%d values=%.0f,%.0f use=%d jump=%d respawn=%d netricsa=%d", enabled, livePlayer(s), s.initialized, fresh(s.input),
            recenterHeld(s.input), s.ui.gameplay, static_cast<unsigned long long>(GetTickCount64() - s.ui.tickMs),
            nativeRiderCurrent(s.player, s.rider), s.rider.seated(), unsigned(s.rider.state), fire[1], fire[0], values[5], values[6],
            s.use, s.jump, respawn, netricsa);
    }
}
static int commandIndex(void *b, uint32_t id) {
    if (b != playerBindings)
        return -1;
    for (int i = 0; i < CommandCount; i++)
        if (ids[i] == id)
            return i;
    return -1;
}
static bool respawnRoute(const Snapshot &s) {
    return livePlayer(s) && s.initialized && local(s.player) && isAlive && !isAlive(s.player) &&
           fresh(s.input) && !recenterHeld(s.input) && !menus::active();
}
static bool netricsaOpen(const Snapshot &s) {
    return livePlayer(s) && s.initialized && local(s.player) && isNetricsa && isNetricsa(s.player) &&
           fresh(s.input) && !recenterHeld(s.input) && !menus::active();
}
static bool respawnCommand(int i, const Snapshot &s) {
    if (!respawnTick || GetTickCount64() - respawnTick >= 200) return false;
    if (i == 15) return netricsaOpen(s);
    return (i == 5 || i == 6 || i == 7) && respawnRoute(s);
}
static float __fastcall commandValue(void *b, void *, uint32_t id) {
    auto snapshot = copySnapshot();
    if (!vrSession(snapshot))
        return originalValue(b, id);
    {
        AcquireSRWLockShared(&controlsLock);
        const int i = commandIndex(b, id);
        const bool respawn = respawnCommand(i, snapshot);
        const float v = respawn ? respawnValues[i] : 0;
        ReleaseSRWLockShared(&controlsLock);
        if (respawn) return v;
    }
    bool enabled = fresh(snapshot.input) && !recenterHeld(snapshot.input) && snapshot.ui.gameplay &&
                   GetTickCount64() - snapshot.ui.tickMs < 200 && livePlayer(snapshot);
    AcquireSRWLockShared(&controlsLock);
    int i = commandIndex(b, id);
    float v = i < 0 || !enabled ? 0 : values[i];
    const auto captured = sampledControls;
    ReleaseSRWLockShared(&controlsLock);
    if (!currentControls(i, snapshot, captured, v > 0))
        v = 0;
    return i < 0 || (i >= FirstPassThroughCommand && v <= 0) ? originalValue(b, id) : v;
}
static int query(void *b, uint32_t id, int mode, CommandInt original) {
    auto snapshot = copySnapshot();
    if (!vrSession(snapshot))
        return original(b, id);
    {
        AcquireSRWLockShared(&controlsLock);
        const int i = commandIndex(b, id);
        const bool respawn = respawnCommand(i, snapshot);
        const float v = respawn ? respawnValues[i] : 0, prev = respawn ? respawnPrior[i] : 0;
        ReleaseSRWLockShared(&controlsLock);
        if (respawn) return nativeCommandQuery(v, prev, mode, true);
    }
    bool enabled = fresh(snapshot.input) && !recenterHeld(snapshot.input) && snapshot.ui.gameplay &&
                   GetTickCount64() - snapshot.ui.tickMs < 200 && livePlayer(snapshot);
    AcquireSRWLockShared(&controlsLock);
    int i = commandIndex(b, id);
    float v = i < 0 || !enabled ? 0 : values[i], prev = i < 0 ? 0 : priorValues[i];
    const auto captured = sampledControls;
    ReleaseSRWLockShared(&controlsLock);
    if (i < 0 || (i >= FirstPassThroughCommand && v <= 0 && prev <= 0))
        return original(b, id);
    return nativeCommandQuery(v, prev, mode, currentControls(i, snapshot, captured, v > 0));
}
static int __fastcall commandDown(void *b, void *, uint32_t id) {
    return query(b, id, 0, originalDown);
}
static int __fastcall commandPressed(void *b, void *, uint32_t id) {
    return query(b, id, 1, originalPressed);
}
static int __fastcall commandReleased(void *b, void *, uint32_t id) {
    return query(b, id, 2, originalReleased);
}
static int __fastcall commandRepeated(void *b, void *, uint32_t id) {
    return query(b, id, 1, originalRepeated);
}
struct LocalRoomscaleAttempt {
    SimulationInterval *interval=nullptr;
    Snapshot snapshot;
    DWORD thread=0;
    uintptr_t world=0;
    uint32_t physics=0,worldInfoHandle=0,brainHandle=0;
    uintptr_t worldInfo=0,brain=0;
    uint64_t simulationSequence=0,ticket=0;
    roomscale::BodyGeometry body;
    roomscale::BodySweepCover cover;
    Pose anchorBefore{},anchorAfter{},targetModel{},candidateRoot{};
    roomscale::OriginSettlement settlement;
    float maximumMove=0,contactBudget=0,errorBudget=0;
    bool failed=false,prepared=false;
    NativeReadLease reads;
};
static bool roomscaleWord(uintptr_t object,uintptr_t offset,uint32_t &value) noexcept {
    if(!object||offset>UINTPTR_MAX-object||object+offset>UINTPTR_MAX-4||
       !readableMemory(reinterpret_cast<void*>(object+offset),4))return false;
    std::memcpy(&value,reinterpret_cast<void*>(object+offset),4);return true;
}
static bool roomscaleOwnedWord(const LocalRoomscaleAttempt& c,uintptr_t object,uintptr_t offset,uint32_t& value) noexcept {
    if(!object||offset>UINTPTR_MAX-object||object+offset>UINTPTR_MAX-4||
       !c.reads.contains(reinterpret_cast<void*>(object+offset),4))return false;
    std::memcpy(&value,reinterpret_cast<void*>(object+offset),4);return true;
}
static bool roomscaleCleanupReady(const LocalRoomscaleAttempt& c) noexcept {
    if(!roomscaleEngineBase)return false;
    const uint32_t base=uint32_t(roomscaleEngineBase);
    NativeRayCleanupList list;
    list.head=base+0x2f1a14;
    if(!roomscaleOwnedWord(c,list.head,0,list.first)||!roomscaleOwnedWord(c,list.head,4,list.sentinelNext)||
       !roomscaleOwnedWord(c,list.head,8,list.last))return false;
    const std::array<uint32_t,2> links{base+0x2d9754,base+0x2eab2c};
    const std::array<uint32_t,2> tables{base+0x209260,base+0x214700};
    const std::array<uint32_t,2> callbacks{base+0x28b50,base+0xd8880};
    for(unsigned i=0;i<2;++i) {
        auto& node=list.nodes[i];node.link=links[i];
        if(!roomscaleOwnedWord(c,links[i]-4,0,node.vtable)||node.vtable!=tables[i]||
           !roomscaleOwnedWord(c,tables[i],4,node.callback)||
           !roomscaleOwnedWord(c,links[i],0,node.next)||!roomscaleOwnedWord(c,links[i],4,node.previous))return false;
    }
    if(!knownNativeRayCleanup(list,links,tables,callbacks))return false;
    // Normal cld traversal releases its visited array. Cleanup callbacks do not
    // repair an interrupted traversal, so a retained pointer/count is refused.
    for(uintptr_t offset:{0x2d9708u,0x2d970cu,0x2d9710u}) {
        uint32_t value=0;if(!roomscaleOwnedWord(c,base,offset,value)||value)return false;
    }
    return laserModelScratchIdle();
}
static bool roomscalePhaseCurrent(const LocalRoomscaleAttempt& c,bool checkControls=true) noexcept {
    if(!roomscaleConfigured||!roomscaleBusy||roomscaleFaulted.load()||
       nativeRayFaulted.load(std::memory_order_acquire)||!hooksReady.load(std::memory_order_acquire)||
       !nativeMainThread||!nativeMainThread()||GetCurrentThreadId()!=c.thread||
       simulationThread.load()!=c.thread||simulationInterval!=c.interval||!c.interval||
       c.interval->previous||!c.interval->managerPrepared||!c.interval->failure||*c.interval->failure||
       c.interval->nativeCaller!=roomscaleSimulationReturn||simulationRevision!=c.simulationSequence||
       !c.world||!c.physics||activeNativeRay||laserQuerying||snapshotNativeReadDepth||
       eyeIndex>=0||scopeSource.active||physicalWeapon||nativeShotDepth||primaryInvocation||
       zoomManagerFrame||zoomContext||uiOwnerDepth||uiOverlayDepth||executingView||
       !nativePresentationIdleForBodyMove())return false;
    uint32_t value=0,pool=0;
    const bool phase=roomscaleOwnedWord(c,roomscaleEngineBase,0x2f1a70,value)&&value==reinterpret_cast<uintptr_t>(c.interval->simulation)&&
        roomscaleOwnedWord(c,reinterpret_cast<uintptr_t>(c.interval->simulation),0x4c,value)&&value==0&&
        roomscaleOwnedWord(c,roomscaleEngineBase,0x2f1b3c,value)&&value==c.world&&
        c.world==reinterpret_cast<uintptr_t>(c.interval->preparedWorld)&&
        roomscaleOwnedWord(c,c.world,4,value)&&!(value&1u)&&
        roomscaleOwnedWord(c,c.world,0x74,value)&&value==reinterpret_cast<uintptr_t>(c.interval->preparedManager)&&
        roomscaleOwnedWord(c,c.world,0x6c,value)&&value==c.physics&&
        roomscaleOwnedWord(c,roomscaleEngineBase,0x2ec948,pool)&&pool&&
        roomscaleOwnedWord(c,pool,0x10,value)&&value==0;
    if(!phase||!checkControls)return phase;
    // Brain RenderView selects an external world camera before the player's
    // view. Refuse even a stale nonzero camera handle, rather than move the
    // puppet behind a cinematic or take over an unrecognized controller.
    return c.worldInfoHandle&&c.worldInfo&&c.brainHandle&&c.brain&&
        roomscaleOwnedWord(c,c.world,0x64,value)&&value==c.worldInfoHandle&&
        resolve(value)==reinterpret_cast<void*>(c.worldInfo)&&
        roomscaleOwnedWord(c,c.worldInfo,0,value)&&value==roomscaleWorldInfoTable&&
        roomscaleOwnedWord(c,c.worldInfo,0x10,value)&&!(value&2u)&&
        roomscaleOwnedWord(c,c.worldInfo,0x6c,value)&&value==0&&
        roomscaleOwnedWord(c,reinterpret_cast<uintptr_t>(c.snapshot.player),0x38c,value)&&value==c.brainHandle&&
        resolve(value)==reinterpret_cast<void*>(c.brain)&&
        roomscaleOwnedWord(c,c.brain,0,value)&&value==roomscaleBrainTable&&
        roomscaleOwnedWord(c,c.brain,0x10,value)&&!(value&2u)&&
        roomscaleOwnedWord(c,c.brain,0x28,value)&&value==c.snapshot.playerHandle;
}
// Lab diagnostic only, evaluated after roomscalePhaseCurrent already failed:
// the first failing term in its original order (0 when it now passes).
static uint32_t roomscalePhaseFailure(const LocalRoomscaleAttempt& c,bool checkControls=true) noexcept {
    uint32_t n=0;
    const auto term=[&](bool ok) {++n;return !ok;};
    if(term(roomscaleConfigured)||term(roomscaleBusy)||term(!roomscaleFaulted.load())||
       term(!nativeRayFaulted.load(std::memory_order_acquire))||term(hooksReady.load(std::memory_order_acquire))||
       term(nativeMainThread&&nativeMainThread())||term(GetCurrentThreadId()==c.thread)||
       term(simulationThread.load()==c.thread)||term(simulationInterval==c.interval&&c.interval)||
       term(!c.interval->previous&&c.interval->managerPrepared&&c.interval->failure&&!*c.interval->failure)||
       term(c.interval->nativeCaller==roomscaleSimulationReturn)||term(simulationRevision==c.simulationSequence)||
       term(c.world&&c.physics)||term(!activeNativeRay)||term(!laserQuerying)||term(!snapshotNativeReadDepth)||
       term(eyeIndex<0)||term(!scopeSource.active)||term(!physicalWeapon)||term(!nativeShotDepth)||
       term(!primaryInvocation)||term(!zoomManagerFrame)||term(!zoomContext)||term(!uiOwnerDepth)||
       term(!uiOverlayDepth)||term(!executingView)||term(nativePresentationIdleForBodyMove()))return n;
    uint32_t value=0,pool=0;
    if(term(roomscaleOwnedWord(c,roomscaleEngineBase,0x2f1a70,value)&&value==reinterpret_cast<uintptr_t>(c.interval->simulation))||
       term(roomscaleOwnedWord(c,reinterpret_cast<uintptr_t>(c.interval->simulation),0x4c,value)&&value==0)||
       term(roomscaleOwnedWord(c,roomscaleEngineBase,0x2f1b3c,value)&&value==c.world)||
       term(c.world==reinterpret_cast<uintptr_t>(c.interval->preparedWorld))||
       term(roomscaleOwnedWord(c,c.world,4,value)&&!(value&1u))||
       term(roomscaleOwnedWord(c,c.world,0x74,value)&&value==reinterpret_cast<uintptr_t>(c.interval->preparedManager))||
       term(roomscaleOwnedWord(c,c.world,0x6c,value)&&value==c.physics)||
       term(roomscaleOwnedWord(c,roomscaleEngineBase,0x2ec948,pool)&&pool)||
       term(roomscaleOwnedWord(c,pool,0x10,value)&&value==0))return n;
    if(!checkControls)return 0;
    if(term(c.worldInfoHandle&&c.worldInfo&&c.brainHandle&&c.brain)||
       term(roomscaleOwnedWord(c,c.world,0x64,value)&&value==c.worldInfoHandle&&resolve(value)==reinterpret_cast<void*>(c.worldInfo))||
       term(roomscaleOwnedWord(c,c.worldInfo,0,value)&&value==roomscaleWorldInfoTable)||
       term(roomscaleOwnedWord(c,c.worldInfo,0x10,value)&&!(value&2u))||
       term(roomscaleOwnedWord(c,c.worldInfo,0x6c,value)&&value==0)||
       term(roomscaleOwnedWord(c,reinterpret_cast<uintptr_t>(c.snapshot.player),0x38c,value)&&value==c.brainHandle&&
            resolve(value)==reinterpret_cast<void*>(c.brain))||
       term(roomscaleOwnedWord(c,c.brain,0,value)&&value==roomscaleBrainTable)||
       term(roomscaleOwnedWord(c,c.brain,0x10,value)&&!(value&2u))||
       term(roomscaleOwnedWord(c,c.brain,0x28,value)&&value==c.snapshot.playerHandle))return n;
    return 0;
}
static bool roomscaleSnapshotMatches(const Snapshot& a,const Snapshot& b) noexcept {
    return a.player==b.player&&a.playerHandle==b.playerHandle&&a.rider==b.rider&&
        a.generation==b.generation&&a.rigRevision==b.rigRevision&&
        a.inputProducer==b.inputProducer&&a.input.session==b.input.session&&
        a.input.reference==b.input.reference&&a.input.sequence==b.input.sequence&&
        a.turn==b.turn&&std::memcmp(&a.origin,&b.origin,sizeof(Pose))==0;
}
static bool captureRoomscaleBody(const LocalRoomscaleAttempt& c,roomscale::BodyGeometry& body) {
    const roomscale::BodyLayout layout{uint32_t(roomscaleEngineBase+0x217af0),
                                       uint32_t(roomscaleEngineBase+0x209268)};
    return roomscale::readBodyGeometry([&](uint32_t address,void* target,size_t size) {
        if(!c.reads.contains(reinterpret_cast<void*>(address),size))return false;
        std::memcpy(target,reinterpret_cast<void*>(address),size);return true;
    },[](uint32_t handle) {return uint32_t(reinterpret_cast<uintptr_t>(resolve(handle)));},
       layout,c.snapshot.playerHandle,body);
}
static bool __cdecl roomscaleOwnerCurrent(void *opaque) noexcept {
    auto& c=*static_cast<LocalRoomscaleAttempt*>(opaque);
    if(c.failed||!roomscalePhaseCurrent(c)||!roomscaleCleanupReady(c))return false;
    const auto live=copySnapshot();
    if(!roomscaleSnapshotMatches(c.snapshot,live)||!vrSession(live)||!fresh(live.input)||
       !live.ui.gameplay||!live.rider.handheld()||recenterHeld(live.input)||!livePlayer(live)||
       !local(live.player)||(!singlePlayer()&&!multiplayer::server()&&!multiplayer::negotiatedLocal()))return false;
    roomscale::BodyGeometry body;
    return captureRoomscaleBody(c,body)&&roomscale::sameBodyGeometry(c.body,body);
}
static bool __cdecl prepareRoomscaleMove(void *opaque) {
    auto& c=*static_cast<LocalRoomscaleAttempt*>(opaque);
    if(!roomscalePhaseCurrent(c)||!roomscaleCleanupReady(c)||!captureRoomscaleBody(c,c.body))return false;
    const auto relative=relativeTracking(c.snapshot.origin,c.snapshot.input.head);
    if(!finite(relative)||!finite(c.anchorBefore))return false;
    const auto bounded=boundHeadTranslation(relative.p);
    if(bounded.x!=relative.p.x||bounded.y!=relative.p.y||bounded.z!=relative.p.z)return false;
    const auto head=worldHeadTracking(c.anchorBefore,c.snapshot.origin,c.snapshot.turn,c.snapshot.input.head);
    Vec3 delta=head.p-c.anchorBefore.p;delta.y=0;
    const double distance=std::sqrt(double(delta.x)*delta.x+double(delta.z)*delta.z);
    float minimumRadius=std::numeric_limits<float>::max();
    for(unsigned i=0;i<c.body.hullCount;++i)minimumRadius=std::min(minimumRadius,c.body.hulls[i].primitive.width*.5f);
    if(!std::isfinite(distance)||!std::isfinite(minimumRadius)||minimumRadius<=0||distance<=minimumRadius*.002f)return false;
    const float limit=std::min(.25f,minimumRadius*.5f);
    if(distance>limit)delta=delta*float(double(limit)/distance);
    c.maximumMove=limit+minimumRadius*.02f;
    c.contactBudget=minimumRadius*.002f;
    c.errorBudget=std::max(.00003f,minimumRadius*.004f);
    c.targetModel=c.body.modelPose;c.targetModel.p=c.targetModel.p+delta;
    c.prepared=finite(c.targetModel)&&roomscaleOwnerCurrent(&c);
    if(c.prepared)c.reads.seal();
    return c.prepared;
}
static bool __cdecl checkRoomscaleTargetMath(void *opaque) {
    auto& c=*static_cast<LocalRoomscaleAttempt*>(opaque);
    if(!roomscaleOwnerCurrent(&c))return false;
    const Vec3 delta=c.candidateRoot.p-c.body.rootPose.p;
    const double distance=std::sqrt(double(delta.x)*delta.x+double(delta.y)*delta.y+double(delta.z)*delta.z);
    if(!std::isfinite(distance)||distance>c.maximumMove)return false;
    c.cover=roomscale::coverBodyRootSweep(c.body,c.candidateRoot,.04f);
    if(!c.cover.valid)return false;
    Pose predicted=c.anchorBefore;
    predicted.p=predicted.p+(c.targetModel.p-c.body.modelPose.p);
    if(!roomscale::settleTranslatedAnchor(c.snapshot.origin,c.snapshot.turn,c.snapshot.input.head,
        c.anchorBefore,predicted,c.maximumMove,c.errorBudget).valid)return false;
    if(!runRoomscaleSweepQueries(c.body,c.cover,c.contactBudget,c.thread,roomscaleOwnerCurrent,&c,c.failed)||
       !roomscaleOwnerCurrent(&c))return false;
    AcquireSRWLockExclusive(&snapshotLock);
    if(roomscaleSnapshotMatches(c.snapshot,current)&&rigPublication.usable(current.rigRevision)&&
       c.interval->failure&&!*c.interval->failure) {
        c.ticket=rigPublication.begin(current.rigRevision);
        if(c.ticket) {current.rigRevision=c.ticket;rigMutationActive=true;}
    }
    ReleaseSRWLockExclusive(&snapshotLock);
    if(c.ticket)c.reads.clear(); // The original commit is outside the read-only lease.
    return c.ticket!=0;
}
static bool __cdecl checkRoomscaleTarget(void *opaque,const Pose& target) noexcept {
    auto& c=*static_cast<LocalRoomscaleAttempt*>(opaque);
    std::memcpy(&c.candidateRoot,&target,sizeof(target));
    return runRoomscaleMathFrame(c.failed,c.thread,checkRoomscaleTargetMath,&c);
}
static bool sameRoomscalePose(const Pose& a,const Pose& b) noexcept {
    return a.q.x==b.q.x&&a.q.y==b.q.y&&a.q.z==b.q.z&&a.q.w==b.q.w&&
        a.p.x==b.p.x&&a.p.y==b.p.y&&a.p.z==b.p.z;
}
static bool __cdecl settleRoomscaleMoveMath(void *opaque) {
    auto& c=*static_cast<LocalRoomscaleAttempt*>(opaque);
    if(!roomscalePhaseCurrent(c,false))return false;
    roomscale::BodyGeometry after;
    if(!captureRoomscaleBody(c,after)||after.player!=c.body.player||after.playerHandle!=c.body.playerHandle||
       after.mechanism!=c.body.mechanism||after.mechanismHandle!=c.body.mechanismHandle||
       after.root!=c.body.root||after.rootHandle!=c.body.rootHandle||after.parts!=c.body.parts||
       after.model!=c.body.model||after.modelHandle!=c.body.modelHandle||
       after.rootFlags!=c.body.rootFlags||after.hullCount!=c.body.hullCount||
       !sameRoomscalePose(after.rootPose,c.candidateRoot))return false;
    for(unsigned i=0;i<after.hullCount;++i) {
        const auto& a=after.hulls[i];const auto& b=c.body.hulls[i];
        if(a.address!=b.address||a.category!=b.category||a.flags!=b.flags||
           std::memcmp(&a.primitive,&b.primitive,sizeof(a.primitive))||
           std::memcmp(&a.relativePose,&b.relativePose,sizeof(a.relativePose)))return false;
    }
    c.settlement=roomscale::settleTranslatedAnchor(c.snapshot.origin,c.snapshot.turn,c.snapshot.input.head,
        c.anchorBefore,c.anchorAfter,c.maximumMove,c.errorBudget);
    return c.settlement.valid;
}
static void finishRoomscaleRevision(LocalRoomscaleAttempt& c,bool moved,bool settled) noexcept {
    if(!c.ticket)return;
    AcquireSRWLockExclusive(&snapshotLock);
    if(rigPublication.current()==c.ticket&&current.rigRevision==c.ticket) {
        const bool resetOwner=!current.initialized||current.player!=c.snapshot.player||
            current.playerHandle!=c.snapshot.playerHandle;
        const bool sameOrigin=current.inputProducer==c.snapshot.inputProducer&&
            current.input.session==c.snapshot.input.session&&current.input.reference==c.snapshot.input.reference&&
            std::memcmp(&current.turn,&c.snapshot.turn,sizeof(float))==0&&
            std::memcmp(&current.origin,&c.snapshot.origin,sizeof(Pose))==0;
        const auto action=roomscale::publicationAction(true,moved,resetOwner,settled,sameOrigin);
        if(action==roomscale::PublicationAction::retireWithoutOrigin||
           action==roomscale::PublicationAction::publishOrigin) {
            if(action==roomscale::PublicationAction::publishOrigin)
                std::memcpy(&current.origin,&c.settlement.origin,sizeof(Pose));
            current.rigRevision=c.ticket+1;
            if(!rigPublication.finish(c.ticket)) {
                current.rigRevision=rigPublication.current();roomscaleFaulted.store(true);nativeInputFailed();
            }
        } else {
            // No native getters, rollback or retry in this path, including on
            // native unwind. Ordinary fresh-origin recovery follows the existing
            // interrupted-input epoch boundary; further body moves stay off.
            roomscaleFaulted.store(true);nativeInputFailed();
            current.generation=advanceEpochUnlocked();
            current.ui.trackingGeneration=current.generation;
            current.ui.gameplay=0;
        }
    }
    ReleaseSRWLockExclusive(&snapshotLock);
}
static __attribute__((noinline)) void runPostSimulationRoomscale(SimulationInterval &interval) {
    if(!settings.roomscale||!roomscaleConfigured||roomscaleBusy||roomscaleFaulted.load()||
       interval.previous||!interval.managerPrepared||!nativeInputHealthy())return;
    LocalRoomscaleAttempt c;
    c.interval=&interval;c.snapshot=copySnapshot();c.thread=GetCurrentThreadId();
    c.simulationSequence=simulationRevision;c.world=reinterpret_cast<uintptr_t>(interval.preparedWorld);
    if(!vrSession(c.snapshot)||!fresh(c.snapshot.input)||!c.snapshot.ui.gameplay||
       !c.snapshot.rider.handheld()||c.snapshot.ui.wheel[0].open||c.snapshot.ui.wheel[1].open||
       recenterHeld(c.snapshot.input)||!livePlayer(c.snapshot)||!local(c.snapshot.player)||
       (!singlePlayer()&&!multiplayer::server()&&!multiplayer::negotiatedLocal())||
       !roomscaleWord(c.world,0x6c,c.physics)||!c.physics)return;
    // Only the controlling XR machine applies this checked physical move.
    // Remote puppets and headless servers keep ordinary native ClientAction,
    // discrepancy/correction and physics; there is no second authority payment.
    auto* prepared=preparedPrimary(c.snapshot.player);
    if(!prepared||!prepared->prepared||prepared->revoked||!prepared->recognized||
       prepared->handle!=c.snapshot.playerHandle||prepared->rider!=c.snapshot.rider)return;
    uint32_t table=0;
    if(!roomscaleWord(reinterpret_cast<uintptr_t>(c.snapshot.player),0,table)||table!=swimmingPlayerVtable)return;
    RoomscalePlacementOutcome outcome;
    bool settled=false;
    withNativeFinally([&] {
        roomscaleBusy=true;
        if(!roomscalePhaseCurrent(c,false)||!roomscaleWord(c.world,0x64,c.worldInfoHandle)||
           !roomscaleWord(reinterpret_cast<uintptr_t>(c.snapshot.player),0x38c,c.brainHandle))return;
        c.worldInfo=reinterpret_cast<uintptr_t>(resolve(c.worldInfoHandle));
        c.brain=reinterpret_cast<uintptr_t>(resolve(c.brainHandle));
        if(!roomscalePhaseCurrent(c)||!trackingEligible(c.snapshot.player)||!isAlive(c.snapshot.player)||
           !nativeTrackingAnchor(c.snapshot.player,c.anchorBefore,&c.snapshot.rider)||
           !runRoomscaleMathFrame(c.failed,c.thread,prepareRoomscaleMove,&c))return;
        outcome=runCheckedRoomscalePlacement(reinterpret_cast<void*>(c.body.mechanism),
            reinterpret_cast<void*>(c.body.parts),reinterpret_cast<void*>(c.body.root),
            c.targetModel,c.thread,checkRoomscaleTarget,&c);
        c.reads.clear(); // Native commit callbacks ran outside the read-only query lease.
        if(outcome.completed&&outcome.mayHaveMoved&&c.ticket&&roomscalePhaseCurrent(c,false)&&
           livePlayer(c.snapshot)&&isAlive(c.snapshot.player)&&
           nativeTrackingAnchor(c.snapshot.player,c.anchorAfter,&c.snapshot.rider))
            settled=runRoomscaleMathFrame(c.failed,c.thread,settleRoomscaleMoveMath,&c);
    },[&](bool aborted) noexcept {
        if(aborted) {roomscaleFaulted.store(true);nativeInputFailed();}
        finishRoomscaleRevision(c,aborted||outcome.mayHaveMoved,settled&&!aborted);
        rigMutationActive=false;roomscaleBusy=false;
    });
}

struct HeadVisibilityAttempt {
    LocalRoomscaleAttempt owner;
    Request request{};
    roomscale::HeadQueryBinding binding;
    Pose head{};
    float radius=0,nearZ=.05f,numericalGuard=.002f;
    bool prepared=false,clear=false;
    HeadQueryDiagnostic diagnostic;
};
static bool captureHeadQueryBinding(HeadVisibilityAttempt& h,roomscale::HeadQueryBinding& result) {
    auto& c=h.owner;
    return roomscale::readHeadQueryBinding([&](uint32_t address,void* target,size_t size) {
        if(!c.reads.contains(reinterpret_cast<void*>(address),size))return false;
        std::memcpy(target,reinterpret_cast<void*>(address),size);return true;
    },[](uint32_t handle){return uint32_t(reinterpret_cast<uintptr_t>(resolve(handle)));},
       c.snapshot.playerHandle,uint32_t(swimmingPlayerVtable),headQueryCategory,result);
}
static bool __cdecl headQueryOwnerCurrent(void* opaque) noexcept {
    auto& h=*static_cast<HeadVisibilityAttempt*>(opaque);auto& c=h.owner;
    // Diagnostic: remember only the first failing term; same predicate order.
    const auto fail=[&](uint32_t code) {
        if(!h.diagnostic.ownerFailure) {
            h.diagnostic.ownerFailure=code;
            if(code==2)h.diagnostic.phaseFailure=roomscalePhaseFailure(c);
        }
        return false;
    };
    if(c.failed)return fail(1);
    if(!roomscalePhaseCurrent(c))return fail(2);
    if(!roomscaleCleanupReady(c))return fail(3);
    const auto live=copySnapshot();
    if(!roomscaleSnapshotMatches(c.snapshot,live))return fail(4);
    if(!vrSession(live))return fail(5);
    if(!fresh(live.input))return fail(6);
    if(!live.ui.gameplay)return fail(7);
    if(!live.rider.handheld()&&!live.rider.seated())return fail(8);
    if(recenterHeld(live.input))return fail(9);
    if(!livePlayer(live))return fail(10);
    if(!nativeRiderCurrent(live.player,live.rider))return fail(11);
    roomscale::HeadQueryBinding current;
    if(!captureHeadQueryBinding(h,current))return fail(12);
    if(!roomscale::sameHeadQueryBinding(h.binding,current))return fail(13);
    return true;
}
static void __cdecl queryHeadVolume(void* opaque) noexcept {
    auto& h=*static_cast<HeadVisibilityAttempt*>(opaque);auto& c=h.owner;
    h.clear=runOwnedSphereQueries(h.binding.subject,c.cover,0,c.thread,headQueryOwnerCurrent,&h,c.failed,true);
}
static bool __cdecl prepareAndQueryHeadVolume(void* opaque) {
    auto& h=*static_cast<HeadVisibilityAttempt*>(opaque);auto& c=h.owner;
    auto& d=h.diagnostic;
    d.stage=HeadQueryStage::Phase;if(!roomscalePhaseCurrent(c))return false;
    d.stage=HeadQueryStage::Cleanup;if(!roomscaleCleanupReady(c))return false;
    d.stage=HeadQueryStage::Binding;if(!captureHeadQueryBinding(h,h.binding))return false;
    d.categoryCount=h.binding.subject.categoryCount;
    const auto relative=relativeTracking(c.snapshot.origin,h.request.input.head);
    d.stage=HeadQueryStage::RelativePose;
    if(!finite(relative)||!finite(c.anchorBefore))return false;
    const auto bounded=boundHeadTranslation(relative.p);
    d.stage=HeadQueryStage::Translation;
    if(bounded.x!=relative.p.x||bounded.y!=relative.p.y||bounded.z!=relative.p.z)return false;
    h.head=worldHeadTracking(c.anchorBefore,c.snapshot.origin,c.snapshot.turn,h.request.input.head);
    h.nearZ=std::bit_cast<float>(headNearBits.load(std::memory_order_relaxed));
    d.stage=HeadQueryStage::NearPose;
    if(!std::isfinite(h.nearZ)||h.nearZ<=0||!roomscale::bodyUnitQuaternion(c.anchorBefore.q)||
       !roomscale::bodyUnitQuaternion(c.snapshot.origin.q))return false;
    double radius=settings.headRadius;
    double worldGrid=0,stageMagnitude=1;
    for(float value:{h.head.p.x,h.head.p.y,h.head.p.z}) {
        const double upper=double(std::nextafter(value,INFINITY))-value;
        const double lower=double(value)-std::nextafter(value,-INFINITY);
        worldGrid=std::max(worldGrid,std::max(upper,lower));
    }
    for(float value:{c.snapshot.origin.p.x,c.snapshot.origin.p.y,c.snapshot.origin.p.z,
                    h.request.input.head.p.x,h.request.input.head.p.y,h.request.input.head.p.z})
        stageMagnitude=std::max(stageMagnitude,std::abs(double(value)));
    // The last observed native near plane is checked again while both eyes
    // render. Larger/new clipping planes reject the pair and prime a new query.
    for(unsigned eye=0;eye<2;++eye) {
        const auto pose=worldEyeTracking(c.anchorBefore,c.snapshot.origin,c.snapshot.turn,
                                         h.request.input.head,h.request.eye[eye]);
        const auto f=h.request.fov[eye];
        d.stage=HeadQueryStage::EyePoseFov;
        if(!finite(pose)||!roomscale::bodyUnitQuaternion(pose.q)||
           !std::isfinite(f.left)||!std::isfinite(f.right)||!std::isfinite(f.up)||!std::isfinite(f.down)||
           f.left>=f.right||f.down>=f.up||std::abs(f.left)>=1.5f||std::abs(f.right)>=1.5f||
           std::abs(f.up)>=1.5f||std::abs(f.down)>=1.5f)return false;
        const double x=std::max(std::abs(std::tan(double(f.left))),std::abs(std::tan(double(f.right))));
        const double y=std::max(std::abs(std::tan(double(f.down))),std::abs(std::tan(double(f.up))));
        const double eyeDistance=std::hypot(double(pose.p.x)-h.head.p.x,double(pose.p.y)-h.head.p.y,
                                           double(pose.p.z)-h.head.p.z);
        radius=std::max(radius,eyeDistance+double(h.nearZ)*std::sqrt(1+x*x+y*y)+.001);
    }
    const double guard=std::max(.002,32*worldGrid+128*std::numeric_limits<float>::epsilon()*stageMagnitude);
    d.computedGuard=guard;d.computedRadius=radius;d.stage=HeadQueryStage::Guard;
    if(!std::isfinite(guard)||guard>.25)return false;
    h.numericalGuard=roomscale::outwardFloat(guard);
    radius+=h.numericalGuard;
    radius+=settings.headClearanceMargin; // Explicit room for later tracked poses inside this clear volume.
    d.computedRadius=radius;d.stage=HeadQueryStage::Radius;
    if(!std::isfinite(radius)||radius<=0||radius>.5)return false;
    h.radius=roomscale::outwardFloat(radius);
    c.cover=coverHeadVolumeSweep(h.binding.subject.categoryCount,c.anchorBefore.p,h.head.p,h.radius,.001f);
    d.stage=HeadQueryStage::Cover;if(!c.cover.valid)return false;
    d.stage=HeadQueryStage::Owner;if(!headQueryOwnerCurrent(&h))return false;
    c.reads.seal();h.prepared=true;
    d.stage=HeadQueryStage::ResourceQuery;
    resetSphereQueryOutcome();resetRoomscaleScopeFailure();resetRoomscaleResourceCancel();
    const bool clear=runRoomscaleResourceScope(c.failed,c.thread,queryHeadVolume,&h)&&h.clear;
    d.sphere=lastSphereQueryOutcome();d.kernel=roomscaleScopeFailure();d.cancel=roomscaleResourceCancel();
    if(clear)d.stage=HeadQueryStage::Complete;
    return clear;
}
static __attribute__((noinline)) void runPostSimulationHeadVisibility(SimulationInterval& interval) {
    if(!settings.headFade||!roomscaleConfigured||roomscaleBusy||roomscaleFaulted.load()||
       interval.previous||!interval.managerPrepared||!nativeInputHealthy())return;
    HeadVisibilityAttempt h;auto& c=h.owner;
    c.interval=&interval;c.snapshot=copySnapshot();c.thread=GetCurrentThreadId();
    c.simulationSequence=simulationRevision;c.world=reinterpret_cast<uintptr_t>(interval.preparedWorld);
    if(!vrSession(c.snapshot)||!fresh(c.snapshot.input)||!c.snapshot.ui.gameplay||
       (!c.snapshot.rider.handheld()&&!c.snapshot.rider.seated())||recenterHeld(c.snapshot.input)||!livePlayer(c.snapshot)||
       !roomscaleWord(c.world,0x6c,c.physics)||!c.physics)return;
    auto* prepared=preparedPrimary(c.snapshot.player);
    if(!prepared||!prepared->prepared||prepared->revoked||!prepared->recognized||
       prepared->handle!=c.snapshot.playerHandle||prepared->rider!=c.snapshot.rider)return;
    uint32_t table=0;
    if(!roomscaleWord(reinterpret_cast<uintptr_t>(c.snapshot.player),0,table)||table!=swimmingPlayerVtable)return;
    {
        Lock lock(channel,1);
        if(!lock)return;
        bool found=false;
        for(const auto& slot:channel.shared->slot)
            if(slot.state==SlotState::Requested&&!slot.cancelled&&
               nativeFrameIdentity(slot.request,c.snapshot.input,c.snapshot.generation,c.snapshot.initialized,
                                   c.snapshot.ui.gameplay,true)&&fresh(slot.request.input)) {
                h.request=slot.request;found=true;break;
            }
        if(!found)return;
    }
    withNativeFinally([&] {
        roomscaleBusy=true;
        if(!roomscalePhaseCurrent(c,false)||!roomscaleWord(c.world,0x64,c.worldInfoHandle)||
           !roomscaleWord(reinterpret_cast<uintptr_t>(c.snapshot.player),0x38c,c.brainHandle))return;
        c.worldInfo=reinterpret_cast<uintptr_t>(resolve(c.worldInfoHandle));
        c.brain=reinterpret_cast<uintptr_t>(resolve(c.brainHandle));
        if(!roomscalePhaseCurrent(c)||!trackingEligible(c.snapshot.player)||!isAlive(c.snapshot.player)||
           !nativeTrackingAnchor(c.snapshot.player,c.anchorBefore,&c.snapshot.rider))return;
        const bool mathReturned=runRoomscaleMathFrame(c.failed,c.thread,prepareAndQueryHeadVolume,&h);
        const bool clear=mathReturned&&h.clear;
        const HeadVolumeObservation sample{c.anchorBefore,h.head,c.snapshot.playerHandle,
            h.request.trackingGeneration,h.request.session,h.request.reference,h.request.sequence,
            h.request.input.sequence,c.simulationSequence,c.snapshot.rigRevision,GetTickCount64(),
            h.radius,h.nearZ,h.numericalGuard,h.prepared,clear};
        h.diagnostic.prepared=h.prepared;h.diagnostic.clear=h.clear;h.diagnostic.failed=c.failed;
        h.diagnostic.mathReturned=mathReturned;h.diagnostic.coverValid=c.cover.valid;
        AcquireSRWLockExclusive(&laserLock);
        headVolumeObservation=sample;headQueryDiagnostic=h.diagnostic;
        ReleaseSRWLockExclusive(&laserLock);
    },[&](bool aborted) noexcept {
        if(aborted) {nativeRayFaulted.store(true,std::memory_order_release);nativeInputFailed();}
        roomscaleBusy=false;
    });
}

static __attribute__((noinline)) void runPostSimulationSniper(SimulationInterval &interval) {
    if(!labSniperConfigured || interval.previous ||
       !interval.managerPrepared || !nativeInputHealthy() ||
       !nativeMainThread || !nativeMainThread())return;
    if(!labSniper.begin())return; // Before any callback-capable validation or native getter.
    const uint64_t revision=simulationRevision;
    withNativeFinally([&] {
        auto fail=[&](const char *why) {
            labSniper.stage=LabSniperStage::Failed;
            log("%s preparation failed reason=%s",labPreparation->receiptPrefix,why);
        };
        if(!labOnlineIsolationInstalled()) {fail("isolation");return;}
        validateLabOnlineIsolation(true);
        const char *phaseFailure="unsampled";
        bool presentationBusy=false;
        auto phaseCheck=[&](bool passed,const char *label) {
            if(!passed)phaseFailure=label;
            return passed;
        };
        auto phaseCurrent=[&] {
            phaseFailure="none";
            presentationBusy=false;
            auto presentationCurrent=[&] {
                const bool idle=nativePresentationIdleForBodyMove();
                presentationBusy=!idle;
                return phaseCheck(idle,"presentation-idle");
            };
            // Preserve original evaluation order and short-circuiting. Labels
            // report the first failed existing check; no extra native queries.
            return phaseCheck(interval.simulation && interval.preparedWorld && interval.preparedManager,"prepared") &&
                phaseCheck(simulationInterval==&interval,"interval") &&
                phaseCheck(interval.nativeCaller==roomscaleSimulationReturn,"caller") &&
                phaseCheck(currentSimulation()==interval.simulation,"simulation") &&
                phaseCheck(currentWorld()==interval.preparedWorld,"world") &&
                phaseCheck(primaryField(interval.simulation,0x4c)==0,"native-phase") &&
                phaseCheck(primaryField(interval.preparedWorld,0x74)==reinterpret_cast<uintptr_t>(interval.preparedManager),"manager") &&
                phaseCheck(simulationRevision==revision,"revision") &&
                phaseCheck(nativeInputHealthy(),"input-health") && presentationCurrent();
        };
        if(!phaseCurrent()) {
            // Initial admission has borrowed no player/resource and issued no
            // action. Wait for the existing presentation transaction to retire.
            // Later pre-action/currentOwner phase failures still latch failure.
            if(presentationBusy)return;
            log("%s phase failure check=%s stage=%u",labPreparation->receiptPrefix,phaseFailure,unsigned(labSniper.stage));
            fail("simulation-phase");return;
        }
        const auto s=copySnapshot();
        const bool bound=labSniper.player!=0;
        if(bound && (GetTickCount64()>labSniper.deadline ||
           currentSimulation()!=labSniper.simulation || currentWorld()!=labSniper.world ||
           s.playerHandle!=labSniper.player)) {fail("owner-or-deadline");return;}
        if(currentSimulation()!=interval.simulation || currentWorld()!=interval.preparedWorld ||
           !nativePresentationIdleForBodyMove() || !singlePlayer() || !livePlayer(s) ||
           !vrSession(s) || !fresh(s.input) || !s.ui.gameplay || !s.rider.handheld() ||
           !nativeRiderCurrent(s.player,s.rider) || !trackingEligible(s.player))return;
        if(s.use || s.jump || s.sprint || s.input.zoomDownMask)return fail("nonneutral");
        for(unsigned hand=0;hand<2;++hand)
            if(s.input.trigger[hand]!=0 || s.input.axis[hand][0]!=0 || s.input.axis[hand][1]!=0 ||
               (s.input.buttons[hand] & ~Button::JumpBound) || s.fire[hand] || s.zoom[hand] || s.selecting[hand] ||
               s.ui.wheel[hand].open) return fail("nonneutral");
        uint32_t gameHandle=0;nativeGameInfo(&gameHandle);
        void *game=gameHandle?resolve(gameHandle):nullptr;
        if(!livePlayer(s)) {fail("game-getter-owner");return;}
        const uint32_t brainHandle=primaryField(s.player,0x38c);
        void *brain=brainHandle?resolve(brainHandle):nullptr;
        auto currentOwner=[&] {
            return phaseCurrent() && livePlayer(s) &&
                resolve(gameHandle)==game && game && resolve(brainHandle)==brain && brain &&
                primaryField(s.player,0x38c)==brainHandle && primaryField(brain,0x28)==s.playerHandle &&
                !(primaryField(s.player,0x10)&2) && !(primaryField(brain,0x10)&2) &&
                !(primaryField(game,0x10)&2) && phaseCurrent();
        };
        if(!currentOwner() || (bound && (gameHandle!=labSniper.game || brainHandle!=labSniper.brain))) {
            fail("native-owner");return;
        }
        const auto table=primaryField(game,0);
        if(table>UINTPTR_MAX-0x1b8 || !readableMemory(reinterpret_cast<void *>(table+0x1b0),8) ||
           primaryField(reinterpret_cast<void *>(table),0x1b0)!=labBrainGetter ||
           primaryField(reinterpret_cast<void *>(table),0x1b4)!=labPuppetGetter) {fail("roster-getters");return;}
        unsigned population=0;
        for(int slot=0;slot<17;++slot) {
            uint32_t rosterBrain=0;
            reinterpret_cast<LabPlayerGet>(labBrainGetter)(game,&rosterBrain,slot);
            if(!currentOwner()) {fail("roster-owner");return;}
            if(!rosterBrain)continue;
            uint32_t rosterPawn=0;
            reinterpret_cast<LabPlayerGet>(labPuppetGetter)(game,&rosterPawn,slot);
            if(!currentOwner() || ++population!=1 || rosterBrain!=brainHandle || rosterPawn!=s.playerHandle) {
                fail("ambiguous-roster");return;
            }
        }
        uint32_t latestGame=0;nativeGameInfo(&latestGame);
        if(population!=1 || latestGame!=gameHandle || !currentOwner() || !local(s.player) || !currentOwner()) {
            fail("local-target");return;
        }
        if(!bound) {
            labSniper.player=s.playerHandle;labSniper.brain=brainHandle;labSniper.game=gameHandle;
            labSniper.simulation=interval.simulation;labSniper.world=interval.preparedWorld;
            labSniper.deadline=GetTickCount64()+30000;
        }
        if(labSniper.issuedTick==simulationRevision)return;
        if(labSniper.stage==LabSniperStage::Waiting) {
            const bool cheats=labCheatsEnabled(game)!=0;
            if(!currentOwner()) {fail("cheat-owner");return;}
            if(!cheats)return;
            if(!phaseCurrent()) {fail("grant-phase");return;}
            labSniper.stage=LabSniperStage::Granted;labSniper.issuedTick=simulationRevision;
            labGiveAll(game);
            log("%s preparation issued stage=grant owner=%u",labPreparation->receiptPrefix,labSniper.player);
            return;
        }
        if(labSniper.stage==LabSniperStage::Granted) {
            const bool inventory=inInventory(static_cast<uint8_t *>(s.player)+0x8f8,labPreparation->nativeId)!=0;
            if(!currentOwner()) {fail("inventory-owner");return;}
            if(!inventory) {fail(labPreparation->sniperZoom?"native-grant-excluded-sniper":"native-grant-excluded-autosg");return;}
            const bool change=canChange(s.player,1)!=0;
            if(!currentOwner()) {fail("selection-owner");return;}
            if(!change)return;
            if(!phaseCurrent()) {fail("selection-phase");return;}
            labSniper.stage=LabSniperStage::Selected;labSniper.issuedTick=simulationRevision;
            labSelectSniper(s.player,labPreparation->nativeId,1,0,0);
            // Selection may delete old weapon bindings. Reacquire next tick.
            log("%s preparation issued stage=select owner=%u",labPreparation->receiptPrefix,labSniper.player);
            return;
        }
        if(labSniper.stage==LabSniperStage::Selected || labSniper.stage==LabSniperStage::SaveIssued) {
            const uint32_t handle=nativeHandle(s.player,1);
            void *weapon=handle?resolve(handle):nullptr;
            if(!weapon || handle==nativeHandle(s.player,0) || primaryField(weapon,0x28)!=s.playerHandle ||
               primaryField(weapon,0xb4)!=uint32_t(labPreparation->nativeId) || primaryField(weapon,0xb0)!=1)return;
            if(labPreparation->sniperZoom) {
                if(!nativeSniper(weapon) || primaryField(weapon,0xd4))return;
                const bool zooming=nativeZoomFlag(weapon)!=0;
                if(!currentOwner() || resolve(handle)!=weapon || nativeHandle(s.player,1)!=handle) {fail("sniper-owner");return;}
                if(zooming)return;
            } else {
                // AutoSG inherits base idle state; +d4 is not a sniper zoom field.
                if(primaryField(weapon,0)!=labAutoShotgunVtable)return;
                if(!currentOwner() || resolve(handle)!=weapon || nativeHandle(s.player,1)!=handle) {fail("autosg-owner");return;}
            }
        }
        if(labSniper.stage==LabSniperStage::Selected) {
            char preload[1100]{};
            std::snprintf(preload,sizeof(preload),"%s.preload",labSniper.savePath);
            if(GetFileAttributesA(labSniper.savePath)!=INVALID_FILE_ATTRIBUTES ||
               GetFileAttributesA(preload)!=INVALID_FILE_ATTRIBUTES) {fail("save-collision");return;}
            char directory[768]{},expected[1024]{};
            const DWORD cwd=GetCurrentDirectoryA(sizeof(directory),directory);
            if(!cwd || cwd>=sizeof(directory)) {fail("save-directory");return;}
            std::snprintf(expected,sizeof(expected),"%s\\Temp\\SS2VR\\%s",directory,labPreparation->saveBasename);
            if(_stricmp(expected,labSniper.savePath)) {fail("save-directory-owner");return;}
            if(!phaseCurrent()) {fail("save-phase");return;}
            labSniper.stage=LabSniperStage::SaveIssued;labSniper.issuedTick=simulationRevision;
            // Core's resource writer prefixes its engine root. An ordinary
            // absolute Windows path would receive that root a second time.
            labSaveSniper(labPreparation->resourcePath);
            // Save can temporarily switch worlds. Borrow nothing across it.
            log("%s preparation issued stage=save owner=%u",labPreparation->receiptPrefix,labSniper.player);
            return;
        }
        if(labSniper.stage==LabSniperStage::SaveIssued) {
            const DWORD attributes=GetFileAttributesA(labSniper.savePath);
            if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) {
                fail("native-save-missing");return;
            }
            labSniper.stage=LabSniperStage::Complete;
            log("%s preparation complete owner=%u nativeId=%d path=%s",labPreparation->receiptPrefix,labSniper.player,labPreparation->nativeId,labSniper.savePath);
        }
    },[&](bool aborted) noexcept {
        const bool healthy=nativeInputHealthy();
        labSniper.finish(aborted,healthy);
        if(aborted || !healthy)nativeInputFailed();
    });
}

static std::vector<void *> ownedHooks;
static bool attachPoisoned = false;
// Wrist HUD: the physical hand holding the weapon an ammo element shows (game
// hands: 0 left, 1 right; -1 the lone weapon). mountHand maps a lone weapon to
// the controller actually holding it.
static int hudAmmoSide(int hand) {
    const auto s = copySnapshot();
    if (hand < 0) {
        if ((s.handle[0] != 0) == (s.handle[1] != 0)) return -2;
        hand = s.handle[0] ? 0 : 1;
    }
    return int(mountHand(s, unsigned(hand) & 1u));
}
static bool rollbackNativeHooks() {
    hooksReady = false;
    hud_wrist::uninstall();
    rideModelRenderableReturn=0;
    bool result = rollbackHooks(
        ownedHooks,
        [](void *address) {
            // Cancel any queued enable before directly disabling. MinHook's
            // ApplyQueued can stop midway; a second queued apply is not rollback.
            auto queued = MH_QueueDisableHook(address);
            auto status = MH_DisableHook(address);
            bool ok = queued == MH_OK && (status == MH_OK || status == MH_ERROR_DISABLED);
            if (!ok)
                log("Hook disable rollback failed: queued=%d disable=%d", int(queued), int(status));
            return ok;
        },
        [](void *address) {
            auto status = MH_RemoveHook(address);
            if (status != MH_OK)
                log("Hook removal rollback failed: %d", int(status));
            return status == MH_OK;
        });
    if(result) {
        resetRoomscaleSweepQueriesAfterQuiescence();
        resetRoomscalePlacementHooksAfterRemoval();
        resetRoomscaleTriangleHookAfterRemoval();
        resetRoomscaleResourceGatesAfterRemoval();
        roomscaleConfigured=false;roomscaleEngineBase=roomscaleSimulationReturn=0;
        roomscaleWorldInfoTable=roomscaleBrainTable=0;
    }
    attachPoisoned = !result;
    return result;
}
static bool hook(HMODULE m, const char *name, void *target, void **original) {
    void *address = reinterpret_cast<void *>(GetProcAddress(m, name));
    if (!address || MH_CreateHook(address, target, original) != MH_OK) {
        log("Required hook missing: %s", name);
        return false;
    }
    ownedHooks.push_back(address);
    return MH_QueueEnableHook(address) == MH_OK;
}
static bool internalHook(HMODULE m, uint32_t rva, void *target, void **original) {
    void *address = reinterpret_cast<uint8_t *>(m) + rva;
    if (MH_CreateHook(address, target, original) != MH_OK) {
        log("Required fingerprinted internal hook missing: %x", unsigned(rva));
        return false;
    }
    ownedHooks.push_back(address);
    return MH_QueueEnableHook(address) == MH_OK;
}
template <class T> static bool symbol(HMODULE m, const char *name, T &out) {
    out = loadProc<T>(m, name);
    if (!out)
        log("Required symbol missing: %s", name);
    return out != nullptr;
}
// Eye replay support (bridge eye_replay): the left eye's geometry for the
// right-eye rewrite, and the right eye's root capture derived from it.
using BindMatrix = void(__cdecl *)(uint32_t, int32_t);
static BindMatrix originalBindMatrix = nullptr;
static void __cdecl bindMatrix(uint32_t type, int32_t index) {
    const bool tag = replay::recording();
    if (tag) replay::matrixBegin(type);
    withNativeFinally([&] { originalBindMatrix(type, index); }, [&](bool) noexcept { if (tag) replay::matrixEnd(); });
}
static ss2vr::replay::ViewState replayRoot;
static WeaponWorldView replayRootWorld;
static Pose replayRightCamera;
const ss2vr::replay::ViewState &replayRootState() noexcept { return replayRoot; }
bool replayCaptureEyeGeometry(ss2vr::replay::EyeChange &out) noexcept {
    out = {};
    if (!eyePlayer || eyeIndex != 0 || !eyeSnapshot.initialized || !eyeAnchorReady || weaponPairFault || !rootCaptureAttempted ||
        !executedWeaponWorld.valid || !executedUiProjectionValid) return false;
    // Frames with the mod's own 3D overlays (weapon wheels, vehicle steering
    // wheel) render the right eye natively: their replay is not yet exact.
    if (eyeSnapshot.ui.wheel[0].open || eyeSnapshot.ui.wheel[1].open || !eyeSnapshot.rider.handheld()) return false;
    const Pose left = worldEyeTracking(eyeAnchor, eyeSnapshot.origin, eyeSnapshot.turn, eyeRequest.input.head, eyeRequest.eye[0]);
    const Pose right = worldEyeTracking(eyeAnchor, eyeSnapshot.origin, eyeSnapshot.turn, eyeRequest.input.head, eyeRequest.eye[1]);
    if (!finite(left) || !finite(right)) return false;
    out = ss2vr::replay::eyeChange(matrix(compose(inverse(right), left)), eyeRequest.fov[0], eyeRequest.fov[1]);
    std::memcpy(replayRoot.adjusted.data(), executedUiProjection.m, sizeof(replayRoot.adjusted));
    std::memcpy(replayRoot.projection.data(), executedWeaponWorld.projection.m, sizeof(replayRoot.projection));
    std::memcpy(replayRoot.view.data(), executedWeaponWorld.view.m, sizeof(replayRoot.view));
    replayRootWorld = executedWeaponWorld;
    replayRightCamera = right;
    return out.valid;
}
// Single-pass stereo. The world pass ran as eye 0 of the centre request; each
// eye's camera is the centre camera moved by that eye's offset and frustum.
static ss2vr::replay::ViewState singlePassRoot;
static WeaponWorldView singlePassRootWorld;
static Request singlePassCentre;
static bool singlePassEyeChange(const Request &real, const Request &centre, int i, ss2vr::replay::EyeChange &out) {
    out = {};
    if (!eyeAnchorReady || i < 0 || i > 1) return false;
    const Pose c = worldEyeTracking(eyeAnchor, eyeSnapshot.origin, eyeSnapshot.turn, real.input.head, centre.eye[0]);
    const Pose e = worldEyeTracking(eyeAnchor, eyeSnapshot.origin, eyeSnapshot.turn, real.input.head, real.eye[i]);
    if (!finite(c) || !finite(e)) return false;
    out = ss2vr::replay::eyeChange(matrix(compose(inverse(e), c)), centre.fov[0], real.fov[i]);
    return out.valid;
}
bool singlePassChanges(const Request &real, const Request &centre, ss2vr::replay::EyeChange (&out)[2]) noexcept {
    return singlePassEyeChange(real, centre, 0, out[0]) && singlePassEyeChange(real, centre, 1, out[1]);
}
// After the centre world pass (eye 0 of `centre`): keep its root capture.
bool singlePassCaptureCentre(const Request &centre) noexcept {
    if (!eyePlayer || eyeIndex != 0 || weaponPairFault || !rootCaptureAttempted || !executedWeaponWorld.valid ||
        !executedUiProjectionValid) return false;
    std::memcpy(singlePassRoot.adjusted.data(), executedUiProjection.m, sizeof(singlePassRoot.adjusted));
    std::memcpy(singlePassRoot.projection.data(), executedWeaponWorld.projection.m, sizeof(singlePassRoot.projection));
    std::memcpy(singlePassRoot.view.data(), executedWeaponWorld.view.m, sizeof(singlePassRoot.view));
    singlePassRootWorld = executedWeaponWorld;
    singlePassCentre = centre;
    return true;
}
// Eye i's root capture (HUD projection, markers) derived from the centre pass,
// as a native render of that eye would have produced it.
bool singlePassAdoptEye(void *p, const Request &real, int i) noexcept {
    ss2vr::replay::EyeChange change;
    if (!p || !singlePassRootWorld.valid || !singlePassEyeChange(real, singlePassCentre, i, change)) return false;
    eyePlayer = p;
    eyeRequest = real;
    eyeSnapshot.input = real.input;
    eyeIndex = i;
    const auto r = ss2vr::replay::rewrite(singlePassRoot, change);
    if (!r.valid) return false;
    WeaponWorldView world = singlePassRootWorld;
    for (unsigned k = 0; k < 12; ++k) world.view.m[k] = float(r.view[k]);
    for (unsigned k = 0; k < 16; ++k) world.projection.m[k] = float(r.projection[k]);
    Matrix44 adjusted{};
    for (unsigned k = 0; k < 16; ++k) adjusted.m[k] = float(r.adjusted[k]);
    if (!finiteMatrix(world.view) || !finiteProjection(world.projection) || !finiteProjection(adjusted)) return false;
    preparedWeaponWorld = executedWeaponWorld = world;
    executedUiProjection = adjusted;
    executedUiProjectionValid = true;
    executedRootGeneration = graphicsResourceGeneration();
    rootCaptureAttempted = true;
    rootCaptureArmed = false;
    // Lab receipt in the native camera format: this eye's camera as derived.
    static const bool labTrace = [] { wchar_t value[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_TRACE",value,2)==1 && value[0]==L'1'; }();
    static uint64_t loggedRequest[2]{};
    static unsigned receipts = 0;
    if (labTrace && receipts < 16384 && loggedRequest[i] != real.sequence) {
        ++receipts; loggedRequest[i] = real.sequence;
        const auto &head = real.input.head;
        const Pose c = worldEyeTracking(eyeAnchor, eyeSnapshot.origin, eyeSnapshot.turn, real.input.head, real.eye[i]);
        log("Lab native camera eye=%d request=%llu session=%u reference=%u tracking=%u head=%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f camera=%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f",
            i,static_cast<unsigned long long>(real.sequence),real.session,real.reference,real.trackingGeneration,
            head.p.x,head.p.y,head.p.z,head.q.x,head.q.y,head.q.z,head.q.w,c.p.x,c.p.y,c.p.z,c.q.x,c.q.y,c.q.z,c.q.w);
    }
    return true;
}
// The view and projection the single pass drew eye `i` of `real` with (the
// water environment cube is filled from that image).
bool singlePassEyeView(const Request &real, int i, float (&view)[12], float (&projection)[16]) noexcept {
    ss2vr::replay::EyeChange change;
    if (!singlePassRootWorld.valid || !singlePassEyeChange(real, singlePassCentre, i, change)) return false;
    const auto r = ss2vr::replay::rewrite(singlePassRoot, change);
    if (!r.valid) return false;
    for (unsigned k = 0; k < 12; ++k) view[k] = float(r.view[k]);
    for (unsigned k = 0; k < 16; ++k) projection[k] = float(r.projection[k]);
    for (float v : view) if (!std::isfinite(v)) return false;
    for (float v : projection) if (!std::isfinite(v)) return false;
    return true;
}
// Lab check (SS2VR_LAB_REPLAY_VERIFY=1): after a native right eye, compare its
// root capture with what the replay derivation would have produced.
void replayVerifyEye(const ss2vr::replay::EyeChange &change) noexcept {
    if (eyeIndex != 1 || !executedWeaponWorld.valid || !executedUiProjectionValid) return;
    const auto r = ss2vr::replay::rewrite(replayRoot, change);
    if (!r.valid) return;
    double view = 0, projection = 0, adjusted = 0;
    for (unsigned i = 0; i < 12; ++i) view = std::max(view, std::abs(r.view[i] - executedWeaponWorld.view.m[i]));
    for (unsigned i = 0; i < 16; ++i) projection = std::max(projection, std::abs(r.projection[i] - executedWeaponWorld.projection.m[i]));
    for (unsigned i = 0; i < 16; ++i) adjusted = std::max(adjusted, std::abs(r.adjusted[i] - executedUiProjection.m[i]));
    static unsigned logged = 0;
    if (logged++ < 6) {
        const auto &a = executedUiProjection.m;
        log("Eye replay verify max|diff| view=%.5f projection=%.5f adjusted=%.5f native AP1 row0 %.4f %.4f %.4f %.4f derived %.4f %.4f %.4f %.4f",
            view, projection, adjusted, a[0], a[1], a[2], a[3], r.adjusted[0], r.adjusted[1], r.adjusted[2], r.adjusted[3]);
        const auto &p = executedWeaponWorld.projection.m;
        log("Eye replay verify native P1 row0 %.4f %.4f %.4f %.4f derived %.4f %.4f %.4f %.4f | P0 row0 %.4f %.4f %.4f %.4f",
            p[0], p[1], p[2], p[3], r.projection[0], r.projection[1], r.projection[2], r.projection[3],
            replayRoot.projection[0], replayRoot.projection[1], replayRoot.projection[2], replayRoot.projection[3]);
    }
}
// After beginEye(1) and a successful replay: the right eye's root capture, as
// a native right-eye render would have produced it.
bool replayAdoptEye(const ss2vr::replay::EyeChange &change) noexcept {
    static unsigned rejections = 0;
    if (!eyePlayer || eyeIndex != 1 || weaponPairFault || !replayRootWorld.valid) {
        if (rejections < 8) {
            ++rejections;
            log("Eye replay adopt rejected player=%d eye=%d pairFault=%d rootWorld=%d", eyePlayer != nullptr, eyeIndex,
                int(weaponPairFault), int(replayRootWorld.valid));
        }
        return false;
    }
    const auto r = ss2vr::replay::rewrite(replayRoot, change);
    if (!r.valid) {
        if (rejections < 8) { ++rejections; log("Eye replay adopt rejected: root rewrite invalid"); }
        return false;
    }
    WeaponWorldView world = replayRootWorld;
    for (unsigned i = 0; i < 12; ++i) world.view.m[i] = float(r.view[i]);
    for (unsigned i = 0; i < 16; ++i) world.projection.m[i] = float(r.projection[i]);
    Matrix44 adjusted{};
    for (unsigned i = 0; i < 16; ++i) adjusted.m[i] = float(r.adjusted[i]);
    if (!finiteMatrix(world.view) || !finiteProjection(world.projection) || !finiteProjection(adjusted)) {
        if (rejections < 8) {
            ++rejections;
            log("Eye replay adopt rejected: view=%d projection=%d adjusted=%d P row3 %.4f %.4f %.4f %.4f", finiteMatrix(world.view),
                finiteProjection(world.projection), finiteProjection(adjusted), world.projection.m[12], world.projection.m[13],
                world.projection.m[14], world.projection.m[15]);
        }
        return false;
    }
    preparedWeaponWorld = world;
    executedWeaponWorld = world;
    executedUiProjection = adjusted;
    executedUiProjectionValid = true;
    executedRootGeneration = graphicsResourceGeneration();
    rootCaptureAttempted = true;
    rootCaptureArmed = false;
    // Lab receipt in the native camera format: the right eye's camera as replayed.
    static const bool labTrace = [] { wchar_t value[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_TRACE",value,2)==1 && value[0]==L'1'; }();
    static uint64_t loggedRequest = 0;
    static unsigned receipts = 0;
    if (labTrace && receipts < 16384 && loggedRequest != eyeRequest.sequence) {
        ++receipts; loggedRequest = eyeRequest.sequence;
        const auto &head = eyeRequest.input.head; const auto &c = replayRightCamera;
        log("Lab native camera eye=1 request=%llu session=%u reference=%u tracking=%u head=%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f camera=%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f",
            static_cast<unsigned long long>(eyeRequest.sequence),eyeRequest.session,eyeRequest.reference,eyeRequest.trackingGeneration,
            head.p.x,head.p.y,head.p.z,head.q.x,head.q.y,head.q.z,head.q.w,c.p.x,c.p.y,c.p.z,c.q.x,c.q.y,c.q.z,c.q.w);
    }
    return true;
}
// Dynamic shadow reuse (ren_iDynamicShadowQuality 3). The engine renders each
// shadow caster's light-space image into one shared canvas and projects it onto
// nearby hulls, once per eye. The image depends only on the light and the model,
// so the frame's first eye renders each caster into its own pooled canvas and the
// second eye projects that image through the engine's own quality<=2 path (which
// skips image creation and binds gfx_hCircleTexture), with the pooled handle.
// Fingerprinted Engine RVAs: RenderModelShadow 0xe54c0 (instance, placement,
// lod), shadow image+projection 0xe39c0 (box, light, light projection, colour),
// shared canvas pointer 0x2eadf4, shadow resource end callback 0xe3290.
namespace shadow_reuse {
using ModelShadow = void(__cdecl *)(void *, const void *, float);
using Image = void(__cdecl *)(const void *, const void *, const void *, uint32_t);
using End = void(__cdecl *)();
using CreateCanvas = void *(__cdecl *)(int32_t, int32_t, int32_t, int32_t, int32_t);
using DeleteCanvas = void(__cdecl *)(void *);
using Scissor = void(__cdecl *)(int32_t, int32_t, int32_t, int32_t);
static ModelShadow originalModelShadow = nullptr;
static Image originalImage = nullptr;
static End originalEnd = nullptr;
static CreateCanvas createCanvas = nullptr;
static DeleteCanvas deleteCanvas = nullptr;
static Scissor *scissor = nullptr;
static int32_t *quality = nullptr, *imageSize = nullptr, *circleTexture = nullptr, *showPolys = nullptr, *showTexture = nullptr;
static void **sharedCanvas = nullptr;
static uint8_t *textures = nullptr; // _gfx_atpTextures: data pointer at +4, 0x1c-byte records, texture first.
static thread_local void *caster = nullptr;
constexpr unsigned PoolSize = 64;
struct Entry { void *instance; uint8_t light[48], projection[64]; int32_t handle; bool used; };
static void *pool[PoolSize]{};
static int32_t poolImageSize = 0;
static Entry entries[PoolSize];
static unsigned entryCount = 0;
static uint64_t frameSequence = 0;
static int producerEye = -1;
static unsigned created = 0, reused = 0, missed = 0;
static double createdSeconds = 0, reusedSeconds = 0; // Engine time inside the image function, per path.
static double since(LARGE_INTEGER start) noexcept {
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&frequency);
    return double(now.QuadPart - start.QuadPart) / double(frequency.QuadPart);
}
static void releasePool() noexcept {
    for (auto *&canvas : pool) {
        if (canvas && sharedCanvas && *sharedCanvas != canvas) deleteCanvas(canvas);
        canvas = nullptr;
    }
    entryCount = 0; frameSequence = 0; producerEye = -1;
}
static void __cdecl resourceEnd() {
    releasePool(); // The renderer's shadow resources are going away: pooled canvases first.
    originalEnd();
}
static void __cdecl modelShadow(void *instance, const void *placement, float lod) {
    void *previous = caster;
    caster = instance;
    withNativeFinally([&] { originalModelShadow(instance, placement, lod); },
                      [&](bool) noexcept { caster = previous; });
}
static int32_t canvasHandle(void *canvas) noexcept {
    int32_t handle = 0;
    std::memcpy(&handle, static_cast<uint8_t *>(canvas) + 0x14, 4);
    return handle;
}
static void __cdecl image(const void *box, const void *light, const void *projection, uint32_t color) {
    const bool eligible = settings.shadowReuse && caster && light && projection && eyePlayer &&
        (eyeIndex == 0 || eyeIndex == 1) && *quality >= 3 && !*showPolys && !*showTexture && *imageSize > 0;
    if (!eligible) return originalImage(box, light, projection, color);
    if (eyeRequest.sequence != frameSequence) {
        frameSequence = eyeRequest.sequence; producerEye = eyeIndex; entryCount = 0;
    }
    if (eyeIndex == producerEye) {
        if (poolImageSize != *imageSize) { releasePool(); poolImageSize = *imageSize;
            frameSequence = eyeRequest.sequence; producerEye = eyeIndex; }
        if (entryCount >= PoolSize) return originalImage(box, light, projection, color);
        void *&canvas = pool[entryCount];
        if (!canvas) canvas = createCanvas(*imageSize, *imageSize, 0, 0xde, 0);
        if (!canvas) return originalImage(box, light, projection, color);
        void *saved = *sharedCanvas;
        *sharedCanvas = canvas;
        bool aborted = false;
        LARGE_INTEGER start{}; QueryPerformanceCounter(&start);
        withNativeFinally([&] { originalImage(box, light, projection, color); }, [&](bool unwound) noexcept {
            void *now = *sharedCanvas;
            *sharedCanvas = saved;
            canvas = now; // The engine replaces a canvas whose size no longer matches.
            aborted = unwound;
        });
        createdSeconds += since(start);
        if (aborted || !canvas) return;
        auto &entry = entries[entryCount++];
        entry.instance = caster;
        std::memcpy(entry.light, light, sizeof(entry.light));
        std::memcpy(entry.projection, projection, sizeof(entry.projection));
        entry.handle = canvasHandle(canvas);
        entry.used = false;
        ++created;
        return;
    }
    for (unsigned i = 0; i < entryCount; ++i) {
        auto &entry = entries[i];
        if (entry.used || entry.instance != caster || !entry.handle ||
            std::memcmp(entry.light, light, sizeof(entry.light)) ||
            std::memcmp(entry.projection, projection, sizeof(entry.projection))) continue;
        entry.used = true;
        const int32_t savedQuality = *quality, savedCircle = *circleTexture;
        *quality = 2; *circleTexture = entry.handle;
        LARGE_INTEGER start{}; QueryPerformanceCounter(&start);
        (*scissor)(0, 0, -1, -1); // The skipped drawport restore resets the scissor.
        withNativeFinally([&] { originalImage(box, light, projection, color); }, [&](bool) noexcept {
            *quality = savedQuality; *circleTexture = savedCircle;
        });
        reusedSeconds += since(start);
        ++reused;
        return;
    }
    ++missed; // Visible only in this eye, or a different light/LOD: native path.
    originalImage(box, light, projection, color);
}
static bool install(HMODULE engine) {
    const auto exported = [&](const char *name) { return reinterpret_cast<void *>(GetProcAddress(engine, name)); };
    createCanvas = reinterpret_cast<CreateCanvas>(exported("?gfxCreateTextureCanvas@SeriousEngine@@YAPAVCTextureCanvas@1@JJHW4GfxColorFormat@1@0@Z"));
    deleteCanvas = reinterpret_cast<DeleteCanvas>(exported("?gfxDeleteCanvas@SeriousEngine@@YAXPAVCCanvas@1@@Z"));
    scissor = static_cast<Scissor *>(exported("?_gfxScissor@SeriousEngine@@3P6AXJJJJ@ZA"));
    quality = static_cast<int32_t *>(exported("?ren_iDynamicShadowQuality@SeriousEngine@@3JA"));
    imageSize = static_cast<int32_t *>(exported("?ren_pixDynamicShadowSize@SeriousEngine@@3JA"));
    circleTexture = static_cast<int32_t *>(exported("?gfx_hCircleTexture@SeriousEngine@@3JA"));
    showPolys = static_cast<int32_t *>(exported("?shd_bShowPolys@SeriousEngine@@3HA"));
    showTexture = static_cast<int32_t *>(exported("?shd_bShowTexture@SeriousEngine@@3HA"));
    sharedCanvas = reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(engine) + 0x2eadf4);
    textures = static_cast<uint8_t *>(exported("?_gfx_atpTextures@SeriousEngine@@3V?$CStaticStackArray@VCTextureParams@SeriousEngine@@@1@A"));
    if (!createCanvas || !deleteCanvas || !scissor || !quality || !imageSize || !circleTexture || !showPolys ||
        !showTexture) { log("Shadow reuse unavailable: engine symbols missing"); return true; }
    return internalHook(engine, 0xe54c0, reinterpret_cast<void *>(modelShadow), reinterpret_cast<void **>(&originalModelShadow)) &&
        internalHook(engine, 0xe39c0, reinterpret_cast<void *>(image), reinterpret_cast<void **>(&originalImage)) &&
        internalHook(engine, 0xe3290, reinterpret_cast<void *>(resourceEnd), reinterpret_cast<void **>(&originalEnd));
}
} // namespace shadow_reuse
// Lab diagnostic: the device texture behind the first pooled shadow canvas.
void *shadowReuseProbeTexture() noexcept {
    using namespace shadow_reuse;
    if (!textures || !pool[0] || !entryCount) return nullptr;
    const int32_t handle = canvasHandle(pool[0]);
    uint8_t *data = nullptr;
    std::memcpy(&data, textures + 4, 4);
    if (handle <= 0 || !data) return nullptr;
    void *texture = nullptr;
    std::memcpy(&texture, data + size_t(handle - 1) * 0x1c, 4);
    return texture;
}
void shadowReuseStats(unsigned &created, unsigned &reused, unsigned &missed, double &createdMs, double &reusedMs) noexcept {
    createdMs = 1000 * std::exchange(shadow_reuse::createdSeconds, 0.);
    reusedMs = 1000 * std::exchange(shadow_reuse::reusedSeconds, 0.);
    created = std::exchange(shadow_reuse::created, 0u);
    reused = std::exchange(shadow_reuse::reused, 0u);
    missed = std::exchange(shadow_reuse::missed, 0u);
}
bool attach(bool headless) {
    if (hooksReady)
        return true;
    if (attachPoisoned) {
        log("Native VR attach refused after incomplete rollback; restart required");
        return false;
    }
    if (!supported(headless))
        return false;
    std::wstring settingsDirectory;
    if (moduleDirectory(nullptr,settingsDirectory)) {
        settingsFile=settingsDirectory+L"SS2VR\\SS2VR.ini";
        settings=loadSettings(settingsFile);
        settingsWritten(settingsSeen);
    }
    setDesktopWorld(settings.desktopWorld);
    setEyeReplay(settings.eyeReplay);
    setSinglePass(settings.singlePass);
    single_pass::setInstancing(settings.singlePassInstancing);
    setLabAutoContinue(settings.labAutoContinue);
    auto g = GetModuleHandleW(L"Sam2Game.dll"), e = GetModuleHandleW(L"Engine.dll"),
         c = GetModuleHandleW(L"Core.dll");
    if (g && !headless) {
        // gfx_fAspectRatio (Sam2Game RVA 3F9DE0): the game letterboxes its whole 2D
        // view to this aspect. The eye-sized canvas is square, so use a widescreen
        // layout; the flat screen and HUD panel show exactly that band.
        auto *aspect = reinterpret_cast<float *>(reinterpret_cast<uintptr_t>(g) + 0x3f9de0);
        log("2D aspect %.4f -> %.4f", *aspect, settings.uiAspect);
        *aspect = settings.uiAspect;
    }
    const auto graphics = headless ? nullptr : GetModuleHandleW(L"GfxD3D.dll");
    roomscaleConfigured=false;
    meleeConfigured=false;
    labSniperConfigured=false;
    labPreparation=nullptr;labAutoShotgunVtable=0;
    wchar_t prepareSniper[3]{},prepareIdle[3]{};
    const DWORD legacyLength=GetEnvironmentVariableW(L"SS2VR_LAB_PREPARE_SNIPER",prepareSniper,3);
    const DWORD idleLength=GetEnvironmentVariableW(L"SS2VR_LAB_PREPARE_IDLE",prepareIdle,3);
    if(legacyLength>=3 || idleLength>=3)return false;
    const int preparationId=labPreparationSelection(std::wstring_view(prepareSniper,legacyLength),
        std::wstring_view(prepareIdle,idleLength),selectedIdleProbeWeapon());
    if(preparationId<0)return false;
    if(preparationId) {
        if(headless || !labOnlineIsolationInstalled())return false;
        labPreparation=labPreparationTarget(preparationId);
        if(!labPreparation)return false;
        char directory[768]{},executable[1024]{};
        const DWORD cwd=GetCurrentDirectoryA(sizeof(directory),directory);
        const DWORD exe=GetModuleFileNameA(nullptr,executable,sizeof(executable));
        if(!cwd || cwd>=sizeof(directory) || !exe || exe>=sizeof(executable))return false;
        char *tail=std::strrchr(executable,'\\');if(!tail)return false;*tail=0;
        tail=std::strrchr(executable,'\\');if(!tail)return false;*tail=0;
        if(_stricmp(directory,executable))return false; // Launcher owns lab cwd, not user's game.
        std::snprintf(labSniper.savePath,sizeof(labSniper.savePath),"%s\\Temp\\SS2VR\\%s",directory,labPreparation->saveBasename);
        labGiveAll=reinterpret_cast<VoidThis>(reinterpret_cast<uintptr_t>(g)+0xd5b70);
        labSelectSniper=reinterpret_cast<LabSelect>(reinterpret_cast<uintptr_t>(g)+0x102340);
        labSaveSniper=reinterpret_cast<decltype(labSaveSniper)>(reinterpret_cast<uintptr_t>(g)+0x22f00);
        labBrainGetter=reinterpret_cast<uintptr_t>(g)+0xd41b0;
        labPuppetGetter=reinterpret_cast<uintptr_t>(g)+0xd41e0;
        labAutoShotgunVtable=reinterpret_cast<uintptr_t>(g)+0x2ccc78;
        labSniperConfigured=true;
    }
    roomscaleEngineBase=reinterpret_cast<uintptr_t>(e);
    weaponConfigurationVtable=uint32_t(reinterpret_cast<uintptr_t>(e)+0x2095b4);
    roomscaleSimulationReturn=reinterpret_cast<uintptr_t>(g)+0x258de;
    roomscaleWorldInfoTable=reinterpret_cast<uintptr_t>(g)+0x29ddf0;
    roomscaleBrainTable=reinterpret_cast<uintptr_t>(g)+0x29be90;
    // Cached function addresses and trampolines belong to these exact modules.
    // Keep their mappings resident rather than accepting a stale reload target.
    for (auto module : {g, e, c, graphics}) {
        if (!module) continue;
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(module), &pinned)) {
            log("Native dependency pin failed: %lu", GetLastError());
            return false;
        }
    }
    // Fingerprinted Engine renPrepareRender invokes root Prepare at RVA 0x14c512 (return 0x14c517).
    rootPrepareReturn = reinterpret_cast<uintptr_t>(e) + 0x14c517;
    rootProjectionReturn = reinterpret_cast<uintptr_t>(g) + 0x94398;
    nativeFrustumReturn = reinterpret_cast<uintptr_t>(g) + 0x9436f;
    playerFovGetter = reinterpret_cast<uintptr_t>(g) + 0xf8050;
    scopeInjectionReturn = reinterpret_cast<uintptr_t>(g) + 0xfda0b;
    if (!headless && !scopeCaptureCommands.configure(e,g)) {
        log("Scope native capture command ABI unavailable");
        scopeInjectionReturn = 0; // Optional source cannot queue or become publishable.
    }
    markerParentReturn = reinterpret_cast<uintptr_t>(g) + 0xfea8a;
    overlayParentReturn = reinterpret_cast<uintptr_t>(g) + 0xeb85d;
    uiFadeReturn[0] = reinterpret_cast<uintptr_t>(g) + 0xfe90a;
    uiFadeReturn[1] = reinterpret_cast<uintptr_t>(g) + 0xfea07;
    // Audited CLOSRequest setup: every collision setter follows this rayInit call.
    allowedRayReturn = reinterpret_cast<uintptr_t>(g) + 0x21ae69;
    nativeShotAttachmentReturn = reinterpret_cast<uintptr_t>(g) + 0x4a824;
    laserEngineBase = reinterpret_cast<uintptr_t>(e);
    laserTurretVtable = reinterpret_cast<uintptr_t>(g) + 0x2b39e8;
    turretShootDirection = reinterpret_cast<uintptr_t>(g) + 0x155ce0; // CTurretPuppetEntity::GetShootDirection
    aircraftVtables[0] = reinterpret_cast<uintptr_t>(g) + 0x2b79b0; // CAircraftPuppetEntity
    aircraftVtables[1] = reinterpret_cast<uintptr_t>(g) + 0x2b7348; // CAircraftCharacterEntity
    aircraftVtables[2] = reinterpret_cast<uintptr_t>(g) + 0x2b0760; // CAirBikePuppetEntity
    ballVtables[0] = reinterpret_cast<uintptr_t>(g) + 0x2b9240; // CRollingBallPuppetEntity
    ballVtables[1] = reinterpret_cast<uintptr_t>(g) + 0x2b98f0; // CRollingBallCharacterEntity
    dinoVtables[0] = reinterpret_cast<uintptr_t>(g) + 0x29e180; // CLeggedPuppetEntity (dinosaurs)
    dinoVtables[1] = reinterpret_cast<uintptr_t>(g) + 0x2b8ac8; // CLeggedCharacterEntity
    weaponFrustumReturn = reinterpret_cast<uintptr_t>(g) + 0x4c8e9;
    weaponInverseReturn = reinterpret_cast<uintptr_t>(g) + 0x4c915;
    weaponDepthReturn = reinterpret_cast<uintptr_t>(g) + 0x4c9a6;
    weaponPlacementReturn = reinterpret_cast<uintptr_t>(g) + 0x4c9bb;
    weaponChargeReturn = reinterpret_cast<uintptr_t>(g) + 0x4c058;
    ordinaryWeaponRenderReturn = reinterpret_cast<uintptr_t>(g) + 0x4bf3a;
    weaponRestoreReturn = reinterpret_cast<uintptr_t>(g) + 0x4ca93;
    rootDepthReturn = reinterpret_cast<uintptr_t>(e) + 0x155dd1;
    playerAlternativePressReturn = reinterpret_cast<uintptr_t>(g) + 0x101f8b;
    primaryOperatorReturn = reinterpret_cast<uintptr_t>(g) + 0xa6e09;
    primaryHeldReturn = reinterpret_cast<uintptr_t>(g) + 0xf6f1f;
    sniperBaseStepReturn = reinterpret_cast<uintptr_t>(g)+0x172830;
    sawVtable = reinterpret_cast<uintptr_t>(g)+0x2cdd10;
    sawReleaseReturn = reinterpret_cast<uintptr_t>(g)+0x165498;
    weaponHeldReturn = reinterpret_cast<uintptr_t>(g)+0xf6f1f;
    baseHeldReturn[0] = reinterpret_cast<uintptr_t>(g)+0x4ef8b;
    baseHeldReturn[1] = reinterpret_cast<uintptr_t>(g)+0x4f096;
    operatorPrimaryReturn[0] = reinterpret_cast<uintptr_t>(g)+0x8e6ef;
    operatorPrimaryReturn[1] = reinterpret_cast<uintptr_t>(g)+0x8e733;
    sniperCrossDeleteReturn = reinterpret_cast<uintptr_t>(g)+0x171a54;
    mountedAvatarReturn = reinterpret_cast<uintptr_t>(g) + 0x943b6;
    mountedClampReturn = reinterpret_cast<uintptr_t>(g) + 0xf3255;
    boardingSeatReturn = reinterpret_cast<uintptr_t>(g) + 0xea4fe;
    useSeatReturn = reinterpret_cast<uintptr_t>(g) + 0xea465;
    {
        // The view ray search's camera call: call dword ptr [edx+0x5f4] at 0xef005.
        static constexpr uint8_t expected[]{0xff, 0x92, 0xf4, 0x05, 0x00, 0x00};
        viewRayCameraReturn = !memcmp(reinterpret_cast<const uint8_t *>(g) + 0xef005, expected, sizeof expected)
                                  ? reinterpret_cast<uintptr_t>(g) + 0xef00b : 0;
        if (!viewRayCameraReturn) log("View ray search not recognised; Use keeps the native camera ray");
    }
    grenadeViewReturn = reinterpret_cast<uintptr_t>(g) + 0x1020cf; // Hand-grenade throw's GetViewOrigin(0)
    playerControlsReturn = reinterpret_cast<uintptr_t>(g) + 0xf35d3;
    inactivePlayerControlsReturn = reinterpret_cast<uintptr_t>(g) + 0xf2ddc;
    controlsGameBase = reinterpret_cast<uintptr_t>(g);
    rideEngineModelRenderable = reinterpret_cast<uintptr_t>(GetProcAddress(e, "??_7CModelRenderable@SeriousEngine@@6B@"));
    controlsExeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    swimmingPlayerVtable = reinterpret_cast<uintptr_t>(g) + 0x29e878;
    wchar_t rideControlFlag[2]{};
    labRideControlConfigured=!headless &&
        GetEnvironmentVariableW(L"SS2VR_LAB_RIDE_CONTROL",rideControlFlag,2)==1 && rideControlFlag[0]==L'1';
    if((labRideControlConfigured || (!headless && settings.immersiveVehicleGrips)) &&
       (reinterpret_cast<uintptr_t>(GetProcAddress(g,"?ClampLookDirEulAsRide@CPuppetEntity@SeriousEngine@@UAEXAAVVector3f@2@@Z"))!=reinterpret_cast<uintptr_t>(g)+0x901b0 ||
        primaryField(reinterpret_cast<void*>(swimmingPlayerVtable),0x5a0)!=reinterpret_cast<uintptr_t>(g)+0x901e0))return false;
    if((labRideControlConfigured || (!headless && settings.immersiveVehicleGrips)) &&
       (reinterpret_cast<uintptr_t>(GetProcAddress(g,"?GetModelInstance@CBaseEntity@SeriousEngine@@QAEPAVCModelInstance@2@XZ"))!=controlsGameBase+0x450b0 ||
        reinterpret_cast<uintptr_t>(GetProcAddress(g,"?GetToolModelInstance@CBaseEntity@SeriousEngine@@UAEPAVCModelInstance@2@XZ"))!=controlsGameBase+0x450b0 ||
        reinterpret_cast<uintptr_t>(GetProcAddress(g,"?GetModelRenderable@CPuppetEntity@SeriousEngine@@UAEPAVCModelRenderable@2@XZ"))!=controlsGameBase+0x83790 ||
        reinterpret_cast<uintptr_t>(GetProcAddress(e,"?GetModelInstance@CModelRenderable@SeriousEngine@@QAEPAVCModelInstance@2@XZ"))!=reinterpret_cast<uintptr_t>(e)+0x15b220 ||
        primaryField(g,0x294cf0)!=reinterpret_cast<uintptr_t>(e)+0x15b220))return false;
    if(labRideControlConfigured || (!headless && settings.immersiveVehicleGrips))
        for(auto table:{0x2a8558u,0x2b8420u})
            if(primaryField(reinterpret_cast<void*>(controlsGameBase+table),0xbc)!=controlsGameBase+0x83790 ||
               primaryField(reinterpret_cast<void*>(controlsGameBase+table),0x128)!=controlsGameBase+0x450b0)return false;
    if (!headless) {
        expectedDepthRange = reinterpret_cast<DepthRange>(reinterpret_cast<uint8_t *>(graphics) + 0x56a0);
        expectedProjectionSet = reinterpret_cast<ProjectionSet>(reinterpret_cast<uint8_t *>(graphics) + 0x69a0);
    }
    bool ok = true;
#define S(module, name, out) ok = symbol(module, name, out) && ok
    S(c, "?hvHandleToPointer@SeriousEngine@@YAPAXK@Z", resolve);
    S(c, "?hvPointerToHandle@SeriousEngine@@YAKPAX@Z", pointerHandle);
    S(c, "?thrIsThisMainThread@SeriousEngine@@YAHXZ", nativeMainThread);
    S(c, "?_st_idInvalid@SeriousEngine@@3UInvalidIdent@1@B", invalidSeatIdent);
    if (!headless) {
        S(c, "?mthQuaternionToEuler@SeriousEngine@@YA?AVVector3f@1@ABVQuaternion4f@1@@Z", quaternionEuler);
        S(g, "?GetOperatorMoveDir@CLeggedPuppetEntity@SeriousEngine@@UAE?AVVector3f@2@V32@0@Z", nativeOperatorMoveDir);
        if (quaternionEuler) { // Native look convention of OpenXR turns left / pitches up (turret stick signs).
            Vec3 left{}, up{};
            quaternionEuler(&left, yaw(.5f));
            quaternionEuler(&up, Quat{std::sin(.15f), 0, 0, std::cos(.15f)});
            log("Native euler of yaw +0.5 (left)=%.3f,%.3f,%.3f pitch +0.3 (up)=%.3f,%.3f,%.3f",
                left.x, left.y, left.z, up.x, up.y, up.z);
        }
    }
    S(c, "?mthMatrixToQuatVect@SeriousEngine@@YA?AVQuatVect@1@ABVMatrix34f@1@@Z", matrixPose);
    S(c, "?strConvertStringToID@SeriousEngine@@YA?AVIDENT@1@PBD@Z", toId);
    S(e, "?simGetCurrent@SeriousEngine@@YAPAVCSimulation@1@XZ", currentSimulation);
    S(e, "?wldGetCurrent@SeriousEngine@@YAPAVCWorld@1@XZ", currentWorld);
    S(g, "?samIsSinglePlayer@SeriousEngine@@YAHXZ", singlePlayer);
    S(g, "?samGetGameInfoEntity@SeriousEngine@@YA?AV?$Handle@VCGameInfoEntity@SeriousEngine@@@1@XZ", nativeGameInfo);
    if(labSniperConfigured) {
        S(g,"?IsCheatingEnabled@CGameInfoEntity@SeriousEngine@@QAEHXZ",labCheatsEnabled);
        ok=reinterpret_cast<uintptr_t>(labCheatsEnabled)==reinterpret_cast<uintptr_t>(g)+0x118cf0 && ok;
    }
    S(g, "?IsComboWeaponsEnabled@CGameInfoEntity@SeriousEngine@@QAEHXZ", nativeComboWeapons);
    S(g, "?IsFlippingFireButtons@CPlayerPuppetEntity@SeriousEngine@@UAEHXZ", nativeFlipButtons);
    S(g, "?GetWeaponFiringButton@CPlayerPuppetEntity@SeriousEngine@@UAEJV?$Handle@VCBaseWeaponEntity@SeriousEngine@@@2@@Z", nativeWeaponButton);
    S(g, "??_7CSniperWeaponEntity@SeriousEngine@@6B@", sniperVtable);
    S(g, "?IsZooming@CSniperWeaponEntity@SeriousEngine@@UAEHXZ", nativeZoomFlag);
    S(g, "?OnAlternativeFirePressed@CBaseWeaponEntity@SeriousEngine@@UAEHXZ", nativeBaseAlternativePress);
    S(g, "?OnAlternativeFireReleased@CBaseWeaponEntity@SeriousEngine@@UAEXXZ", nativeBaseAlternativeRelease);
    S(g, "?CanFireFromOwnWeapons@CPuppetEntity@SeriousEngine@@QAEHXZ", nativeOwnWeapons);
    nativeTeleport = loadProc<TeleportFn>(g, "?Teleport@CPuppetEntity@SeriousEngine@@UAEHVQuatVect@2@@Z");
    labLoadGame = reinterpret_cast<decltype(labLoadGame)>(reinterpret_cast<uintptr_t>(g) + 0x26980);
    nativeSimNow = loadProc<decltype(nativeSimNow)>(e, "?simNow@SeriousEngine@@YA?AVTime@1@XZ");
    ticksToSeconds = loadProc<decltype(ticksToSeconds)>(c, "?timTicksToFloat@SeriousEngine@@YAM_J@Z");
    {
        // The performance counter's step here (logged once: coarse steps judder anything timed by it).
        LARGE_INTEGER a{}, b{}, f{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        double smallest = 1e9;
        const ULONGLONG until = GetTickCount64() + 40;
        while (GetTickCount64() < until) {
            QueryPerformanceCounter(&b);
            if (b.QuadPart != a.QuadPart) { smallest = std::min(smallest, double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart)); a = b; }
        }
        log("Startup clock: performance counter %.0f Hz, smallest step %.4f ms", double(f.QuadPart), smallest);
    }
    S(g, "?IsLocal@CPuppetEntity@SeriousEngine@@QAEHXZ", isLocal);
    S(g, "?IsAlive@CPuppetEntity@SeriousEngine@@UAEHXZ", isAlive);
    S(g, "?RendersIn3rdPerson@CPuppetEntity@SeriousEngine@@UAEHXZ", thirdPerson);
    S(g, "?GetAbsPlacement@CPuppetEntity@SeriousEngine@@UAE?AVQuatVect@2@XZ", nativeBodyPlacement);
    S(g, "?GetViewOrigin@CPuppetEntity@SeriousEngine@@UAE?AVQuatVect@2@W4LOSRequestPurpose@2@@Z",
      baseViewOrigin);
    S(g, "?GetHealth@CPuppetEntity@SeriousEngine@@UAEJXZ", health);
    S(g, "?GetArmor@CPuppetEntity@SeriousEngine@@UAEJXZ", armor);
    S(g, "?IsDualWielding@CPlayerPuppetEntity@SeriousEngine@@QAEHXZ", isDual);
    S(g, "?IsNetricsaScreenActive@CPlayerPuppetEntity@SeriousEngine@@QAEHXZ", isNetricsa);
    S(g, "?SetCurrentWeapon@CPlayerPuppetEntity@SeriousEngine@@UAEXW4WeaponIndex@2@W4PlayerHand@2@H@Z",
      setWeapon);
    S(g, "?ToggleDualWielding@CPlayerPuppetEntity@SeriousEngine@@QAEXH@Z", toggleDual);
    S(g, "?CanChangeWeapon@CPlayerPuppetEntity@SeriousEngine@@QAEHW4PlayerHand@2@@Z", canChange);
    S(g, "?IsWeaponInInventory@CPlayerInventory@SeriousEngine@@QAEHW4WeaponIndex@2@@Z", inInventory);
    S(g, "?GetAmmoQuantityForWeapon@CPlayerInventory@SeriousEngine@@QAEJW4WeaponIndex@2@@Z", ammo);
    if (!headless) {
        S(e, "?GetModelInstance@CModelRenderable@SeriousEngine@@QAEPAVCModelInstance@2@XZ", laserModelInstance);
        S(e, "?mdlGetAttachmentAbsolutePlacement@SeriousEngine@@YAHPAVCModelInstance@1@VIDENT@1@AAVMatrix34f@1@@Z", laserAttachment);
        symbol(e, "?mdlModelInstanceGetBoundingBox@SeriousEngine@@YA?AVBox3f@1@PBVCModelInstance@1@@Z", modelBounds);
        symbol(e, "?cldGetHullsInRange@SeriousEngine@@YAXABVVector3f@1@MAAV?$CDynamicContainer@VCHull@SeriousEngine@@@1@@Z", hullsInRange);
        symbol(e, "?GetOwner@CAspect@SeriousEngine@@QAEPAVCEntity@2@XZ", hullOwner);
        symbol(e, "?GetBody@CHull@SeriousEngine@@QAEPAVCBody@2@XZ", hullBody);
        symbol(e, "?SetMaxLinVel@CManipulatorJoint@SeriousEngine@@QAEXM@Z", jointMaxLinVel);
        symbol(e, "?GetMechanism@CHull@SeriousEngine@@QAEPAVCMechanism@2@XZ", hullMechanism);
        symbol(e, "?AddToIgnoreList@CMechanism@SeriousEngine@@QAEXPAV12@@Z", mechanismIgnore);
        symbol(e, "?RemoveFromIgnoreList@CMechanism@SeriousEngine@@QAEXPAV12@@Z", mechanismUnignore);
        symbol(e, "?IsIgnored@CMechanism@SeriousEngine@@QAEHPAV12@@Z", mechanismIgnored);
        symbol(e, "?mdlGetBoneAbsolutePlacement@SeriousEngine@@YAHPAVCModelInstance@1@VIDENT@1@AAVMatrix34f@1@@Z", boneAbsolute);
        symbol(e, "?mdlGetBonesAbsPlacement@SeriousEngine@@YAXPAVCModelInstance@1@AAV?$CStaticStackArray@UBonePlacement@SeriousEngine@@@1@@Z", bonesAbsolute);
        symbol(c, "?strConvertIDToString@SeriousEngine@@YAPBDVIDENT@1@@Z", identToString);
        symbol(g, "?OnGiveAll@CPlayerPuppetEntity@SeriousEngine@@QAEXH@Z", giveAllWeapons);
        symbol(g, "?GetAmmoQuantity@CPlayerInventory@SeriousEngine@@QAEJW4AmmoIndex@2@@Z", grenadeAmmoQuantity);
        symbol(c, "?memPreDeleteArrayRC_internal@SeriousEngine@@YAXPBXPAVCDataType@1@@Z", preDeleteArray);
        symbol(c, "?memDeleteArray_internal@SeriousEngine@@YAXPAX@Z", deleteArray);
        symbol(c, "?memPostDeleteArrayRC_internal@SeriousEngine@@YAXPAVCDataType@1@@Z", postDeleteArray);
        S(g, "?RenderNavigationalBeacons@CPuppetEntity@SeriousEngine@@QAEXABVMatrix34f@2@ABVMatrix44f@2@ABVVector2l@2@M@Z", nativeNavigation);
        S(g, "?RenderMissionObjectives@CPuppetEntity@SeriousEngine@@QAEXABVMatrix34f@2@ABVMatrix44f@2@ABVVector2l@2@M@Z", nativeObjectives);
        S(g, "?samGetWorldInfo@SeriousEngine@@YAPAVCWorldInfoEntity@1@XZ", nativeWorldInfo);
        S(e, "?gfxGetCurrentDrawPort@SeriousEngine@@YAPAVCDrawPort@1@XZ", nativeDrawPort);
        S(e, "?gfuBlendType@SeriousEngine@@YAXW4GfuBlendType@1@@Z", nativeBlendType);
        S(e, "?GetData@CInputBindings@SeriousEngine@@QAEPAVCInputBindingsData@2@XZ", getData);
        S(g, "?GetMechanism@CPuppetEntity@SeriousEngine@@UAEPAVCMechanism@2@XZ", getMechanism);
        S(e, "?raySetRay@SeriousEngine@@YAXABVRay3f@1@@Z", setRay);
        S(e, "?raySetMaxDistance@SeriousEngine@@YAXM@Z", rayMaximum);
        S(e, "?raySetMinDistance@SeriousEngine@@YAXM@Z", rayMinimum);
        S(e, "?raySetRayRadius@SeriousEngine@@YAXM@Z", rayRadius);
        S(e, "?cldCheckRay@SeriousEngine@@YAHXZ", checkRay);
        S(e, "?rayIsHit@SeriousEngine@@YAHXZ", rayHit);
        S(e, "?rayGetHitDistance@SeriousEngine@@YAMXZ", hitDistance);
        S(e, "?cldSetRayCategory@SeriousEngine@@YAXVIDENT@1@@Z", rayCategory);
        S(e, "?cldSetThickRayCategory@SeriousEngine@@YAXVIDENT@1@@Z", thickCategory);
        S(e, "?cldSetAvatar@SeriousEngine@@YAXPAVCEntity@1@@Z", rayAvatar);
        S(e, "?cldSetAvatarMechanism@SeriousEngine@@YAXPAVCMechanism@1@@Z", rayMechanism);
        S(e, "?cldSetRayTestsFluids@SeriousEngine@@YAXH@Z", rayFluids);
        S(e, "?gfuDrawLine3f@SeriousEngine@@YAXABVVector3f@1@0KK@Z", drawLine);
        // Steering-wheel entries are optional: a missing one disables only the wheel.
        bool wheelSymbols = true;
#define WHEEL(module, name, out) wheelSymbols = symbol(module, name, out) && wheelSymbols
        WHEEL(c, "?resObtainResource@SeriousEngine@@YA?AV?$Ptr@VCResource@SeriousEngine@@@1@PAVCDataType@1@W4ResObtainMethod@1@HHPBD2@Z",
          resourceObtain);
        WHEEL(e, "?md_pdtDataType@CTexture@SeriousEngine@@2PAVCDataType@2@A", textureDataType);
        WHEEL(e, "?gfuCurrentTexture@SeriousEngine@@YAXJPAVCTexture@1@W4TexBindType@1@@Z", currentTexture);
        WHEEL(c, "?AddRef@CSmartObject@SeriousEngine@@QAEXPBX@Z", smartAddRef);
        WHEEL(c, "?RemRef@CSmartObject@SeriousEngine@@QAEJPBX@Z", smartRemRef);
        {
            void *table=nullptr;
            WHEEL(e, "??_7CStaticTexture@SeriousEngine@@6B@", table);
            staticTextureVtable=reinterpret_cast<uintptr_t>(table);
        }
        WHEEL(e, "?gfuUseSimpleShader@SeriousEngine@@YAXW4GfuSimpleShader@1@@Z", useSimpleShader);
        WHEEL(e, "?gfuConstantColor@SeriousEngine@@YAXK@Z", constantColor);
        WHEEL(e, "?_gfxUnbindVertexBuffers@SeriousEngine@@3P6AXXZA", unbindVertexBuffers);
        WHEEL(e, "?_gfxBindVertexArray@SeriousEngine@@3P6AXJPBXW4GfxUsageType@1@JJ@ZA", bindVertexArray);
        WHEEL(e, "?_gfxDrawVertices@SeriousEngine@@3P6AXW4GfxPrimitive@1@JJ@ZA", drawVertices);
#undef WHEEL
        if (!wheelSymbols) {
            settings.steeringWheel.enabled = false;
            log("Steering wheel disabled: native drawing/resource entries unavailable");
        }
        // Weapon-wheel presentation is optional as well: without these entries the
        // wheel still selects, labelled only by the host. Menu order is cosmetic.
        bool weaponWheelSymbols = wheelSymbols;
#define WEAPON_WHEEL(module, name, out) weaponWheelSymbols = symbol(module, name, out) && weaponWheelSymbols
        WEAPON_WHEEL(g, "?samGetWeaponIcon@SeriousEngine@@YAPAVCTexture@1@W4WeaponIndex@1@@Z", nativeWeaponIcon);
        WEAPON_WHEEL(e, "?_gfx_eBlendSrc@SeriousEngine@@3W4GfxBlend@1@A", blendSource);
        WEAPON_WHEEL(e, "?_gfx_eBlendDst@SeriousEngine@@3W4GfxBlend@1@A", blendDestination);
        WEAPON_WHEEL(e, "?_gfxBlendFunc@SeriousEngine@@3P6AXW4GfxBlend@1@0@ZA", setBlendFunction);
#undef WEAPON_WHEEL
        if (!weaponWheelSymbols) {
            setBlendFunction = nullptr;
            log("Weapon wheel drawing disabled: native icon/blend entries unavailable");
        }
        // Floating meshes are optional on top of that: without them, icons remain.
        bool weaponMeshSymbols = weaponWheelSymbols;
#define WEAPON_MESH(module, name, out) weaponMeshSymbols = symbol(module, name, out) && weaponMeshSymbols
        WEAPON_MESH(e, "?md_pdtDataType@CModelConfiguration@SeriousEngine@@2PAVCDataType@2@A", modelDataType);
        WEAPON_MESH(e, "?_gfxEnableFog@SeriousEngine@@3P6AXXZA", enableFog);
        WEAPON_MESH(e, "?_gfxDisableFog@SeriousEngine@@3P6AXXZA", disableFog);
        WEAPON_MESH(e, "?_gfx_bFogging@SeriousEngine@@3HA", fogging);
        {
            void *table=nullptr;
            WEAPON_MESH(e, "??_7CModelConfiguration@SeriousEngine@@6B@", table);
            modelConfigurationVtable=reinterpret_cast<uintptr_t>(table);
        }
        WEAPON_MESH(e, "?mdlCreateModelInstance@SeriousEngine@@YAPAVCModelInstance@1@XZ", createModelInstance);
        WEAPON_MESH(e, "?mdlDeleteModelInstance@SeriousEngine@@YAXPAVCModelInstance@1@@Z", deleteModelInstance);
        WEAPON_MESH(e, "?mdlModelInstanceSetConfiguration@SeriousEngine@@YAXPAVCModelInstance@1@PAVCModelConfiguration@1@@Z",
          configureModelInstance);
        WEAPON_MESH(e, "?mdlModelInstanceSetStretch@SeriousEngine@@YAXPAVCModelInstance@1@ABVVector3f@1@@Z",
          stretchModelInstance);
        WEAPON_MESH(e, "?mdlModelInstanceGetBoundingBox@SeriousEngine@@YA?AVBox3f@1@PBVCModelInstance@1@@Z",
          modelInstanceBox);
        WEAPON_MESH(e, "?mdlRenderModel@SeriousEngine@@YAXPAVCModelInstance@1@ABVMatrix34f@1@MH@Z", renderModelInstance);
        WEAPON_MESH(e, "?mdlModelConfigGetMeshCount@SeriousEngine@@YAJPAVCModelConfiguration@1@@Z", modelMeshCount);
        WEAPON_MESH(e, "?mdlModelConfigGetMesh@SeriousEngine@@YAPAVCMesh@1@PAVCModelConfiguration@1@J@Z", modelMesh);
        WEAPON_MESH(e, "?mshGetRenderMesh@SeriousEngine@@YAPAVCRenderMesh@1@AAVCMesh@1@@Z", meshRenderMesh);
        WEAPON_MESH(e, "?mdlGetRenderMeshLODCount@SeriousEngine@@YAJPAVCRenderMesh@1@@Z", renderMeshLodCount);
        WEAPON_MESH(e, "?mdlGetRenderMeshBoundingBox@SeriousEngine@@YA?AVBox3f@1@PBVCRenderMesh@1@@Z", renderMeshBox);
#undef WEAPON_MESH
        if (!weaponMeshSymbols) {
            renderModelInstance = nullptr;
            stretchModelInstance = nullptr;
            log("Weapon wheel meshes disabled: native model entries unavailable; icons remain");
        }
        if (!symbol(g, "?samGetWeaponsMenuDir@SeriousEngine@@YAJW4WeaponIndex@1@@Z", nativeWeaponMenuDirection) ||
            !symbol(g, "?samGetWeaponsMenuOrder@SeriousEngine@@YAJW4WeaponIndex@1@@Z", nativeWeaponMenuOrder))
            nativeWeaponMenuDirection = nativeWeaponMenuOrder = nullptr;
        S(e, "?_gfxEnableDepthBuffer@SeriousEngine@@3P6AXXZA", enableDepth);
        S(e, "?_gfxDisableDepthBuffer@SeriousEngine@@3P6AXXZA", disableDepth);
        S(e, "?_gfxEnableDepthWrite@SeriousEngine@@3P6AXXZA", enableDepthWrite);
        S(e, "?_gfxDisableDepthWrite@SeriousEngine@@3P6AXXZA", disableDepthWrite);
        S(e, "?_gfx_bDepthBuffer@SeriousEngine@@3HA", depthEnabled);
        S(e, "?_gfx_bDepthWrite@SeriousEngine@@3HA", depthWriting);
        S(e, "?_gfxEnableBlend@SeriousEngine@@3P6AXXZA", enableBlend);
        S(e, "?_gfxDisableBlend@SeriousEngine@@3P6AXXZA", disableBlend);
        S(e, "?_gfxEnableAlphaTest@SeriousEngine@@3P6AXXZA", enableAlpha);
        S(e, "?_gfxDisableAlphaTest@SeriousEngine@@3P6AXXZA", disableAlpha);
        S(e, "?_gfx_bBlending@SeriousEngine@@3HA", blending);
        S(e, "?_gfx_bAlphaTest@SeriousEngine@@3HA", alphaTesting);
        S(e, "?_gfx_eDepthFunc@SeriousEngine@@3W4GfxComp@1@A", depthComparison);
        S(e, "?_gfxDepthFunc@SeriousEngine@@3P6AXW4GfxComp@1@@ZA", setDepthComparison);
        S(e, "?gfuOrtho@SeriousEngine@@YAXXZ", ortho);
        S(e, "?ActivateExecution@CViewRenCmd@SeriousEngine@@QAEXXZ", activateView);
        S(e, "?_gfx_pcvCurrent@SeriousEngine@@3PAVCCanvas@1@A", currentCanvas);
        S(e, "?_gfx_mCurrentProjection@SeriousEngine@@3VMatrix44f@1@A", nativeCurrentProjection);
        S(e, "?_gfx_mAdjustedProjection@SeriousEngine@@3VMatrix44f@1@A", nativeAdjustedProjection);
        S(e, "?_gfx_hCurrentVertexProgram@SeriousEngine@@3JA", uiVertexHandle);
        S(e, "?_gfx_hCurrentPixelProgram@SeriousEngine@@3JA", uiPixelHandle);
        S(e, "?_gfx_avppPrograms@SeriousEngine@@3V?$CStaticStackArray@UVexPixProgram@SeriousEngine@@@1@A", uiPrograms);
        uiSimplePrograms = reinterpret_cast<int32_t *>(reinterpret_cast<uint8_t *>(e)+0x2e6f2c);
        S(e, "?_gfx_mCurrentView@SeriousEngine@@3VMatrix34f@1@A", nativeCurrentView);
        S(e, "?_gfx_fDepthNear@SeriousEngine@@3MA", nativeDepthNear);
        S(e, "?_gfx_fDepthFar@SeriousEngine@@3MA", nativeDepthFar);
        S(e, "?_gfxDepthRange@SeriousEngine@@3P6AXMM@ZA", nativeDepthRange);
        S(e, "?_gfxProjectionMatrix@SeriousEngine@@3P6AXABVMatrix44f@1@@ZA", nativeProjectionSet);
        S(e, "?_gfx_ulCachedMatrices@SeriousEngine@@3KA", nativeCachedMatrices);
    }
#undef S
    if (!ok)
        return false;
    // Exact installed-build instruction boundaries, checked before any hook is
    // created. The five trampolines relocate these original instructions; in
    // particular 8E75A remains the ONLY writer of native operator history.
    struct PrimaryBytes { uint32_t rva; uint8_t size; uint8_t bytes[6]; };
    constexpr PrimaryBytes primaryBytes[] = {
        {0x8e580,6,{0x55,0x8b,0xec,0x83,0xec,0x18}},
        {0x8e480,6,{0x55,0x8b,0xec,0x56,0x8b,0xf1}},
        {0x8e5bb,6,{0x8a,0x86,0x48,0x03,0x00,0x00}},
        {0x8e5e0,5,{0x83,0xc4,0x04,0x84,0xcb}},
        {0x8e610,5,{0x83,0xc4,0x04,0x84,0xcb}},
        {0x8e75a,6,{0x88,0x8e,0x48,0x03,0x00,0x00}},
        {0x8e4dd,5,{0x83,0xc4,0x04,0x23,0xc3}},
    };
    for (const auto &entry : primaryBytes)
        if (memcmp(reinterpret_cast<const uint8_t *>(g) + entry.rva, entry.bytes, entry.size)) {
            log("Native primary instruction mismatch: %x", unsigned(entry.rva));
            return false;
        }
    if (!headless) {
        toId(&bulletCategory, "bullet");
        toId(&headQueryCategory, "player");
        toId(&laserIdleAttachment, "Barrel01");
        toId(&boneHandIdent, "Hand");
        toId(&bonePalmIdent, "Palm");
        toId(&boneArmIdent, "Arm");
    }
#define H(module, name, fn, orig)                                                                            \
    ok = hook(module, name, reinterpret_cast<void *>(fn), reinterpret_cast<void **>(&orig)) && ok
    if (!headless)
        H(c, "?sysIsAppForeground@SeriousEngine@@YAHXZ", appForeground, originalAppForeground);
    if (!headless) {
        H(e, "?sfuChangeAPI_t@SeriousEngine@@YAXW4SfxAPI@1@J@Z", changeSoundApi, originalChangeApi);
        H(e, "?AddSoundListener@CSoundSyncList@SeriousEngine@@QAEXMMABVQuatVect@2@ABVVector3f@2@@Z", soundListener,
          originalSoundListener);
    }
    if(!headless && labSniperConfigured && labPreparation->sniperZoom)
        H(e,"?mdlGetAttachmentAbsolutePlacement@SeriousEngine@@YAHPAVCModelInstance@1@VIDENT@1@AAVMatrix34f@1@@Z",
          observedAttachment,originalObservedAttachment);
    if (headless) {
        ok = symbol(g, "?GetCameraPlacement@CPuppetEntity@SeriousEngine@@UAE?AVQuatVect@2@XZ",
                    originalCamera) &&
             ok;
        ok = symbol(g, "?GetWeaponAbsPlacement@CBaseWeaponEntity@SeriousEngine@@QAEHABVMatrix34f@2@AAV32@@Z",
                    originalWeaponAbs) &&
             ok;
    } else {
        H(g, "?GetCameraPlacement@CPuppetEntity@SeriousEngine@@UAE?AVQuatVect@2@XZ", camera, originalCamera);
        H(g, "?GetProjectionMatrix@CPuppetEntity@SeriousEngine@@QAE?AVMatrix44f@2@XZ", project,
          originalProjection);
        H(g, "?Render3D@CPuppetEntity@SeriousEngine@@UAEXXZ", render, originalRender);
        H(g, "?GetViewOrigin@CPlayerPuppetEntity@SeriousEngine@@UAE?AVQuatVect@2@W4LOSRequestPurpose@2@@Z", playerViewOrigin,
          originalViewOrigin);
        H(g, "?RenderView@CPlayerBrainEntity@SeriousEngine@@UAEXXZ", brainRender, originalBrainRender);
        H(g, "?RenderOverlay@CPlayerPuppetEntity@SeriousEngine@@UAEXH@Z", overlayRender, originalOverlayRender);
        H(e, "?gfuFill@SeriousEngine@@YAXK@Z", nativeFill, originalNativeFill);
        H(g, "?RendersIn3rdPerson@CPuppetEntity@SeriousEngine@@UAEHXZ", renderThirdPerson,
          originalThirdPerson);
        H(g, "?ClampLookDirEul@CPlayerBrainEntity@SeriousEngine@@QAEXAAVVector3f@2@@Z", mountedLookClamp,
          originalLookClamp);
        if(labRideControlConfigured || settings.immersiveVehicleGrips)
            H(g,"?ClampLookDirEulAsRide@CPuppetEntity@SeriousEngine@@UAEXAAVVector3f@2@@Z",
              observedRideLookClamp,originalRideLookClamp);
        if(labRideControlConfigured) {
            // Both instance aliases name this same native body: install once.
            H(g,"?GetModelInstance@CBaseEntity@SeriousEngine@@QAEPAVCModelInstance@2@XZ",
              observedRideModelInstance,originalRideModelInstance);
            H(g,"?GetModelRenderable@CPuppetEntity@SeriousEngine@@UAEPAVCModelRenderable@2@XZ",
              observedRideRenderable,originalRideRenderable);
            // MinHook steals the virtual call: its return is in this original
            // trampoline, not at Sam2Game450B8. Bind both prefix and jump back.
            const auto trampoline=reinterpret_cast<uintptr_t>(originalRideModelInstance);
            rideModelRenderableReturn=0;
            if(ok && readableMemory(reinterpret_cast<void*>(trampoline),13))
                rideModelRenderableReturn=rideModelTrampolineReturn(
                    std::span(static_cast<const uint8_t*>(reinterpret_cast<void*>(trampoline)),13),
                    uint32_t(trampoline),uint32_t(controlsGameBase+0x450b8));
            ok=ok && rideModelRenderableReturn!=0; // Reach the existing owned-hook rollback on every failure.
        }
        H(e, "?Prepare@CViewRenCmd@SeriousEngine@@QAEXABVMatrix34f@2@ABVMatrix44f@2@ABVBox1f@2@K@Z",
          viewPrepare, originalViewPrepare);
        H(e, "?Execute@CViewRenCmd@SeriousEngine@@UAEXXZ", viewExecute, originalViewExecute);
    }
    H(e, "?rayInit@SeriousEngine@@YAXXZ", trackedRayInit, originalRayInit);
    H(e, "?cldCheckRay@SeriousEngine@@YAHXZ", observedCheckRay, originalCheckRay);
    H(e, "?cldContinueRay@SeriousEngine@@YAHXZ", observedContinueRay, originalContinueRay);
    H(e, "?mdlModelCheckRay@SeriousEngine@@YAHPAVCModelInstance@1@ABVMatrix34f@1@H@Z",
      observedModelCheckRay, originalModelCheckRay);
    H(g, "?OnStep@CPlayerPuppetEntity@SeriousEngine@@UAEXXZ", step, originalStep);
    H(g, "?OnDelete@CPlayerPuppetEntity@SeriousEngine@@UAEXXZ", deleted, originalDelete);
    H(g, "?OnDelete@CBaseWeaponEntity@SeriousEngine@@UAEXXZ", weaponDeleted, originalWeaponDelete);
    H(g, "?OnStep@CSniperWeaponEntity@SeriousEngine@@UAEXXZ", sniperStep, originalSniperStep);
    H(g, "?OnStep@CBaseWeaponEntity@SeriousEngine@@UAEXXZ", baseWeaponStep, originalBaseWeaponStep);
    H(g, "?OnFire@CSniperWeaponEntity@SeriousEngine@@UAEHM@Z", sniperFire, originalSniperFire);
    H(g,"?IsWeaponFiringPressed@CPlayerPuppetEntity@SeriousEngine@@UAEHV?$Handle@VCBaseWeaponEntity@SeriousEngine@@@2@@Z",weaponFiringPressed,originalWeaponHeld);
    H(g, "?ActivateZoomMode@CSniperWeaponEntity@SeriousEngine@@UAEXXZ", zoomActivated, originalZoomActivate);
    H(g, "?DeactivateZoomMode@CSniperWeaponEntity@SeriousEngine@@UAEXXZ", zoomDeactivated, originalZoomDeactivate);
    H(g, "?PutDown@CSniperWeaponEntity@SeriousEngine@@UAEXH@Z", sniperPutDown, originalSniperPutDown);
    H(g, "?OnDelete@CSniperWeaponEntity@SeriousEngine@@UAEXXZ", sniperDeleted, originalSniperDelete);
    H(g, "?OnAlternativeFirePressed@CSniperWeaponEntity@SeriousEngine@@UAEHXZ",
      sniperAlternativePressed, originalSniperAlternativePress);
    H(g, "?ExecuteOperatorFiring@CPuppetEntity@SeriousEngine@@UAEXXZ", operatorFiring, originalOperatorFiring);
    H(g, "?IsFireButtonPressed@CPuppetEntity@SeriousEngine@@UAEHJ@Z", fireButtonPressed, originalFireButtonPressed);
    H(g, "?GetShootingPlacement@CBaseWeaponEntity@SeriousEngine@@UAE?AVQuatVect@2@XZ", shot, originalShot);
    H(g, "?GetWeaponChargeDisplaceMatrix@CBaseWeaponEntity@SeriousEngine@@UAE?AVMatrix34f@2@XZ",
      weaponCharge, originalWeaponCharge);
    H(g, "?GetShootingPlacement@CSniperWeaponEntity@SeriousEngine@@UAE?AVQuatVect@2@XZ", sniperShot,
      originalSniperShot);
    if((!headless && labDualTrace()) || settings.physicalMelee)
        H(g,"?OnFireReleased@CBaseWeaponEntity@SeriousEngine@@UAEXXZ",labFireRelease,originalLabFireRelease);
    if (settings.physicalMelee) {
        H(g,"?OnFireButtonPressed@CPuppetEntity@SeriousEngine@@UAEXJ@Z",primaryPressed,originalPrimaryPress);
        H(g,"?OnFireButtonReleassed@CPuppetEntity@SeriousEngine@@UAEXJ@Z",primaryReleased,originalPrimaryRelease);
        H(g,"?OnDelete@CCircularSawWeaponEntity@SeriousEngine@@UAEXXZ",sawDeleted,originalSawDelete);
        H(g,"?OnWeaponPutDown@CCircularSawWeaponEntity@SeriousEngine@@UAEXXZ",sawWeaponPutDown,originalSawWeaponPutDown);
        H(g,"?PutDown@CCircularSawWeaponEntity@SeriousEngine@@UAEXH@Z",sawPutDown,originalSawPutDown);
        H(g,"??0CCircularSawWeaponEntity@SeriousEngine@@QAE@ABV01@@Z",sawCopied,originalSawCopy);
        H(g,"??4CCircularSawWeaponEntity@SeriousEngine@@QAEAAV01@ABV01@@Z",sawAssigned,originalSawAssign);
    }
    if (!headless) {
        H(g, "?DoTheFiring@CBaseWeaponEntity@SeriousEngine@@UAEHM@Z", nativeFire, originalNativeFire);
        H(g, "?GetWeaponAbsPlacement@CBaseWeaponEntity@SeriousEngine@@QAEHABVMatrix34f@2@AAV32@@Z", weaponAbs,
          originalWeaponAbs);
        H(g, "?Render@CBaseWeaponEntity@SeriousEngine@@UAEXVMatrix34f@2@@Z", weaponRender,
          originalWeaponRender);
        H(g, "?Render@CSniperWeaponEntity@SeriousEngine@@UAEXVMatrix34f@2@@Z", sniperRender,
          originalSniperRender);
        H(c, "?mthFrustumFOVX@SeriousEngine@@YA?AVMatrix44f@1@MMMM@Z", weaponFrustum, originalFrustum);
        H(c, "?mthInvertM34f@SeriousEngine@@YA?AVMatrix34f@1@ABV21@@Z", weaponMatrixInverse,
          originalMatrixInverse);
        ok = internalHook(graphics, 0x56a0, reinterpret_cast<void *>(weaponDepthRange),
                          reinterpret_cast<void **>(&originalDepthRange)) && ok;
        ok = shadow_reuse::install(e) && ok;
        {
            const auto exported = [&](const char *name) { return reinterpret_cast<const float *>(GetProcAddress(e, name)); };
            replay::setEngineMatrices(exported("?_gfx_mAdjustedProjection@SeriousEngine@@3VMatrix44f@1@A"),
                                      exported("?_gfx_mCurrentProjection@SeriousEngine@@3VMatrix44f@1@A"),
                                      exported("?_gfx_mCurrentView@SeriousEngine@@3VMatrix34f@1@A"));
            single_pass::setEngineMatrices(exported("?_gfx_mAdjustedProjection@SeriousEngine@@3VMatrix44f@1@A"),
                                           exported("?_gfx_mCurrentProjection@SeriousEngine@@3VMatrix44f@1@A"),
                                           exported("?_gfx_mCurrentView@SeriousEngine@@3VMatrix34f@1@A"));
            ok = hook(e, "?gfxBindMatrix@SeriousEngine@@YAXW4GfxMatrixType@1@J@Z", reinterpret_cast<void *>(bindMatrix),
                      reinterpret_cast<void **>(&originalBindMatrix)) && ok;
        }
        H(g, "?ProcessPlayerControls@CPlayerBrainEntity@SeriousEngine@@UAEXEVVector3f@2@0@Z",
          waterPlayerControls, originalPlayerControls);
        H(g, "?HandleObjectCarrying@CPuppetEntity@SeriousEngine@@QAEXXZ", carryStep, originalCarrying);
        H(e, "?SetDesiredPlacements@CManipulatorJoint@SeriousEngine@@QAEXABVQuatVect@2@0@Z", carryJointPlacements,
          jointPlacements);
        H(g, "?IsComboWeaponsEnabled@CGameInfoEntity@SeriousEngine@@QAEHXZ", comboWeapons, originalComboWeapons);
        H(e, "?PollValues@CInputBindings@SeriousEngine@@QAEXXZ", poll, originalPoll);
        if (!headless) {
            // Optional: bloom renders screen-locked when ProcRender is loaded.
            const HMODULE procRender = GetModuleHandleW(L"ProcRender.dll");
            const char *bloom = "?RenderEffect@CBloomRenderable@SeriousEngine@@UAEXPAVCEffectRenCmd@2@@Z";
            if (procRender && GetProcAddress(procRender, bloom)) H(procRender, bloom, bloomRenderEffect, originalBloomRender);
            else log("Bloom renderable not hooked (ProcRender.dll %s)", procRender ? "lacks the export" : "not loaded");
        }
        H(e, "?GetControlValue@CKbdMouseInputDevice@SeriousEngine@@UAEMJ@Z", mouseControlValue, originalMouseControl);
        H(e, "?SubstituteCommands@CInputBindings@SeriousEngine@@QAE?AVCString@2@PBD@Z", substituteCommands,
          originalSubstitute);
        H(e, "?GetCommandValue@CInputBindings@SeriousEngine@@QAEMVIDENT@2@@Z", commandValue, originalValue);
        H(e, "?IsCommandDown@CInputBindings@SeriousEngine@@QAEHVIDENT@2@@Z", commandDown, originalDown);
        H(e, "?IsCommandPressed@CInputBindings@SeriousEngine@@QAEHVIDENT@2@@Z", commandPressed,
          originalPressed);
        H(e, "?IsCommandReleased@CInputBindings@SeriousEngine@@QAEHVIDENT@2@@Z", commandReleased,
          originalReleased);
        H(e, "?IsCommandPressedOrRepeated@CInputBindings@SeriousEngine@@QAEHVIDENT@2@@Z", commandRepeated,
          originalRepeated);
    }
    H(e, "?Step@CSimulation@SeriousEngine@@QAEXXZ", simulationStep, originalSimulationStep);
    H(g, "?PlayRideAndRiderSequenceState@CLeggedPuppetEntity@SeriousEngine@@UAEXXZ", rideSequence, originalRideSequence);
    weaponTraceBase = reinterpret_cast<uintptr_t>(g);
    H(g, "?UnregisterFromRide@CPuppetEntity@SeriousEngine@@UAEXXZ", unregisterFromRide, originalUnregisterFromRide);
    H(g, "?CantBeUsed@CPlayerPuppetEntity@SeriousEngine@@UAEXV?$Handle@VCBaseWeaponEntity@SeriousEngine@@@2@@Z", cantBeUsed,
      originalCantBeUsed);
    H(g, "?SetCurrentWeapon@CPlayerPuppetEntity@SeriousEngine@@UAEXW4WeaponIndex@2@W4PlayerHand@2@H@Z", setCurrentWeaponTraced,
      originalSetCurrentWeapon);
    H(g, "?SetNewWeapon@CPlayerPuppetEntity@SeriousEngine@@UAEXW4WeaponIndex@2@HHH@Z", setNewWeaponTraced, originalSetNewWeapon);
    H(g, "?SetNewWeaponToPlayerHand@CPlayerPuppetEntity@SeriousEngine@@UAEXW4WeaponIndex@2@HW4PlayerHand@2@H@Z", setToHandTraced,
      originalSetToHand);
    H(g, "?OnWeaponPutDown@CPlayerPuppetEntity@SeriousEngine@@UAEXV?$Handle@VCBaseWeaponEntity@SeriousEngine@@@2@@Z", weaponPutDownTraced,
      originalWeaponPutDown);
    if (!headless) {
        hud_wrist::install(g, e); // Wrist HUD element tags (no effect in [HUD] Mode=Head).
        hud_wrist::setAmmoSide(&hudAmmoSide);
    }
    if (labRideKind() >= 2) {
        H(g, "?ProcessViewRayCasting@CPlayerBrainEntity@SeriousEngine@@QAEXXZ", viewRayCasting, originalViewRayCasting);
        if (labRideKind() == 2) {
            H(g, "?OnStep@CHovercraftPuppetEntity@SeriousEngine@@UAEXXZ", hoverStep, originalHoverStep);
            H(g, "?OnCreate@CHovercraftPuppetEntity@SeriousEngine@@UAEXPAVCEntityProperties@2@@Z", hoverCreate, originalHoverCreate);
        } else if (labRideKind() == 4) {
            H(g, "?OnStep@CLeggedPuppetEntity@SeriousEngine@@UAEXXZ", leggedStep, originalLeggedStep);
        } else {
            H(g, "?OnStep@CRollingBallPuppetEntity@SeriousEngine@@UAEXXZ", ballStep, originalBallStep);
            H(g, "?ResetDynamicMembers@CRollingBallPuppetEntity@SeriousEngine@@UAEXXZ", ballReset, originalBallReset);
        }
    }
#undef H
    if (!headless)
        ok = internalHook(g, 0x176dc0, reinterpret_cast<void *>(damageHudRender),
                          reinterpret_cast<void **>(&originalDamageHudRender)) && ok;
    ok = internalHook(g, 0x8e5bb, reinterpret_cast<void *>(primaryDownPredicate), &primaryDownPredicate_original) && ok;
    ok = internalHook(g, 0x8e5e0, reinterpret_cast<void *>(primaryPressPredicate), &primaryPressPredicate_original) && ok;
    ok = internalHook(g, 0x8e610, reinterpret_cast<void *>(primaryReleasePredicate), &primaryReleasePredicate_original) && ok;
    ok = internalHook(g, 0x8e75a, reinterpret_cast<void *>(primaryHistoryPredicate), &primaryHistoryPredicate_original) && ok;
    ok = internalHook(g, 0x8e4dd, reinterpret_cast<void *>(primaryHeldPredicate), &primaryHeldPredicate_original) && ok;
    ok = internalHook(g,0x171d00,reinterpret_cast<void *>(zoomActivatePredicate),&zoomActivatePredicate_original) && ok;
    ok = internalHook(g,0x171e05,reinterpret_cast<void *>(zoomOwnerPredicate),&zoomOwnerPredicate_original) && ok;
    ok = internalHook(g,0x171e7b,reinterpret_cast<void *>(zoomFovPredicate),&zoomFovPredicate_original) && ok;
    ok = internalHook(g,0x172854,reinterpret_cast<void *>(zoomInterpolatePredicate),&zoomInterpolatePredicate_original) && ok;
    ok = internalHook(g,0x17297f,reinterpret_cast<void *>(zoomSoundStartPredicate),&zoomSoundStartPredicate_original) && ok;
    ok = internalHook(g,0x172a3c,reinterpret_cast<void *>(zoomSoundStopPredicate),&zoomSoundStopPredicate_original) && ok;
    if(ok&&(settings.roomscale||settings.headFade)&&!headless) {
        ok=configureRoomscaleSweepQueries(e)&&queueRoomscaleTriangleHook(c,hook)&&
            queueRoomscalePrimitiveHook(c,hook)&&queueRoomscaleResourceGates(e,internalHook);
        if(ok)configureRoomscaleProviderBuffers(graphics); // Fingerprinted by supported().
        if(ok&&settings.roomscale)ok=queueRoomscalePlacementHooks(e,hook,internalHook);
    }
    ok = multiplayer::initialize(e, c, g, hook) && ok;
    if (!headless && voice::start()) multiplayer::setVoiceSink(&voice::receive);
    ok = internalHook(e, 0x1b3b60, reinterpret_cast<void *>(entityStep),
                      reinterpret_cast<void **>(&originalEntityStep)) &&
         ok;
    if (!headless) {
        ok = menus::initialize(g, internalHook) && ok;
        ok = remote_render::initialize(e, c, g, internalHook, settings.remoteHeadTracking,labRideControlConfigured,
            settings.immersiveVehicleGrips) && ok;
    }
    if (!ok) {
        rollbackNativeHooks();
        return false;
    }
    ok = MH_ApplyQueued() == MH_OK;
    if(ok&&(settings.roomscale||settings.headFade)&&!headless) {
        roomscaleConfigured=(!settings.roomscale||armRoomscalePlacementAfterEnable())&&roomscaleCollisionKernelsUsable()&&
            roomscaleResourceGatesUsable();
        ok=roomscaleConfigured;
    }
    if (!ok)
        rollbackNativeHooks();
    meleeConfigured = ok && settings.physicalMelee;
    hooksReady = ok;
    log("Native VR hooks %s; no game/headset verification", ok ? "attached" : "failed");
    return ok;
}
} // namespace ss2vr::game
