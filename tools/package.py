"""the release zip: the source (what git tracks, and new files it doesn't ignore), the Builder and the recompiler prebuilt in tools/bin
with its licences. Never the game's files: only what git tracks plus those built tools, each checked by name.
python -I tools/package.py -> out/release/BlackQuest-<VERSION>.zip"""
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
print(out, os.path.getsize(out) // 1024, 'KB')
