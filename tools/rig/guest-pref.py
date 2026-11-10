#!/usr/bin/env python3
"""Merge preference values into a running guest's Data, as build-droidvm-data-seed.py's PREFS put them into a new seed (a new seed
is a new Data UUID: an existing VM takes the values here instead).

  guest-pref.py USER DOMAIN KEY=VALUE... [--restart PROCESS]

USER is mobile or root (/private/var/USER/Library/Preferences/DOMAIN.plist); VALUE is true, false, an integer or a string. The file
is read through export.py, merged, staged under /private/var/ngpu and moved over the original as its user (mode 0600), then flushed.
--restart kills both cfprefsd and PROCESS by PID (launchd starts them again), so the process reads the new values; killing
backboardd restarts SpringBoard and every app with it. Example (the seed's display orientation, `BKForceMirroredOrientation`):
  guest-pref.py mobile com.apple.backboardd BKForceMirroredOrientation=portrait --restart backboardd"""
import plistlib
import subprocess
import sys
import tempfile
from pathlib import Path

RIG = Path(__file__).resolve().parent
UIDS = {'mobile': 501, 'root': 0}
TOYBOX = '/private/var/ngpu/bin/toybox'
STAGED = '/private/var/ngpu/guest-pref.plist'


def value_of(text):
    if text in ('true', 'false'):
        return text == 'true'
    try:
        return int(text, 0)
    except ValueError:
        return text


def tool(*args, timeout=300):
    return subprocess.run([str(a) for a in args], cwd=RIG, capture_output=True, text=True, timeout=timeout)


def pids(name):
    out = tool('python3', 'export.py', 'ps').stdout.split('\n')
    return [int(f[0]) for f in (line.split() for line in out) if len(f) >= 3 and f[2] == name]


def main():
    argv = sys.argv[1:]
    restart = None
    if '--restart' in argv:
        i = argv.index('--restart')
        restart = argv[i + 1] if i + 1 < len(argv) else sys.exit(__doc__)
        del argv[i:i + 2]
    if len(argv) < 3 or argv[0] not in UIDS:
        sys.exit(__doc__)
    user, domain, assignments = argv[0], argv[1], argv[2:]
    values = {}
    for assignment in assignments:
        key, _, text = assignment.partition('=')
        if not key or not text:
            sys.exit(f'{assignment}: KEY=VALUE')
        values[key] = value_of(text)
    path = f'/private/var/{user}/Library/Preferences/{domain}.plist'
    with tempfile.TemporaryDirectory() as td:
        local = Path(td) / 'prefs.plist'
        got = tool('python3', 'export.py', 'get', path, local)
        if local.exists() and local.stat().st_size:
            data = plistlib.loads(local.read_bytes())
        elif 'error 2' in got.stdout + got.stderr:
            data = {}
        else:
            sys.exit(f'{path}: {got.stdout.strip()} {got.stderr.strip()}')
        before = {k: data.get(k) for k in values}
        data.update(values)
        local.write_bytes(plistlib.dumps(data, fmt=plistlib.FMT_BINARY))
        put = tool('python3', 'export.py', 'put', local, STAGED, '0600')
        if put.returncode:
            sys.exit(f'put: {put.stdout.strip()} {put.stderr.strip()}')
    uid = UIDS[user]
    script = (f'{TOYBOX} mkdir -p {Path(path).parent} && {TOYBOX} mv -f {STAGED} {path} && {TOYBOX} chown {uid}:{uid} {path} && '
              f'{TOYBOX} chmod 600 {path} && /private/var/ngpu/bin/gfxprobe sync {path}')
    done = tool('./guest-run.sh', '-t', '90', '/private/var/ngpu/bin/zsh', '-c', script, timeout=200)
    print(path, 'was', before, 'now', values, '|', done.stdout.strip().splitlines()[-1:] or done.stderr.strip())
    if restart:
        for name in ('cfprefsd', restart):
            for pid in pids(name):
                print(name, tool('python3', 'export.py', 'kill', pid).stdout.strip())


if __name__ == '__main__':
    main()
