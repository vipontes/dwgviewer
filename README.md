# DWG/DXF Viewer

A lightweight DWG/DXF viewer built on top of [`libdxfrw`](third_party/libdxfrw)
(vendored from LibreCAD) with a small, purpose-built Qt6 rendering layer.
Rather than forking LibreCAD's own `RS_Graphic`/`RS_GraphicView`/entity
classes, this project implements `libdxfrw`'s `DRW_Interface` callback
contract directly — see `CLAUDE.md` for the full architectural rationale.

```
file.dxf/.dwg -> libdxfrw (third_party/libdxfrw) -> DwgDocument -> ViewerWidget -> QPainter -> screen
```

## Features

- Parses both `.dxf` and `.dwg` via the same `DRW_Interface` code path.
- Entity support: `LINE`, `CIRCLE`, `ARC`, `LWPOLYLINE`/`POLYLINE` (including
  bulge-based arc segments), `TEXT`, `MTEXT`, `INSERT` (block references,
  recursively resolved, including `ATTRIB`/`ATTDEF` attribute text),
  `HATCH`/`MPOLYGON` (solid, gradient, and pattern fills), and `DIMENSION`
  (`DIMLINEAR`, `DIMALIGNED`, `DIMRADIAL`, `DIMDIAMETRIC`, `DIMANGULAR`,
  `DIMANGULAR3P` — geometry synthesized from each entity's own definition
  points, not from pre-rendered AutoCAD block graphics).
- Layer and color resolution, including `BYLAYER`.
- Linetype/dash pattern resolution from the file's `LTYPE` table, honoring
  `$LTSCALE` and per-entity linetype scale.
- Dimension style resolution (`DIMSTYLE` table, header variables, and
  per-entity XDATA overrides) for arrow size, extension line offset/extend,
  and text height.
- Interactive pan (left-drag) and zoom (mouse wheel, centered on the
  cursor).
- Headless PNG rendering (`--png`) for scripted/CI verification without a
  display.

See `CLAUDE.md` for the complete list of known gaps (e.g. `SPLINE`,
non-uniform block scale, XREF resolution, line weight).

## What's in here

```
CMakeLists.txt          top-level build: builds libdxfrw as a plain static
                         lib, then links a Qt6 executable against it
third_party/libdxfrw/    vendored copy of LibreCAD's in-tree libdxfrw fork
src/
  dwg_document.h/.cpp    implements DRW_Interface directly; collects
                         parsed geometry into plain Shape structs
  viewer_widget.h/.cpp   QWidget subclass: paints Shapes with QPainter,
                         handles wheel-zoom (around cursor) and drag-pan
  main.cpp               loads a file from argv[1] and shows a window;
                         `--png out.png WxH` renders offscreen instead,
                         for headless testing / CI
sample_data/basic.dxf    small hand-written DXF used to sanity-check the
                         pipeline (line, circle, arc, closed polyline)
```

## Building

### Linux

Install the toolchain, then configure and build:

```bash
sudo apt install cmake qt6-base-dev g++
mkdir build && cd build
cmake ..
make -j$(nproc)
```

The executable is `build/dwgviewer`.

### Windows

**Prerequisites:**

- **Visual Studio 2022** (Community or higher) with the "Desktop development
  with C++" workload, for the MSVC toolchain.
- **Qt 6**, MSVC build — install via the
  [Qt Online Installer](https://www.qt.io/download-qt-installer) and select
  an `msvc2022_64` kit (not `mingw_64` — it must match VS's ABI). This
  project has been built and tested against Qt 6.11.2.
- **CMake** (3.16+) — bundled with the Qt Online Installer's "Developer and
  Designer Tools", or install standalone.

**Configure and build**, from a regular PowerShell/Command Prompt (adjust
the Qt path/version to match your install):

```powershell
mkdir build
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.11.2/msvc2022_64"
cmake --build build --config Release
```

This generates `build\dwgviewer_prototype.sln`, which can also be opened
directly in Visual Studio for building/debugging instead of using
`cmake --build`.

The executable is `build\Release\dwgviewer.exe` (or `build\Debug\...` for a
Debug config).

Running it outside Visual Studio requires Qt's DLLs to be reachable — either
add `C:\Qt\6.11.2\msvc2022_64\bin` to `PATH`, or copy the needed DLLs
alongside the exe with:

```powershell
C:\Qt\6.11.2\msvc2022_64\bin\windeployqt.exe build\Release\dwgviewer.exe
```

## Running

```bash
./dwgviewer path/to/file.dxf                 # opens a window
./dwgviewer path/to/file.dwg                 # same code path, DWG instead
./dwgviewer file.dxf --png out.png 800x600   # headless, writes a PNG
```

On Windows, run `.\dwgviewer.exe` (from `build\Release\`) with the same
arguments.

Left-drag to pan, mouse wheel to zoom (around the cursor).

## `third_party/libdxfrw`

Vendored, unmodified copy of LibreCAD's in-tree `libdxfrw` fork — not the
standalone `LibreCAD/libdxfrw` repository, which has diverged and has
weaker DWG support. It's built here from its own source manifest
(`libdxfrw_sources.cmake`) rather than via its own `CMakeLists.txt`, which
doesn't build standalone as-is (see `CLAUDE.md` for details). Treat this
directory as read-only; if a real bug fix is needed there, it should be
upstreamed to LibreCAD rather than patched silently.

## License

`libdxfrw` and LibreCAD are both GPLv2 (some files "or later"). Anything
built on top of the vendored `libdxfrw` copy here inherits that — this
project needs to stay GPL-licensed with source available if distributed.
Not legal advice; see `third_party/libdxfrw/COPYING` before shipping
anything built from this.
