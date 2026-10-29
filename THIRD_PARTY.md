# Dependencies and research credits

SS2VR's original adapter, host, server module and installer source is licensed under the GNU General Public License version 3 (GPL-3.0-only; full text in LICENSE). It was implemented with Codex and an Astra senior design review; implementation and review assistance included GPT-6.1 Sol and GPT-5.6 Terra. No generated artwork or game assets are included. Serious Sam belongs to its respective rights holders; this project is unofficial.

| Component | Pinned revision | License / notice |
|---|---|---|
| MinHook 1.3.4 | c3fcafdc10146beb5919319d0683e44e3c30d537 | BSD 2-clause; licenses/MinHook.txt includes bundled decoder notice |
| Khronos OpenXR SDK 1.1.53 | 75c53b6e853dc12c7b3c771edc9c9c841b15faaa | Apache 2.0 / MIT alternative; licenses/OpenXR.txt |
| OpenXR's bundled jsoncpp | SDK-pinned source | MIT / public-domain alternative; licenses/jsoncpp.txt |
| MinGW-w64 CRT and winpthreads | Installed cross-toolchain | Notices in licenses/MinGW*.txt |
| Static GCC/libstdc++ support | Installed GCC cross-toolchain | GPLv3 with GCC Runtime Library Exception; both texts in licenses/ |
| Opus 1.5.2 (voice chat codec) | 1.5.2 release sources, built static (float) | BSD 3-clause; licenses/Opus.txt |
| OpenAL Soft 1.24.3 (spatial audio, Bin/SS2VR/OpenAL32.dll) | Official 1.24.3 release source (sha256 7e1fecde...c631ce), built unmodified by tools/build_openal.sh as a separate DLL | LGPL 2.0-or-later; licenses/OpenAL-Soft.txt. Source: https://github.com/kcat/openal-soft/releases/tag/1.24.3 |
| vkd3d shader compiler | 1.17 release, build-only experimental optic shader tool | LGPL2.1-or-later; vendor COPYING. Compiler/library not distributed in mod packages. |

[Nokama0's SS2 Mod Menu](https://github.com/Nokama0/Serious-Sam-2-Mod-Menu) (Unlicense) informed ABI research. Engine facts were checked against the user's installed binaries; no proprietary code or assets are redistributed. Voice chat follows the design of [QuakeSpasm VR](https://github.com/hiinaspace/quakespasm-openvr)'s voice chat (48 kHz Opus frames, voice-activity gate with pre-roll and hangover, adaptive jitter buffer, server relay budget, positional voice fading to radio); its GPL source was not incorporated. Spatial audio follows QuakeSpasm VR's spatial audio design (the tracked head as listener with HRTF, one occlusion ray per sound refreshed every 0.2 s and smoothed over 75 ms, an occluded sound lowpassed with less reverb send, the player's own sounds dry); its GPL source was not incorporated. [F.E.A.R. VR](https://github.com/DR-89/fear-vr) informed the process split as a research reference; its GPL source was not incorporated. Native game inventory, weapon simulation and rendering remain owned by Serious Sam 2.
