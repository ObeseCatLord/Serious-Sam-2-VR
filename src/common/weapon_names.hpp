#pragma once
namespace ss2vr {
// samGetWeaponParamsPath switch in the fingerprinted Sam2Game.dll.
inline const wchar_t *weaponName(int id) {
    static const wchar_t *names[] = {L"Saw",     L"Zap Gun",  L"Auto SG", L"Double SG", L"Uzi",    L"Minigun",
                                     L"Rockets", L"Grenades", L"Plasma",  L"Klodovik",  L"Cannon", L"Bomb",
                                     L"Colt",    L"Sniper",   L"Unused",  L"Beam",      L"Flamer"};
    return id >= 0 && id < 17 ? names[id] : L"None";
}
} // namespace ss2vr
