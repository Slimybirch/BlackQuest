"""the release zip and its notes: the source (what git tracks, and new files it doesn't ignore), the Builder and the recompiler prebuilt in tools/bin
with its licences. Never the game's files: only what git tracks plus those built tools, each checked by name.
python -I tools/package.py -> out/release/BlackQuest-<VERSION>.zip and notes.md (the release's body: what changed
since the last tag, and the markers the Builder and the headset read: "Rebuild: full" when the game's code is
recompiled, "HD textures: redo" when the HD textures must be converted again). MANIFEST.txt in the zip lists its
files, so the Builder's update removes the ones a release drops"""
import os, subprocess, sys, zipfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
version = open(os.path.join(root, 'VERSION')).read().strip()
tools = os.path.join(root, 'out', 'build', 'prebuilt-tools')
extra = {
    'BlackQuestBuilder.exe': os.path.join(root, 'BlackQuestBuilder.exe'),
    'tools/bin/xbe_recomp.exe': os.path.join(tools, 'xbe_recomp.exe'),
    'tools/bin/xbe_symbols.exe': os.path.join(tools, 'xbe_symbols.exe'),
    'tools/bin/LICENSE-XboxXBE-Recomp.txt': os.path.join(root, 'ref', 'XboxXBE-Recomp', 'LICENSE'),
    'tools/bin/THIRD_PARTY-XboxXBE-Recomp.txt': os.path.join(root, 'ref', 'XboxXBE-Recomp', 'THIRD_PARTY.txt'),
    'tools/bin/LICENSE-zydis.txt': os.path.join(tools, '_deps', 'zydis-src', 'LICENSE'),
    'tools/bin/LICENSE-tomlplusplus.txt': os.path.join(tools, '_deps', 'tomlplusplus-src', 'LICENSE'),
}
dirty = subprocess.run(['git', 'status', '--porcelain'], cwd=root, capture_output=True, text=True).stdout.strip()
if dirty:
    print('warning: uncommitted changes go in too (a release is packed from a clean commit)')
files = subprocess.run(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], cwd=root,
                       capture_output=True, check=True).stdout.decode().split(chr(0))
os.makedirs(os.path.join(root, 'out', 'release'), exist_ok=True)
out = os.path.join(root, 'out', 'release', 'BlackQuest-%s.zip' % version)
bad = ('.iso', '.xbe', '.apk', '.keystore', '.jks', '.dds')
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
    for f in sorted(x for x in files if x):
        path = os.path.join(root, f)
        if not os.path.isfile(path):
            continue  # deleted, not yet committed
        assert not f.lower().endswith(bad) and os.path.getsize(path) < 2 << 20, f
        z.write(path, 'BlackQuest/' + f)
    for name, path in extra.items():
        z.write(path, 'BlackQuest/' + name)
    names = sorted(f for f in files if f and os.path.isfile(os.path.join(root, f))) + sorted(extra)
    z.writestr('BlackQuest/MANIFEST.txt', '\n'.join(names + ['MANIFEST.txt']) + '\n')

# the notes, against the newest tag before this version's
def git(*a):
    return subprocess.run(['git'] + list(a), cwd=root, capture_output=True, text=True).stdout.strip()

tags = [t for t in git('tag', '--list', 'v*', '--sort=-v:refname').split() if t != 'v' + version]
prev = tags[0] if tags else None
span = (prev + '..HEAD') if prev else 'HEAD'
subjects = [s for s in git('log', '--no-merges', '--format=%s', span).splitlines() if s]
def changed(*paths):
    return prev is None or bool(git('diff', '--name-only', prev, 'HEAD', '--', *paths))
# the recompiled code's inputs: its hooks and symbols, the recompiler's pin, the native build's flags
full = changed('config/game.toml', 'tools/CMakeLists.txt', 'CMakeLists.txt')
hd = prev is not None and changed('tools/hd_map.txt', 'builder/HdPack.cs')
# NEXT_RELEASE.md: the player's list of what's new, kept while commits gather (user: a few commits make an update);
# the commit subjects when it's empty. Empty it after the release
nf = os.path.join(root, 'NEXT_RELEASE.md')
items = [l.strip() for l in open(nf, encoding='utf-8').read().splitlines() if l.strip().startswith('- ')] if os.path.exists(nf) else []
if not items:
    items = ['- ' + s for s in subjects[:40]]
notes = ['## What\'s Changed'] + items + ['', 'Rebuild: ' + ('full' if full else 'quick')]
if hd:
    notes.append('HD textures: redo')
notes += ['', 'Update with BlackQuest Builder on your PC (it checks for updates when it starts); a first install: '
          'download the zip below and see the README.']
open(os.path.join(root, 'out', 'release', 'notes.md'), 'w', encoding='utf-8', newline='\n').write('\n'.join(notes) + '\n')
print('notes: since', prev or 'the start', '-', len(subjects), 'commits, rebuild', 'full' if full else 'quick',
      '(HD redo)' if hd else '')
print(out, os.path.getsize(out) // 1024, 'KB')
