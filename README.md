# DWG/DXF viewer prototype

This is a minimal, working proof of the split described in the planning
conversation: **libdxfrw does the parsing, a from-scratch ~150-line class
does the rendering — no LibreCAD `RS_Graphic`/`RS_Entity`/`RS_GraphicView`
code is used at all.**

It builds cleanly with CMake + Qt6, parses `.dxf` (and, via the same code
path, `.dwg` — untested here since I had no DWG sample file handy, but
`dwgRW::read()` implements the same `DRW_Interface` callback contract as
`dxfRW::read()`), and paints lines, circles, arcs, and polylines with pan
and zoom.

## What's in here

```
CMakeLists.txt          top-level build: builds libdxfrw as a plain static
                         lib, then links a Qt6 executable against it
third_party/libdxfrw/    pristine, unmodified vendored copy of LibreCAD's
                         in-tree libdxfrw fork
src/
  dwg_document.h/.cpp    implements DRW_Interface directly; collects
                         parsed geometry into plain Shape structs
  viewer_widget.h/.cpp   QWidget subclass: paints Shapes with QPainter,
                         handles wheel-zoom (around cursor) and drag-pan
  main.cpp               loads a file from argv[1] and shows a window;
                         `--png out.png WxH` renders offscreen instead,
                         for headless testing / CI
sample_data/basic.dxf    hand-written 4-entity DXF used to verify the
                         pipeline (line, circle, arc, closed polyline)
```

## Building

```
sudo apt install cmake qt6-base-dev libboost-dev g++
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Running

```
./dwgviewer path/to/file.dxf          # opens a window
./dwgviewer path/to/file.dwg          # same code path, DWG instead
./dwgviewer file.dxf --png out.png 800x600   # headless, writes a PNG
```

Left-drag to pan, mouse wheel to zoom (around the cursor).

## What this validates

- `libdxfrw` really is decoupled from the rest of LibreCAD: it's built
  here from its own source manifest (`libdxfrw_sources.cmake`) with zero
  Qt or LibreCAD dependencies, only `<cstdint>`/STL.
- **Its own standalone `CMakeLists.txt` doesn't currently build as-is**
  when just `add_subdirectory()`'d: it references
  `cmake/libdxfrwConfig.cmake.in`, which doesn't exist in the vendored
  copy, and its `target_include_directories()` only exposes an
  `INSTALL_INTERFACE` include path (no `BUILD_INTERFACE`), so it's not
  actually usable as an in-tree subdirectory dependency without either
  installing it first or writing your own minimal wrapper, which is what
  this project's top-level `CMakeLists.txt` does instead, via
  `libdxfrw_sources.cmake`. Worth flagging upstream if you end up
  depending on this long-term.
- A viewer really can be built without touching `RS_Graphic`,
  `RS_Document`, `RS_GraphicView`, or any of `lib/actions`/`ui`/`actions`.
  The `DRW_Interface` callback contract is a clean enough boundary that a
  fresh, small implementation is genuinely less code and less risk than
  forking and stripping the existing entity/document/render classes.

## What's deliberately not here yet

- Only 4 entity types are handled (`LINE`, `CIRCLE`, `ARC`,
  `LWPOLYLINE`/`POLYLINE`). Real drawings will also have `TEXT`/`MTEXT`,
  `INSERT` (block references — these need recursive resolution against
  `addBlock`/`setBlock`/`endBlock`, which are currently no-ops here),
  `SPLINE`, `HATCH`, dimensions, etc. Extending `DwgDocument` is
  additive: one more `override` + one more `case` in the paint switch
  per entity type.
- No layer visibility/color handling (everything paints black on white
  regardless of DXF layer/color codes — `DRW_Entity` carries a `color`
  and `layer` field on every entity, so this is a small addition to
  `Shape`).
- Not tested against a real-world DWG file yet — only the hand-written
  sample DXF. Worth throwing a handful of real DWG exports at it early,
  since DWG version/feature coverage is where parsers usually surprise
  you.
- No file-open dialog, drag-and-drop, or multi-document support — it's
  a single-file CLI-launched prototype on purpose, to keep the "does the
  split work" question separate from "is this a real app yet".

## License

`libdxfrw` and LibreCAD are both GPLv2 (some files "or later"). Anything
built on top of the vendored `libdxfrw` copy here inherits that — this
prototype, and any real application built from it, needs to stay
GPL-licensed with source available if distributed. Not legal advice;
worth a real read of `third_party/libdxfrw/COPYING` if you're shipping
this.
