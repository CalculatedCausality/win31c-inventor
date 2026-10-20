# win31c - CAD for Windows 3.1

An Inventor-style CAD application written in C for **Windows 3.1 (Win16, 80286)**,
cross-compiled on Linux/WSL with Open Watcom and run in the
[PCjs](https://www.pcjs.org/) IBM PC AT emulator.

![Inventor-style ribbon, model browser, ViewCube and navigation bar](docs/screenshot.png)

## Features

- Inventor-style window chrome at 640x480 in 16 colours (`cad_chrome.c`):
  - Ribbon with a blue **File** button (opens the File menu), tabs *3D Model, Sketch,
    Assemble, Constrain, Manage, View*, captioned panels of 32x32 large and stacked
    16x16 small buttons, hover/pressed/checked/disabled states, Undo/Redo quick access
    and a status-bar prompt for the button under the mouse. Starting a sketch switches
    to the Sketch tab; *Finish Sketch* returns to 3D Model. The classic menu bar stays
    (keyboard access); *View > Ribbon tab* selects tabs from the keyboard
  - Model browser tree: *Part1* root with a collapsed *Origin* folder, features and
    profiles with icons, *End of Part*, then *Constraints*, *Components* and a
    collapsed *Wireframe* folder (vertices/edges). Click +/-, double-click, or
    Right/Left on a folder row expands/collapses it; errors show red with a (!) icon,
    blocked/suppressed features grey
  - Viewport: white-to-grey-blue banded background, ViewCube with orbit ring, Home
    button and a navigation bar (Pan, Zoom, Orbit as left-drag modes - Esc ends them -
    and Zoom All); vertex grips pre-highlight under the mouse
  - Status bar: prompt/help at the left, document counts at the right
  - Rendering on a 286: the scene is rendered into a cached viewport bitmap only
    when geometry, camera, view mode, selection or the document change; menus,
    dialogs and other exposures just blit it (direct rendering if GDI memory is
    short). Hover feedback (ribbon, ViewCube, Home, navigation bar, grips, status
    help) repaints only the affected pixels and never renders the scene
- Lines, XY rectangles, boxes and polygon extrusions with stable entity IDs
- Vertex grips (drag, snap, numeric properties), transactional undo/redo
- Versioned, CRC-checked `.CAD` documents with recoverable saves
- DXF (R12) and OBJ wireframe export; third-angle orthographic drawing sheet
- Demos: House, and a shaded/wireframe Boeing 747-400
- Parametric part modeling (Inventor-style feature tree):
  - **Sketch** tab / **Part** menu: *Start 2D Sketch* on XY, XZ or YZ with an offset
    expression (enters sketch mode, turns the view to the plane, dims other geometry),
    *Rectangle*, *Circle* (8..32 sides) and *Polygon* (`x,y;x,y;...`) profiles,
    *Finish Sketch*
  - **3D Model** tab: *Extrude* (sketch, profile ALL or number, distance, normal /
    reverse / symmetric) and *Revolve* (axis u or v, 12..24 segments)
  - Every dimension is an expression over Parameters (fx), e.g. `W/2 + 10`; features
    regenerate after each feature edit, parameter change, unit change, undo and load,
    so changing `W` resizes every extrusion that uses it
  - Browser group **Part** at the top: features and profiles with health markers
    `(!) message`, `(blocked)`, `(suppressed)`. Double-click/Enter edits (prefilled),
    Del removes (refused with an explanation while another feature depends on it);
    Part menu: Rename, Suppress/Unsuppress, Move Up/Down, Delete
  - An edit that would introduce a new error is rejected and the dialog stays open
  - Wireframe draws sketch curves blue and body edges black; *Shaded* fills feature
    bodies with the same orthographic renderer as the 747; Zoom all, DXF/OBJ export
    and the drawing sheet include body edges (within the 256/512 export limits; an
    omission count is reported)
  - Saved as a `[FEATURES]` section of the `.CAD` file (atomic load); feature edits
    mark the document dirty but are not model undo steps — *Part > Undo Last Feature
    Edit* reverts up to 4 of them

Limits: 256 vertices, 512 edges, integer coordinates ±400. Features: 24 features,
32 profiles (8 per sketch), regenerated output 1024 segments / 256 mesh vertices /
512 faces; the whole document extension (parameters, constraints, components,
features) must fit 16 KiB. Bodies are independent: no booleans, holes, fillets or
hidden-line removal; there is no solid kernel.

## Build

```sh
# One-off: install the pinned toolchain (see toolchain.lock.json) and mtools
mkdir -p ~/opt/openwatcom
curl -fL https://github.com/open-watcom/open-watcom-v2/releases/download/2026-10-01-Build/ow-snapshot.tar.xz \
  | tar -xJ -C ~/opt/openwatcom
sudo apt-get install mtools

sh build.sh cad          # -> out/cad.exe and out/disk.img (1.2 MB floppy)
```

`WATCOM=/path/to/openwatcom` overrides the toolchain location.

## Run and debug

`launch-debug.cmd` (Windows) or `./launch-debug.sh` (WSL) opens a local PCjs machine
in an app window. Wait for Program Manager, then **Mount latest A:** and **Launch CAD**.
**Capture mouse** gives the guest relative mouse input; Esc releases it.

You can also mount `out/disk.img` as drive A: on the public PCjs Windows 3.10 page
and run `A:\CAD.EXE` from Program Manager's File > Run.

## Tests

```sh
sh tests/run.sh                                   # host unit tests (C89, sanitizers)
sh tests/win16-check.sh                           # Watcom Win16 compile checks
python3 debug/test_generic_ui.py --windows --hold # visible end-to-end test in Edge
python3 debug/test_generic_ui.py                  # same test, headless (server: ./launch-debug.sh --server-only)
```

The headless run also saves every ribbon tab as `out/ui-tests/chrome-*.png`.

The end-to-end test drives the real Windows 3.1 guest, reading results from OCR and
a checksummed pixel strip that CAD.EXE draws in `/UITEST` mode.

## Layout

| Path | Purpose |
|---|---|
| `cad.c`, `cad.rc`, `cad_ui_ids.h` | Win16 front end |
| `cad_*.c/h` | Portable C89 modules (model, documents, export, drawing, sketch, params, constraints, assembly, features, aircraft, mesh view, ViewCube) |
| `cad_featureui.c/h` | Feature-tree glue for CAD.EXE: storage, dialogs, browser rows, persistence |
| `cad_chrome.c/h` | Win16 window chrome for CAD.EXE: ribbon, browser tree rows, navigation bar, background, status bar |
| `res/icons/`, `tools/make_icons.py` | 16-colour ribbon/browser bitmaps (generated) |
| `debug/` | Local PCjs harness, server and browser tests |
| `tests/` | Host tests and Win16 compile checks |
| `house.c`, `stars.c`, `wallgen.c` | Earlier experiments |

PCjs is © Jeff Parsons (MIT). Windows 3.1 disk images are fetched at runtime and are
not part of this repository.
