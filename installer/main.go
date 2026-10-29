// Command ss2vr-installer finds Serious Sam 2, downloads the latest SS2VR
// release from GitHub and installs, updates or removes it.
package main

import (
	"bufio"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// version is set at build time with -ldflags "-X main.version=...".
var version = "dev"

// steamRootsFunc is replaceable in tests.
var steamRootsFunc = steamRoots

var errAborted = errors.New("cancelled; nothing was changed")

type options struct {
	game, pkg, repo, apiURL       string
	yes, dry, force, offline, ver bool
}

type app struct {
	in     *bufio.Reader
	out    io.Writer
	opts   options
	prompt bool // interactive menu mode

	release    *Release
	releaseErr error
	fetched    bool
}

const usageText = `SS2VR installer %s - installs, updates or removes the Serious Sam 2 VR mod.

Usage:
  ss2vr-installer                 interactive menu (also what double-clicking does)
  ss2vr-installer install         install SS2VR, or update it to the latest release
  ss2vr-installer uninstall       remove SS2VR (keeps your VR settings and logs)
  ss2vr-installer status          show game folder, installed and latest versions

Options:
`

func main() {
	os.Exit(run(os.Args[1:], os.Stdin, os.Stdout, os.Stderr))
}

func run(args []string, stdin io.Reader, stdout, stderr io.Writer) int {
	var o options
	fs := flag.NewFlagSet("ss2vr-installer", flag.ContinueOnError)
	fs.SetOutput(stderr)
	fs.StringVar(&o.game, "game", "", "Serious Sam 2 folder (default: find it through Steam)")
	fs.StringVar(&o.pkg, "package", "", "install from a local SS2VR-<version>.zip or unpacked package folder instead of downloading")
	fs.StringVar(&o.repo, "repo", defaultRepo, "GitHub repository (owner/name) to take releases from")
	fs.StringVar(&o.apiURL, "release-api-url", "", "override the GitHub releases/latest API URL (testing)")
	fs.BoolVar(&o.yes, "yes", false, "do not ask for confirmation")
	fs.BoolVar(&o.dry, "dry-run", false, "check everything but change nothing")
	fs.BoolVar(&o.force, "force-reinstall", false, "reinstall even if the same version is installed")
	fs.BoolVar(&o.offline, "offline", false, "status: do not contact GitHub")
	fs.BoolVar(&o.ver, "version", false, "print the installer version")
	fs.Usage = func() {
		fmt.Fprintf(stderr, usageText, version)
		fs.PrintDefaults()
	}
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	cmd := ""
	if fs.NArg() > 0 {
		// Allow flags after the subcommand too.
		cmd = fs.Arg(0)
		if err := fs.Parse(fs.Args()[1:]); err != nil {
			return 2
		}
		if fs.NArg() > 0 {
			fmt.Fprintf(stderr, "Unexpected argument: %s\n", fs.Arg(0))
			return 2
		}
	}
	if o.ver || cmd == "version" {
		fmt.Fprintf(stdout, "ss2vr-installer %s\n", version)
		return 0
	}
	if o.apiURL == "" {
		if !strings.Contains(o.repo, "/") {
			fmt.Fprintln(stderr, "--repo must look like owner/name")
			return 2
		}
		o.apiURL = releaseAPIURL(o.repo)
	}
	a := &app{in: bufio.NewReader(stdin), out: stdout, opts: o}
	var err error
	switch cmd {
	case "":
		return a.interactive(stderr)
	case "install":
		err = a.cmdInstall()
	case "uninstall":
		err = a.cmdUninstall()
	case "status":
		err = a.cmdStatus()
	case "help":
		fs.Usage()
		return 0
	default:
		fmt.Fprintf(stderr, "Unknown command %q (use install, uninstall or status)\n", cmd)
		return 2
	}
	if err != nil {
		reportError(stderr, err)
		return 1
	}
	return 0
}

func reportError(w io.Writer, err error) {
	fmt.Fprintln(w, "Error:", err)
	switch {
	case errors.Is(err, ErrModified):
		fmt.Fprintln(w, "Some SS2VR files were changed or deleted, so they are left alone. Restore them (for example by reinstalling the same release) or remove the SS2VR files by hand.")
	case errors.Is(err, ErrNetwork):
		fmt.Fprintln(w, "Check your internet connection, or download SS2VR-<version>.zip yourself and use --package <zip>.")
	}
}

// ask prints a question and returns the trimmed answer; io.EOF if none.
func (a *app) ask(question string) (string, error) {
	fmt.Fprint(a.out, question)
	line, err := a.in.ReadString('\n')
	if err != nil && line == "" {
		fmt.Fprintln(a.out)
		return "", io.EOF
	}
	return strings.TrimSpace(line), nil
}

func (a *app) confirm(question string) bool {
	answer, err := a.ask(question + " [y/N] ")
	return err == nil && (strings.EqualFold(answer, "y") || strings.EqualFold(answer, "yes"))
}

// findGame uses --game, else Steam auto-detection, else asks for a path.
func (a *app) findGame() (string, error) {
	if a.opts.game != "" {
		dir, err := normalizeGameDir(a.opts.game)
		if err != nil {
			return "", err
		}
		return resolveGame(dir)
	}
	games := findGamesInSteamRoots(steamRootsFunc())
	if len(games) == 1 {
		return resolveGame(games[0])
	}
	if len(games) == 0 {
		fmt.Fprintln(a.out, "Serious Sam 2 (Steam app 204340) was not found automatically.")
	} else {
		fmt.Fprintln(a.out, "Several Serious Sam 2 folders were found:")
		for i, g := range games {
			fmt.Fprintf(a.out, "  [%d] %s\n", i+1, g)
		}
	}
	for tries := 0; tries < 3; tries++ {
		q := "Enter the Serious Sam 2 folder (the one containing Bin/Sam2.exe): "
		if len(games) > 1 {
			q = "Choose a number, or enter another folder: "
		}
		answer, err := a.ask(q)
		if err != nil {
			break
		}
		if n, convErr := strconv.Atoi(answer); convErr == nil && n >= 1 && n <= len(games) {
			return resolveGame(games[n-1])
		}
		dir, err := normalizeGameDir(answer)
		if err == nil {
			return resolveGame(dir)
		}
		fmt.Fprintln(a.out, err)
	}
	if len(games) > 1 {
		return "", errors.New("several Serious Sam 2 folders found and none chosen; pass one with --game \"<path to Serious Sam 2>\"")
	}
	return "", errors.New("Serious Sam 2 folder not found; pass it with --game \"<path to Serious Sam 2>\"")
}

func installedVersion(game string) string {
	rec, err := readReceipt(game)
	switch {
	case errors.Is(err, errNoReceipt):
		if exists(filepath.Join(game, "Bin", "SS2VR")) || exists(filepath.Join(game, "Bin", "d3d9.dll")) {
			return "not installed by this tool (Bin/SS2VR or Bin/d3d9.dll present without a receipt)"
		}
		return "not installed"
	case err != nil:
		return "unknown (" + err.Error() + ")"
	}
	return rec.Version
}

func (a *app) latestRelease() (*Release, error) {
	if !a.fetched {
		a.fetched = true
		a.release, a.releaseErr = fetchLatestRelease(a.opts.apiURL)
	}
	return a.release, a.releaseErr
}

// packageSource is an unpacked package ready to install.
type packageSource struct {
	dir      string
	manifest *Manifest
	label    string
	cleanup  func()
}

// obtainPackage resolves --package (zip or folder) or downloads the latest release.
func (a *app) obtainPackage() (*packageSource, error) {
	tempDir := func() (string, func(), error) {
		dir, err := os.MkdirTemp("", "ss2vr-installer-")
		if err != nil {
			return "", nil, err
		}
		return dir, func() { os.RemoveAll(dir) }, nil
	}
	var zipPath, label string
	cleanup := func() {}
	if a.opts.pkg != "" {
		info, err := os.Stat(a.opts.pkg)
		if err != nil {
			return nil, fmt.Errorf("package not found: %s", a.opts.pkg)
		}
		if info.IsDir() {
			m, err := readManifest(filepath.Join(a.opts.pkg, "manifest.json"))
			if err != nil {
				return nil, err
			}
			return &packageSource{dir: a.opts.pkg, manifest: m, label: "local folder " + a.opts.pkg, cleanup: cleanup}, nil
		}
		zipPath, label = a.opts.pkg, "local zip "+a.opts.pkg
	}
	dir, done, err := tempDir()
	if err != nil {
		return nil, err
	}
	if zipPath == "" {
		r, err := a.latestRelease()
		if err != nil {
			done()
			return nil, err
		}
		if zipPath, err = downloadRelease(a.out, r, dir); err != nil {
			done()
			return nil, err
		}
		label = "GitHub release " + r.Tag
	}
	root, err := extractPackage(zipPath, dir)
	if err != nil {
		done()
		return nil, err
	}
	m, err := readManifest(filepath.Join(root, "manifest.json"))
	if err != nil {
		done()
		return nil, err
	}
	return &packageSource{dir: root, manifest: m, label: label, cleanup: done}, nil
}

// doInstall installs or updates; confirmFirst asks before changing anything.
func (a *app) doInstall(game string, confirmFirst bool) error {
	src, err := a.obtainPackage()
	if err != nil {
		return err
	}
	defer src.cleanup()
	fmt.Fprintf(a.out, "Package: SS2VR %s (%s)\n", src.manifest.Version, src.label)
	force := a.opts.force
	if rec, err := readReceipt(game); err == nil && rec.Version == src.manifest.Version && !force {
		if !a.prompt {
			_, err := installOrUpdate(a.out, game, src.dir, src.manifest, a.opts.dry, false)
			return err
		}
		fmt.Fprintf(a.out, "SS2VR %s is already installed and up to date.\n", rec.Version)
		if !a.confirm("Reinstall it anyway?") {
			return nil
		}
		force, confirmFirst = true, false
	}
	if confirmFirst && !a.opts.dry && !a.opts.yes && !a.confirm("Install SS2VR "+src.manifest.Version+" into this game folder?") {
		return errAborted
	}
	_, err = installOrUpdate(a.out, game, src.dir, src.manifest, a.opts.dry, force)
	return err
}

func (a *app) doUninstall(game string, confirmFirst bool) error {
	if confirmFirst && !a.opts.dry && !a.opts.yes {
		if _, err := readReceipt(game); err != nil {
			return err
		}
		if !a.confirm("Remove SS2VR from this game folder? (your VR settings and logs are kept)") {
			return errAborted
		}
	}
	return uninstall(a.out, game, a.opts.dry)
}

func (a *app) cmdInstall() error {
	game, err := a.findGame()
	if err != nil {
		return err
	}
	fmt.Fprintf(a.out, "Game folder: %s\n", game)
	fmt.Fprintf(a.out, "Installed:   %s\n", installedVersion(game))
	return a.doInstall(game, true)
}

func (a *app) cmdUninstall() error {
	game, err := a.findGame()
	if err != nil {
		return err
	}
	fmt.Fprintf(a.out, "Game folder: %s\n", game)
	fmt.Fprintf(a.out, "Installed:   %s\n", installedVersion(game))
	return a.doUninstall(game, true)
}

func (a *app) cmdStatus() error {
	game, err := a.findGame()
	if err != nil {
		return err
	}
	fmt.Fprintf(a.out, "Game folder: %s\n", game)
	installed := ""
	rec, err := readReceipt(game)
	if err == nil {
		installed = rec.Version
		state := "all files unmodified"
		if verr := verifyOwned(game, rec); verr != nil {
			state = verr.Error()
		}
		fmt.Fprintf(a.out, "Installed:   %s (%s)\n", rec.Version, state)
	} else {
		fmt.Fprintf(a.out, "Installed:   %s\n", installedVersion(game))
	}
	latest := ""
	switch {
	case a.opts.pkg != "":
		src, err := a.obtainPackage()
		if err != nil {
			return err
		}
		defer src.cleanup()
		latest = src.manifest.Version
		match := "matches this package"
		if ferr := checkFingerprints(game, src.manifest); ferr != nil {
			match = ferr.Error()
		}
		fmt.Fprintf(a.out, "Package:     %s (%s)\n", latest, src.label)
		fmt.Fprintf(a.out, "Game build:  %s\n", match)
	case a.opts.offline:
		fmt.Fprintln(a.out, "Latest:      not checked (--offline)")
	default:
		if r, err := a.latestRelease(); err != nil {
			fmt.Fprintf(a.out, "Latest:      unavailable (%v)\n", err)
		} else {
			latest = r.Version
			fmt.Fprintf(a.out, "Latest:      %s (GitHub release %s)\n", r.Version, r.Tag)
		}
	}
	if latest != "" {
		switch {
		case installed == latest:
			fmt.Fprintln(a.out, "Status:      up to date")
		case installed == "":
			fmt.Fprintln(a.out, "Status:      not installed; run \"install\"")
		default:
			fmt.Fprintln(a.out, "Status:      update available; run \"install\"")
		}
	}
	return nil
}

// interactive is the double-click menu. It always waits for Enter before
// returning so a Windows console window does not vanish.
func (a *app) interactive(stderr io.Writer) int {
	a.prompt = true
	code := a.menu(stderr)
	a.ask("\nPress Enter to exit.")
	return code
}

func (a *app) menu(stderr io.Writer) int {
	fmt.Fprintf(a.out, "SS2VR installer %s\n\n", version)
	game, err := a.findGame()
	if err != nil {
		reportError(a.out, err)
		return 1
	}
	fmt.Fprintf(a.out, "Game folder: %s\n", game)
	fmt.Fprintf(a.out, "Installed:   %s\n", installedVersion(game))
	if a.opts.pkg != "" {
		fmt.Fprintf(a.out, "Package:     %s\n", a.opts.pkg)
	} else if r, err := a.latestRelease(); err != nil {
		fmt.Fprintf(a.out, "Latest:      unavailable (%v)\n", err)
	} else {
		fmt.Fprintf(a.out, "Latest:      %s (GitHub release %s)\n", r.Version, r.Tag)
	}
	if a.opts.dry {
		fmt.Fprintln(a.out, "Dry run: nothing will be changed.")
	}
	for {
		fmt.Fprintln(a.out, "\n  [1] Install / Update\n  [2] Uninstall\n  [3] Quit")
		choice, err := a.ask("Choose 1-3: ")
		if err != nil {
			return 0
		}
		switch choice {
		case "1":
			err = a.doInstall(game, false)
		case "2":
			err = a.doUninstall(game, true)
		case "3", "q", "Q", "":
			return 0
		default:
			fmt.Fprintln(a.out, "Please type 1, 2 or 3.")
			continue
		}
		if err != nil {
			if errors.Is(err, errAborted) {
				fmt.Fprintln(a.out, err)
				return 0
			}
			reportError(a.out, err)
			return 1
		}
		return 0
	}
}
