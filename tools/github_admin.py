"""Local GitHub API and push helper. Never stores or prints the token."""
import argparse
import base64
import http.client
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        return None


def credential(env_file):
    token = os.environ.get('GH_TOKEN') or os.environ.get('GITHUB_TOKEN')
    if not token and env_file:
        values = {}
        for line in Path(env_file).read_text(encoding='utf-8-sig').splitlines():
            if '=' in line and not line.lstrip().startswith('#'):
                key, value = line.split('=', 1)
                values[key.strip()] = value.strip().strip("\"'")
        token = values.get('GH_TOKEN') or values.get('GITHUB_TOKEN')
    if not token:
        raise SystemExit('No GitHub credential available')
    return token


def api(token, method, endpoint, body=None):
    request = urllib.request.Request(
        'https://api.github.com/' + endpoint.lstrip('/'),
        method=method,
        headers={
            'Authorization': 'Bearer ' + token,
            'Accept': 'application/vnd.github+json',
            'User-Agent': 'TB710FU-clean-build',
            'X-GitHub-Api-Version': '2022-11-28',
            'Content-Type': 'application/json',
        },
        data=None if body is None else json.dumps(body).encode(),
    )
    try:
        with urllib.request.build_opener(NoRedirect).open(request, timeout=45) as response:
            data = response.read()
            return json.loads(data) if data else {'status': response.status}
    except urllib.error.HTTPError as error:
        try:
            message = json.loads(error.read()).get('message', '')
        except ValueError:
            message = 'GitHub API error'
        raise SystemExit(f'GitHub API HTTP {error.code}: {message}') from None
    except OSError as error:
        raise SystemExit(f'GitHub network request failed: {type(error).__name__}') from None


def download(token, endpoint, destination):
    request = urllib.request.Request(
        'https://api.github.com/' + endpoint.lstrip('/'),
        headers={'Authorization': 'Bearer ' + token, 'User-Agent': 'TB710FU-clean-build'},
    )
    try:
        response = urllib.request.build_opener(NoRedirect).open(request, timeout=45)
    except urllib.error.HTTPError as error:
        if error.code not in (301, 302, 303, 307, 308):
            raise SystemExit(f'GitHub download HTTP {error.code}') from None
        location = error.headers['Location']
        # Signed artifact/log URLs are fetched without the GitHub authorization header.
        destination = Path(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        partial = destination.with_name(destination.name + '.part')
        offset, total = 0, None
        with partial.open('wb') as stream:
            while total is None or offset < total:
                request = urllib.request.Request(location, headers={
                    'Accept-Encoding': 'identity',
                    'Range': f'bytes={offset}-{offset + 262143}',
                })
                for attempt in range(3):
                    try:
                        with urllib.request.urlopen(request, timeout=45) as part:
                            match = re.fullmatch(r'bytes (\d+)-(\d+)/(\d+)', part.headers.get('Content-Range', ''))
                            if match:
                                start, end, expected = map(int, match.groups())
                                if start != offset or (total is not None and expected != total):
                                    raise SystemExit('Artifact range identity changed')
                                total = expected
                            elif offset == 0 and part.status == 200:
                                total = int(part.headers['Content-Length'])
                            else:
                                raise SystemExit('Artifact server did not honor the requested byte range')
                            try:
                                data = part.read()
                            except http.client.IncompleteRead as truncated:
                                data = truncated.partial
                        if not data or len(data) > total - offset:
                            raise OSError('Incomplete artifact response')
                        break
                    except OSError as signed_error:
                        if attempt == 2:
                            raise SystemExit(f'Signed download failed at byte {offset}: {type(signed_error).__name__}') from None
                        time.sleep(1)
                stream.write(data)
                offset += len(data)
        if partial.stat().st_size != total:
            raise SystemExit('Artifact size check failed')
        partial.replace(destination)
        print(f'Downloaded {total} bytes to {destination}')
        return
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with response, destination.open('wb') as stream:
        shutil.copyfileobj(response, stream)
    print(f'Downloaded {destination.stat().st_size} bytes to {destination}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--env-file')
    sub = parser.add_subparsers(dest='command', required=True)
    call = sub.add_parser('api')
    call.add_argument('method', choices=['GET', 'POST', 'PUT', 'PATCH'])
    call.add_argument('endpoint')
    call.add_argument('--body-file')
    fetch = sub.add_parser('download')
    fetch.add_argument('endpoint')
    fetch.add_argument('--output', required=True)
    push = sub.add_parser('push')
    push.add_argument('--branch', default='main')
    args = parser.parse_args()
    token = credential(args.env_file)
    if args.command == 'download':
        download(token, args.endpoint, args.output)
        return
    if args.command == 'api':
        body = json.loads(Path(args.body_file).read_text(encoding='utf-8-sig')) if args.body_file else None
        result = api(token, args.method, args.endpoint, body)
        print(json.dumps(result, indent=2, ensure_ascii=False))
        return
    git = shutil.which('git') or 'C:/Program Files/Git/cmd/git.exe'
    env = dict(os.environ)
    # The credential is process-local, not an argument or a repository config.
    env['GIT_CONFIG_COUNT'] = '1'
    env['GIT_CONFIG_KEY_0'] = 'http.https://github.com/.extraheader'
    encoded = base64.b64encode(('x-access-token:' + token).encode()).decode()
    env['GIT_CONFIG_VALUE_0'] = 'AUTHORIZATION: basic ' + encoded
    env['GIT_TERMINAL_PROMPT'] = '0'
    root = Path(__file__).resolve().parents[1]
    result = subprocess.run([git, '-C', str(root), 'push', '-u', 'origin', args.branch], env=env)
    if result.returncode:
        # An interrupted response can occur after the remote ref has already advanced.
        # Check the exact ref before treating it as a failed push or retrying it.
        local = subprocess.check_output([git, '-C', str(root), 'rev-parse', args.branch], text=True).strip()
        remote_url = subprocess.check_output([git, '-C', str(root), 'remote', 'get-url', 'origin'], text=True).strip()
        prefix = 'https://github.com/'
        if not remote_url.startswith(prefix):
            raise SystemExit(result.returncode)
        repository = remote_url[len(prefix):].removesuffix('.git')
        remote = api(token, 'GET', f'repos/{repository}/git/ref/heads/{args.branch}')
        if remote['object']['sha'] != local:
            raise SystemExit(result.returncode)
        print('Remote revision verified at ' + local)


if __name__ == '__main__':
    main()
