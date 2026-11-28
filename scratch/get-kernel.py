"""Download kernel/kernel.macho from a successful prelink run."""
import hashlib
import io
import json
import pathlib
import subprocess
import sys
import urllib.error
import urllib.request
import zipfile

REPO = 'limingerf/UniXNU-tb710fu'
RUN = sys.argv[1]
TAG = sys.argv[2] if len(sys.argv) > 2 else 'pl107'
DEST = pathlib.Path(r'd:\unixnu-tb710fu-clean\out') / TAG

out = subprocess.check_output(
    ['git', 'credential', 'fill'],
    input='protocol=https\nhost=github.com\n\n', text=True,
    cwd=r'd:\unixnu-tb710fu-clean')
tok = next(l.split('=', 1)[1].strip() for l in out.splitlines()
           if l.startswith('password='))
hdr = {'Authorization': f'Bearer {tok}',
       'Accept': 'application/vnd.github+json',
       'User-Agent': 'unixnu-tb710fu'}


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None


def api(path):
    req = urllib.request.Request(f'https://api.github.com{path}', headers=hdr)
    with urllib.request.urlopen(req, timeout=90) as r:
        raw = r.read()
        return json.loads(raw) if raw else {}


def download(url):
    op = urllib.request.build_opener(NoRedirect)
    try:
        with op.open(urllib.request.Request(url, headers=hdr), timeout=120) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        if e.code not in (301, 302, 303, 307, 308):
            raise
        loc = e.headers['Location']
    with urllib.request.urlopen(loc, timeout=600) as r:
        return r.read()


arts = api(f'/repos/{REPO}/actions/runs/{RUN}/artifacts')['artifacts']
if not arts:
    sys.exit('no artifacts')
print(f"artifact: {arts[0]['name']} ({arts[0]['size_in_bytes']} bytes)")

blob = download(
    f"https://api.github.com/repos/{REPO}/actions/artifacts/{arts[0]['id']}/zip")
zf = zipfile.ZipFile(io.BytesIO(blob))

targets = [n for n in zf.namelist() if n.endswith('kernel.macho')]
print('candidates:', targets)
if not targets:
    sys.exit('kernel.macho not found')

member = targets[0]
data = zf.read(member)
DEST.mkdir(parents=True, exist_ok=True)
(DEST / 'kernel.macho').write_bytes(data)
print(f'wrote {DEST / "kernel.macho"} ({len(data)} bytes)')
print(f'  sha256={hashlib.sha256(data).hexdigest()}')

# also keep the build result json for provenance
for extra in ('build-result.json', 'kernel/kernel.json'):
    if extra in zf.namelist():
        (DEST / pathlib.Path(extra).name).write_bytes(zf.read(extra))
        print(f'  + {pathlib.Path(extra).name}')

# confirm the probe symbol made it into the image
if b'tb710fu_dyld_probe' in data:
    print('  probe symbol present in kernel: YES')
else:
    print('  probe symbol present in kernel: NO')
if b'TB710FU DYLDPROBE' in data:
    print('  DYLDPROBE print string present: YES')
else:
    print('  DYLDPROBE print string present: NO')
