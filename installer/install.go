package main

// Install / uninstall / update, ported from tools/install.py.
//
// Differences from install.py are deliberate hardening only:
//   - every check (fingerprints, package hashes, collisions, the Bin/SS2VR
//     folder state) runs before anything is moved, so a refused install never
//     leaves runtime leftovers renamed to *.previous;
//   - an update (or --force-reinstall) preflights both the removal and the new
//     install before touching the game, backs up the old owned files, and
//     restores them if the new install fails part-way;
//   - a failed install moves runtime leftovers back into Bin/SS2VR.

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"
)

// ErrGameMismatch is returned when a game_fingerprints entry differs.
var ErrGameMismatch = errors.New("the game build doesn't match this mod release")

// ErrModified is returned when an owned file was modified or is missing.
var ErrModified = errors.New("modified/missing mod file")

// failCopyHook lets tests make the copy phase fail after preflight.
var failCopyHook func(relative string) error

// resolveGame turns a game folder into an absolute, symlink-free path, the
// equivalent of install.py's args.game.resolve().
func resolveGame(game string) (string, error) {
	abs, err := filepath.Abs(game)
	if err != nil {
		return "", err
	}
	resolved, err := realPath(abs)
	if err != nil {
		return "", fmt.Errorf("game folder not found: %s", game)
	}
	return resolved, nil
}

// realPath is filepath.EvalSymlinks, falling back to the cleaned path when
// the path exists but cannot be resolved (Windows can fail to resolve final
// paths on some drives, e.g. Wine's Z: drive, RAM disks or subst drives).
// destination() still refuses symlinks along the way with Lstat checks.
func realPath(p string) (string, error) {
	r, err := filepath.EvalSymlinks(p)
	if err == nil {
		return r, nil
	}
	if _, serr := os.Stat(p); serr != nil {
		return "", err
	}
	return filepath.Clean(p), nil
}

// resolveExisting resolves symlinks in the longest existing prefix of p and
// appends the rest, like pathlib's non-strict resolve().
func resolveExisting(p string) (string, error) {
	var rest []string
	cur := p
	for {
		if _, err := os.Lstat(cur); err == nil {
			r, err := realPath(cur)
			if err != nil {
				return "", err
			}
			for i := len(rest) - 1; i >= 0; i-- {
				r = filepath.Join(r, rest[i])
			}
			return r, nil
		}
		parent := filepath.Dir(cur)
		if parent == cur {
			return p, nil
		}
		rest = append(rest, filepath.Base(cur))
		cur = parent
	}
}

func within(root, p string) bool {
	rel, err := filepath.Rel(root, p)
	return err == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator)) && !filepath.IsAbs(rel)
}

func isSymlink(p string) bool {
	info, err := os.Lstat(p)
	return err == nil && info.Mode()&fs.ModeSymlink != 0
}

func exists(p string) bool {
	_, err := os.Lstat(p)
	return err == nil
}

// destination is install.py destination(): refuse writes or deletions that
// would go through a symlink or escape the game folder.
func destination(game, relative string) (string, error) {
	target := filepath.Join(game, filepath.FromSlash(relative))
	resolved, err := resolveExisting(target)
	bad := err != nil || !within(game, resolved) || isSymlink(target)
	for dir := filepath.Dir(target); !bad && dir != game && within(game, dir); dir = filepath.Dir(dir) {
		bad = isSymlink(dir)
	}
	if bad {
		return "", fmt.Errorf("destination escapes installation or is a symlink: %s", relative)
	}
	return target, nil
}

func isRuntimeLeftover(name string) bool {
	return (strings.HasPrefix(name, "ss2vr_host") && filepath.Ext(name) == ".log") ||
		name == "headset-view.ini" || name == "SS2VR.user.ini"
}

// checkFingerprints compares each game_fingerprints entry with <game>/Bin/<name>.
func checkFingerprints(game string, m *Manifest) error {
	names := make([]string, 0, len(m.GameFingerprints))
	for name := range m.GameFingerprints {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		got, err := fileSHA256(filepath.Join(game, "Bin", name))
		if err != nil {
			if errors.Is(err, fs.ErrNotExist) {
				return fmt.Errorf("%w: Bin/%s is missing (is this the Serious Sam 2 folder?)", ErrGameMismatch, name)
			}
			return err
		}
		if got != m.GameFingerprints[name].SHA256 {
			return fmt.Errorf("%w: Bin/%s differs from the verified build (update the game through Steam, or use the SS2VR release made for your game version)", ErrGameMismatch, name)
		}
	}
	return nil
}

// uninstallDirs is install.py's set of folders uninstall tries to rmdir: the
// immediate parent of each owned file under Bin/SS2VR/, deepest first.
func uninstallDirs(files map[string]string) []string {
	set := map[string]bool{}
	for r := range files {
		if strings.HasPrefix(r, "Bin/SS2VR/") {
			set[path.Dir(r)] = true
		}
	}
	dirs := make([]string, 0, len(set))
	for d := range set {
		dirs = append(dirs, d)
	}
	sort.Slice(dirs, func(i, j int) bool {
		ci, cj := strings.Count(dirs[i], "/"), strings.Count(dirs[j], "/")
		if ci != cj {
			return ci > cj
		}
		return dirs[i] < dirs[j]
	})
	return dirs
}

// pendingRemoval describes the old install that an update removes first, so
// the new install can be preflighted against the post-removal state.
type pendingRemoval struct {
	files map[string]bool // owned files and the receipt
	dirs  map[string]bool // folders uninstall would rmdir if empty
}

func newPendingRemoval(rec *Receipt) *pendingRemoval {
	p := &pendingRemoval{files: map[string]bool{receiptPath: true}, dirs: map[string]bool{}}
	for r := range rec.Files {
		p.files[r] = true
	}
	for _, d := range uninstallDirs(rec.Files) {
		p.dirs[d] = true
	}
	return p
}

// remaining lists the entries of dirRel left after `removal` is applied
// (all entries when removal is nil).
func remaining(game, dirRel string, removal *pendingRemoval) ([]os.DirEntry, error) {
	entries, err := os.ReadDir(filepath.Join(game, filepath.FromSlash(dirRel)))
	if err != nil || removal == nil {
		return entries, err
	}
	var keep []os.DirEntry
	for _, e := range entries {
		rel := dirRel + "/" + e.Name()
		if e.Type().IsRegular() && removal.files[rel] {
			continue
		}
		if e.IsDir() && removal.dirs[rel] {
			sub, err := remaining(game, rel, removal)
			if err != nil {
				return nil, err
			}
			if len(sub) == 0 {
				continue
			}
		}
		keep = append(keep, e)
	}
	return keep, nil
}

// installPreflight runs every install.py check without changing anything and
// returns the runtime leftovers (names in Bin/SS2VR) the install will carry over.
func installPreflight(game, pkg string, m *Manifest, removal *pendingRemoval) ([]string, error) {
	if err := checkFingerprints(game, m); err != nil {
		return nil, err
	}
	folder := filepath.Join(game, "Bin", "SS2VR")
	info, err := os.Lstat(folder)
	present := err == nil
	var leftovers []string
	if present {
		ok := info.IsDir() && info.Mode()&fs.ModeSymlink == 0
		var entries []os.DirEntry
		if ok {
			if entries, err = remaining(game, "Bin/SS2VR", removal); err != nil {
				return nil, err
			}
			if len(entries) == 0 && removal != nil && removal.dirs["Bin/SS2VR"] {
				present = false // uninstall would remove the emptied folder
			}
		}
		if present {
			// A folder holding only the mod's own runtime files is ours.
			for _, e := range entries {
				if !e.Type().IsRegular() || !isRuntimeLeftover(e.Name()) {
					ok = false
					break
				}
				leftovers = append(leftovers, e.Name())
			}
			if !ok || len(entries) == 0 {
				return nil, errors.New("Bin/SS2VR already exists and is not an SS2VR install this tool can update; refusing to overwrite it")
			}
		}
	}
	pkgRoot, err := resolveGame(pkg)
	if err != nil {
		return nil, fmt.Errorf("package folder not found: %s", pkg)
	}
	for _, relative := range sortedKeys(m.Files) {
		source := filepath.Join(pkgRoot, filepath.FromSlash(relative))
		resolved, rerr := resolveExisting(source)
		var digest string
		if rerr == nil && within(pkgRoot, resolved) && !isSymlink(source) {
			digest, _ = fileSHA256(source)
		}
		if digest != m.Files[relative] {
			return nil, fmt.Errorf("package content/hash mismatch: %s", relative)
		}
		target, err := destination(game, relative)
		if err != nil {
			return nil, err
		}
		if exists(target) && (removal == nil || !removal.files[relative]) {
			return nil, fmt.Errorf("refusing to overwrite a file SS2VR does not own: %s", relative)
		}
	}
	return leftovers, nil
}

// install is install.py install() for a game without a receipt.
func install(out io.Writer, game, pkg string, m *Manifest, dry bool) error {
	leftovers, err := installPreflight(game, pkg, m, nil)
	if err != nil {
		return err
	}
	if dry {
		fmt.Fprintln(out, "Preflight passed: matching game, verified package, no collisions. No files changed.")
		return nil
	}
	pkg, _ = resolveGame(pkg)
	folder := filepath.Join(game, "Bin", "SS2VR")
	var viewCache, userSettings []byte
	var moved []string
	restoreLeftovers := func() {
		if len(moved) == 0 {
			return
		}
		_ = os.MkdirAll(folder, 0o755)
		for _, name := range moved {
			_ = os.Rename(filepath.Join(game, "Bin", name+".previous"), filepath.Join(folder, name))
		}
	}
	for _, name := range leftovers {
		src := filepath.Join(folder, name)
		switch name {
		case "headset-view.ini":
			if viewCache, err = os.ReadFile(src); err != nil {
				return err
			}
		case "SS2VR.user.ini":
			if userSettings, err = os.ReadFile(src); err != nil {
				return err
			}
		}
		if err := os.Rename(src, filepath.Join(game, "Bin", name+".previous")); err != nil {
			restoreLeftovers()
			return err
		}
		moved = append(moved, name)
	}
	if len(leftovers) > 0 {
		if err := os.Remove(folder); err != nil {
			restoreLeftovers()
			return err
		}
	}

	var created, createdDirs []string
	receiptCreated := ""
	rollback := func() {
		if receiptCreated != "" {
			_ = os.Remove(receiptCreated)
		}
		// Roll back only newly created payload files, never existing files.
		for _, target := range created {
			if info, err := os.Lstat(target); err == nil && info.Mode().IsRegular() {
				_ = os.Remove(target)
			}
		}
		for i := len(createdDirs) - 1; i >= 0; i-- {
			_ = os.Remove(createdDirs[i])
		}
		restoreLeftovers()
	}
	copyOne := func(relative string) error {
		if failCopyHook != nil {
			if err := failCopyHook(relative); err != nil {
				return err
			}
		}
		target, err := destination(game, relative)
		if err != nil {
			return err
		}
		var missing []string
		for parent := filepath.Dir(target); !exists(parent); parent = filepath.Dir(parent) {
			missing = append(missing, parent)
		}
		for i := len(missing) - 1; i >= 0; i-- {
			if err := os.Mkdir(missing[i], 0o755); err != nil {
				if info, serr := os.Stat(missing[i]); serr != nil || !info.IsDir() {
					return err
				}
				continue
			}
			createdDirs = append(createdDirs, missing[i])
		}
		// Exclusive creation prevents a race from overwriting another mod.
		output, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
		if err != nil {
			return err
		}
		created = append(created, target)
		source, err := os.Open(filepath.Join(pkg, filepath.FromSlash(relative)))
		if err == nil {
			_, err = io.Copy(output, source)
			source.Close()
		}
		if cerr := output.Close(); err == nil {
			err = cerr
		}
		if err != nil {
			return err
		}
		if digest, err := fileSHA256(target); err != nil || digest != m.Files[relative] {
			return fmt.Errorf("copied package content changed: %s", relative)
		}
		return nil
	}
	writeReceipt := func() error {
		target, err := destination(game, receiptPath)
		if err != nil {
			return err
		}
		output, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
		if err != nil {
			return err
		}
		receiptCreated = target
		data, _ := json.MarshalIndent(Receipt{Files: m.Files, Version: m.Version}, "", "  ")
		_, err = output.Write(data)
		if cerr := output.Close(); err == nil {
			err = cerr
		}
		return err
	}
	for _, relative := range sortedKeys(m.Files) {
		if err := copyOne(relative); err != nil {
			rollback()
			return err
		}
	}
	if err := writeReceipt(); err != nil {
		rollback()
		return err
	}
	// Keep the runtime's last recommended eye size and the user's VR options.
	for name, data := range map[string][]byte{"headset-view.ini": viewCache, "SS2VR.user.ini": userSettings} {
		if data == nil {
			continue
		}
		if target, err := destination(game, "Bin/SS2VR/"+name); err == nil && !exists(target) {
			if err := os.WriteFile(target, data, 0o644); err != nil {
				fmt.Fprintf(out, "Warning: could not restore %s: %v\n", name, err)
			}
		}
	}
	fmt.Fprintf(out, "Installed SS2VR %s. No game/runtime was launched.\n", m.Version)
	return nil
}

// readReceipt loads and validates the install receipt; errNoReceipt if absent.
func readReceipt(game string) (*Receipt, error) {
	target, err := destination(game, receiptPath)
	if err != nil {
		return nil, err
	}
	data, err := os.ReadFile(target)
	if errors.Is(err, fs.ErrNotExist) {
		return nil, errNoReceipt
	}
	if err != nil {
		return nil, err
	}
	var rec Receipt
	if err := json.Unmarshal(data, &rec); err != nil {
		return nil, fmt.Errorf("invalid install receipt: %w", err)
	}
	if err := validateEntries(rec.Files); err != nil {
		return nil, fmt.Errorf("invalid install receipt: %w", err)
	}
	return &rec, nil
}

// verifyOwned is the uninstall.py preflight: every owned file is unchanged.
func verifyOwned(game string, rec *Receipt) error {
	for _, relative := range sortedKeys(rec.Files) {
		target, err := destination(game, relative)
		if err != nil {
			return err
		}
		info, err := os.Lstat(target)
		var digest string
		if err == nil && info.Mode().IsRegular() {
			digest, _ = fileSHA256(target)
		}
		if digest != rec.Files[relative] {
			return fmt.Errorf("%w; refusing removal: %s", ErrModified, relative)
		}
	}
	return nil
}

// uninstall is install.py uninstall().
func uninstall(out io.Writer, game string, dry bool) error {
	rec, err := readReceipt(game)
	if err != nil {
		return err
	}
	if err := verifyOwned(game, rec); err != nil {
		return err
	}
	if dry {
		fmt.Fprintln(out, "Removal preflight passed. No files changed.")
		return nil
	}
	if err := removeOwned(game, rec); err != nil {
		return err
	}
	fmt.Fprintln(out, "Removed unchanged SS2VR payload; other files retained.")
	return nil
}

func removeOwned(game string, rec *Receipt) error {
	for _, relative := range sortedKeys(rec.Files) {
		target, err := destination(game, relative)
		if err != nil {
			return err
		}
		if err := os.Remove(target); err != nil {
			return err
		}
	}
	if target, err := destination(game, receiptPath); err == nil {
		if err := os.Remove(target); err != nil {
			return err
		}
	}
	// Empty mod-owned directories only; runtime logs or other additions are retained.
	for _, dir := range uninstallDirs(rec.Files) {
		_ = os.Remove(filepath.Join(game, filepath.FromSlash(dir)))
	}
	return nil
}

// InstallResult says what installOrUpdate did.
type InstallResult int

const (
	ResultInstalled InstallResult = iota
	ResultUpdated
	ResultUpToDate
)

// installOrUpdate installs, or updates by removing the old version (with the
// uninstall checks) and installing the new one. Same version is "up to date"
// unless force is set.
func installOrUpdate(out io.Writer, game, pkg string, m *Manifest, dry, force bool) (InstallResult, error) {
	rec, err := readReceipt(game)
	if errors.Is(err, errNoReceipt) {
		fmt.Fprintf(out, "Installing SS2VR %s...\n", m.Version)
		return ResultInstalled, install(out, game, pkg, m, dry)
	}
	if err != nil {
		return 0, err
	}
	if rec.Version == m.Version && !force {
		fmt.Fprintf(out, "SS2VR %s is already installed and up to date. (Use --force-reinstall to reinstall it.)\n", m.Version)
		return ResultUpToDate, nil
	}
	if rec.Version == m.Version {
		fmt.Fprintf(out, "Reinstalling SS2VR %s...\n", m.Version)
	} else {
		fmt.Fprintf(out, "Updating SS2VR %s -> %s...\n", rec.Version, m.Version)
	}
	if err := verifyOwned(game, rec); err != nil {
		return 0, err
	}
	if _, err := installPreflight(game, pkg, m, newPendingRemoval(rec)); err != nil {
		return 0, err
	}
	if dry {
		fmt.Fprintln(out, "Removal preflight passed: the installed SS2VR files are unmodified.")
		fmt.Fprintln(out, "Install preflight passed: matching game, verified package, no collisions after removing the old version. No files changed.")
		return ResultUpdated, nil
	}
	backup, err := backupOwned(game, rec)
	if err != nil {
		return 0, fmt.Errorf("could not back up the installed version: %w", err)
	}
	defer os.RemoveAll(backup)
	if err := removeOwned(game, rec); err != nil {
		restoreOwned(game, rec, backup)
		return 0, fmt.Errorf("removing the old version failed: %w (previous version restored)", err)
	}
	fmt.Fprintf(out, "Removed SS2VR %s; user settings are carried over.\n", rec.Version)
	if err := install(out, game, pkg, m, false); err != nil {
		restoreOwned(game, rec, backup)
		return 0, fmt.Errorf("%w (previous version %s restored)", err, rec.Version)
	}
	return ResultUpdated, nil
}

// backupOwned copies the owned files and receipt into a temporary folder.
func backupOwned(game string, rec *Receipt) (string, error) {
	dir, err := os.MkdirTemp("", "ss2vr-backup-")
	if err != nil {
		return "", err
	}
	for _, relative := range append(sortedKeys(rec.Files), receiptPath) {
		dst := filepath.Join(dir, filepath.FromSlash(relative))
		if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
			os.RemoveAll(dir)
			return "", err
		}
		if err := copyFile(filepath.Join(game, filepath.FromSlash(relative)), dst); err != nil {
			os.RemoveAll(dir)
			return "", err
		}
	}
	return dir, nil
}

// restoreOwned puts back any backed-up file that is now missing (best effort).
func restoreOwned(game string, rec *Receipt, backup string) {
	for _, relative := range append(sortedKeys(rec.Files), receiptPath) {
		target, err := destination(game, relative)
		if err != nil || exists(target) {
			continue
		}
		_ = os.MkdirAll(filepath.Dir(target), 0o755)
		_ = copyFile(filepath.Join(backup, filepath.FromSlash(relative)), target)
	}
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
	if err != nil {
		return err
	}
	_, err = io.Copy(out, in)
	if cerr := out.Close(); err == nil {
		err = cerr
	}
	return err
}
