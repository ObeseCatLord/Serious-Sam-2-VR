# SS2VR installer

A small program that finds your Serious Sam 2 (Steam) folder, downloads the
latest SS2VR release from GitHub and installs it, updates it, or removes it.

## Use it

1. Download the installer for your system from the latest SS2VR release:
   - Windows: `ss2vr-installer-windows-x86_64.exe`
   - Linux: `ss2vr-installer-linux-x86_64`
2. Run it.
   - Windows: double-click it. A console window opens.
   - Linux: make it executable first, then run it from a terminal:
     `chmod +x ss2vr-installer-linux-x86_64 && ./ss2vr-installer-linux-x86_64`
3. It shows the game folder it found, the SS2VR version you have and the latest
   release. Pick **1** to install or update, **2** to uninstall, **3** to quit.

If it can't find the game (or finds more than one copy), it asks you for the
folder, which is the one that contains `Bin/Sam2.exe`, for example
`C:\Program Files (x86)\Steam\steamapps\common\Serious Sam 2`.

**Linux:** SS2VR is a Windows mod that runs through Proton, so the Linux
installer just puts the files into the game folder; nothing else is needed.

## What it does (and doesn't do)

- It only installs the files listed in the release's `manifest.json`
  (`Bin/d3d9.dll`, `Bin/SS2VRServer.dll`, `Content/SS2VR.mod` and `Bin/SS2VR/`),
  after checking every file's SHA-256 (and the release's `.sha256` file when
  there is one).
- It first checks that your game files match the build the release was made
  for. If you get *"the game build doesn't match this mod release"*, update the
  game through Steam or use the SS2VR release for your game version.
- It never overwrites a file it didn't install (for example another mod's
  `d3d9.dll`), and it records what it installed in
  `Bin/SS2VR/install-receipt.json`.
- **Update** removes the old version (only if its files are unchanged) and
  installs the new one. Your VR settings (`SS2VR.user.ini`) and the cached
  headset view size are kept. If the new install fails, the old version is put
  back.
- **Uninstall** removes only the files it installed, and refuses if you
  edited or deleted any of them. Your settings and logs stay in `Bin/SS2VR/`.

## Command line

```
ss2vr-installer status                 # game folder, installed and latest version
ss2vr-installer install                # install or update (asks first; --yes skips)
ss2vr-installer uninstall
```

Options: `--game PATH`, `--package FILE.zip|FOLDER` (offline install),
`--dry-run` (check everything, change nothing), `--force-reinstall`, `--yes`,
`--repo owner/name`, `--offline` (status without contacting GitHub),
`--version`.

## Building (developers)

Go standard library only. `./build.sh [version]` cross-compiles
`dist/ss2vr-installer-linux-x86_64` and
`dist/ss2vr-installer-windows-x86_64.exe` (CGO disabled, version embedded via
`-X main.version`). Run the tests with `go test ./...`.

A release needs an asset named `SS2VR-<version>.zip` whose root holds
`manifest.json` and the mod files (as produced by `tools/package.py`);
optionally add `SS2VR-<version>.zip.sha256` (`sha256sum` output).
