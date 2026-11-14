"""Local GitHub API and push helper. Never stores or prints the token."""
import argparse
import base64
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.error
import urllib.request


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
        with urllib.request.urlopen(request, timeout=45) as response:
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--env-file')
    sub = parser.add_subparsers(dest='command', required=True)
    call = sub.add_parser('api')
    call.add_argument('method', choices=['GET', 'POST', 'PUT', 'PATCH'])
    call.add_argument('endpoint')
    call.add_argument('--body-file')
    push = sub.add_parser('push')
    push.add_argument('--branch', default='main')
    args = parser.parse_args()
    token = credential(args.env_file)
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
    subprocess.run([git, '-C', str(root), 'push', '-u', 'origin', args.branch], env=env, check=True)


if __name__ == '__main__':
    main()
