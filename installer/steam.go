package main

// Finding Serious Sam 2 (Steam app 204340) through Steam's library folders.

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
)

const (
	steamAppID     = "204340"
	defaultInstall = "Serious Sam 2"
	gameExecutable = "Sam2.exe"
)

// isGameDir reports whether dir looks like a Serious Sam 2 install.
func isGameDir(dir string) bool {
	info, err := os.Stat(filepath.Join(dir, "Bin", gameExecutable))
	return err == nil && info.Mode().IsRegular()
}

// normalizeGameDir accepts the game folder, its Bin folder or Sam2.exe itself.
func normalizeGameDir(p string) (string, error) {
	p = strings.TrimSpace(strings.Trim(strings.TrimSpace(p), `"'`))
	if p == "" {
		return "", errors.New("no game folder given")
	}
	if strings.HasPrefix(p, "~"+string(filepath.Separator)) || strings.HasPrefix(p, "~/") {
		if home, err := os.UserHomeDir(); err == nil {
			p = filepath.Join(home, p[2:])
		}
	}
	candidates := []string{p}
	if strings.EqualFold(filepath.Base(p), gameExecutable) {
		candidates = append(candidates, filepath.Dir(filepath.Dir(p)))
	}
	if strings.EqualFold(filepath.Base(p), "Bin") {
		candidates = append(candidates, filepath.Dir(p))
	}
	for _, c := range candidates {
		if isGameDir(c) {
			return filepath.Clean(c), nil
		}
	}
	return "", errors.New("not a Serious Sam 2 folder (Bin/Sam2.exe not found): " + p)
}

func findGamesInSteamRoots(roots []string) []string {
	seen := map[string]bool{}
	var games []string
	add := func(dir string) {
		if !isGameDir(dir) {
			return
		}
		key := dir
		if r, err := filepath.EvalSymlinks(dir); err == nil {
			key = r
		}
		if !seen[key] {
			seen[key] = true
			games = append(games, filepath.Clean(dir))
		}
	}
	for _, root := range roots {
		for _, lib := range libraryFolders(root) {
			steamapps := filepath.Join(lib, "steamapps")
			installdir := defaultInstall
			if data, err := os.ReadFile(filepath.Join(steamapps, "appmanifest_"+steamAppID+".acf")); err == nil {
				if v, ok := parseVDF(string(data))["AppState"].(map[string]any); ok {
					if s, ok := lookupFold(v, "installdir").(string); ok && s != "" {
						installdir = s
					}
				}
			}
			add(filepath.Join(steamapps, "common", installdir))
			add(filepath.Join(steamapps, "common", defaultInstall))
		}
	}
	return games
}

// libraryFolders lists the Steam root plus libraries from libraryfolders.vdf.
func libraryFolders(root string) []string {
	libs := []string{root}
	for _, name := range []string{filepath.Join(root, "steamapps", "libraryfolders.vdf"), filepath.Join(root, "config", "libraryfolders.vdf")} {
		data, err := os.ReadFile(name)
		if err != nil {
			continue
		}
		top, _ := lookupFold(parseVDF(string(data)), "libraryfolders").(map[string]any)
		for _, v := range top {
			switch entry := v.(type) {
			case map[string]any: // modern format: "0" { "path" "..." }
				if p, ok := lookupFold(entry, "path").(string); ok && p != "" {
					libs = append(libs, filepath.FromSlash(p))
				}
			case string: // old format: "1" "D:\\SteamLibrary"
				if entry != "" && strings.ContainsAny(entry, `/\`) {
					libs = append(libs, filepath.FromSlash(entry))
				}
			}
		}
	}
	return libs
}

func lookupFold(m map[string]any, key string) any {
	if v, ok := m[key]; ok {
		return v
	}
	for k, v := range m {
		if strings.EqualFold(k, key) {
			return v
		}
	}
	return nil
}

// parseVDF parses Valve's KeyValues text format into nested maps.
// Malformed input yields whatever was parsed so far.
func parseVDF(s string) map[string]any {
	tokens := tokenizeVDF(s)
	pos := 0
	var parse func() map[string]any
	parse = func() map[string]any {
		out := map[string]any{}
		for pos < len(tokens) {
			tok := tokens[pos]
			pos++
			if tok == "}" {
				return out
			}
			if tok == "{" || pos >= len(tokens) {
				continue
			}
			next := tokens[pos]
			pos++
			if next == "{" {
				out[tok] = parse()
			} else if next != "}" {
				out[tok] = next
			} else {
				return out
			}
		}
		return out
	}
	return parse()
}

func tokenizeVDF(s string) []string {
	var tokens []string
	for i := 0; i < len(s); {
		c := s[i]
		switch {
		case c == ' ' || c == '\t' || c == '\r' || c == '\n':
			i++
		case c == '/' && i+1 < len(s) && s[i+1] == '/':
			for i < len(s) && s[i] != '\n' {
				i++
			}
		case c == '{' || c == '}':
			tokens = append(tokens, string(c))
			i++
		case c == '"':
			var b strings.Builder
			i++
			for i < len(s) && s[i] != '"' {
				if s[i] == '\\' && i+1 < len(s) {
					i++
					switch s[i] {
					case 'n':
						b.WriteByte('\n')
					case 't':
						b.WriteByte('\t')
					default:
						b.WriteByte(s[i])
					}
				} else {
					b.WriteByte(s[i])
				}
				i++
			}
			i++
			tokens = append(tokens, b.String())
		default:
			start := i
			for i < len(s) && !strings.ContainsRune(" \t\r\n{}\"", rune(s[i])) {
				i++
			}
			tokens = append(tokens, s[start:i])
		}
	}
	return tokens
}
