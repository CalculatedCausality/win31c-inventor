#!/usr/bin/env bash
set -euo pipefail
repo="$(dirname "$(readlink -f "$0")")"
url='http://localhost:8310/'
# Reuse only this repository's server; reject a conflicting port.
state="$(python3 - "$repo" <<'PY'
import json, sys, urllib.request
try:
    with urllib.request.urlopen('http://127.0.0.1:8310/api/health', timeout=2) as r:
        h = json.load(r)
    print('reuse' if h.get('service') == 'win31c-pcjs-debug' and h.get('repo') == sys.argv[1] else 'conflict')
except Exception:
    import socket
    with socket.socket() as s:
        print('conflict' if s.connect_ex(('127.0.0.1', 8310)) == 0 else 'start')
PY
)"
if [[ "$state" == conflict ]]; then
    printf '%s\n' 'Port 8310 belongs to another service. Stop it first.' >&2
    exit 1
fi
if [[ "$state" == start ]]; then
    # Separate, nonblocking server; log and cache remain under debug/.
    nohup python3 -u "$repo/debug/server.py" >"$repo/debug/server.log" 2>&1 </dev/null &
fi
python3 - <<'PY'
import time, urllib.request
for i in range(50):
    try:
        urllib.request.urlopen('http://127.0.0.1:8310/api/health', timeout=1).close()
        break
    except Exception:
        time.sleep(.1)
else:
    raise SystemExit('Server failed to start. See debug/server.log')
PY
if [[ "${1:-}" == --server-only ]]; then exit 0; fi
for browser in microsoft-edge google-chrome chromium chromium-browser; do
    if command -v "$browser" >/dev/null 2>&1; then
        "$browser" --app="$url" >/dev/null 2>&1 &
        exit 0
    fi
done
if command -v powershell.exe >/dev/null 2>&1; then
    powershell.exe -NoProfile -Command "\$paths=@('\$env:ProgramFiles\\Microsoft\\Edge\\Application\\msedge.exe', '\${env:ProgramFiles(x86)}\\Microsoft\\Edge\\Application\\msedge.exe', '\$env:ProgramFiles\\Google\\Chrome\\Application\\chrome.exe'); \$found=\$false; foreach(\$p in \$paths){\$p=\$ExecutionContext.InvokeCommand.ExpandString(\$p); if(Test-Path \$p){Start-Process \$p -ArgumentList '--app=$url'; \$found=\$true; break}}; if(-not \$found){Start-Process '$url'}"
elif command -v xdg-open >/dev/null 2>&1; then
    xdg-open "$url" >/dev/null 2>&1 &
else
    printf 'Open %s in a browser.\n' "$url"
fi
