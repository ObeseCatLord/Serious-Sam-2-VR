#pragma once
#include "math.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ss2vr {
// [Weapons] AmmoScreens: every first-person gun that uses ammo shows the held
// weapon's count as a small seven-segment readout. Nothing is loaded for it:
// the digits are flat triangles drawn after the eye's world, depth tested
// against the gun, in the frame of the model bone that carries the spot, so
// they follow the weapon's animation in either hand.
//
// The placement follows Serious Sam Fusion's VR weapons (2017 VR meshes, all
// alike): three digit quads on the gun's centre line at the rear of its top,
// in front of the hand, where the eye looks along the barrel; facing the
// shooter, tilted back 0-30 degrees (normal (0, .49, .87) on most); digits
// 0.63 as wide as tall, 13 mm on the sniper to 22 mm on the double shotgun
// (30-39 mm on the rocket launcher, minigun and cannon); the weapon's ammo
// with leading zeros blank, cyan, blinking red below a tenth of the maximum.
// Guns without ammunition (revolvers, pistol, knife, chainsaw) and the
// Serious Bomb have none; here the bomb shows its count and the revolvers
// the rounds left in the cylinder, one digit each.
// Here an existing screen or panel is used where the gun has one (uzi gauge,
// minigun and rocket launcher screens, beam gun display); otherwise the
// flattest face at that spot, chosen on the stock meshes in their idle pose.
//
// Each spot was measured on the stock meshes (patch 2.090 minigun, 2.100 uzi
// and rocket launcher, latest patch of the rest): surface vertices give the
// plane and extent in bind-pose model space, and the skeleton gives the
// carrying bone's bind pose. Directions: right is model +X unless noted (the
// readout is read from behind the gun, looking down -Z), up runs along the
// surface away from the shooter. Sizes give about 45 x 23 mm in the headset
// at each weapon's display scale (weapon_alignment.hpp).
struct AmmoScreenSpec {
    uint32_t nativeId;
    const char *bone; // The bone the readout's surface is skinned to.
    Pose bind;        // That bone's bind pose (model space).
    Vec3 centre, right, up; // Readout centre and unit axes, bind-pose model space.
    float width, height;    // Readout panel size, model units.
    // 0xRRGGBBAA per AmmoReadoutLayer (panel, unlit, halo, lit): the screen's
    // or the gun's own hue, so the readout reads as part of it.
    uint32_t colour[4];
    uint8_t places = 3; // Digit places at least; another appears while a count needs it.
    // Rounds a load holds for a gun that shows what is left of it (the
    // revolvers' cylinder), 0 to show the inventory ammo.
    uint8_t load = 0;
};
// Uzi: the octagonal gauge on top of the receiver (18.5 x 13.1 mm, sloped up
// toward the muzzle), around the gauge's bullet icon; bone Uzi, bind (0,.1,-.1).
// Minigun: the flip-up screen at the left rear of the body (62 x 43 mm, plane
// z = -.1485) on the Display bone, which is hinged at its left edge and turned
// a quarter turn on the Body bone. Rocket launcher: the sloped screen on top
// (80 x 73 mm), over the foot of its rocket icon; bone RocketLauncher.
// Auto shotgun: the triangular rust-coloured slope at the top rear (38 mm base),
// its wide end. Double shotgun: the breech block's rear face between the two
// hammers (26 mm wide), on the frame (bone Shotgun; the barrels break away).
// Grenade launcher: the carry handle's rear ramp (59 mm wide, tilted 42
// degrees), below its warning label; its own screen is the charge meter.
// Plasma rifle: the top deck just behind the sight hood (a shallow ridge).
// Klodovik: a wrist display on top of Sam's forearm behind the hand the bird
// sits on, in the idle pose (bone LowerArm, turned a quarter on its bind).
// Cannon: the rear ring sloping up to the barrel's top (two facets, 4 mm
// ridge). Serious bomb: one digit on the bomb's forehead above its eyes.
// Revolvers: one digit for the rounds in the cylinder, standing on the rear of
// the top strap like a rear sight (tilted as Fusion's). Sniper: the receiver's
// rear slope below the scope (28 mm wide, rounded). Beam gun: the middle of its
// winged display. Flamer: the receiver's rear face behind the napalm tank.
// Colours (V878 captures): the uzi's screen glows green, the minigun's is
// violet-blue with a cyan trace, the rocket launcher's yellow and red; the
// rest take their gun's accent (rust, shell brass, the launcher's orange
// screen, plasma teal, cyan beam display, fire). Unlit segments stay barely
// above the panel: brighter ghosts read as an 8 (V879).
inline constexpr AmmoScreenSpec AmmoScreens[] = {
    {2, "Main", {{0, 0, 0, 1}, {0, .016f, .0204f}}, {0, .0979f, .1106f}, {1, 0, 0}, {0, .4832f, -.8755f}, .028f,
     .0145f, {0x180904ffu, 0x2a1107ffu, 0xd26a28ffu, 0xffd6b0ffu}},
    {3, "Shotgun", {{0, 0, 0, 1}, {0, .2075f, -.5115f}}, {0, .2425f, -.5998f}, {1, 0, 0}, {0, .9539f, -.3f}, .026f,
     .0135f, {0x161002ffu, 0x261c06ffu, 0xc89a20ffu, 0xfff0b0ffu}},
    {4, "Uzi", {{0, 0, 0, 1}, {0, .1f, -.1f}}, {0, .1736f, -.0837f}, {1, 0, 0}, {0, .9076f, -.4198f}, .0136f, .0072f,
     {0x06140bffu, 0x0c2214ffu, 0x1f9a4affu, 0xb4ffc8ffu}},
    {5, "Display", {{0, -.70710678f, 0, .70710678f}, {-.1485f, .164f, -.15f}}, {-.11f, .1883f, -.1485f}, {1, 0, 0},
     {0, 1, 0}, .046f, .022f, {0x070a1cffu, 0x0e142effu, 0x2f74d8ffu, 0xc0ecffffu}},
    {6, "RocketLauncher", {{0, 0, 0, 1}, {0, 0, -.005f}}, {0, .2685f, -.1221f}, {1, 0, 0}, {0, .6936f, -.7209f}, .042f,
     .02f, {0x170b02ffu, 0x251205ffu, 0xc8641cffu, 0xffdc96ffu}},
    {7, "Back", {{0, 0, 0, 1}, {0, .25f, .01f}}, {0, .3863f, -.084f}, {1, 0, 0}, {0, .6726f, -.74f}, .038f, .0195f,
     {0x1a0d02ffu, 0x2c1606ffu, 0xf08a1effu, 0xffe0b0ffu}},
    {8, "PlasmaRifle", {{0, 0, 0, 1}, {0, 0, 0}}, {0, .1424f, -.11f}, {1, 0, 0}, {0, .3221f, -.9467f}, .026f, .013f,
     {0x041614ffu, 0x082824ffu, 0x20d0a8ffu, 0xb8ffe8ffu}},
    {9, "LowerArm", {{-.0061f, -.7071f, -.0061f, .7071f}, {-.3175f, -.185f, -.013f}}, {-.0887f, -.1425f, -.0066f},
     {.0279f, .3371f, .9411f}, {.9966f, -.0826f, .0001f}, .04f, .021f,
     {0x16061affu, 0x280c2cffu, 0xc43cd0ffu, 0xfbc0ffffu}},
    {10, "Body", {{0, 0, 0, 1}, {0, 0, 0}}, {0, .1538f, .193f}, {1, 0, 0}, {0, .769f, -.6392f}, .046f, .024f,
     {0x1a0603ffu, 0x2e0c06ffu, 0xe0301cffu, 0xffb8a8ffu}},
    {11, "Bomb", {{-.1326f, -.2125f, -.0291f, .9677f}, {.1617f, -.112f, -.5848f}}, {.1397f, .1053f, -.5513f},
     {.9919f, .093f, .0868f}, {0, .6823f, -.731f}, .017f, .021f, {0x1a0803ffu, 0x2e1006ffu, 0xe8501affu, 0xffd0a0ffu},
     1},
    {12, "Colt", {{0, 0, 0, 1}, {-.0051f, -.0134f, -.3996f}}, {-.0051f, .1187f, -.4875f}, {1, 0, 0},
     {0, .867f, -.4983f}, .011f, .014f, {0x0c1014ffu, 0x182028ffu, 0x7090b8ffu, 0xeef6ffffu}, 1, 6},
    {13, "Sniper", {{0, 0, 0, 1}, {0, 0, 0}}, {0, .2335f, .168f}, {1, 0, 0}, {0, .6222f, -.7828f}, .028f, .0145f,
     {0x160802ffu, 0x281006ffu, 0xe05414ffu, 0xffc89affu}},
    {15, "Body", {{0, 0, 0, 1}, {0, .075f, .025f}}, {0, .2295f, -.3359f}, {1, 0, 0}, {0, .7911f, -.6117f}, .038f, .019f,
     {0x041020ffu, 0x081c34ffu, 0x30c8ffffu, 0xd0f6ffffu}},
    {16, "Body", {{0, 0, 0, 1}, {0, .075f, 0}}, {0, .1335f, .1297f}, {1, 0, 0}, {0, .6838f, -.7297f}, .035f, .018f,
     {0x1a0e02ffu, 0x2c1804ffu, 0xf0a01affu, 0xfff0a0ffu}},
};
inline const AmmoScreenSpec *ammoScreen(int nativeId) {
    for (const auto &screen : AmmoScreens)
        if (int(screen.nativeId) == nativeId) return &screen;
    return nullptr;
}
// Readout centre lifted off the screen toward the viewer, metres in the world:
// clear of the screen's own triangles for the depth test, still behind the rim.
inline constexpr float AmmoScreenLift = .0015f;

inline Vec3 ammoScreenApply(const Matrix34 &m, Vec3 p) {
    return {m.m[0] * p.x + m.m[1] * p.y + m.m[2] * p.z + m.m[3], m.m[4] * p.x + m.m[5] * p.y + m.m[6] * p.z + m.m[7],
            m.m[8] * p.x + m.m[9] * p.y + m.m[10] * p.z + m.m[11]};
}
// The readout's world frame: half-extent axes (right, up) at its lifted centre.
struct AmmoScreenFrame {
    Vec3 centre, right, up;
    Vec3 at(float u, float v) const { return centre + right * u + up * v; } // u, v in -1..1
};
// placement: the model's world placement as rendered; stretch: the model's
// rendered stretch (the left hand's model is mirrored in X); bone: the
// carrying bone's current absolute placement in model space (animated).
// Skinning maps a bind-pose point p to bone * inverse(bind) * p, so the screen
// goes through the bone's bind-relative frame and then the live bone.
inline bool ammoScreenFrame(const AmmoScreenSpec &screen, const Matrix34 &placement, Vec3 stretch,
                            const Matrix34 &bone, AmmoScreenFrame &out) {
    const Pose fromBind = inverse(screen.bind);
    const auto world = [&](Vec3 bindPoint) {
        const Vec3 local = fromBind.p + rotate(fromBind.q, bindPoint);
        const Vec3 model = ammoScreenApply(bone, local);
        return ammoScreenApply(placement, {model.x * stretch.x, model.y * stretch.y, model.z * stretch.z});
    };
    const Vec3 c = world(screen.centre);
    Vec3 right = world(screen.centre + screen.right * (screen.width / 2)) - c;
    const Vec3 up = world(screen.centre + screen.up * (screen.height / 2)) - c;
    Vec3 normal = world(screen.centre + cross(screen.right, screen.up) * (screen.height / 2)) - c;
    const float n = std::sqrt(dot(normal, normal));
    if (!(n > 1e-6f) || !std::isfinite(n) || !(dot(right, right) > 1e-12f) || !(dot(up, up) > 1e-12f)) return false;
    normal = normal * (1 / n);
    // A mirrored model (negative stretch) would mirror the digits: keep them
    // reading left to right as seen from the screen's front.
    if (dot(cross(right, up), normal) < 0) right = right * -1.f;
    out = {c + normal * AmmoScreenLift, right, up};
    return std::isfinite(out.centre.x + out.centre.y + out.centre.z + right.x + right.y + right.z + up.x + up.y + up.z);
}

// Seven-segment digits. Bits: 0 top, 1 upper right, 2 lower right, 3 bottom,
// 4 lower left, 5 upper left, 6 middle.
inline constexpr uint8_t SevenSegmentDigits[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
// Three places (one on the bomb and the revolvers), another only while a
// count needs it (full bullets are 1000), up to four.
inline constexpr unsigned AmmoReadoutPlaces = 3, AmmoReadoutMaxPlaces = 4;
// The places of an ammo count, most significant first: a digit, or -1 for an
// unused leading place (shown as unlit segments). Counts clamp to 0..9999.
struct AmmoReadoutText {
    unsigned places = AmmoReadoutPlaces;
    int8_t digit[AmmoReadoutMaxPlaces]{};
};
inline AmmoReadoutText ammoReadoutText(int32_t count, unsigned minimumPlaces = AmmoReadoutPlaces) {
    AmmoReadoutText text;
    int32_t value = std::clamp<int32_t>(count, 0, 9999);
    unsigned needed = 1;
    for (int32_t rest = value; rest >= 10; rest /= 10) ++needed;
    text.places = std::clamp(std::max(minimumPlaces, needed), 1u, AmmoReadoutMaxPlaces);
    for (unsigned i = text.places; i-- > 0;) {
        text.digit[i] = i == text.places - 1 || value ? int8_t(value % 10) : int8_t(-1);
        value /= 10;
    }
    return text;
}
// What a readout counts: the inventory ammo, or for a gun that shows its load
// (the revolvers) the rounds left of it after the shots fired since the last
// reload (CBaseWeaponEntity+AC, which DoReload clears).
inline int32_t ammoScreenCount(const AmmoScreenSpec &screen, int32_t inventory, int32_t shotsSinceReload) {
    if (!screen.load) return inventory;
    return std::clamp<int32_t>(int32_t(screen.load) - std::max<int32_t>(shotsSinceReload, 0), 0, screen.load);
}
// Layers in draw order: a dark panel, the unlit segments (an LCD's faint
// ghosts), a soft halo around the lit segments, the lit segments.
enum class AmmoReadoutLayer : uint8_t { Panel, Unlit, Halo, Lit };
inline constexpr unsigned AmmoReadoutLayerCount = 4;
// Calls emit(layer, u0, v0, u1, v1, u2, v2) for each triangle of the readout,
// in panel coordinates (-1..1 across its width and height), layer by layer.
template <class Emit>
void ammoReadoutTriangles(int32_t count, float width, float height, Emit &&emit,
                          unsigned minimumPlaces = AmmoReadoutPlaces) {
    if (!(width > 0) || !(height > 0)) return;
    const auto text = ammoReadoutText(count, minimumPlaces);
    // Work in panel units of height: the panel spans +-aspect by +-1.
    const float aspect = width / height;
    const auto put = [&](AmmoReadoutLayer layer, float x0, float y0, float x1, float y1, float x2, float y2) {
        emit(layer, x0 / aspect, y0, x1 / aspect, y1, x2 / aspect, y2);
    };
    {   // The panel: a rectangle with chamfered corners, as a fan from its centre.
        const float a = aspect, c = .3f;
        const float corner[8][2] = {{-a + c, 1}, {a - c, 1}, {a, 1 - c}, {a, -1 + c},
                                    {a - c, -1}, {-a + c, -1}, {-a, -1 + c}, {-a, 1 - c}};
        for (unsigned i = 0; i != 8; ++i)
            put(AmmoReadoutLayer::Panel, 0, 0, corner[i][0], corner[i][1], corner[(i + 1) % 8][0], corner[(i + 1) % 8][1]);
    }
    // Digit cells: height 1.5 (of the panel's 2), width 0.55 of that, spaced
    // by 0.2 of it; shrunk to fit a narrow panel. A slight forward slant.
    const unsigned places = text.places;
    float digitHeight = 1.5f;
    const float fit = (2 * aspect - .4f) / ((places * .55f + (places - 1) * .2f) * digitHeight);
    if (fit < 1) digitHeight *= fit;
    const float digitWidth = .55f * digitHeight, spacing = .2f * digitHeight, thickness = .15f * digitHeight;
    const float gap = .035f * digitHeight, slant = .1f;
    const float total = places * digitWidth + (places - 1) * spacing;
    // A segment from (x0, y0) to (x1, y1): a hexagon with pointed ends, half
    // thickness h, its ends pulled in by the gap between neighbouring segments.
    const auto segment = [&](AmmoReadoutLayer layer, float x0, float y0, float x1, float y1, float h, float shorten) {
        float dx = x1 - x0, dy = y1 - y0;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (!(length > 2 * (h + shorten))) return;
        dx /= length; dy /= length;
        const float nx = -dy, ny = dx;
        const float ax = x0 + dx * shorten, ay = y0 + dy * shorten, bx = x1 - dx * shorten, by = y1 - dy * shorten;
        float p[6][2] = {{ax, ay}, {ax + (dx + nx) * h, ay + (dy + ny) * h}, {bx + (nx - dx) * h, by + (ny - dy) * h},
                         {bx, by}, {bx - (dx + nx) * h, by - (dy + ny) * h}, {ax + (dx - nx) * h, ay + (dy - ny) * h}};
        for (auto &q : p) q[0] += slant * q[1];
        for (unsigned i = 1; i + 1 < 6; ++i) put(layer, p[0][0], p[0][1], p[i][0], p[i][1], p[i + 1][0], p[i + 1][1]);
    };
    const float halfX = digitWidth / 2 - thickness / 2, halfY = digitHeight / 2 - thickness / 2;
    for (const auto layer : {AmmoReadoutLayer::Unlit, AmmoReadoutLayer::Halo, AmmoReadoutLayer::Lit})
        for (unsigned place = 0; place != places; ++place) {
            const int digit = text.digit[place];
            const uint8_t lit = digit >= 0 ? SevenSegmentDigits[digit] : 0;
            const float cx = -total / 2 + digitWidth / 2 + place * (digitWidth + spacing);
            // Segment ends: top, middle and bottom rows at the cell's sides.
            const float ends[7][4] = {{-halfX, halfY, halfX, halfY},   {halfX, halfY, halfX, 0},
                                      {halfX, 0, halfX, -halfY},       {-halfX, -halfY, halfX, -halfY},
                                      {-halfX, -halfY, -halfX, 0},     {-halfX, 0, -halfX, halfY},
                                      {-halfX, 0, halfX, 0}};
            for (unsigned s = 0; s != 7; ++s) {
                const bool on = (lit >> s) & 1;
                if ((layer == AmmoReadoutLayer::Unlit) == on) continue;
                const bool halo = layer == AmmoReadoutLayer::Halo;
                segment(layer, cx + ends[s][0], ends[s][1], cx + ends[s][2], ends[s][3],
                        thickness / 2 * (halo ? 1.7f : 1.f), halo ? gap * .2f : gap);
            }
        }
}
} // namespace ss2vr
