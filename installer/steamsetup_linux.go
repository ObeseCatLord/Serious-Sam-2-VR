//go:build linux

package main

// Steam setup on Linux (Steam Deck, Steam Frame, desktop): the game's launch
// options and, on the Steam Frame, SteamVR's refresh rate for the game.

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

const frameVrcmd = "/opt/steamvr/bin/linuxarm64/vrcmd"

func steamPids() []string {
	var pids []string
	entries, _ := os.ReadDir("/proc")
	for _, e := range entries {
		comm, err := os.ReadFile(filepath.Join("/proc", e.Name(), "comm"))
		if err == nil && strings.TrimSpace(string(comm)) == "steam" {
			pids = append(pids, e.Name())
		}
	}
	return pids
}

func steamRunning() bool { return len(steamPids()) > 0 }

// anyAlive: one of these processes still runs (a Steam the session restarted
// has a new pid and does not count).
func anyAlive(pids []string) bool {
	for _, p := range pids {
		if _, err := os.Stat(filepath.Join("/proc", p)); err == nil {
			return true
		}
	}
	return false
}

type localConfig struct {
	path    string
	text    string
	root    *vdfNode
	options string // Current launch options (unescaped).
}

// localConfigs finds every Steam user's localconfig.vdf that knows the game.
func localConfigs() []localConfig {
	var found []localConfig
	seen := map[string]bool{}
	for _, root := range steamRoots() {
		matches, _ := filepath.Glob(filepath.Join(root, "userdata", "*", "config", "localconfig.vdf"))
		for _, m := range matches {
			real, err := filepath.EvalSymlinks(m)
			if err != nil || seen[real] {
				continue
			}
			seen[real] = true
			data, err := os.ReadFile(m)
			if err != nil {
				continue
			}
			tree, err := vdfParse(string(data))
			if err != nil {
				continue
			}
			app := tree.path(false, "UserLocalConfigStore", "Software", "Valve", "Steam", "apps", steamAppID)
			if app == nil {
				continue
			}
			lc := localConfig{path: m, text: string(data), root: tree}
			if leaf, ok := app.leaf("LaunchOptions"); ok {
				lc.options = vdfUnescape(leaf.value)
			}
			found = append(found, lc)
		}
	}
	return found
}

func (a *app) steamSetup() error {
	arm := runtime.GOARCH == "arm64"
	configs := localConfigs()
	var pending []localConfig
	for _, lc := range configs {
		if want := mergeLaunchOptions(lc.options, arm); want != strings.TrimSpace(lc.options) {
			pending = append(pending, lc)
		}
	}
	if len(configs) == 0 {
		fmt.Fprintf(a.out, "Steam launch options: no Steam user has played Serious Sam 2 yet; set them in Steam:\n  %s\n",
			mergeLaunchOptions("", arm))
	} else if len(pending) == 0 {
		fmt.Fprintln(a.out, "Steam launch options: already set.")
	} else {
		if err := a.setLaunchOptions(pending, arm); err != nil {
			fmt.Fprintf(a.out, "Steam launch options not changed (%v). Set them in Steam:\n  %s\n", err,
				mergeLaunchOptions(pending[0].options, arm))
		}
	}
	if arm {
		a.frameRefreshRate()
	}
	return nil
}

func (a *app) setLaunchOptions(pending []localConfig, arm bool) error {
	if a.opts.dry {
		for _, lc := range pending {
			fmt.Fprintf(a.out, "Would set launch options in %s:\n  %s\n", lc.path, mergeLaunchOptions(lc.options, arm))
		}
		return nil
	}
	before := steamPids()
	running := len(before) > 0
	if running {
		// Steam rewrites its config when it exits: it has to be closed for the edit.
		if !a.opts.yes &&
			!a.confirm("Steam needs to close briefly to set the game's launch options. Close and restart Steam now?") {
			return errors.New("Steam left running")
		}
		fmt.Fprintln(a.out, "Closing Steam...")
		_ = exec.Command("steam", "-shutdown").Start()
		// The Steam Frame / Deck session starts Steam again at once: edit as soon
		// as the old Steam is gone, before the new one reads its config.
		deadline := time.Now().Add(90 * time.Second)
		for anyAlive(before) && time.Now().Before(deadline) {
			time.Sleep(50 * time.Millisecond)
		}
		if anyAlive(before) {
			return errors.New("Steam did not close")
		}
	}
	var failed error
	for _, lc := range pending {
		// Read again: Steam saved it on exit.
		data, err := os.ReadFile(lc.path)
		if err != nil {
			failed = err
			continue
		}
		tree, err := vdfParse(string(data))
		app := (*vdfNode)(nil)
		if err == nil {
			app = tree.path(false, "UserLocalConfigStore", "Software", "Valve", "Steam", "apps", steamAppID)
		}
		if app == nil {
			failed = errors.New("unreadable " + lc.path)
			continue
		}
		current := ""
		if leaf, ok := app.leaf("LaunchOptions"); ok {
			current = vdfUnescape(leaf.value)
		}
		want := mergeLaunchOptions(current, arm)
		backup := lc.path + ".ss2vr-backup"
		if _, err := os.Stat(backup); errors.Is(err, os.ErrNotExist) {
			_ = os.WriteFile(backup, data, 0o644)
		}
		app.set("LaunchOptions", vdfEscape(want))
		if err := os.WriteFile(lc.path, []byte(vdfWrite(tree)), 0o644); err != nil {
			failed = err
			continue
		}
		fmt.Fprintf(a.out, "Steam launch options set:\n  %s\n", want)
	}
	if running {
		// The Steam Frame / Deck session starts Steam again by itself; a desktop
		// does not.
		time.Sleep(8 * time.Second)
		if !steamRunning() {
			cmd := exec.Command("steam")
			cmd.Stdin, cmd.Stdout, cmd.Stderr = nil, nil, nil
			_ = cmd.Start()
			if cmd.Process != nil {
				_ = cmd.Process.Release()
			}
		}
		fmt.Fprintln(a.out, "Steam restarted.")
	}
	return failed
}

// frameRefreshRate: the Steam Frame plays the game at 72 Hz unless the player
// chose another rate for it in SteamVR (its per-game setting).
func (a *app) frameRefreshRate() {
	if _, err := os.Stat(frameVrcmd); err != nil {
		return
	}
	key := "steam.app." + steamAppID + ".preferredRefreshRate"
	run := func(args ...string) string {
		cmd := exec.Command(frameVrcmd, args...)
		cmd.Env = append(os.Environ(), "LD_LIBRARY_PATH="+filepath.Dir(frameVrcmd))
		out, _ := cmd.CombinedOutput()
		return string(out)
	}
	current := run("--settings-int", key)
	if i := strings.Index(current, key+"="); i >= 0 {
		value := strings.TrimSpace(strings.SplitN(current[i+len(key)+1:], "\n", 2)[0])
		if value != "" && value != "0" {
			fmt.Fprintf(a.out, "SteamVR refresh rate for the game: %s Hz (kept).\n", value)
			return
		}
	}
	if a.opts.dry {
		fmt.Fprintln(a.out, "Would set SteamVR's refresh rate for the game to 72 Hz.")
		return
	}
	run("--set-settings-int", key, "72")
	fmt.Fprintln(a.out, "SteamVR refresh rate for the game set to 72 Hz (change it in SteamVR's per-game settings).")
}
