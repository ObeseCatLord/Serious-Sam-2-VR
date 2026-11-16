//go:build linux

package main

import (
	"os"
	"os/exec"
	"syscall"
	"unsafe"
)

// isTerminal: fd is a real terminal (TCGETS succeeds); /dev/null, pipes and files are not.
func isTerminal(fd uintptr) bool {
	var t syscall.Termios
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, fd, uintptr(syscall.TCGETS), uintptr(unsafe.Pointer(&t)))
	return errno == 0
}

// relaunchInTerminal: started with no terminal (a double-click in a file
// manager), the installer's questions and output would go nowhere and nothing
// would appear. Open it again in a terminal emulator instead. False when it is
// already in a terminal, has no desktop, or no known terminal is installed.
func relaunchInTerminal() bool {
	if os.Getenv("SS2VR_IN_TERMINAL") != "" || (os.Getenv("DISPLAY") == "" && os.Getenv("WAYLAND_DISPLAY") == "") {
		return false
	}
	if isTerminal(os.Stdin.Fd()) || isTerminal(os.Stdout.Fd()) {
		return false
	}
	self, err := os.Executable()
	if err != nil {
		return false
	}
	candidates := [][]string{}
	if t := os.Getenv("TERMINAL"); t != "" {
		candidates = append(candidates, []string{t, "-e"})
	}
	candidates = append(candidates,
		[]string{"x-terminal-emulator", "-e"}, []string{"kgx", "--"}, []string{"gnome-terminal", "--"},
		[]string{"konsole", "-e"}, []string{"xfce4-terminal", "-x"}, []string{"kitty"}, []string{"alacritty", "-e"},
		[]string{"foot"}, []string{"wezterm", "start", "--"}, []string{"ptyxis", "--"}, []string{"tilix", "-e"},
		[]string{"xterm", "-e"})
	for _, c := range candidates {
		path, err := exec.LookPath(c[0])
		if err != nil {
			continue
		}
		cmd := exec.Command(path, append(c[1:], self)...)
		cmd.Env = append(os.Environ(), "SS2VR_IN_TERMINAL=1")
		if cmd.Start() == nil {
			_ = cmd.Process.Release()
			return true
		}
	}
	return false
}
