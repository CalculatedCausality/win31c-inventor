#!/bin/sh
# Cross-compile Win16 programs with Open Watcom and pack them onto a
# 1.2MB floppy image (the PCjs Windows 3.10 machine has a 1.2MB drive A:).
#
# Usage: build.sh [program ...]     (default: cad)
# Each program needs <name>.c and <name>.lnk; the image is out/disk.img.
set -e

: "${WATCOM:=$HOME/opt/openwatcom}"
export WATCOM
export PATH="$WATCOM/binl64:$PATH"
export INCLUDE="$WATCOM/h:$WATCOM/h/win"

cd "$(dirname "$0")"
mkdir -p out

CFLAGS="-q -bt=windows -zW -ms -2 -ox -w4 -fr"
[ "$#" -gt 0 ] || set -- cad
[ -x "$WATCOM/binl64/wcc" ] && [ -x "$WATCOM/binl64/wlink" ] || {
    printf '%s\n' "Open Watcom missing at $WATCOM" >&2
    exit 1
}
image="out/disk-$$.tmp"
trap 'rm -f "$image"' EXIT HUP INT TERM
mformat -C -f 1200 -N 03100002 -v WIN31CAD -i "$image" ::

for p do
    case "$p" in
        ''|*[!a-z0-9_]*) printf '%s\n' 'Invalid program name' >&2; exit 1 ;;
    esac
    ext=exe
    [ "$p" != stars ] || ext=scr
    rm -f "out/$p.$ext"
    if [ "$p" = cad ]; then
        # The generic document and staging objects cannot share a 64KiB DGROUP.
        # All callers/dependencies use the same segmented large-model ABI.
        cadflags="-q -bt=windows -zW -ml -zt=1 -2 -ox -w4 -fr"
        "$WATCOM/binl64/wcc" $cadflags -we -fo=out/cad.obj cad.c
        for module in model create document sketch export drawing aircraft meshview viewcube \
                params userparams constraints vconstraints script assembly parts features featureui; do
            "$WATCOM/binl64/wcc" $cadflags "-fo=out/cad_$module.obj" "cad_$module.c"
        done
        # Window chrome (ribbon, browser tree, navigation bar): Win16 GDI only.
        "$WATCOM/binl64/wcc" $cadflags -we -fo=out/cad_chrome.obj cad_chrome.c
        "$WATCOM/binl64/wrc" -q -bt=windows -31 -r -fo=out/cad.res cad.rc
    else
        "$WATCOM/binl64/wcc" $CFLAGS "-fo=out/$p.obj" "$p.c"
    fi
    if [ "$p" = house ]; then
        "$WATCOM/binl64/wcc" $CFLAGS -fo=out/persist.obj persist.c
        "$WATCOM/binl64/wcc" $CFLAGS -fo=out/cad_export.obj cad_export.c
        "$WATCOM/binl64/wcc" $CFLAGS -fo=out/cad_drawing.obj cad_drawing.c
        "$WATCOM/binl64/wcc" $CFLAGS -fo=out/cad_macro.obj cad_macro.c
    fi
    "$WATCOM/binl64/wlink" "@$p.lnk"
    test -s "out/$p.$ext"
    mcopy -i "$image" "out/$p.$ext" ::
    if [ "$p" = house ]; then
        mcopy -o -i "$image" examples/RAISE.CMD ::
    fi
    if [ "$p" = cad ]; then
        # Macros for Tools > Run Macro (CADSCRIPT; RAISE.CMD is HOUSE's HCMD).
        for macro in examples/*.CMD; do
            mcopy -o -i "$image" "$macro" ::
        done
    fi
done
mv "$image" out/disk.img
mdir -i out/disk.img ::
