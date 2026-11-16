//go:build !linux

package main

// Windows opens a console window for the installer by itself.
func relaunchInTerminal() bool { return false }
