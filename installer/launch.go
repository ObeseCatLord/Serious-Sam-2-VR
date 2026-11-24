package main

// Steam launch options the mod needs under Proton (Linux, Steam Deck, Steam Frame).

import "strings"

const (
	dllOverride   = `WINEDLLOVERRIDES="d3d9=n,b"`
	openxrImport  = "PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1"
	fexFastConfig = "STEAM_COMPAT_FEX_CONFIG=TSOEnabled:0,Multiblock:1"
)

// mergeLaunchOptions returns the launch options with what the mod needs added
// and everything already there kept: the d3d9 override (merged into an existing
// WINEDLLOVERRIDES), the OpenXR runtime import, and on ARM64 (Steam Frame) the
// FEX settings that let the 32-bit game keep 72 fps (relaxed x86 memory
// ordering, multi-block translation). An existing FEX choice is left alone.
func mergeLaunchOptions(existing string, arm64 bool) string {
	opts := strings.TrimSpace(existing)
	var add []string
	switch {
	case strings.Contains(opts, "d3d9=n"):
	case strings.Contains(opts, `WINEDLLOVERRIDES="`):
		opts = strings.Replace(opts, `WINEDLLOVERRIDES="`, `WINEDLLOVERRIDES="d3d9=n,b;`, 1)
	case strings.Contains(opts, "WINEDLLOVERRIDES="):
		opts = strings.Replace(opts, "WINEDLLOVERRIDES=", "WINEDLLOVERRIDES=d3d9=n,b;", 1)
	default:
		add = append(add, dllOverride)
	}
	if !strings.Contains(opts, "PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES") {
		add = append(add, openxrImport)
	}
	if arm64 && !strings.Contains(opts, "STEAM_COMPAT_FEX_CONFIG") {
		add = append([]string{fexFastConfig}, add...)
	}
	prefix := strings.Join(add, " ")
	switch {
	case opts == "":
		return prefix + " %command%"
	case !strings.Contains(opts, "%command%"):
		// Only game arguments were set: they follow the command.
		if prefix != "" {
			return prefix + " %command% " + opts
		}
		return "%command% " + opts
	case prefix == "":
		return opts
	default:
		return prefix + " " + opts
	}
}
