"""Installer checks on synthetic files only; never modifies the actual installation."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('installer',ROOT/'tools/install.py')
installer=importlib.util.module_from_spec(spec);spec.loader.exec_module(installer)
def digest(data):return hashlib.sha256(data).hexdigest()
with tempfile.TemporaryDirectory(prefix='ss2vr-installer-') as folder:
 root=Path(folder);game=root/'game';package=root/'package';(game/'Bin').mkdir(parents=True);package.mkdir()
 native=b'synthetic game fixture';(game/'Bin/Sam2.exe').write_bytes(native)
 files={'Bin/d3d9.dll':b'synthetic proxy','Bin/SS2VR/ss2vr_host.exe':b'synthetic host',
        'Bin/SS2VRServer.dll':b'synthetic native server module','Content/SS2VR.mod':b'synthetic module selection'}
 for relative,data in files.items():p=package/relative;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
 manifest={'files':{p:digest(data) for p,data in files.items()},'version':'fixture','game_fingerprints':{'Sam2.exe':{'sha256':digest(native)}}}
 (package/'manifest.json').write_text(json.dumps(manifest))
 installer.install(game,package,True);assert not (game/'Bin/d3d9.dll').exists()
 collision=game/'Bin/d3d9.dll';collision.write_bytes(b'existing unrelated mod')
 try:installer.install(game,package,False);raise AssertionError('Collision accepted')
 except FileExistsError:pass
 assert collision.read_bytes()==b'existing unrelated mod';collision.unlink()
 content_collision=game/'Content/SS2VR.mod';content_collision.parent.mkdir();content_collision.write_bytes(b'other module selection')
 try:installer.install(game,package,False);raise AssertionError('Module collision accepted')
 except FileExistsError:pass
 assert content_collision.read_bytes()==b'other module selection' and not collision.exists();content_collision.unlink()
 installer.install(game,package,False);assert collision.read_bytes()==files['Bin/d3d9.dll']
 collision.write_bytes(b'modified payload')
 try:installer.uninstall(game,False);raise AssertionError('Modified payload removed')
 except ValueError:pass
 assert collision.read_bytes()==b'modified payload';collision.write_bytes(files['Bin/d3d9.dll'])
 installer.uninstall(game,False);assert not collision.exists();assert (game/'Bin/Sam2.exe').read_bytes()==native
 # The VR options panel's SS2VR.user.ini survives an update; the host log moves aside.
 installer.install(game,package,False)
 (game/'Bin/SS2VR/SS2VR.user.ini').write_bytes(b'[Render]\nFoveation=2\n');(game/'Bin/SS2VR/ss2vr_host.log').write_bytes(b'log')
 installer.uninstall(game,False);installer.install(game,package,False)
 assert (game/'Bin/SS2VR/SS2VR.user.ini').read_bytes()==b'[Render]\nFoveation=2\n' and (game/'Bin/ss2vr_host.log.previous').exists()
 (game/'Bin/SS2VR/SS2VR.user.ini').unlink();(game/'Bin/ss2vr_host.log.previous').unlink();(game/'Bin/SS2VR.user.ini.previous').unlink()
 installer.uninstall(game,False)
 # Unsafe paths must be rejected before any mutation.
 for relative in ['../escape','Bin/../escape','Bin/Engine.dll','Bin/SS2VR/../../escape',
                  'Content/SeriousSam2.mod','Content/SS2VR.mod/child','Bin/SS2VRServer.dll/child']:
  try:installer.paths({'files':{relative:'0'*64}});raise AssertionError('Unsafe path accepted')
  except ValueError:pass
 print('Synthetic collision, integrity, install/remove and path checks passed; actual game untouched.')

# Match Windows filename semantics even when preflight runs on Linux/Proton.
for relative in ['Bin/SS2VR', 'Bin/SS2VR/install-receipt.json',
                 'Bin/SS2VR/INSTALL-RECEIPT.JSON', 'Bin/SS2VR/install-receipt.json/child', 'Bin/SS2VR/NUL .txt', 'Bin//SS2VR/file',
                 'Bin/SS2VR/./file', 'Bin/SS2VR/file.', 'Bin/SS2VR/file ',
                 'Bin/SS2VR/NUL', 'Bin/SS2VR/con.txt', 'Bin/SS2VR/COM1.ini',
                 'Bin/SS2VR/LPT²', 'Bin/SS2VR/a:stream', 'Bin/SS2VR/a?b',
                 'Bin/SS2VR/control\nname', 'Bin/SS2VR/trailing/']:
 try: installer.paths({'files':{relative:'0'*64}}); raise AssertionError('Windows alias/reserved path accepted')
 except ValueError: pass
try:
 installer.paths({'files':{'Bin/SS2VR/A.ini':'0'*64,'Bin/SS2VR/a.ini':'1'*64}})
 raise AssertionError('Case-alias collision accepted')
except ValueError: pass

# A change between source preflight and copying must fail and leave a retryable
# fixture. The mutation is injected into the production copy boundary only.
with tempfile.TemporaryDirectory(prefix='ss2vr-copy-failure-') as folder:
 root=Path(folder);game=root/'game';package=root/'package'
 (game/'Bin').mkdir(parents=True);(package/'Bin/SS2VR').mkdir(parents=True)
 (game/'Bin/Sam2.exe').write_bytes(b'fixture')
 source=package/'Bin/SS2VR/ss2vr_host.exe';source.write_bytes(b'expected')
 manifest={'files':{'Bin/SS2VR/ss2vr_host.exe':digest(b'expected')},'version':'fixture',
           'game_fingerprints':{'Sam2.exe':{'sha256':digest(b'fixture')}}}
 (package/'manifest.json').write_text(json.dumps(manifest))
 original_copy=installer.shutil.copyfileobj
 def changed_copy(source, destination): destination.write(b'changed after preflight')
 installer.shutil.copyfileobj=changed_copy
 try:
  try: installer.install(game,package,False); raise AssertionError('Changed copy accepted')
  except ValueError: pass
 finally: installer.shutil.copyfileobj=original_copy
 assert not (game/'Bin/SS2VR').exists()
 assert (game/'Bin/Sam2.exe').read_bytes()==b'fixture'
 installer.install(game,package,False)
 installer.uninstall(game,False)
 print('Windows path aliases and copy-integrity rollback/retry checks passed.')

try:
 installer.paths({'files':{'Bin/SS2VR/file':'0'*64,'Bin/SS2VR/file/child':'1'*64,'Bin/SS2VR/file-other':'2'*64}})
 raise AssertionError('File/directory collision accepted')
except ValueError: pass

# A source can disappear after preflight, before its second open for copying.
# Opening the destination succeeds first: its ownership must already be tracked
# when opening the source fails, otherwise rollback leaves an empty collision.
with tempfile.TemporaryDirectory(prefix='ss2vr-source-open-failure-') as folder:
 root=Path(folder);game=root/'game';package=root/'package'
 (game/'Bin').mkdir(parents=True);(package/'Bin/SS2VR').mkdir(parents=True)
 (game/'Bin/Sam2.exe').write_bytes(b'fixture')
 source=package/'Bin/SS2VR/ss2vr_host.exe';source.write_bytes(b'expected')
 manifest={'files':{'Bin/SS2VR/ss2vr_host.exe':digest(b'expected')},'version':'fixture',
           'game_fingerprints':{'Sam2.exe':{'sha256':digest(b'fixture')}}}
 (package/'manifest.json').write_text(json.dumps(manifest))
 original_open=Path.open
 source_reads=0
 def fail_second_source_open(path, mode='r', *args, **kwargs):
  global source_reads
  if path==source and mode=='rb':
   source_reads+=1
   if source_reads==2: raise OSError('injected source-open failure after preflight')
  return original_open(path,mode,*args,**kwargs)
 Path.open=fail_second_source_open
 try:
  try: installer.install(game,package,False); raise AssertionError('Missing source accepted')
  except OSError as error:
   assert 'injected source-open failure' in str(error)
 finally: Path.open=original_open
 assert source_reads==2
 assert not (game/'Bin/SS2VR').exists(), 'Source-open failure left an orphan destination'
 assert (game/'Bin/Sam2.exe').read_bytes()==b'fixture'
 installer.install(game,package,False)
 installer.uninstall(game,False)
 print('Source-open rollback and clean retry passed.')
