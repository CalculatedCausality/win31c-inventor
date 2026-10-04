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
# --- vconstraints (large model) ---
"$WATCOM/binl64/wcc" -q -bt=windows -zW -ml -zt=1 -2 -ox -w4 -we -fr "-fo=$scratch/cad_vconstraints.obj" "$root/cad_vconstraints.c"
printf 'Win16 compile: cad_vconstraints (large model) PASS\n'
# --- userparams (large model) ---
"$WATCOM/binl64/wcc" -q -bt=windows -zW -ml -zt=1 -2 -ox -w4 -we -fr "-fo=$scratch/cad_userparams.obj" "$root/cad_userparams.c"
printf 'Win16 compile: cad_userparams (large model) PASS\n'
# --- script (large model) ---
"$WATCOM/binl64/wcc" -q -bt=windows -zW -ml -zt=1 -2 -ox -w4 -we -fr "-fo=$scratch/cad_script.obj" "$root/cad_script.c"
printf 'Win16 compile: cad_script (large model) PASS\n'
# --- parts (large model) ---
"$WATCOM/binl64/wcc" -q -bt=windows -zW -ml -zt=1 -2 -ox -w4 -we -fr "-fo=$scratch/cad_parts.obj" "$root/cad_parts.c"
printf 'Win16 compile: cad_parts (large model) PASS\n'
# --- features (large model; module and its test) ---
"$WATCOM/binl64/wcc" -q -bt=windows -zW -ml -zt=1 -2 -ox -w4 -we -fr "-fo=$scratch/cad_features.obj" "$root/cad_features.c"
"$WATCOM/binl64/wcc" -q -bt=windows -zW -ml -zt=1 -2 -ox -w4 -we -fr "-fo=$scratch/features_test.obj" "$root/tests/features_test.c"
printf 'Win16 compile: cad_features + features_test (large model) PASS\n'
# --- featureui (large model, Windows dialogs included via __WINDOWS__) ---
"$WATCOM/binl64/wcc" -q -bt=windows -zW -ml -zt=1 -2 -ox -w4 -we -fr "-fo=$scratch/cad_featureui.obj" "$root/cad_featureui.c"
printf 'Win16 compile: cad_featureui (large model) PASS\n'
# --- chrome (CAD.EXE ribbon/browser/navigation drawing; Win16 GDI only) ---
"$WATCOM/binl64/wcc" -q -bt=windows -zW -ml -zt=1 -2 -ox -w4 -we -fr "-fo=$scratch/cad_chrome.obj" "$root/cad_chrome.c"
printf 'Win16 compile: cad_chrome (large model) PASS\n'
