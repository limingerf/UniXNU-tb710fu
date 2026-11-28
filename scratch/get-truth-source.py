"""
Extract patched-sources/systemcalls.c from a run artifact and save it locally.

This is the file the build actually compiles: the bare feed with every series
patch applied up to (and including) whatever the run had enabled. Regenerating
0053 against THIS file removes the guessing that produced five rejected builds.
"""
import io
import json
import pathlib
import subprocess
import sys
import urllib.error
import urllib.request
import zipfile

REPO = 'limingerf/UniXNU-tb710fu'
RUN = sys.argv[1] if len(sys.argv) > 1 else '46116666719'
DEST = pathlib.Path(r'd:\unixnu-tb710fu-clean\.work\build-truth') / f'systemcalls-{RUN}.c'

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
    with urllib.request.urlopen(loc, timeout=300) as r:
        return r.read()


arts = api(f'/repos/{REPO}/actions/runs/{RUN}/artifacts')['artifacts']
if not arts:
    sys.exit('no artifacts')

blob = download(
    f"https://api.github.com/repos/{REPO}/actions/artifacts/{arts[0]['id']}/zip")
zf = zipfile.ZipFile(io.BytesIO(blob))

member = 'patched-sources/systemcalls.c'
if member not in zf.namelist():
    sys.exit(f'{member} not in artifact; members with "systemcalls": '
             f'{[n for n in zf.namelist() if "systemcalls" in n]}')

data = zf.read(member)
DEST.parent.mkdir(parents=True, exist_ok=True)
DEST.write_bytes(data)
print(f'saved {len(data)} bytes -> {DEST}')

text = data.decode('utf-8', 'replace')
lines = text.splitlines()
print(f'lines: {len(lines)}')
print(f'has 0043 enter hook : {"tb710fu_pid1_sc_enter" in text}')
print(f'has 0043 exit hook  : {"tb710fu_pid1_sc_exit" in text}')
print(f'has our probe       : {"tb710fu_dyld_probe" in text}')
print(f'CRLF present        : {chr(13) in text[:4000]}')

print('\n--- around the decl anchor ---')
for i, line in enumerate(lines):
    if 'mach_trap_count' in line:
        for j in range(max(0, i - 2), min(len(lines), i + 6)):
            print(f'{j+1:5} {lines[j]!r}')
        break

print('\n--- around the dispatch call ---')
for i, line in enumerate(lines):
    if 'callp->sy_call' in line:
        for j in range(max(0, i - 4), min(len(lines), i + 12)):
            print(f'{j+1:5} {lines[j]!r}')
        break
