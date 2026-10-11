#!/usr/bin/env python3
"""Install/remove only SS2VR-owned files, with collision and hash checks."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import sys
RECEIPT = Path('Bin/SS2VR/install-receipt.json')

def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def paths(manifest):
    entries = manifest['files']
    if not isinstance(entries, dict) or not entries:
        raise ValueError('Package file list must be a nonempty object')
    seen = set()
    devices = {'CON', 'PRN', 'AUX', 'NUL', 'CONIN$', 'CONOUT$'} | {
        prefix + suffix for prefix in ('COM', 'LPT') for suffix in '123456789¹²³'}
    for relative, expected in entries.items():
        if not isinstance(relative, str) or not isinstance(expected, str):
            raise ValueError('Invalid package entry')
        p = PurePosixPath(relative)
        if (p.is_absolute() or str(p) != relative or '..' in p.parts or '\\' in relative or
            any(ord(c) < 32 or c in '<>:"|?*' for c in relative) or
            any(part.endswith((' ', '.')) or part.split('.')[0].rstrip(' ').upper() in devices for part in p.parts)):
            raise ValueError('Unsafe package path')
        if relative not in ('Bin/d3d9.dll', 'Bin/SS2VRServer.dll', 'Content/SS2VR.mod') and p.parts[:2] != ('Bin', 'SS2VR'):
            raise ValueError('File outside mod ownership')
        folded = relative.casefold()
        if folded in seen or (folded + '/').startswith(str(RECEIPT).replace('\\', '/').casefold() + '/') or relative == 'Bin/SS2VR':
            raise ValueError('Duplicate/reserved package path')
        seen.add(folded)
        if len(expected) != 64 or any(c not in '0123456789abcdef' for c in expected):
            raise ValueError('Invalid manifest hash')
    for path in seen:
        parts = path.split('/')
        if any('/'.join(parts[:count]) in seen for count in range(1, len(parts))):
            raise ValueError('Package file is also a parent directory')
    return entries

def destination(game, relative):
    target = game/relative
    # Resolve existing parents to prevent writes/deletions through a mod-folder symlink.
    if target.resolve().is_relative_to(game) and not target.is_symlink(): return target
    raise ValueError('Destination escapes installation or is a symlink')

def install(game, package, dry):
    manifest = json.loads((package/'manifest.json').read_text())
    entries = paths(manifest)
    for name, metadata in manifest['game_fingerprints'].items():
        if Path(name).name != name: raise ValueError('Unsafe fingerprint filename')
        if digest(game/'Bin'/name) != metadata['sha256']:
            raise ValueError('Game build does not match verified fingerprints: ' + name)
    folder = game/'Bin/SS2VR'
    view_cache = None
    # Uninstall keeps the mod's own runtime files (host log, cached headset view
    # size); a folder holding only those is ours.
    leftovers = [] if folder.is_symlink() or not folder.is_dir() else list(folder.iterdir())
    if folder.is_dir() and not folder.is_symlink() and leftovers and \
       all(f.is_file() and not f.is_symlink() and
           ((f.name.startswith('ss2vr_host') and f.suffix == '.log') or f.name == 'headset-view.ini')
           for f in leftovers):
        if not dry:
            for f in leftovers:
                if f.name == 'headset-view.ini': view_cache = f.read_bytes()
                f.rename(game/'Bin'/(f.name + '.previous'))
            folder.rmdir()
    elif folder.exists() or folder.is_symlink():
        raise FileExistsError('SS2VR destination already exists; refusing overwrite')
    for relative, expected in entries.items():
        source = package/relative
        if not source.resolve().is_relative_to(package) or source.is_symlink() or digest(source) != expected:
            raise ValueError('Package content/hash mismatch: ' + relative)
        target = destination(game, relative)
        if target.exists(): raise FileExistsError('Refusing to overwrite: ' + relative)
    if dry:
        print('Preflight passed: matching game, verified package, no collisions. No files changed.'); return
    created = []
    created_directories = []
    receipt_created=False
    try:
        for relative in entries:
            target = destination(game, relative)
            missing = []
            parent = target.parent
            while not parent.exists():
                missing.append(parent)
                parent = parent.parent
            for parent in reversed(missing):
                try:
                    parent.mkdir()
                    created_directories.append(parent)
                except FileExistsError:
                    if not parent.is_dir(): raise
            # Exclusive creation prevents a race from overwriting another mod.
            with target.open('xb') as output:
                # Exclusive creation has already succeeded. Record ownership
                # before opening the source, which can fail after preflight.
                created.append(target)
                with (package/relative).open('rb') as source:
                    shutil.copyfileobj(source, output)
            if digest(target) != entries[relative]:
                raise ValueError('Copied package content changed: ' + relative)
        with destination(game, str(RECEIPT)).open('x') as output:
            receipt_created=True
            json.dump({'files':entries, 'version':manifest['version']}, output, indent=2)
    except Exception:
        if receipt_created: destination(game,str(RECEIPT)).unlink()
        # Roll back only newly-created payload files, never existing files.
        for target in created:
            if target.is_file() and not target.is_symlink(): target.unlink()
        for folder in reversed(created_directories):
            try: folder.rmdir()
            except OSError: pass
        raise
    if view_cache is not None:
        # Keep the runtime's last recommended eye size so the canvas starts at it.
        cache = destination(game, 'Bin/SS2VR/headset-view.ini')
        if not cache.exists(): cache.write_bytes(view_cache)
    print('Installed SS2VR development build. No game/runtime was launched.')

def uninstall(game, dry):
    receipt = destination(game, str(RECEIPT))
    manifest = json.loads(receipt.read_text())
    entries = paths(manifest)
    for relative, expected in entries.items():
        target = destination(game, relative)
        if not target.is_file() or digest(target) != expected:
            raise ValueError('Modified/missing mod file; refusing removal: ' + relative)
    if dry:
        print('Removal preflight passed. No files changed.'); return
    for relative in entries: destination(game, relative).unlink()
    receipt.unlink()
    # Empty mod-owned directories only; runtime logs or other additions are retained.
    folders = sorted({(game/r).parent for r in entries if r.startswith('Bin/SS2VR/')},
                     key=lambda p:len(p.parts), reverse=True)
    for folder in folders:
        try: folder.rmdir()
        except OSError: pass
    print('Removed unchanged SS2VR payload; other files retained.')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['install','uninstall'])
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--package', type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    try:
        game=args.game.resolve()
        if args.action=='install': install(game,args.package.resolve(),args.dry_run)
        else: uninstall(game,args.dry_run)
    except (OSError,ValueError,KeyError) as error:
        print('Stopped:',error,file=sys.stderr);sys.exit(1)
