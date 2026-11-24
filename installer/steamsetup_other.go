//go:build !linux

package main

// Steam's launch options matter only under Proton (Linux).
func (a *app) steamSetup() error { return nil }
