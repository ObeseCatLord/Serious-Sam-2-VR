#pragma once
#include "network.hpp"

namespace ss2vr {
// Caller owns native getter/model calibration admission and current hand/rig
// checks. Preserve the native-selected attachment vector: a fixed controller
// radius would pull long weapons' lasers/shots back inside their rendered barrel.
// This does not establish an anatomical grip or a particular animated child point.
inline Pose retargetCalibratedMuzzle(Pose camera, Pose nativeShot, Vec3 nativeModelLocal,
                                    Pose hand, Vec3 nativeDisplacement = {},
                                    Vec3 nativeAlignment = {}, float scale = 1) {
    return retargetShot(camera, nativeShot, nativeModelLocal, hand, .5f,
                        nativeDisplacement, nativeAlignment, true, scale);
}
// Weapons held along the hand (no flat viewmodel tilt): the native shot point,
// taken into model axes relative to the uncharged model origin, is placed from
// the grip anchor offset; the shot leaves along the hand's forward axis.
inline Pose handAlignedMuzzle(Pose camera, Pose nativeShot, Pose modelInCamera, Vec3 chargeInCamera,
                              Vec3 anchorOffset, Pose hand, float scale) {
    const Vec3 shot = compose(inverse(camera), nativeShot).p;
    const Quat toModel = inverse(modelInCamera.q);
    const Vec3 attachment = rotate(toModel, shot - (modelInCamera.p - chargeInCamera));
    const Vec3 charge = rotate(toModel, chargeInCamera);
    return {normalize(hand.q), hand.p + rotate(hand.q, (charge + anchorOffset + attachment) * scale)};
}
// Transient evidence from native ownership and the frozen authoritative sample.
// This stores no policy across ticks and never resolves game pointers itself.
struct MuzzleBinding {
    uint32_t owner = 0, sampleAvatar = 0, peerAvatar = 0;
    uint32_t sampleIncarnation = 0, peerIncarnation = 0;
    uint32_t handle = 0, boundHandle = 0, currentHandle = 0, otherHandle = 0;
    int nativeId = -1;
    unsigned hand = 2;
    bool negotiated = false, peerValid = false, sampleValid = false, alive = false;
    uint64_t nowMs = 0, receivedMs = 0;
};
inline bool eligibleAuthorityMuzzle(const network::PosePacket &pose, const MuzzleBinding &b) {
    if (!b.negotiated || !b.peerValid || !b.sampleValid || !b.alive || b.hand >= 2 ||
        !b.owner || b.sampleAvatar != b.owner || b.peerAvatar != b.owner || !b.peerIncarnation ||
        b.sampleIncarnation != b.peerIncarnation || !b.handle || b.boundHandle != b.handle ||
        b.currentHandle != b.handle || b.otherHandle == b.handle || b.nowMs < b.receivedMs ||
        b.nowMs - b.receivedMs > network::MaxPoseAgeMs || b.nativeId < 0 ||
        pose.nativeWeaponId[b.hand] != b.nativeId)
        return false;
    const uint8_t required = uint8_t(1u | (2u << b.hand));
    return (pose.validMask & required) == required && finite(pose.head) && finite(pose.grip[b.hand]);
}
inline bool eligibleLocalMuzzle(const Input &input, unsigned hand, bool owned, bool session,
                                bool playerEligible, bool fresh) {
    return owned && session && playerEligible && fresh && hand < 2 && input.headValid &&
           input.handValid[hand] && finite(input.head) && finite(weaponTracking(input, hand));
}
// Selection and adaptation consume the same accepted context. Rejected and
// nested calls invoke only the original getter, without any VR retarget.
template <class NativeGetter, class AttachmentGetter, class Retarget>
void invokeNativeMuzzle(bool sniper, bool accepted, bool nested, NativeGetter native,
                       AttachmentGetter attachment, Retarget retarget, bool *calibrated = nullptr) {
    if (calibrated)
        *calibrated = false;
    if (sniper && accepted && !nested)
        attachment();
    else
        native();
    if (accepted && !nested)
        retarget();
}
} // namespace ss2vr
