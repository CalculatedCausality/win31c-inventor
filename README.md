# win31c — CAD for Windows 3.1

An Inventor-style CAD application written in C for **Windows 3.1 (Win16, 80286)**,
cross-compiled on Linux/WSL with Open Watcom and run in the
[PCjs](https://www.pcjs.org/) IBM PC AT emulator.

![Boeing 747 demo with ViewCube](docs/screenshot.png)

## Features

- Ribbon (3D Model / Sketch / View), model browser and ViewCube with orbit ring
- Lines, XY rectangles, boxes and polygon extrusions with stable entity IDs
- Vertex grips (drag, snap, numeric properties), transactional undo/redo
- Versioned, CRC-checked `.CAD` documents with recoverable saves
- DXF (R12) and OBJ wireframe export; third-angle orthographic drawing sheet
- Demos: House, and a shaded/wireframe Boeing 747-400

Limits: 256 vertices, 512 edges, integer coordinates ±400. Geometry is a wireframe
graph plus demo meshes; there is no solid kernel or parametric feature tree yet.

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
```

The end-to-end test drives the real Windows 3.1 guest, reading results from OCR and
a checksummed pixel strip that CAD.EXE draws in `/UITEST` mode.

## Layout

| Path | Purpose |
|---|---|
| `cad.c`, `cad.rc`, `cad_ui_ids.h` | Win16 front end |
| `cad_*.c/h` | Portable C89 modules (model, documents, export, drawing, sketch, params, constraints, assembly, aircraft, mesh view, ViewCube) |
| `debug/` | Local PCjs harness, server and browser tests |
| `tests/` | Host tests and Win16 compile checks |
| `house.c`, `stars.c`, `wallgen.c` | Earlier experiments |

PCjs is © Jeff Parsons (MIT). Windows 3.1 disk images are fetched at runtime and are
not part of this repository.
