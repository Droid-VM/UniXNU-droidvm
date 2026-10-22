#!/usr/bin/env python3
"""List the guest's crash reports (ReportCrash's .ips files), fetch the new ones and print what each says.

  crashes.py                 the reports not seen before (by name), fetched into $RIG_OUT/crashes/ and summarized
  crashes.py --all           every report there is now
  crashes.py --list          only list the names, fetch nothing
  crashes.py NAME...         only reports whose name contains one of these (e.g. MobileSafari, Preferences, Jetsam)

A crash (bug_type 309): process, exception, termination, and the crashing thread's frames -- ReportCrash symbolizes the
system's own frames on the guest already. A JetsamEvent (298): the largest process and every process jetsam killed, with its
reason. Anything else: its header. Reports are read through the kernel's file mailbox (export.py)."""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rigenv  # noqa: F401
import export

REPORTS = '/private/var/mobile/Library/Logs/CrashReporter'
OUT = os.path.join(os.environ.get('RIG_OUT', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out')), 'crashes')
FRAMES = 12


def listing(directory):
    """Names of the .ips files in the directory and its subdirectories (one level, as ReportCrash writes them)."""
    names = []
    try:
        body, _ = export.request(1, directory)
    except SystemExit:
        return names
    for line in body.decode(errors='replace').splitlines():
        kind, _, name = line.partition(' ')
        if name in ('.', '..'):
            continue
        if kind == '4' and directory == REPORTS:
            names += [f'{name}/{inner}' for inner in listing(f'{directory}/{name}')]
        elif name.endswith('.ips'):
            names.append(name)
    return names


def fetch(name):
    local = os.path.join(OUT, name.replace('/', '_'))
    if not os.path.exists(local):
        data, offset = b'', 0
        while True:
            body, total = export.request(0, f'{REPORTS}/{name}', offset)
            data += body
            offset += len(body)
            if not body or offset >= total:
                break
        open(local, 'wb').write(data)
    return local


def frame(images, entry):
    image = images[entry['imageIndex']] if entry.get('imageIndex', -1) < len(images) else {}
    where = image.get('name', '?')
    if 'symbol' in entry:
        return f"{where}: {entry['symbol']} + {entry.get('symbolLocation', 0)}"
    return f"{where} + {entry.get('imageOffset', 0):#x}"


def summary(name, raw):
    head, _, body = raw.partition('\n')
    try:
        header, report = json.loads(head), json.loads(body)
    except json.JSONDecodeError:
        return [f'== {name}', *raw.splitlines()[:8]]
    lines = [f"== {name}  (bug_type {header.get('bug_type')}, {header.get('timestamp', report.get('captureTime', ''))})"]
    if 'exception' in report:
        exception, termination = report['exception'], report.get('termination', {})
        lines.append(f"{report.get('procName')} pid {report.get('pid')} ({report.get('procPath')})")
        lines.append(f"{exception.get('type')} {exception.get('signal', '')} {exception.get('subtype', '')}".rstrip())
        lines.append(f"termination: {termination.get('indicator', termination.get('code'))} by {termination.get('byProc', '?')}")
        if report.get('asi'):
            lines += [f'  {key}: {" | ".join(value)}' for key, value in report['asi'].items()]
        # An uncaught exception aborts from the terminate handler; where it was thrown is the last exception backtrace.
        if report.get('lastExceptionBacktrace'):
            lines.append('last exception backtrace (where it was thrown):')
            lines += ['  ' + frame(report.get('usedImages', []), entry) for entry in report['lastExceptionBacktrace'][:FRAMES]]
        thread = report['threads'][report.get('faultingThread', 0)]
        lines.append(f"crashed thread {report.get('faultingThread')} {thread.get('name', thread.get('queue', ''))}")
        lines += ['  ' + frame(report.get('usedImages', []), entry) for entry in thread.get('frames', [])[:FRAMES]]
    elif 'memoryStatus' in report:
        pages = report['memoryStatus'].get('memoryPages', {})
        lines.append(f"memory pages: {', '.join(f'{k} {v}' for k, v in pages.items())}; largest process {report.get('largestProcess')}")
        for process in report.get('processes', []):
            if process.get('reason'):
                lines.append(f"  killed {process.get('name')} ({process.get('reason')}), {process.get('rpages', 0)} resident pages")
        top = sorted(report.get('processes', []), key=lambda p: -p.get('rpages', 0))[:5]
        lines.append('  largest: ' + ', '.join(f"{p.get('name')} {p.get('rpages', 0)}" for p in top))
    else:
        lines.append(f"{header.get('name') or header.get('app_name') or ''}: {', '.join(list(report)[:12])}")
    return lines


def main():
    arguments = [a for a in sys.argv[1:] if not a.startswith('--')]
    os.makedirs(OUT, exist_ok=True)
    seen_path = os.path.join(OUT, 'seen')
    seen = set(open(seen_path).read().split()) if os.path.exists(seen_path) else set()
    names = sorted(listing(REPORTS), key=lambda n: n.rsplit('-', 4)[-4:] if n.count('-') >= 4 else [n])
    if arguments:
        names = [n for n in names if any(a in n for a in arguments)]
    elif '--all' not in sys.argv:
        names = [n for n in names if n not in seen]
    if '--list' in sys.argv or not names:
        print('\n'.join(names) if names else 'no new crash reports')
        return
    for name in names:
        print('\n'.join(summary(name, open(fetch(name), errors='replace').read())), end='\n\n')
        seen.add(name)
    open(seen_path, 'w').write('\n'.join(sorted(seen)) + '\n')


if __name__ == '__main__':
    main()
