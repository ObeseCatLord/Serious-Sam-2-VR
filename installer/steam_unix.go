//go:build !windows

package main

import (
	"os"
	"path/filepath"
)

// steamRoots lists the usual Steam install locations on Linux (native and
// Flatpak). The mod itself runs through Proton; only the game folder matters.
func steamRoots() []string {
	home, err := os.UserHomeDir()
	if err != nil {
		return nil
	}
	return []string{
		filepath.Join(home, ".local", "share", "Steam"),
		filepath.Join(home, ".steam", "steam"),
		filepath.Join(home, ".steam", "root"),
		filepath.Join(home, ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam"),
		filepath.Join(home, "snap", "steam", "common", ".local", "share", "Steam"),
	}
}
