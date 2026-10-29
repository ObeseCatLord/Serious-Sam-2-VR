//go:build windows

package main

import (
	"os"
	"path/filepath"
	"syscall"
	"unsafe"
)

// steamRoots lists Steam install locations from the registry plus defaults.
func steamRoots() []string {
	var roots []string
	if p := registryString(syscall.HKEY_CURRENT_USER, `Software\Valve\Steam`, "SteamPath", 0); p != "" {
		roots = append(roots, filepath.Clean(p))
	}
	if p := registryString(syscall.HKEY_LOCAL_MACHINE, `SOFTWARE\Valve\Steam`, "InstallPath", syscall.KEY_WOW64_32KEY); p != "" {
		roots = append(roots, filepath.Clean(p))
	}
	for _, env := range []string{"ProgramFiles(x86)", "ProgramFiles"} {
		if base := os.Getenv(env); base != "" {
			roots = append(roots, filepath.Join(base, "Steam"))
		}
	}
	roots = append(roots, `C:\Program Files (x86)\Steam`)
	return roots
}

// registryString reads a REG_SZ value using only the standard library.
func registryString(root syscall.Handle, path, name string, extra uint32) string {
	var key syscall.Handle
	p, _ := syscall.UTF16PtrFromString(path)
	if syscall.RegOpenKeyEx(root, p, 0, syscall.KEY_READ|extra, &key) != nil {
		return ""
	}
	defer syscall.RegCloseKey(key)
	n, _ := syscall.UTF16PtrFromString(name)
	var typ, size uint32
	if syscall.RegQueryValueEx(key, n, nil, &typ, nil, &size) != nil || typ != syscall.REG_SZ || size < 2 {
		return ""
	}
	buf := make([]uint16, size/2+1)
	if syscall.RegQueryValueEx(key, n, nil, &typ, (*byte)(unsafe.Pointer(&buf[0])), &size) != nil {
		return ""
	}
	return syscall.UTF16ToString(buf)
}
