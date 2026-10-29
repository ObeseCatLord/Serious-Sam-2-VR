package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io/fs"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

func sum(data string) string {
	h := sha256.Sum256([]byte(data))
	return hex.EncodeToString(h[:])
}

func writeFile(t *testing.T, path, data string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(data), 0o644); err != nil {
		t.Fatal(err)
	}
}

func readFile(t *testing.T, path string) string {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return string(data)
}

// fakeGame creates a game folder with a fake Bin/Sam2.exe and Bin/Core.dll.
func fakeGame(t *testing.T) string {
	t.Helper()
	game := filepath.Join(t.TempDir(), "Serious Sam 2")
	writeFile(t, filepath.Join(game, "Bin", "Sam2.exe"), "fake Sam2.exe")
	writeFile(t, filepath.Join(game, "Bin", "Core.dll"), "fake Core.dll")
	writeFile(t, filepath.Join(game, "Content", "SeriousSam2", "Levels.gro"), "base game data")
	return game
}

func payload(version string) map[string]string {
	return map[string]string{
		"Bin/d3d9.dll":               "d3d9 " + version,
		"Bin/SS2VRServer.dll":        "server " + version,
		"Bin/SS2VR/ss2vr_host.exe":   "host " + version,
		"Bin/SS2VR/SS2VR.ini":        "[SS2VR]\nversion=" + version + "\n",
		"Bin/SS2VR/licenses/MIT.txt": "license",
		"Content/SS2VR.mod":          "mod " + version,
		"Bin/SS2VR/only-" + version:  "version specific",
	}
}

// fakePackage writes an unpacked package whose fingerprints match fakeGame.
func fakePackage(t *testing.T, version string, files map[string]string, fingerprintOverride string) string {
	t.Helper()
	dir := filepath.Join(t.TempDir(), "SS2VR-"+version)
	manifest := map[string]any{"version": version}
	hashes := map[string]string{}
	for rel, data := range files {
		writeFile(t, filepath.Join(dir, filepath.FromSlash(rel)), data)
		hashes[rel] = sum(data)
	}
	manifest["files"] = hashes
	exe := sum("fake Sam2.exe")
	if fingerprintOverride != "" {
		exe = fingerprintOverride
	}
	manifest["game_fingerprints"] = map[string]any{
		"Sam2.exe": map[string]any{"sha256": exe, "machine": "0x14c"},
		"Core.dll": map[string]any{"sha256": sum("fake Core.dll")},
	}
	data, _ := json.MarshalIndent(manifest, "", "  ")
	writeFile(t, filepath.Join(dir, "manifest.json"), string(data))
	// Extras a real release zip carries that the installer must ignore.
	writeFile(t, filepath.Join(dir, "README.md"), "readme")
	writeFile(t, filepath.Join(dir, "ss2vr-installer-linux-x86_64"), "binary")
	return dir
}

// zipDir zips a package folder (plus raw extra entries) into a zip file.
func zipDir(t *testing.T, dir string, extra map[string]string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), filepath.Base(dir)+".zip")
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	var names []string
	filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err == nil && !d.IsDir() {
			rel, _ := filepath.Rel(dir, p)
			names = append(names, filepath.ToSlash(rel))
		}
		return err
	})
	sort.Strings(names)
	for _, n := range names {
		w, _ := zw.Create(n)
		w.Write([]byte(readFile(t, filepath.Join(dir, filepath.FromSlash(n)))))
	}
	for n, data := range extra {
		w, _ := zw.CreateHeader(&zip.FileHeader{Name: n, Method: zip.Deflate})
		w.Write([]byte(data))
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, buf.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

// fakeGitHub serves releases/latest plus the zip (and optional .sha256).
func fakeGitHub(t *testing.T, version, zipPath, checksum string) *httptest.Server {
	t.Helper()
	data, err := os.ReadFile(zipPath)
	if err != nil {
		t.Fatal(err)
	}
	name := "SS2VR-" + version + ".zip"
	mux := http.NewServeMux()
	var srv *httptest.Server
	mux.HandleFunc("/repos/ObeseCatLord/ss2-vr/releases/latest", func(w http.ResponseWriter, r *http.Request) {
		assets := []map[string]any{
			{"name": "ss2vr-installer-windows-x86_64.exe", "browser_download_url": srv.URL + "/dl/installer.exe", "size": 3},
			{"name": name, "browser_download_url": srv.URL + "/dl/" + name, "size": len(data)},
		}
		if checksum != "" {
			assets = append(assets, map[string]any{"name": name + ".sha256", "browser_download_url": srv.URL + "/dl/" + name + ".sha256", "size": 100})
		}
		json.NewEncoder(w).Encode(map[string]any{"tag_name": "v" + version, "name": "SS2VR " + version, "assets": assets})
	})
	mux.HandleFunc("/dl/"+name, func(w http.ResponseWriter, r *http.Request) { w.Write(data) })
	mux.HandleFunc("/dl/"+name+".sha256", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprintf(w, "%s  %s\n", checksum, name)
	})
	srv = httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return srv
}

func zipSum(t *testing.T, path string) string {
	h := sha256.Sum256([]byte(readFile(t, path)))
	return hex.EncodeToString(h[:])
}

// runCLI runs the installer and returns exit code and combined output.
func runCLI(t *testing.T, stdin string, args ...string) (int, string) {
	t.Helper()
	var out bytes.Buffer
	code := run(args, strings.NewReader(stdin), &out, &out)
	return code, out.String()
}

// snapshot maps every path under root to its content (dirs to "<dir>").
func snapshot(t *testing.T, root string) map[string]string {
	t.Helper()
	snap := map[string]string{}
	filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			t.Fatal(err)
		}
		rel, _ := filepath.Rel(root, p)
		if d.IsDir() {
			snap[filepath.ToSlash(rel)] = "<dir>"
		} else {
			snap[filepath.ToSlash(rel)] = readFile(t, p)
		}
		return nil
	})
	return snap
}

func assertSnapshot(t *testing.T, before, after map[string]string) {
	t.Helper()
	for k, v := range before {
		if after[k] != v {
			t.Errorf("changed or removed: %s", k)
		}
	}
	for k := range after {
		if _, ok := before[k]; !ok {
			t.Errorf("unexpected new path: %s", k)
		}
	}
}

func receiptVersion(t *testing.T, game string) string {
	t.Helper()
	rec, err := readReceipt(game)
	if err != nil {
		t.Fatal(err)
	}
	return rec.Version
}

func assertInstalled(t *testing.T, game string, files map[string]string) {
	t.Helper()
	for rel, data := range files {
		if got := readFile(t, filepath.Join(game, filepath.FromSlash(rel))); got != data {
			t.Errorf("%s = %q, want %q", rel, got, data)
		}
	}
	if exists(filepath.Join(game, "README.md")) || exists(filepath.Join(game, "ss2vr-installer-linux-x86_64")) {
		t.Error("files outside the manifest were installed")
	}
}

func TestFreshInstallFromRelease(t *testing.T) {
	game := fakeGame(t)
	files := payload("1.0.0")
	zipPath := zipDir(t, fakePackage(t, "1.0.0", files, ""), nil)
	srv := fakeGitHub(t, "1.0.0", zipPath, zipSum(t, zipPath))
	code, out := runCLI(t, "", "install", "--game", game, "--yes", "--release-api-url", srv.URL+"/repos/ObeseCatLord/ss2-vr/releases/latest")
	if code != 0 {
		t.Fatalf("exit %d\n%s", code, out)
	}
	for _, want := range []string{"Checksum verified", "Installed SS2VR 1.0.0", "Installed:   not installed"} {
		if !strings.Contains(out, want) {
			t.Errorf("output missing %q:\n%s", want, out)
		}
	}
	assertInstalled(t, game, files)
	if v := receiptVersion(t, game); v != "1.0.0" {
		t.Fatalf("receipt version %q", v)
	}
}

func TestFreshInstallNeedsConfirmation(t *testing.T) {
	game := fakeGame(t)
	pkg := fakePackage(t, "1.0.0", payload("1.0.0"), "")
	before := snapshot(t, game)
	code, out := runCLI(t, "n\n", "install", "--game", game, "--package", pkg)
	if code == 0 || !strings.Contains(out, "cancelled") {
		t.Fatalf("exit %d\n%s", code, out)
	}
	assertSnapshot(t, before, snapshot(t, game))
}

func TestUpdatePreservesUserSettings(t *testing.T) {
	game := fakeGame(t)
	v1 := payload("1.0.0")
	if code, out := runCLI(t, "", "install", "--yes", "--game", game, "--package", fakePackage(t, "1.0.0", v1, "")); code != 0 {
		t.Fatalf("v1 install: %d\n%s", code, out)
	}
	// Runtime files the mod writes while playing.
	writeFile(t, filepath.Join(game, "Bin/SS2VR/SS2VR.user.ini"), "[User]\nIPD=63\n")
	writeFile(t, filepath.Join(game, "Bin/SS2VR/headset-view.ini"), "width=2016\n")
	writeFile(t, filepath.Join(game, "Bin/SS2VR/ss2vr_host.log"), "log")

	v2 := payload("2.0.0")
	zipPath := zipDir(t, fakePackage(t, "2.0.0", v2, ""), nil)
	srv := fakeGitHub(t, "2.0.0", zipPath, "")
	code, out := runCLI(t, "", "install", "--yes", "--game", game, "--release-api-url", srv.URL+"/repos/ObeseCatLord/ss2-vr/releases/latest")
	if code != 0 {
		t.Fatalf("update: %d\n%s", code, out)
	}
	if !strings.Contains(out, "Updating SS2VR 1.0.0 -> 2.0.0") {
		t.Errorf("missing update message:\n%s", out)
	}
	assertInstalled(t, game, v2)
	if exists(filepath.Join(game, "Bin/SS2VR/only-1.0.0")) {
		t.Error("old version's file was not removed")
	}
	if got := readFile(t, filepath.Join(game, "Bin/SS2VR/SS2VR.user.ini")); got != "[User]\nIPD=63\n" {
		t.Errorf("user settings not carried over: %q", got)
	}
	if got := readFile(t, filepath.Join(game, "Bin/SS2VR/headset-view.ini")); got != "width=2016\n" {
		t.Errorf("headset view not carried over: %q", got)
	}
	if got := readFile(t, filepath.Join(game, "Bin/ss2vr_host.log.previous")); got != "log" {
		t.Errorf("old log not kept as .previous: %q", got)
	}
	if v := receiptVersion(t, game); v != "2.0.0" {
		t.Fatalf("receipt version %q", v)
	}
	// The new install can itself be removed cleanly, keeping user settings.
	if code, out := runCLI(t, "", "uninstall", "--yes", "--game", game); code != 0 {
		t.Fatalf("uninstall: %d\n%s", code, out)
	}
	if !exists(filepath.Join(game, "Bin/SS2VR/SS2VR.user.ini")) || exists(filepath.Join(game, "Bin/d3d9.dll")) {
		t.Error("uninstall removed user settings or left mod files")
	}
}

func TestUpToDateAndForceReinstall(t *testing.T) {
	game := fakeGame(t)
	files := payload("1.0.0")
	pkg := fakePackage(t, "1.0.0", files, "")
	if code, out := runCLI(t, "", "install", "--yes", "--game", game, "--package", pkg); code != 0 {
		t.Fatalf("install: %d\n%s", code, out)
	}
	writeFile(t, filepath.Join(game, "Bin/SS2VR/SS2VR.user.ini"), "mine")
	before := snapshot(t, game)
	code, out := runCLI(t, "", "install", "--game", game, "--package", pkg)
	if code != 0 || !strings.Contains(out, "already installed and up to date") {
		t.Fatalf("exit %d\n%s", code, out)
	}
	assertSnapshot(t, before, snapshot(t, game))

	// Dry-run reinstall changes nothing.
	code, out = runCLI(t, "", "install", "--dry-run", "--force-reinstall", "--game", game, "--package", pkg)
	if code != 0 || !strings.Contains(out, "Install preflight passed") || !strings.Contains(out, "No files changed") {
		t.Fatalf("dry run: %d\n%s", code, out)
	}
	assertSnapshot(t, before, snapshot(t, game))

	code, out = runCLI(t, "", "install", "--yes", "--force-reinstall", "--game", game, "--package", pkg)
	if code != 0 || !strings.Contains(out, "Reinstalling SS2VR 1.0.0") {
		t.Fatalf("reinstall: %d\n%s", code, out)
	}
	assertInstalled(t, game, files)
	if readFile(t, filepath.Join(game, "Bin/SS2VR/SS2VR.user.ini")) != "mine" {
		t.Error("user settings lost on reinstall")
	}
}

func TestUninstallRefusesModifiedFiles(t *testing.T) {
	game := fakeGame(t)
	pkg := fakePackage(t, "1.0.0", payload("1.0.0"), "")
	if code, out := runCLI(t, "", "install", "--yes", "--game", game, "--package", pkg); code != 0 {
		t.Fatalf("install: %d\n%s", code, out)
	}
	writeFile(t, filepath.Join(game, "Bin/SS2VR/SS2VR.ini"), "edited by the player")
	before := snapshot(t, game)
	code, out := runCLI(t, "", "uninstall", "--yes", "--game", game)
	if code == 0 || !strings.Contains(out, "modified/missing mod file; refusing removal: Bin/SS2VR/SS2VR.ini") {
		t.Fatalf("exit %d\n%s", code, out)
	}
	assertSnapshot(t, before, snapshot(t, game))
	// An update must refuse too, before touching anything.
	code, out = runCLI(t, "", "install", "--yes", "--game", game, "--package", fakePackage(t, "2.0.0", payload("2.0.0"), ""))
	if code == 0 || !strings.Contains(out, "refusing removal") {
		t.Fatalf("update exit %d\n%s", code, out)
	}
	assertSnapshot(t, before, snapshot(t, game))
	// A missing file is refused as well.
	os.Remove(filepath.Join(game, "Bin/SS2VR/SS2VR.ini"))
	if code, _ := runCLI(t, "", "uninstall", "--yes", "--game", game); code == 0 {
		t.Fatal("uninstall with a missing file succeeded")
	}
}

func TestUninstallKeepsRuntimeFiles(t *testing.T) {
	game := fakeGame(t)
	pkg := fakePackage(t, "1.0.0", payload("1.0.0"), "")
	before := snapshot(t, game)
	if code, out := runCLI(t, "", "install", "--yes", "--game", game, "--package", pkg); code != 0 {
		t.Fatalf("install: %d\n%s", code, out)
	}
	if code, out := runCLI(t, "", "uninstall", "--yes", "--game", game); code != 0 {
		t.Fatalf("uninstall: %d\n%s", code, out)
	}
	assertSnapshot(t, before, snapshot(t, game)) // fully clean: no empty dirs left
	if code, out := runCLI(t, "", "install", "--yes", "--game", game, "--package", pkg); code != 0 {
		t.Fatalf("reinstall: %d\n%s", code, out)
	}
	writeFile(t, filepath.Join(game, "Bin/SS2VR/ss2vr_host_2.log"), "log")
	writeFile(t, filepath.Join(game, "Bin/SS2VR/SS2VR.user.ini"), "mine")
	if code, out := runCLI(t, "", "uninstall", "--yes", "--game", game); code != 0 {
		t.Fatalf("uninstall: %d\n%s", code, out)
	}
	after := snapshot(t, game)
	if after["Bin/SS2VR/SS2VR.user.ini"] != "mine" || after["Bin/SS2VR/ss2vr_host_2.log"] != "log" || after["Bin/SS2VR/licenses"] != "" {
		t.Errorf("unexpected state after uninstall: %v", after)
	}
	if code, out := runCLI(t, "", "uninstall", "--yes", "--game", game); code == 0 || !strings.Contains(out, "not installed") {
		t.Fatalf("second uninstall: %d\n%s", code, out)
	}
}

func TestFingerprintMismatch(t *testing.T) {
	game := fakeGame(t)
	pkg := fakePackage(t, "1.0.0", payload("1.0.0"), sum("some other Sam2.exe"))
	before := snapshot(t, game)
	for _, extra := range [][]string{{"--yes"}, {"--dry-run"}} {
		code, out := runCLI(t, "", append([]string{"install", "--game", game, "--package", pkg}, extra...)...)
		if code == 0 || !strings.Contains(out, "the game build doesn't match this mod release: Bin/Sam2.exe") {
			t.Fatalf("exit %d\n%s", code, out)
		}
	}
	assertSnapshot(t, before, snapshot(t, game))
	code, out := runCLI(t, "", "status", "--game", game, "--package", pkg)
	if code != 0 || !strings.Contains(out, "Game build:  the game build doesn't match") {
		t.Fatalf("status %d\n%s", code, out)
	}
}

func TestRefusesFilesItDoesNotOwn(t *testing.T) {
	game := fakeGame(t)
	writeFile(t, filepath.Join(game, "Bin/d3d9.dll"), "another mod's d3d9")
	before := snapshot(t, game)
	code, out := runCLI(t, "", "install", "--yes", "--game", game, "--package", fakePackage(t, "1.0.0", payload("1.0.0"), ""))
	if code == 0 || !strings.Contains(out, "refusing to overwrite a file SS2VR does not own: Bin/d3d9.dll") {
		t.Fatalf("exit %d\n%s", code, out)
	}
	assertSnapshot(t, before, snapshot(t, game))

	game = fakeGame(t)
	writeFile(t, filepath.Join(game, "Bin/SS2VR/notes.txt"), "player file")
	code, out = runCLI(t, "", "install", "--yes", "--game", game, "--package", fakePackage(t, "1.0.0", payload("1.0.0"), ""))
	if code == 0 || !strings.Contains(out, "Bin/SS2VR already exists") {
		t.Fatalf("exit %d\n%s", code, out)
	}
}

func TestUnsafeZipPaths(t *testing.T) {
	pkg := fakePackage(t, "1.0.0", payload("1.0.0"), "")
	for _, bad := range []string{"../evil.txt", "Bin/../../evil.txt", "/etc/evil", `..\evil.txt`, "C:/evil.txt"} {
		zipPath := zipDir(t, pkg, map[string]string{bad: "x"})
		dest := t.TempDir()
		if _, err := extractPackage(zipPath, dest); err == nil || !strings.Contains(err.Error(), "unsafe path") {
			t.Errorf("%q: err = %v", bad, err)
		}
		if entries, _ := os.ReadDir(dest); len(entries) != 0 {
			t.Errorf("%q: something was extracted", bad)
		}
		game := fakeGame(t)
		before := snapshot(t, game)
		if code, out := runCLI(t, "", "install", "--yes", "--game", game, "--package", zipPath); code == 0 {
			t.Errorf("%q: install succeeded:\n%s", bad, out)
		}
		assertSnapshot(t, before, snapshot(t, game))
	}
}

func TestUnsafeManifestPaths(t *testing.T) {
	h := sum("x")
	for _, bad := range []string{"/Bin/SS2VR/x", "Bin/SS2VR/../d3d9.dll", "Bin/SS2VR/./x", "Bin//SS2VR/x", `Bin\SS2VR\x`,
		"Bin/SS2VR/CON", "Bin/SS2VR/aux.txt", "Bin/SS2VR/x.", "Bin/SS2VR/x ", "Bin/SS2VR/a:b", "Bin/evil.dll",
		"Sam2.exe", "Bin/SS2VR", "Bin/SS2VR/install-receipt.json", "Bin/SS2VR/COM¹"} {
		if err := validateEntries(map[string]string{bad: h}); err == nil {
			t.Errorf("%q accepted", bad)
		}
	}
	if err := validateEntries(map[string]string{"Bin/SS2VR/a": h, "Bin/SS2VR/A": h}); err == nil {
		t.Error("case-insensitive duplicate accepted")
	}
	if err := validateEntries(map[string]string{"Bin/SS2VR/a": h, "Bin/SS2VR/a/b": h}); err == nil {
		t.Error("file-as-parent accepted")
	}
	if err := validateEntries(map[string]string{"Bin/SS2VR/a": "ABC"}); err == nil {
		t.Error("bad hash accepted")
	}
	if err := validateEntries(payloadHashes(payload("1"))); err != nil {
		t.Errorf("valid payload rejected: %v", err)
	}
}

func payloadHashes(files map[string]string) map[string]string {
	out := map[string]string{}
	for k, v := range files {
		out[k] = sum(v)
	}
	return out
}

func TestSHA256Mismatch(t *testing.T) {
	game := fakeGame(t)
	zipPath := zipDir(t, fakePackage(t, "1.0.0", payload("1.0.0"), ""), nil)
	srv := fakeGitHub(t, "1.0.0", zipPath, sum("not the zip"))
	before := snapshot(t, game)
	code, out := runCLI(t, "", "install", "--yes", "--game", game, "--release-api-url", srv.URL+"/repos/ObeseCatLord/ss2-vr/releases/latest")
	if code == 0 || !strings.Contains(out, "download sha256 mismatch") {
		t.Fatalf("exit %d\n%s", code, out)
	}
	assertSnapshot(t, before, snapshot(t, game))
}

func TestPackageContentMismatch(t *testing.T) {
	game := fakeGame(t)
	pkg := fakePackage(t, "1.0.0", payload("1.0.0"), "")
	writeFile(t, filepath.Join(pkg, "Bin/SS2VR/SS2VR.ini"), "tampered")
	before := snapshot(t, game)
	code, out := runCLI(t, "", "install", "--yes", "--game", game, "--package", pkg)
	if code == 0 || !strings.Contains(out, "package content/hash mismatch: Bin/SS2VR/SS2VR.ini") {
		t.Fatalf("exit %d\n%s", code, out)
	}
	assertSnapshot(t, before, snapshot(t, game))
}

func TestNetworkErrors(t *testing.T) {
	game := fakeGame(t)
	srv := httptest.NewServer(http.NotFoundHandler())
	defer srv.Close()
	code, out := runCLI(t, "", "install", "--yes", "--game", game, "--release-api-url", srv.URL+"/latest")
	if code == 0 || !strings.Contains(out, "no published release") || !strings.Contains(out, "--package") {
		t.Fatalf("exit %d\n%s", code, out)
	}
	srv.Close()
	code, out = runCLI(t, "", "install", "--yes", "--game", game, "--release-api-url", srv.URL+"/latest")
	if code == 0 || !strings.Contains(out, "network error") {
		t.Fatalf("exit %d\n%s", code, out)
	}
	// status still works without the network.
	code, out = runCLI(t, "", "status", "--game", game, "--release-api-url", srv.URL+"/latest")
	if code != 0 || !strings.Contains(out, "Latest:      unavailable") {
		t.Fatalf("status %d\n%s", code, out)
	}
}

func TestStatus(t *testing.T) {
	game := fakeGame(t)
	pkg := fakePackage(t, "1.0.0", payload("1.0.0"), "")
	runCLI(t, "", "install", "--yes", "--game", game, "--package", pkg)
	zipPath := zipDir(t, fakePackage(t, "2.0.0", payload("2.0.0"), ""), nil)
	srv := fakeGitHub(t, "2.0.0", zipPath, "")
	code, out := runCLI(t, "", "status", "--game", game, "--release-api-url", srv.URL+"/repos/ObeseCatLord/ss2-vr/releases/latest")
	for _, want := range []string{"Installed:   1.0.0 (all files unmodified)", "Latest:      2.0.0 (GitHub release v2.0.0)", "update available"} {
		if code != 0 || !strings.Contains(out, want) {
			t.Errorf("missing %q (exit %d):\n%s", want, code, out)
		}
	}
	code, out = runCLI(t, "", "status", "--offline", "--game", filepath.Join(game, "Bin"))
	if code != 0 || !strings.Contains(out, "not checked (--offline)") {
		t.Errorf("offline status %d:\n%s", code, out)
	}
}

func TestInteractiveMenu(t *testing.T) {
	game := fakeGame(t)
	zipPath := zipDir(t, fakePackage(t, "1.0.0", payload("1.0.0"), ""), nil)
	srv := fakeGitHub(t, "1.0.0", zipPath, zipSum(t, zipPath))
	api := srv.URL + "/repos/ObeseCatLord/ss2-vr/releases/latest"
	steamRootsFunc = func() []string { return nil }
	defer func() { steamRootsFunc = steamRoots }()
	// Not found automatically: the player types the folder, picks 1, then Enter to exit.
	code, out := runCLI(t, game+"\n1\n\n", "--release-api-url", api)
	if code != 0 {
		t.Fatalf("exit %d\n%s", code, out)
	}
	for _, want := range []string{"was not found automatically", "Latest:      1.0.0", "[1] Install / Update", "Installed SS2VR 1.0.0", "Press Enter to exit"} {
		if !strings.Contains(out, want) {
			t.Errorf("missing %q:\n%s", want, out)
		}
	}
	// Up to date: offered a reinstall, declined.
	code, out = runCLI(t, "1\nn\n\n", "--game", game, "--release-api-url", api)
	if code != 0 || !strings.Contains(out, "Reinstall it anyway?") {
		t.Fatalf("exit %d\n%s", code, out)
	}
	// Uninstall from the menu.
	code, out = runCLI(t, "2\ny\n\n", "--game", game, "--release-api-url", api)
	if code != 0 || !strings.Contains(out, "Removed unchanged SS2VR payload") || exists(filepath.Join(game, "Bin/d3d9.dll")) {
		t.Fatalf("exit %d\n%s", code, out)
	}
}

func TestSteamDetection(t *testing.T) {
	root := t.TempDir()
	steam := filepath.Join(root, "Steam")
	lib := filepath.Join(root, "Other Library")
	writeFile(t, filepath.Join(steam, "steamapps", "libraryfolders.vdf"), fmt.Sprintf(`"libraryfolders"
{
	"0"
	{
		"path"		"%s"
		"apps" { "228980" "1" }
	}
	"1"
	{
		"path"		"%s"
		"apps"
		{
			"204340"		"3898842078"
		}
	}
}
`, strings.ReplaceAll(steam, `\`, `\\`), strings.ReplaceAll(lib, `\`, `\\`)))
	writeFile(t, filepath.Join(lib, "steamapps", "common", "Serious Sam 2", "Bin", "Sam2.exe"), "exe")
	games := findGamesInSteamRoots([]string{steam, steam})
	want := filepath.Join(lib, "steamapps", "common", "Serious Sam 2")
	if len(games) != 1 || games[0] != want {
		t.Fatalf("games = %v, want [%s]", games, want)
	}
	steamRootsFunc = func() []string { return []string{steam} }
	defer func() { steamRootsFunc = steamRoots }()
	a := &app{}
	if got, err := a.findGame(); err != nil || filepath.Base(got) != "Serious Sam 2" {
		t.Fatalf("findGame = %q, %v", got, err)
	}
}

func TestPickReleasePrefersTagVersion(t *testing.T) {
	r, err := pickRelease(&releaseInfo{TagName: "v2.0.0", Assets: []releaseAsset{
		{Name: "SS2VR-1.9.0.zip"}, {Name: "SS2VR-2.0.0.zip"}, {Name: "SS2VR-2.0.0.zip.sha256"}, {Name: "ss2vr-installer-linux-x86_64"},
	}})
	if err != nil || r.Version != "2.0.0" || r.Checksum == nil {
		t.Fatalf("%+v %v", r, err)
	}
	if _, err := pickRelease(&releaseInfo{TagName: "v1", Assets: []releaseAsset{{Name: "source.zip"}}}); err == nil {
		t.Fatal("release without package accepted")
	}
}

func TestFailedUpdateRestoresPreviousVersion(t *testing.T) {
	game := fakeGame(t)
	v1 := payload("1.0.0")
	if code, out := runCLI(t, "", "install", "--yes", "--game", game, "--package", fakePackage(t, "1.0.0", v1, "")); code != 0 {
		t.Fatalf("install: %d\n%s", code, out)
	}
	writeFile(t, filepath.Join(game, "Bin/SS2VR/SS2VR.user.ini"), "mine")
	writeFile(t, filepath.Join(game, "Bin/SS2VR/ss2vr_host.log"), "log")
	before := snapshot(t, game)
	failCopyHook = func(relative string) error {
		if relative == "Bin/SS2VR/ss2vr_host.exe" {
			return fmt.Errorf("simulated disk full")
		}
		return nil
	}
	defer func() { failCopyHook = nil }()
	code, out := runCLI(t, "", "install", "--yes", "--game", game, "--package", fakePackage(t, "2.0.0", payload("2.0.0"), ""))
	if code == 0 || !strings.Contains(out, "simulated disk full (previous version 1.0.0 restored)") {
		t.Fatalf("exit %d\n%s", code, out)
	}
	assertSnapshot(t, before, snapshot(t, game))
}

func TestRefusesSymlinkedDestinations(t *testing.T) {
	for _, link := range []string{"Bin/SS2VR", "Content"} {
		game := fakeGame(t)
		os.RemoveAll(filepath.Join(game, link))
		outside := t.TempDir()
		if err := os.Symlink(outside, filepath.Join(game, filepath.FromSlash(link))); err != nil {
			t.Skip("symlinks unavailable:", err)
		}
		code, out := runCLI(t, "", "install", "--yes", "--game", game, "--package", fakePackage(t, "1.0.0", payload("1.0.0"), ""))
		if code == 0 {
			t.Errorf("%s symlink: install succeeded\n%s", link, out)
		}
		if entries, _ := os.ReadDir(outside); len(entries) != 0 {
			t.Errorf("%s symlink: wrote outside the game", link)
		}
	}
}
