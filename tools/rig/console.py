#!/usr/bin/env python3
"""Print a DroidVM VM's serial console history (the daemon returns it URL-encoded per port)."""
import json, subprocess, sys, urllib.parse
port, token, vm = sys.argv[1:4]
out = subprocess.run(['python3', __import__('os').environ['DVMIPC'], port, token,
                      'vm_console_history', json.dumps({'vm_id': vm})], capture_output=True, text=True, timeout=60).stdout
data = json.loads(out[out.index('\n{') + 1:])
for key in sys.argv[4:] or ['serial1']:
    text = urllib.parse.unquote(data.get(key, ''))
    print(f'===== {key} ({len(text)} chars)')
    print(text.replace('\r', ''))
