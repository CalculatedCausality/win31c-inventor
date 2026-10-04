#!/bin/sh
# One entry point for core, harness, and real guest UI regression checks.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root"
sh build.sh cad
sh tests/run.sh
sh tests/win16-check.sh
bash launch-debug.sh --server-only
python3 -m unittest discover -s debug -p 'test_*.py'
python3 debug/test_mouse.py
python3 -u debug/test_generic_ui.py "$@"
