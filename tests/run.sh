#!/bin/sh
# Host-side regression tests; keep generated files outside the source tree.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
cc -std=c89 -pedantic -Wall -Wextra -Werror \
    -I"$root/tests/win_stub" "$root/tests/persist_test.c" \
    -o "$scratch/persist_test"
cd "$scratch"
./persist_test
python3 "$root/tests/editor_test.py"
python3 "$root/tests/cad_editor_test.py"
for module in model params sketch drawing constraints assembly; do
    cc -std=c89 -pedantic -Wall -Wextra -Werror \
        "$root/cad_$module.c" "$root/tests/${module}_test.c" \
        -o "$scratch/${module}_test"
    "$scratch/${module}_test"
done
cc -std=c89 -pedantic -Wall -Wextra -Werror \
    "$root/cad_workspace.c" "$root/cad_model.c" "$root/cad_params.c" \
    "$root/cad_sketch.c" "$root/cad_export.c" "$root/cad_drawing.c" \
    "$root/tests/workspace_test.c" -o "$scratch/workspace_test"
"$scratch/workspace_test"
cc -std=c89 -pedantic -Wall -Wextra -Werror \
    "$root/cad_create.c" "$root/cad_model.c" "$root/cad_sketch.c" \
    "$root/tests/create_test.c" -o "$scratch/create_test"
"$scratch/create_test"
cc -std=c89 -pedantic -Wall -Wextra -Werror \
    "$root/cad_model.c" "$root/tests/document_test.c" \
    -o "$scratch/document_test"
"$scratch/document_test"
cc -std=c89 -pedantic -Wall -Wextra -Werror \
    "$root/cad_aircraft.c" "$root/cad_model.c" "$root/tests/aircraft_test.c" \
    -o "$scratch/aircraft_test"
"$scratch/aircraft_test"
cc -std=c89 -pedantic -Wall -Wextra -Werror \
    "$root/cad_meshview.c" "$root/tests/meshview_test.c" \
    -o "$scratch/meshview_test"
"$scratch/meshview_test"
# These tests include the implementation to inject deterministic I/O faults.
for module in export macro; do
    cc -std=c89 -pedantic -Wall -Wextra -Werror \
        "$root/tests/${module}_test.c" -o "$scratch/${module}_test"
    "$scratch/${module}_test"
done
