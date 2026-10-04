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
# --- vconstraints (vertex constraints over cad_constraints + cad_model) ---
cc -std=c89 -pedantic -Wall -Wextra -Werror "$root/cad_vconstraints.c" "$root/cad_constraints.c" "$root/cad_model.c" "$root/tests/vconstraints_test.c" -o "$scratch/vconstraints_test"
# (separate lines: under set -e a failing non-final && operand does not abort)
"$scratch/vconstraints_test"
# --- userparams (Parameters fx table over cad_params) ---
cc -std=c89 -pedantic -Wall -Wextra -Werror "$root/cad_userparams.c" "$root/cad_params.c" "$root/tests/userparams_test.c" -o "$scratch/userparams_test"
"$scratch/userparams_test"
# --- script (generic CAD.EXE macro language over cad_create) ---
cc -std=c89 -pedantic -Wall -Wextra -Werror "$root/cad_script.c" "$root/cad_create.c" "$root/cad_model.c" "$root/cad_sketch.c" "$root/tests/script_test.c" -o "$scratch/script_test"
"$scratch/script_test" "$root/examples"
# --- parts (Place Component: part-file occurrences; writes PT*.CAD in the scratch cwd) ---
cc -std=c89 -pedantic -Wall -Wextra -Werror "$root/cad_parts.c" "$root/cad_assembly.c" "$root/cad_document.c" "$root/cad_model.c" "$root/cad_create.c" "$root/cad_sketch.c" "$root/tests/parts_test.c" -o "$scratch/parts_test"
(cd "$scratch" && "$scratch/parts_test")
# --- features (parametric feature tree over cad_sketch + cad_userparams) ---
cc -std=c89 -pedantic -Wall -Wextra -Werror "$root/cad_features.c" "$root/cad_sketch.c" "$root/cad_userparams.c" "$root/cad_params.c" "$root/tests/features_test.c" -o "$scratch/features_test"
"$scratch/features_test"
# --- featureui (CAD.EXE part-modeling glue; behaviour tested via cad_editor_test.py) ---
cc -std=c89 -pedantic -Wall -Wextra -Werror -c "$root/cad_featureui.c" -o "$scratch/cad_featureui.o"
CAD_EDITOR_SANITIZE=1 python3 "$root/tests/cad_editor_test.py"
