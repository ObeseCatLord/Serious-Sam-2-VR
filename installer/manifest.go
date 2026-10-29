package main

// Package manifest and receipt handling, ported from tools/install.py
// (paths(), the fingerprint check and the receipt format).

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"sort"
	"strings"
)

// receiptPath is the install receipt, relative to the game folder.
const receiptPath = "Bin/SS2VR/install-receipt.json"

// Fingerprint is one entry of manifest.json "game_fingerprints". Only the
// sha256 is checked; the other PE metadata is informational.
type Fingerprint struct {
	SHA256 string `json:"sha256"`
}

// Manifest is the subset of a package manifest.json the installer uses.
type Manifest struct {
	Version          string                 `json:"version"`
	Files            map[string]string      `json:"files"`
	GameFingerprints map[string]Fingerprint `json:"game_fingerprints"`
}

// Receipt is Bin/SS2VR/install-receipt.json, written by install.
type Receipt struct {
	Files   map[string]string `json:"files"`
	Version string            `json:"version"`
}

// errNoReceipt reports that the game has no SS2VR install receipt.
var errNoReceipt = errors.New("SS2VR is not installed (no install receipt)")

func readManifest(path string) (*Manifest, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("cannot read package manifest: %w", err)
	}
	var m Manifest
	if err := json.Unmarshal(data, &m); err != nil {
		return nil, fmt.Errorf("invalid package manifest.json: %w", err)
	}
	if m.Version == "" {
		return nil, errors.New("invalid package manifest.json: missing version")
	}
	if m.GameFingerprints == nil {
		return nil, errors.New("invalid package manifest.json: missing game_fingerprints")
	}
	for name, fp := range m.GameFingerprints {
		if !validFingerprintName(name) {
			return nil, fmt.Errorf("unsafe fingerprint filename: %q", name)
		}
		if !validHash(fp.SHA256) {
			return nil, fmt.Errorf("invalid fingerprint hash for %s", name)
		}
	}
	if err := validateEntries(m.Files); err != nil {
		return nil, err
	}
	return &m, nil
}

// validFingerprintName mirrors `Path(name).name != name` in install.py: a
// bare file name inside the game's Bin folder.
func validFingerprintName(name string) bool {
	return name != "" && name != "." && name != ".." && !strings.ContainsAny(name, `/\:`)
}

func validHash(h string) bool {
	if len(h) != 64 {
		return false
	}
	for _, c := range h {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

var windowsDevices = func() map[string]bool {
	d := map[string]bool{"CON": true, "PRN": true, "AUX": true, "NUL": true, "CONIN$": true, "CONOUT$": true}
	for _, prefix := range []string{"COM", "LPT"} {
		for _, suffix := range []string{"1", "2", "3", "4", "5", "6", "7", "8", "9", "¹", "²", "³"} {
			d[prefix+suffix] = true
		}
	}
	return d
}()

// validateEntries is install.py paths(): every manifest path must be a
// normalized relative POSIX path, Windows-safe, inside the mod's ownership
// (Bin/d3d9.dll, Bin/SS2VRServer.dll, Content/SS2VR.mod or under Bin/SS2VR/),
// unique case-insensitively, not the receipt, and not a parent of another.
func validateEntries(entries map[string]string) error {
	if len(entries) == 0 {
		return errors.New("package file list must be a nonempty object")
	}
	seen := map[string]bool{}
	receiptFolded := strings.ToLower(receiptPath)
	for relative, expected := range entries {
		if err := validateRelative(relative); err != nil {
			return err
		}
		parts := strings.Split(relative, "/")
		owned := relative == "Bin/d3d9.dll" || relative == "Bin/SS2VRServer.dll" || relative == "Content/SS2VR.mod" ||
			(len(parts) >= 2 && parts[0] == "Bin" && parts[1] == "SS2VR")
		if !owned {
			return fmt.Errorf("file outside mod ownership: %s", relative)
		}
		folded := strings.ToLower(relative)
		if seen[folded] || strings.HasPrefix(folded+"/", receiptFolded+"/") || folded == "bin/ss2vr" {
			return fmt.Errorf("duplicate/reserved package path: %s", relative)
		}
		seen[folded] = true
		if !validHash(expected) {
			return fmt.Errorf("invalid manifest hash for %s", relative)
		}
	}
	for path := range seen {
		parts := strings.Split(path, "/")
		for count := 1; count < len(parts); count++ {
			if seen[strings.Join(parts[:count], "/")] {
				return fmt.Errorf("package file is also a parent directory: %s", strings.Join(parts[:count], "/"))
			}
		}
	}
	return nil
}

// validateRelative applies the path-safety half of install.py paths().
func validateRelative(relative string) error {
	unsafe := fmt.Errorf("unsafe package path: %q", relative)
	if relative == "" || strings.HasPrefix(relative, "/") || strings.Contains(relative, `\`) {
		return unsafe
	}
	for _, c := range relative {
		if c < 32 || strings.ContainsRune(`<>:"|?*`, c) {
			return unsafe
		}
	}
	for _, part := range strings.Split(relative, "/") {
		// Empty / "." parts mean the path is not normalized (str(p) != relative).
		if part == "" || part == "." || part == ".." {
			return unsafe
		}
		if strings.HasSuffix(part, " ") || strings.HasSuffix(part, ".") {
			return unsafe
		}
		stem := strings.ToUpper(strings.TrimRight(strings.SplitN(part, ".", 2)[0], " "))
		if windowsDevices[stem] {
			return unsafe
		}
	}
	return nil
}

func sortedKeys(m map[string]string) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}

func fileSHA256(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}
