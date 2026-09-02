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
  `MTEXT`/`INSERT`/`HATCH`/`DIMENSION` — notably `SPLINE`.
- `HATCH`/`MPOLYGON` (`DwgDocument::addHatch` in `dwg_document.cpp`) render
  solid, gradient (2-stop, AutoCAD's "centered" shift ignored) and pattern
  (real dashed/solid parallel lines from the file's own baked-in pattern
  definition, not a `.pat` library lookup) fills, and boundary loops given
  either as a polyline (bulge) list or as LINE/ARC edges. A boundary loop
  built from ELLIPSE or SPLINE edges isn't supported -- the whole hatch is
  silently dropped rather than drawn with a gap where that edge should be
  (see `buildHatchLoop` in `dwg_document.cpp`).
- `DIMENSION` (`DwgDocument::addDim*` in `dwg_document.cpp`) is handled for
  `DIMLINEAR`/`DIMALIGNED`/`DIMRADIAL`/`DIMDIAMETRIC`/`DIMANGULAR`/
  `DIMANGULAR3P` (the six common types). Geometry (extension lines,
  dimension line/arc, arrowheads, text) is synthesized fresh from each
  entity's own definition points -- the same approach LibreCAD's
  `RS_Dimension`-derived classes take -- rather than trusting the anonymous
  block of pre-rendered graphics AutoCAD also writes into the file
  (`DRW_Dimension::getName()`), which this viewer never reads.
  `DIMORDINATE` and arc-length (`DIMARC`) dimensions aren't implemented --
  their leader/jog geometry rules are distinct enough from the other six
  that they're left unimplemented (silently render nothing) rather than
  approximated with the wrong shape.
  - A linear/rotated dimension's line always lies exactly on the line
    through its `dimPoint` (code 10) in the entity's own direction
    (`theta` -- `getAngle()` for `DIMLINEAR`, the def1->def2 direction for
    `DIMALIGNED`): `addLinearStyleDimension` *projects* `def1`/`def2` onto
    that line rather than translating them by a shared perpendicular
    offset. Translating only stays parallel to `theta` when `def1`/`def2`
    are already equidistant from it (always true for Aligned, since
    `theta` is defined from those same two points, but not for a
    Linear/rotated dimension whose two measured points are at different
    heights/offsets -- a very common real case, e.g. a "horizontal"
    dimension between two features that aren't at the same Y). Getting
    this wrong renders a skewed line instead of a true horizontal/vertical
    one.
  - Dimension text is rendered at `uprightTextAngle(theta)`, not literally
    `theta`: a dimension line's direction is only meaningful up to +/- pi
    (the line reads the same regardless of which of its two definition
    points a file stored as "first"), so using the raw angle renders the
    text upside-down (reads as mirrored) whenever a file happens to store
    them back-to-front relative to a left-to-right reading of `theta` --
    which real files routinely do (e.g. this project's own `teste.dxf` has
    several `DIMLINEAR`s at `angle=180`, numerically "backwards" from 0
    despite being ordinary horizontal dimensions).
  - Arrow size/extension-line offset+extend/text height are resolved per
    dimension (`DwgDocument::resolveDimStyle`) with this precedence: (1) a
    per-entity `"ACAD"`/`"DSTYLE"` XDATA override on the dimension itself
    (`findDstyleXdataOverride` walks `DRW_Entity::extData` for the
    1001/1000/1002/1070/1040 group sequence AutoCAD uses to override
    specific `DIMSTYLE` variables -- `dimscale`=40, `dimasz`=41,
    `dimexo`=42, `dimexe`=44, `dimtxt`=140 -- on one dimension without a
    full named style; real files use this routinely -- every dimension
    this project's own bug reports called out by value in `teste.dxf` has
    one); (2) the file's `DIMSTYLE` table (`addDimStyle`, keyed by the
    entity's own style name, code 3); (3) the header's global
    `$DIMASZ`/`$DIMEXO`/`$DIMEXE`/`$DIMTXT`. `$DIMSCALE` (or an XDATA
    override of it) is then applied as a multiplier on top of whichever of
    those is in effect -- it's a document-wide (or per-entity) scale
    knob, not folded into any one source, since a named style's own
    `dimscale` field is routinely left at the unhelpful default 1.0 even
    when the header's (or an XDATA override's) very much isn't (e.g.
    `teste.dxf`'s "ISO-25" style carries no `dimscale` override at all,
    yet the header's `$DIMSCALE` is 12 -- and several individual
    dimensions' XDATA further override that to 100 for just themselves).
  - **Vendored reader gap** (`third_party/libdxfrw`, not patched --
    flagging per this file's policy above): `DRW_Dimstyle::parseDwg`
    (`drw_objects.cpp`) reads a DWG `DIMSTYLE` table entry's *name* only --
    `dimasz`/`dimexo`/`dimexe`/`dimtxt`/`dimscale` are silently left at
    their `reset()` factory defaults (0.18/0.0625/0.18/0.18/1.0) for every
    DWG file. `DRW_Header::parseDwg` (`drw_header.cpp`) does *not* have
    this gap -- it populates `vars["DIMASZ"]`/`"DIMSCALE"`/etc. from the
    DWG header preamble same as DXF, just under the bare (no `$`) key
    `findDoubleHeaderVar` already checks for. `hasReliableDimHeaderVars_`
    (set in `addHeader`, true when either was found) is how
    `resolveDimStyle` tells a genuine DIMSTYLE apart from the table gap's
    factory-default signature -- and, critically, when the table looks
    unparsed *and* the header is reliable, it must actually **switch** to
    the header-derived values, not merely treat the bogus table ones as
    "trustworthy enough to keep": an earlier version of this logic did the
    latter, which rendered every DWG dimension arrow (and any text with no
    XDATA `dimtxt` override) at the 0.18-unit factory-default size instead
    of the drawing's real one, confirmed against this project's own
    `teste.dwg` -- despite the header carrying a perfectly good `$DIMASZ`
    the whole time. When neither the table nor the header is reliable,
    every dimension in the file instead shares one
    `uniformFallbackDimStyle()` (cached from the first dimension's own
    measured length/radius) rather than each guessing its own size from
    its own measurement, which looked wildly inconsistent from one
    dimension to the next. That fallback *is* still a heuristic, not real
    DIMSTYLE data -- upstreaming a real fix to `DRW_Dimstyle::parseDwg`
    (reading those fields from the DWG binary per the ODA spec) would
    remove the need for it entirely.
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
