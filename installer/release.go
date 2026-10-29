package main

// Fetching the latest GitHub release and unpacking a mod package safely.

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

const (
	defaultRepo      = "ObeseCatLord/Serious-Sam-2-VR"
	maxPackageBytes  = 1 << 30   // download / archive size cap
	maxEntryBytes    = 512 << 20 // single extracted file cap
	maxManifestBytes = 4 << 20
)

// ErrNetwork wraps failures talking to GitHub.
var ErrNetwork = errors.New("network error")

type releaseAsset struct {
	Name string `json:"name"`
	URL  string `json:"browser_download_url"`
	Size int64  `json:"size"`
}

type releaseInfo struct {
	TagName string         `json:"tag_name"`
	Name    string         `json:"name"`
	Assets  []releaseAsset `json:"assets"`
}

// Release is the chosen mod package of a GitHub release.
type Release struct {
	Version  string
	Tag      string
	Package  releaseAsset
	Checksum *releaseAsset
}

var packageAssetPattern = regexp.MustCompile(`^SS2VR-(.+)\.zip$`)

type statusError struct {
	url, status string
	code        int
}

func (e *statusError) Error() string {
	return fmt.Sprintf("%v: GET %s returned %s", ErrNetwork, e.url, e.status)
}

func (e *statusError) Unwrap() error { return ErrNetwork }

func releaseAPIURL(repo string) string {
	return "https://api.github.com/repos/" + repo + "/releases/latest"
}

var httpClient = &http.Client{Timeout: 10 * time.Minute}

func httpGet(url, accept string) (*http.Response, error) {
	req, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", "ss2vr-installer/"+version)
	if accept != "" {
		req.Header.Set("Accept", accept)
	}
	resp, err := httpClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrNetwork, err)
	}
	if resp.StatusCode != http.StatusOK {
		resp.Body.Close()
		return nil, &statusError{url: url, code: resp.StatusCode, status: resp.Status}
	}
	return resp, nil
}

// fetchLatestRelease reads releases/latest and picks the SS2VR-<version>.zip asset.
func fetchLatestRelease(apiURL string) (*Release, error) {
	resp, err := httpGet(apiURL, "application/vnd.github+json")
	var se *statusError
	if errors.As(err, &se) && se.code == http.StatusNotFound {
		return nil, fmt.Errorf("%w: no published release found at %s", ErrNetwork, apiURL)
	}
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	var info releaseInfo
	if err := json.NewDecoder(io.LimitReader(resp.Body, 8<<20)).Decode(&info); err != nil {
		return nil, fmt.Errorf("%w: invalid release data: %v", ErrNetwork, err)
	}
	return pickRelease(&info)
}

func pickRelease(info *releaseInfo) (*Release, error) {
	tagVersion := strings.TrimPrefix(info.TagName, "v")
	var chosen *Release
	for i := range info.Assets {
		match := packageAssetPattern.FindStringSubmatch(info.Assets[i].Name)
		if match == nil {
			continue
		}
		r := &Release{Version: match[1], Tag: info.TagName, Package: info.Assets[i]}
		// Prefer the asset whose version matches the tag if there are several.
		if chosen == nil || match[1] == tagVersion {
			chosen = r
		}
	}
	if chosen == nil {
		return nil, fmt.Errorf("release %s has no SS2VR-<version>.zip asset", info.TagName)
	}
	for i := range info.Assets {
		if info.Assets[i].Name == chosen.Package.Name+".sha256" {
			chosen.Checksum = &info.Assets[i]
		}
	}
	return chosen, nil
}

// downloadRelease downloads the package zip into dir, verifying the
// .sha256 asset when the release has one. Returns the zip path.
func downloadRelease(out io.Writer, r *Release, dir string) (string, error) {
	var want string
	if r.Checksum != nil {
		resp, err := httpGet(r.Checksum.URL, "application/octet-stream")
		if err != nil {
			return "", err
		}
		data, err := io.ReadAll(io.LimitReader(resp.Body, 4096))
		resp.Body.Close()
		if err != nil {
			return "", fmt.Errorf("%w: %v", ErrNetwork, err)
		}
		fields := strings.Fields(string(data))
		if len(fields) == 0 || !validHash(strings.ToLower(fields[0])) {
			return "", fmt.Errorf("invalid checksum file %s", r.Checksum.Name)
		}
		want = strings.ToLower(fields[0])
	}
	fmt.Fprintf(out, "Downloading %s (%.1f MB)...\n", r.Package.Name, float64(r.Package.Size)/(1<<20))
	resp, err := httpGet(r.Package.URL, "application/octet-stream")
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	target := filepath.Join(dir, "package.zip")
	f, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		return "", err
	}
	h := sha256.New()
	n, err := io.Copy(io.MultiWriter(f, h), io.LimitReader(resp.Body, maxPackageBytes+1))
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		return "", fmt.Errorf("%w: download failed: %v", ErrNetwork, err)
	}
	if n > maxPackageBytes {
		return "", errors.New("download is larger than the 1 GiB limit")
	}
	got := hex.EncodeToString(h.Sum(nil))
	if want != "" {
		if got != want {
			return "", fmt.Errorf("download sha256 mismatch for %s: got %s, expected %s", r.Package.Name, got, want)
		}
		fmt.Fprintln(out, "Checksum verified (sha256).")
	} else {
		fmt.Fprintf(out, "No .sha256 asset in this release; package files are still verified against manifest.json.\n")
	}
	return target, nil
}

// unsafeZipName reports zip-slip names: absolute paths, drive letters or "..".
func unsafeZipName(name string) bool {
	n := strings.ReplaceAll(name, `\`, "/")
	if strings.HasPrefix(n, "/") || (len(n) >= 2 && n[1] == ':') {
		return true
	}
	for _, part := range strings.Split(n, "/") {
		if part == ".." {
			return true
		}
	}
	return false
}

// extractPackage unpacks manifest.json and the files it lists from a package
// zip into dest and returns the package root. Every entry name is checked
// for zip-slip first; entries not in the manifest (installer binaries,
// README, sources) are ignored. manifest.json may be at the zip root or in a
// single top-level folder.
func extractPackage(zipPath, dest string) (string, error) {
	zr, err := zip.OpenReader(zipPath)
	if err != nil && !errors.Is(err, zip.ErrInsecurePath) {
		return "", fmt.Errorf("cannot open package zip: %w", err)
	}
	if zr == nil {
		return "", fmt.Errorf("cannot open package zip: %w", err)
	}
	defer zr.Close()
	byName := map[string]*zip.File{}
	var total uint64
	for _, f := range zr.File {
		if unsafeZipName(f.Name) {
			return "", fmt.Errorf("unsafe path in package zip: %q", f.Name)
		}
		byName[f.Name] = f
		total += f.UncompressedSize64
	}
	if total > maxPackageBytes*4 {
		return "", errors.New("package zip expands to an unreasonable size")
	}
	prefix := ""
	if byName["manifest.json"] == nil {
		for name := range byName {
			if dir, file := path.Split(name); file == "manifest.json" && strings.Count(dir, "/") == 1 {
				if prefix != "" {
					return "", errors.New("package zip has several manifest.json files")
				}
				prefix = dir
			}
		}
		if prefix == "" {
			return "", errors.New("package zip has no manifest.json at its root")
		}
	}
	root := filepath.Join(dest, "package")
	if err := os.Mkdir(root, 0o755); err != nil {
		return "", err
	}
	extract := func(relative string, limit int64) error {
		f := byName[prefix+relative]
		if f == nil {
			return fmt.Errorf("package zip is missing %s", relative)
		}
		if !f.Mode().IsRegular() {
			return fmt.Errorf("package zip entry is not a regular file: %s", relative)
		}
		if f.UncompressedSize64 > uint64(limit) {
			return fmt.Errorf("package zip entry too large: %s", relative)
		}
		target := filepath.Join(root, filepath.FromSlash(relative))
		if !within(root, target) {
			return fmt.Errorf("unsafe path in package zip: %q", relative)
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		rc, err := f.Open()
		if err != nil {
			return err
		}
		defer rc.Close()
		out, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
		if err != nil {
			return err
		}
		n, err := io.Copy(out, io.LimitReader(rc, limit+1))
		if cerr := out.Close(); err == nil {
			err = cerr
		}
		if err == nil && n > limit {
			err = fmt.Errorf("package zip entry too large: %s", relative)
		}
		if err != nil {
			return fmt.Errorf("extracting %s: %w", relative, err)
		}
		return nil
	}
	if err := extract("manifest.json", maxManifestBytes); err != nil {
		return "", err
	}
	m, err := readManifest(filepath.Join(root, "manifest.json"))
	if err != nil {
		return "", err
	}
	for _, relative := range sortedKeys(m.Files) {
		if err := extract(relative, maxEntryBytes); err != nil {
			return "", err
		}
	}
	return root, nil
}
