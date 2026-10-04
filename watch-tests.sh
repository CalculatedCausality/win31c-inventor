#!/usr/bin/env bash
set -euo pipefail
repo="$(dirname "$(readlink -f "$0")")"
cd "$repo"
sh build.sh cad
bash launch-debug.sh --server-only
if [[ ! -f out/windows-driver/node_modules/playwright-core/package.json ]]; then
    npm install --prefix out/windows-driver --no-save --ignore-scripts playwright-core
fi
# Launch the actual Windows browser, not WSLg. Use the init interop socket if
# this long-running agent inherited a stale shell-specific socket.
export WSL_INTEROP=/run/WSL/1_interop
profile="$(wslpath -w "$repo/out/windows-edge-tests")"
powershell.exe -NoProfile -NonInteractive -Command \
    "Start-Process 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe' -ArgumentList @('--remote-debugging-port=9223','--user-data-dir=$profile','--no-first-run','--no-default-browser-check','--app=http://localhost:8310/')"
exec python3 -u debug/test_generic_ui.py --windows --hold
