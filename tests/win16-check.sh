#!/bin/sh
# Compile every core for the actual segmented Win16 target, without pretending
# all of them are linked into the current HOUSE frontend.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
: "${WATCOM:=$HOME/opt/openwatcom}"
export WATCOM
export INCLUDE="$WATCOM/h:$WATCOM/h/win"
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
for module in model params sketch drawing constraints assembly export macro; do
    "$WATCOM/binl64/wcc" -q -bt=windows -zW -ms -2 -ox -w4 -we -fr \
        "-fo=$scratch/cad_$module.obj" "$root/cad_$module.c"
    printf 'Win16 compile: cad_%s PASS\n' "$module"
done
# Workspace is a segmented-memory handoff: two caller-owned objects exceed
# one DGROUP together. Check it with the large-model ABI required by its API.
"$WATCOM/binl64/wcc" -q -bt=windows -zW -ml -zt=1 -2 -ox -w4 -we -fr \
    "-fo=$scratch/cad_workspace.obj" "$root/cad_workspace.c"
printf 'Win16 compile: cad_workspace (large model) PASS\n'
for module in create document aircraft meshview; do
    "$WATCOM/binl64/wcc" -q -bt=windows -zW -ml -zt=1 -2 -ox -w4 -we -fr \
        "-fo=$scratch/cad_$module.obj" "$root/cad_$module.c"
    printf 'Win16 compile: cad_%s (large model) PASS\n' "$module"
done
