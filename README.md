# Serious Sam 2 VR

Serious Sam 2 VR is an OpenXR mod for the Steam version of Serious Sam 2, for PC VR
(Windows or Linux) and the **Steam Frame standalone**.

[Install](#installation) · [Install on Steam Frame (standalone)](#steam-frame-standalone)
<p>
  <img src="docs/media/vr-combat.gif" width="360" height="360" alt="VR combat with tracked dual weapons">
  <img src="docs/media/vr-outdoors.gif" width="360" height="360" alt="VR gameplay outdoors">
</p>

## Features

- OpenXR head and controller tracking with stereo rendering of the original game
- Tracked weapons, dual wielding, and a weapon wheel on each hand
- Throwable grenades, grabbing and throwing objects, and arm-stroke swimming
- Sniper scopes you look through; the scoped shot is live while your eye is behind the scope
- Vehicles and turrets with a virtual steering wheel and grabbable handles
- Menus, cutscenes, and movies on a floating screen; wrist or head HUD
- Co-op with VR and desktop players together, voice chat, and level cutscenes for everyone
- Windows and Linux (Proton), and the Steam Frame standalone with eye-tracked foveated
  rendering at a steady 72 fps

## Installation

Download the installer for your system from
[Releases](https://github.com/ObeseCatLord/Serious-Sam-2-VR/releases/latest) and run it:

- **Windows:** `ss2vr-installer-windows-x86_64.exe`
- **Linux:** `ss2vr-installer-linux-x86_64` (`chmod +x` it first)
- **Steam Frame:** `ss2vr-installer-linux-arm64` (see [Steam Frame](#steam-frame-standalone))

It finds Serious Sam 2 through Steam and installs the mod. Run it again to update
(your settings are kept) or to uninstall. On Linux it also sets the game's Steam
launch options (keeping any you have; Steam closes and restarts briefly for it).

Run the game **windowed** with MSAA off, and start your OpenXR runtime (SteamVR,
Monado, WiVRn...) before the game.

On Linux the installer sets the game's Steam launch options to (or add them yourself):

```
WINEDLLOVERRIDES="d3d9=n,b" PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%
```

### Steam Frame (standalone)

The game runs on the Frame itself (Proton for ARM64 with x86 emulation), no PC needed.

1. Install Serious Sam 2 from Steam on the Frame.
2. In Desktop Mode, download `ss2vr-installer-linux-arm64` from
   [Releases](https://github.com/ObeseCatLord/Serious-Sam-2-VR/releases/latest),
   allow it to run as a program (file properties) and open it. It installs the mod and
   sets everything up: the launch options (Steam restarts briefly) and SteamVR's
   72 Hz refresh rate for the game.
3. Start Serious Sam 2 from Steam.

On the Frame the mod uses its low-spec profile: eye-tracked foveated rendering
(sharp where you look), 0.75 render scale, and the game's costliest effects off
(dynamic shadows, refraction, offset mapping, cube reflections; thinner, closer
grass). The launch options also make the x86 emulation faster
(`STEAM_COMPAT_FEX_CONFIG=TSOEnabled:0,Multiblock:1`). Measured on a Steam Frame:
a steady 72 fps in Jungle, City Metropolis, Land of Giants and Snow Base. Your own
SS2VR.user.ini values ([Render] Scale, LowSpec...) override the profile; a higher
refresh rate can be chosen in SteamVR's settings for the game.

The full launch options there:

```
STEAM_COMPAT_FEX_CONFIG=TSOEnabled:0,Multiblock:1 WINEDLLOVERRIDES="d3d9=n,b" PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%
```

## Co-op

Every player and the server need the same mod version. Desktop players can join VR
players. Run the dedicated server with `+mod SS2VR`. Turn on **USE COMBO WEAPONS**
in the co-op settings for independent dual weapons.

## Settings

In VR, open **VR options** from the weapon wheel's options bubble or the button under
the game's menus. Settings are also in
`Bin/SS2VR/SS2VR.ini` in the game folder.

### VR controllers (Valve Index)

| Input | Action |
| --- | --- |
| Left stick | Move (relative to where you look) |
| Right stick left/right | Smooth or snap turn |
| Trigger | Fire that hand's weapon |
| Trackpad press (either hand) | Weapon wheel for that hand |
| Right A | Use, enter/leave vehicles and turrets |
| Right B | Jump |
| Left B | Pause menu |
| Left A | Reload |
| Left stick click | Sprint |
| Right stick click | Grenade |
| Grip | Grab and throw objects |
| Look through a sniper scope | Zoom (scoped shot) |
| Both sticks clicked | Recenter |

Other controllers:

| Profile | Weapon wheel (left / right) | Use | Jump | Menu | Reload |
| --- | --- | --- | --- | --- | --- |
| Oculus Touch and alike (Rift, Rift S, Quest, Pico 4, HP Reverb G2, Vive Cosmos / Focus 3) | Y / B | A | Grip | Left menu | X |
| HTC Vive | Trackpad click | Right menu | Grip | Left menu | — |
| Windows Mixed Reality (and Samsung Odyssey) | Trackpad click | Left menu | Grip | Right menu | — |
| Steam Frame | Left D-pad left / right B | Right A | Grip | Left view / right menu | Left D-pad down |

## License

GNU General Public License version 3 (GPL-3.0-only); see [LICENSE](LICENSE).
Third-party components are listed in [THIRD_PARTY.md](THIRD_PARTY.md). Serious Sam 2
and its assets belong to their rights holders and are not included.
