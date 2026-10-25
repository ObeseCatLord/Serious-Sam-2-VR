#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>
namespace ss2vr {
// MaxDimension only bounds sanity checks: images travel in pixel sections sized
// to the current canvas, which follows the runtime's recommended eye size.
constexpr uint32_t Magic = 0x32565253, Abi = 14, MaxDimension = 16384, WeaponCount = 17;
// Pixel sections: slot0 left/right, slot1 left/right, then the mono menu frame.
constexpr unsigned PixelBufferCount = 5, MenuPixelBuffer = 4;
constexpr unsigned slotPixelBuffer(unsigned slot, unsigned eye) { return slot * 2 + eye; }
constexpr uint64_t pixelBufferBytes(uint32_t width, uint32_t height) { return uint64_t(width) * height * 4; }
// Wheel is the grip (squeeze): grasp, carry, recenter chord and on-foot jump.
// WheelOpen is the separate hold-to-open weapon wheel button.
enum Button : uint32_t {
    Wheel = 1, Use = 2, Jump = 4, Menu = 8, Recenter = 16, Sprint = 32, Reload = 64, Netricsa = 128, WheelOpen = 256,
    // The hand's profile has a dedicated jump button (Index B): the grip only grabs.
    JumpBound = 512
};
enum FramePresentation : uint32_t { NativeUiComplete = 1 };
enum class SlotState : uint32_t { Empty, Requested, Rendering, Ready };
#pragma pack(push, 8)
struct Vec3 {
    float x = 0, y = 0, z = 0;
};
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};
struct Pose {
    Quat q;
    Vec3 p;
};
struct Fov {
    float left = 0, right = 0, up = 0, down = 0;
};
struct Input {
    uint64_t sequence = 0, tickMs = 0;
    uint32_t session = 0, reference = 0, focused = 0, headValid = 0;
    Pose head, hand[2];
    uint32_t handValid[2]{};
    float trigger[2]{}, axis[2][2]{};
    uint32_t buttons[2]{};
    Pose grip[2];
    uint32_t gripValid[2]{}, blockedWheels = 0;
    // Logical action provenance from the same xrSyncActions sample. An inactive
    // action is unavailable, not a physical release. No runtime paths cross IPC.
    uint32_t primaryActiveMask = 0, zoomActiveMask = 0, zoomDownMask = 0;
    uint32_t primaryInputGeneration[2]{}, zoomInputGeneration[2]{};
    uint32_t zoomSourceButton[2]{};
    // Producer release admission and durable cancellation identity for squeeze.
    // An unavailable action is not a release; skipped samples retain the epoch.
    uint32_t wheelAdmissionMask = 0, wheelInputEpoch[2]{};
};
struct Request {
    uint64_t sequence = 0;
    int64_t predictedTime = 0;
    uint32_t session = 0, reference = 0, width = 0, height = 0;
    uint32_t trackingGeneration = 0, reserved = 0;
    Pose eye[2];
    Fov fov[2];
    Input input;
    // One host comfort anchor frozen with the actual requested eye poses.
    Pose uiPanel;
    float uiWidth = 0, uiHeight = 0;
    uint32_t uiRequested = 0;
    // ABI13 foveated readback: each eye returns a downsampled full view
    // (lowWidth x lowHeight) followed by a full-resolution insetWidth x
    // insetHeight crop centred at insetCentre (tangent space x right, y up).
    uint32_t foveated = 0, lowWidth = 0, lowHeight = 0, insetWidth = 0, insetHeight = 0, foveationReserved = 0;
    float insetCentre[2][2]{};
};
struct WheelUi {
    uint32_t open = 0, count = 0;
    int32_t hover = -1;
    int32_t weapon[WeaponCount]{};
    int32_t ammo[WeaponCount]{};
};
struct Ui {
    uint64_t tickMs = 0;
    uint32_t gameplay = 0, trackingGeneration = 0;
    int32_t health = 0, armor = 0, currentWeapon[2]{-1, -1}, currentAmmo[2]{};
    uint32_t fireSequence[2]{}, damageSequence = 0;
    WheelUi wheel[2];
};
enum class HeadClearanceMode : uint32_t { Disabled=0, Opaque=1, Clear=2 };
// Response-only receipt for the actual image pair. Units are OpenXR metres in
// this request's reference space; no native pointer or world handle crosses IPC.
struct HeadClearance {
    uint64_t tickMs=0;
    Vec3 centre{};
    float clearRadius=0,headRadius=0,nearZ=0;
    HeadClearanceMode mode=HeadClearanceMode::Disabled;
    uint32_t reserved=0;
};
static_assert(sizeof(HeadClearance)==40);
struct Slot {
    SlotState state = SlotState::Empty;
    uint32_t cancelled = 0;
    // Response capability belongs to the actual image pair, not its request.
    uint32_t presentation = 0, presentationReserved = 0;
    Request request;
    HeadClearance headClearance;
    // Pixel section generation holding this pair (see Shared::pixels).
    uint32_t pixelGeneration = 0, pixelReserved = 0;
    // Foveated pairs: the crop actually read back per eye (x, y, w, h pixels).
    int32_t insetRect[2][4]{};
    // ABI14 zero-copy pairs: the SharedImages generation the pair was rendered
    // into (0 = pixel sections). gpuReady becomes 1 once the game has observed
    // its GPU work on those images complete; pixel pairs are ready at once.
    uint32_t sharedImage = 0, gpuReady = 0;
};
// Explicitly mono menu/loading frame; never eligible for a projection layer.
struct MenuFrame {
    uint64_t sequence = 0, tickMs = 0;
    uint32_t visible = 0, width = 0, height = 0, interactionGeneration = 0;
    // overlay: MenuOverLevel (a pause menu or Netricsa over a level: the host
    // keeps the last world frame behind it) | MenuKeyed (its alpha is set: the
    // world was left out and black is transparent).
    uint32_t pixelGeneration = 0, overlay = 0;
};
enum MenuOverlay : uint32_t { MenuOverLevel = 1, MenuKeyed = 2 };
// The game creates one section per buffer for its current canvas extent and
// publishes it here; readers reopen when the generation changes.
struct PixelSections {
    uint32_t generation = 0, width = 0, height = 0, reserved = 0;
};
// ABI14: eye images the game renders straight into, shared with the host as
// D3D9 (DXVK) KMT handles, one per slot and eye. The host opens a generation
// and acknowledges it in opened; until then pairs travel as pixel sections.
// A capture tool that needs pixels sets pixelRequestTick (GetTickCount); the
// game then sends pixel pairs for the next few seconds.
struct SharedImages {
    uint32_t generation = 0, width = 0, height = 0, opened = 0;
    uint32_t handle[2][2]{};
    uint32_t pixelRequestTick = 0, reserved = 0;
};
struct MenuPointer {
    uint64_t inputSequence = 0, tickMs = 0, menuSequence = 0;
    uint32_t session = 0, reference = 0, interactionGeneration = 0, active = 0, hand = 0;
    float u = 0, v = 0, trigger = 0;
    uint32_t primaryInputGeneration = 0;
};
// Host feedback for an actually successful native WORLD projection submission.
// Original input time is never renewed by resubmission of a cached pair.
struct WorldSubmission {
    uint64_t requestSequence=0,sourceTickMs=0;
    uint32_t producer=0,session=0,reference=0,trackingGeneration=0;
    uint32_t continuity=0,active=0;
};
struct Shared {
    uint32_t magic = Magic, abi = Abi, bytes = sizeof(Shared), gamePid = 0;
    uint32_t hostPid = 0, width = 0, height = 0, shutdown = 0, rendererReady = 0, error = 0;
    // Win32 Interlocked publication; invalidation does not wait for the IPC mutex.
    uint32_t trackingGeneration = 0;
    Input latest;
    Ui ui;
    Slot slot[2];
    MenuFrame menu;
    MenuPointer pointer;
    // Appended ABI11 metadata. Grip generations accompany latest under the IPC
    // mutex; feedback can advance independently after xrEndFrame succeeds.
    uint32_t gripPoseGeneration[2]{};
    WorldSubmission worldSubmission;
    // ABI12: dynamically sized image sections and the runtime's recommended
    // per-eye image size (the larger of the two views), published by the host.
    PixelSections pixels;
    uint32_t recommendedWidth = 0, recommendedHeight = 0;
    SharedImages images;
};
#pragma pack(pop)
static_assert(sizeof(Vec3) == 12 && sizeof(Quat) == 16 && sizeof(Pose) == 28);
static_assert(std::is_trivially_copyable_v<Request> && std::is_standard_layout_v<Shared>);
static_assert(offsetof(Shared, latest) == 48);
static_assert(sizeof(Input) == 272 && sizeof(Request) == 480);
static_assert(sizeof(MenuPointer) == 64);
static_assert(sizeof(WorldSubmission)==40 && sizeof(PixelSections)==16 && sizeof(SharedImages)==40);
static_assert(sizeof(Shared) == 2056 && offsetof(Shared, pixels) == 1992 && offsetof(Shared, images) == 2016); // Same on x86 and x64.
} // namespace ss2vr
