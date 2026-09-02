# dwgviewer

A DWG/DXF viewer built by pairing libdxfrw (vendored from LibreCAD) with a
small, from-scratch Qt viewer — deliberately *not* a fork of LibreCAD's own
`RS_Graphic`/`RS_GraphicView`/entity classes. See `SETUP_GUIDE.md` and
`README.md` for the reasoning; the summary is: `libdxfrw` exposes a clean
`DRW_Interface` callback boundary, and implementing that fresh is less code
and fewer hidden dependencies than forking and stripping LibreCAD's full
application classes.

## Architecture

```
file.dxf/.dwg -> libdxfrw (third_party/libdxfrw) -> DwgDocument (src/dwg_document.*)
              -> ViewerWidget (src/viewer_widget.*) -> QPainter -> screen
```

- `src/dwg_document.h/.cpp` — `DwgDocument` implements libdxfrw's
  `DRW_Interface`. Each `add<Entity>()` callback that carries geometry we
  care about pushes a `Shape` into `shapes_`; everything else is a no-op
  override (see the header — most of `DRW_Interface`'s ~40 pure virtuals
  are irrelevant to a viewer and stay empty on purpose, not by oversight).
- `src/viewer_widget.h/.cpp` — `ViewerWidget : public QWidget` paints
  `Shape`s with `QPainter`, handles wheel-zoom (around cursor) and
  drag-pan. No toolbars, no editing, no undo stack — this is intentionally
  as thin as LibreCAD's own `RS_GraphicView`/`RS_Painter` pairing, just
  reimplemented without the app-window/dialog-factory singleton
  dependencies those classes currently carry.
- `src/main.cpp` — CLI entry point. `--png out.png WxH` renders offscreen
  for headless testing (useful in CI, or anywhere without a display).

## Conventions for adding a new entity type

1. Add an `override` for the relevant `DRW_Interface::add<X>()` callback in
   `dwg_document.h`, implement it in `dwg_document.cpp` — follow the
   existing pattern in `addLine`/`addCircle`/`addArc`/`addLWPolyline`.
2. Extend `ShapeKind` and `Shape` in `dwg_document.h` if the new entity
   needs fields the struct doesn't have yet.
3. Add the matching `case` in `ViewerWidget::paintEvent`'s switch in
   `viewer_widget.cpp`.
4. **Don't use `QPainter::drawArc()` or any Qt method whose angle
   convention isn't plain trigonometry** — under this project's
   Y-flipped `documentToScreen_` transform, Qt's arc angle convention
   silently mirrors which side gets drawn. This bit us once already (see
   the comment in the `Arc` case in `viewer_widget.cpp`). Sample points
   with `cos`/`sin` directly instead, matching how `Arc` is currently
   drawn.
5. `basePoint`/`secPoint`/vertex coordinates from libdxfrw are in drawing
   units, Y-up, matching standard math convention — the same convention
   the existing `Line`/`Circle`/`Polyline` cases already use.
6. Same Y-flip trap applies to anything with inherent orientation, not
   just angle conventions: drawing glyphs (or any other asymmetric mark)
   straight through `documentToScreen_` mirrors them backwards, because
   the transform's Y flip that keeps polygons reading correctly also
   flips letterforms. The `Text` case builds its own local screen-space
   transform per entity instead (translate + `rotate(-angleDeg)` + a
   plain positive scale, composed from scratch rather than through
   `documentToScreen_`) — follow that pattern for any future entity kind
   that carries its own rotation and isn't symmetric under a flip.
7. Dashed lines (`Shape::dashPattern`, resolved in
   `DwgDocument::resolveEntityLineType`) are **not** drawn via
   `QPen::setDashPattern()`. That API's dash lengths are in units of the
   pen's width, which has no well-defined meaning for the cosmetic
   (always-0-width) pens this viewer uses everywhere. Instead
   `viewer_widget.cpp`'s `drawStroke()` walks the shape's own sampled point
   list in document space and manually emits moveTo/lineTo pairs for each
   "on" (dash) interval — same reasoning as point #6: do the geometry
   ourselves in document space and let `documentToScreen_` handle the
   screen mapping, rather than trust a Qt API whose units/convention don't
   line up with this project's rendering setup.

## third_party/libdxfrw — vendored, treat as read-only

This is a copy of LibreCAD's in-tree `libraries/libdxfrw` fork, not the
standalone `LibreCAD/libdxfrw` repo (that one has diverged and its DWG
support is behind this fork's — see `SETUP_GUIDE.md`). Don't modify files
under here as part of a feature change; if a real bug fix is needed here,
flag it explicitly and ideally upstream it to LibreCAD rather than
patching silently, so re-vendoring later doesn't silently lose the fix.

Its own top-level `CMakeLists.txt` doesn't build standalone as-is (missing
`cmake/libdxfrwConfig.cmake.in`, no `BUILD_INTERFACE` include path) — the
project's root `CMakeLists.txt` deliberately bypasses it and builds
straight from `third_party/libdxfrw/libdxfrw_sources.cmake` instead. Don't
"fix" this by switching to `add_subdirectory(third_party/libdxfrw)` without
checking that broken path first.

## Build & run

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
./dwgviewer ../sample_data/basic.dxf
./dwgviewer path/to/file.dxf --png out.png 800x600   # headless
```

## Not implemented yet (known gaps, not bugs)

- Entity types beyond `LINE`/`CIRCLE`/`ARC`/`LWPOLYLINE`/`POLYLINE`/`TEXT`/
  `MTEXT`/`INSERT` — notably `SPLINE`, `HATCH`, dimensions.
- `INSERT`/block handling (`DwgDocument::addBlock`/`endBlock`/`addInsert`/
  `resolveInserts` in `dwg_document.cpp`) doesn't resolve XREF blocks
  (external file references — `DRW_Block::xrefPath` non-empty) since that
  needs loading another file; an XREF INSERT silently draws nothing rather
  than erroring. Non-uniform block scale (`xscale != yscale`) is also only
  approximated for circles/arcs/text (see `effectiveScale`/
  `effectiveRotation` in `dwg_document.cpp`) since `Shape` has no ellipse
  representation.
- ATTRIB/ATTDEF (block attribute text) render for the common cases: a
  per-instance ATTRIB attached to an INSERT, and a CONSTANT ATTDEF (no
  per-instance value ever exists for those). A non-constant ATTDEF with no
  matching ATTRIB on its INSERT (a malformed/hand-edited file) renders
  nothing rather than falling back to the template's default text — see
  `DwgDocument::addAttDef` in `dwg_document.cpp`.
- Line WEIGHT (thickness, DXF code 370) isn't modeled — every entity still
  strokes as a cosmetic 1-device-pixel line regardless of its lineweight.
  Color (`resolveEntityColor`) and linetype/dash pattern
  (`resolveEntityLineType`, `dwg_document.cpp`) are both implemented,
  including BYLAYER, `$LTSCALE`, and per-entity linetype scale (code 48).
  Complex linetypes with embedded text/shapes (DXF code 74 flags) render as
  their plain dash/dot/gap skeleton (code 49 only) — the shape/text portion
  is silently dropped rather than approximated.
- `TEXT`/`MTEXT` use the system default Qt font at the entity's DXF height,
  not the file's actual `STYLE` table font/width-factor/oblique — style
  lookup would need `addTextStyle` wired up the same way `addLayer` now is.
- No file-open dialog / drag-and-drop — single file via argv on purpose,
  to keep "does the architecture work" separate from "is this a full app".

## License

GPLv2 (libdxfrw and LibreCAD upstream are GPLv2, some files "or later").
This project inherits that. Not legal advice — see
`third_party/libdxfrw/COPYING` before distributing anything built from
this.
