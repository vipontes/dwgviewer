#pragma once

// DwgDocument is a from-scratch, minimal implementation of libdxfrw's
// DRW_Interface callback interface. It does NOT use any of LibreCAD's
// lib/engine classes (RS_Graphic, RS_Entity, ...) — it only needs
// libdxfrw's public headers.
//
// This is the "keep" half of the viewer split: libdxfrw parses the file
// and calls back into whatever object implements DRW_Interface. LibreCAD's
// own callback implementation lives in RS_FilterDXFRW and builds a full
// RS_Graphic (undo stack, layers, blocks, everything). For a viewer-only
// app we don't need any of that — we only need the handful of "add<Shape>"
// callbacks that carry geometry, and we can drop the entities straight
// into simple structs ready for QPainter.

#include <string>
#include <unordered_map>
#include <vector>
#include <limits>

#include "drw_interface.h"

struct Point2D {
    double x = 0.0;
    double y = 0.0;
};

// Plain RGB triple. Kept independent of QColor so the document model (this
// header) has no Qt dependency -- ViewerWidget converts to QColor at paint
// time.
struct RgbColor {
    unsigned char r = 255;
    unsigned char g = 255;
    unsigned char b = 255;
};

enum class ShapeKind {
    Line,
    Circle,
    Arc,
    Polyline,
    Text,
    Hatch
};

// Collapses DRW_Text::HAlign/VAlign (TEXT) and DRW_MText's 1-9 attachment
// point (MTEXT, see DwgDocument::addMText) onto the same simplified model:
// HAligned/HMiddle/HFit (TEXT-only, used for fit-to-width variants) fold
// into Center, since a from-scratch viewer has no reason to reshape glyphs
// to hit an exact width.
enum class TextHAlign { Left, Center, Right };
enum class TextVAlign { Baseline, Bottom, Middle, Top };

// One closed boundary loop of a HATCH/MPOLYGON entity, in the same
// points+bulges representation as Shape::points/bulges (empty bulges means
// all-straight). A loop is always implicitly closed -- a boundary that
// doesn't close back on itself can't bound a fillable area -- so unlike
// Polyline there is no separate `closed` flag.
struct HatchLoop {
    std::vector<Point2D> points;
    std::vector<double> bulges;
};

// One HATCH pattern definition line (DXF codes 53/43-46/79/49), already
// converted to radians. Unlike Shape::dashPattern (built by
// resolveEntityLineType from an LTYPE table entry, which is a *template*
// requiring $LTSCALE/entity-scale to be applied), these lines are written
// into the HATCH entity itself already baked to final world-space
// angle/spacing/dash by whatever authored the file -- so DwgDocument::
// addHatch copies them through as-is, with no further scaling.
struct HatchPatternLine {
    double angleRad = 0.0;
    Point2D basePoint;
    Point2D offset; // displacement between consecutive repeats of this line
    // Raw DXF code-49 values (positive=dash, negative=gap, 0=dot). Empty
    // means a solid (undashed) line. Unlike Shape::dashPattern, this can
    // legitimately start with a gap, so it's kept in its raw signed form
    // rather than resolved to the always-starts-with-dash convention.
    std::vector<double> dashPattern;
};

struct Shape {
    ShapeKind kind;

    // Line: points[0], points[1]
    // Polyline: points[0..N-1], `closed` marks whether to connect the last
    // point back to the first
    std::vector<Point2D> points;

    // Polyline only. Empty, or one entry per point: bulges[i] is the DXF
    // "bulge" (tan(includedAngle/4), signed by sweep direction) of the
    // segment from points[i] to points[i+1] (or back to points[0] when i is
    // the last point and `closed` is set) -- 0 means that segment is a
    // straight line, matching how a polyline with no curved segments at all
    // leaves this empty. See ViewerWidget::paintEvent's bulgeToArc() for the
    // conversion to a drawable arc.
    std::vector<double> bulges;

    // Line/Circle/Arc/Polyline only (never set for Text -- DXF/AutoCAD
    // always render text glyphs solid regardless of the entity's nominal
    // linetype). Empty means solid. Otherwise, alternating dash-length,
    // gap-length, ... in document units, already fully resolved --
    // BYLAYER/BYBLOCK looked up, $LTSCALE and the entity's own linetype
    // scale (DXF code 48) both applied -- so the renderer can walk it
    // directly with no further lookups. See
    // DwgDocument::resolveEntityLineType.
    std::vector<double> dashPattern;

    // Circle / Arc / Text (Text: alignment anchor point)
    Point2D center;
    double radius = 0.0;
    double startAngleRad = 0.0; // Arc only
    double endAngleRad = 0.0;   // Arc only

    bool closed = false; // Polyline only

    // Resolved display color (BYLAYER/BYBLOCK/true-color/ACI already
    // resolved to concrete RGB -- see DwgDocument::resolveEntityColor).
    // Defaults to white, matching this viewer's black background.
    RgbColor color;

    // Text / MText only. May contain embedded '\n' for MTEXT paragraphs.
    std::string text;
    double textHeightDoc = 0.0;
    double textAngleRad = 0.0;
    TextHAlign textHAlign = TextHAlign::Left;
    TextVAlign textVAlign = TextVAlign::Baseline;

    // Hatch only: one or more closed boundary loops (an outer boundary
    // plus any island holes). The viewer fills their union with an
    // even-odd rule so islands read as holes regardless of each loop's own
    // winding direction.
    std::vector<HatchLoop> hatchLoops;

    enum class HatchFillKind { Solid, Gradient, Pattern };
    HatchFillKind hatchFillKind = HatchFillKind::Solid;

    // Hatch/Gradient only. `color` above (already resolved) doubles as the
    // first gradient stop; this is the second stop plus the gradient's
    // axis angle (DXF code 460, already in radians). AutoCAD's gradient
    // "centered" shift (code 461) isn't applied -- this always renders a
    // centered two-stop gradient, which is the common case and a
    // reasonable approximation of a shifted one.
    RgbColor hatchColor2;
    double hatchGradientAngleRad = 0.0;

    // Hatch/Pattern only: the file's own pattern definition lines, already
    // in radians -- see HatchPatternLine.
    std::vector<HatchPatternLine> hatchPatternLines;
};

// 2D affine transform (x' = a*x + c*y + e; y' = b*x + d*y + f), used only to
// place a BLOCK definition's shapes at an INSERT's position/scale/rotation.
// Plain doubles, no QTransform -- this stays in the model layer.
struct Transform2D {
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0, e = 0.0, f = 0.0;
};

// A block reference (INSERT/MINSERT) not yet resolved to placed shapes.
// Resolution is deferred to DwgDocument::resolveInserts() (run once after
// the whole file is parsed) rather than done inline in addInsert(), because
// the referenced block's addBlock()/endBlock() may not have been seen yet
// (block definition order isn't guaranteed) -- and because an INSERT can
// itself appear inside another block's definition (a nested block
// reference), which needs the same deferred/recursive handling.
struct PendingInsert {
    std::string blockName;
    Point2D insertionPoint;
    double xscale = 1.0, yscale = 1.0;
    double angleRad = 0.0;
    int colCount = 1, rowCount = 1;
    double colSpace = 0.0, rowSpace = 0.0;
};

// One BLOCK definition: its own shapes (in block-local coordinates, i.e.
// before the block's basePoint offset is applied) plus any nested INSERTs
// found inside it.
struct BlockDef {
    Point2D basePoint;
    std::vector<Shape> shapes;
    std::vector<PendingInsert> inserts;
};

// Axis-aligned bounding box in drawing units, used by the viewer widget to
// auto-fit the initial zoom/pan.
struct BoundingBox {
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();

    bool isValid() const { return minX <= maxX && minY <= maxY; }
    void expand(double x, double y);
    void expand(const Point2D &p) { expand(p.x, p.y); }
};

class DwgDocument : public DRW_Interface {
public:
    // Loads a .dxf or .dwg file (picked by extension) into this document.
    // Returns false and leaves `errorMessage` set on failure.
    bool loadFile(const std::string &path);

    const std::vector<Shape> &shapes() const { return shapes_; }
    const BoundingBox &boundingBox() const { return bbox_; }
    const std::string &errorMessage() const { return errorMessage_; }

    // --- DRW_Interface: entities we actually care about for a viewer ---
    void addLine(const DRW_Line &data) override;
    void addCircle(const DRW_Circle &data) override;
    void addArc(const DRW_Arc &data) override;
    void addLWPolyline(const DRW_LWPolyline &data) override;
    void addPolyline(const DRW_Polyline &data) override;
    void addLayer(const DRW_Layer &data) override;
    void addText(const DRW_Text &data) override;
    void addMText(const DRW_MText &data) override;
    void addBlock(const DRW_Block &data) override;
    void endBlock() override;
    void addInsert(const DRW_Insert &data) override;
    void addAttDef(const DRW_Attdef &data) override;
    void addHeader(const DRW_Header *data) override;
    void addLType(const DRW_LType &data) override;
    void addHatch(const DRW_Hatch *data) override;
    void addDimAlign(const DRW_DimAligned *data) override;
    void addDimLinear(const DRW_DimLinear *data) override;
    void addDimRadial(const DRW_DimRadial *data) override;
    void addDimDiametric(const DRW_DimDiametric *data) override;
    void addDimAngular(const DRW_DimAngular *data) override;
    void addDimAngular3P(const DRW_DimAngular3p *data) override;
    void addDimStyle(const DRW_Dimstyle &data) override;

    // --- Everything else in DRW_Interface is a no-op for a pure viewer.
    // Header/table/style callbacks are read but not used; the write*()
    // callbacks are only invoked by libdxfrw when writing (we never call
    // write()), so they're dead code paths here, kept only to satisfy the
    // pure-virtual interface.
    void addVport(const DRW_Vport &) override {}
    void addTextStyle(const DRW_Textstyle &) override {}
    void addAppId(const DRW_AppId &) override {}
    void setBlock(int) override {} // never invoked by this vendored reader (see .cpp)
    void addPoint(const DRW_Point &) override {}
    void addRay(const DRW_Ray &) override {}
    void addXline(const DRW_Xline &) override {}
    void addEllipse(const DRW_Ellipse &) override {}
    void addSpline(const DRW_Spline *) override {}
    void addKnot(const DRW_Entity &) override {}
    void addTrace(const DRW_Trace &) override {}
    void add3dFace(const DRW_3Dface &) override {}
    void addSolid(const DRW_Solid &) override {}
    // DIMORDINATE and arc-length (DIMARC) dimensions aren't implemented --
    // their leader/jog geometry rules are distinct enough from the other
    // six types (see CLAUDE.md) that they're left as a documented gap
    // rather than approximated with the wrong shape.
    void addDimOrdinate(const DRW_DimOrdinate *) override {}
    void addDimArc(const DRW_DimArc *) override {}
    void addLeader(const DRW_Leader *) override {}
    void addViewport(const DRW_Viewport &) override {}
    void addImage(const DRW_Image *) override {}
    void linkImage(const DRW_ImageDef *) override {}
    void addComment(const char *) override {}
    void addPlotSettings(const DRW_PlotSettings *) override {}
    void writeHeader(DRW_Header &) override {}
    void writeBlocks() override {}
    void writeBlockRecords() override {}
    void writeEntities() override {}
    void writeLTypes() override {}
    void writeLayers() override {}
    void writeTextstyles() override {}
    void writeVports() override {}
    void writeDimstyles() override {}
    void writeObjects() override {}
    void writeAppId() override {}

private:
    // Adds a shape to whatever the "current" destination is: the active
    // block definition's shape list while inside an addBlock()/endBlock()
    // scope (see addBlock/endBlock), or the top-level document (updating
    // bbox_) otherwise. Block-local shapes are NOT placed/transformed yet,
    // so they must never be counted in the auto-fit bounding box -- only
    // resolveInserts() pushes their transformed copies into shapes_.
    void addShape(Shape shape);

    // Builds a Text-kind Shape from a DRW_Text/DRW_MText/DRW_Attrib without
    // pushing it anywhere (unlike addText/addMText, which call addShape()
    // themselves) -- shared by addText/addMText/addAttDef and by
    // addInsert()'s per-attribute conversion below, none of which have the
    // same "where does this shape go" answer.
    Shape makeTextShape(const DRW_Text &data) const;
    Shape makeMTextShape(const DRW_MText &data) const;
    Shape makeAttribShape(const DRW_Attrib &attrib) const;

    // Resolves an entity's display color the way LibreCAD's rs_filterdxfrw
    // does: true-color (code 420) wins when set; otherwise BYLAYER/BYBLOCK
    // (ACI 256/0) look up layerColors_ (built from addLayer() calls, which
    // the DXF/DWG table section emits before the entities section); any
    // other value is a direct ACI index.
    RgbColor resolveEntityColor(const DRW_Entity &data) const;

    // Resolves an entity's dash pattern the same way resolveEntityColor()
    // resolves color: BYLAYER/BYBLOCK look up layerLineTypes_ (built from
    // addLayer(), same as layerColors_); "CONTINUOUS" (or a name absent
    // from linePatterns_, e.g. an unsupported complex/shape linetype) is
    // solid -- returns empty. Otherwise scales linePatterns_[name] (raw
    // DXF code-49 values, positive=dash/negative=gap/0=dot) by
    // globalLtScale_ * data.ltypeScale into the fully-resolved, always-
    // positive alternating dash/gap sequence Shape::dashPattern expects.
    std::vector<double> resolveEntityLineType(const DRW_Entity &data) const;

    // Recursively places one INSERT's referenced block (and, for an
    // MINSERT-style array, each row/column repeat of it) into shapes_,
    // composing `parentTransform` with this insert's own transform so
    // nested block references (a block whose definition itself contains an
    // INSERT) come out correctly positioned. `depth` guards against a
    // malformed/cyclic block reference recursing forever.
    void instantiateInsert(const PendingInsert &insert, const Transform2D &parentTransform, int depth);

    // Called once after the whole file is parsed (loadFile) to resolve
    // every top-level INSERT collected in topLevelInserts_ -- deferred
    // rather than done inline in addInsert() because block definition
    // order isn't guaranteed (a block can be defined after something that
    // references it).
    void resolveInserts();

    // --- Dimension rendering. This viewer synthesizes dimension geometry
    // (extension lines, dimension line/arc, arrowheads, text) directly from
    // each DIMENSION entity's own definition points, the same approach
    // LibreCAD's RS_Dimension-derived classes take -- rather than trusting
    // the anonymous block of pre-rendered graphics AutoCAD also writes into
    // the file (DRW_Dimension::getName()), which this viewer never reads.

    // Arrow size / extension-line offset+extend / text height for one
    // DIMSTYLE, already multiplied by that style's own dimscale -- see
    // addDimStyle. Values are in document units, ready to use directly.
    struct DimStyleDefaults {
        double arrowSize;
        double extOffset;
        double extExtend;
        double textHeight;
    };

    // Looks up `styleName` (a dimension entity's own getStyle()) in
    // dimStyles_ (populated by addDimStyle from the file's DIMSTYLE table)
    // or falls back to dimStyles_["Standard"], then to the header-derived
    // dimArrowSize_ etc.
    //
    // Both of those normally-reliable sources go silently missing at once
    // for any DWG file: the vendored DWG reader's DRW_Dimstyle::parseDwg
    // (third_party/libdxfrw/src/drw_objects.cpp) reads a DIMSTYLE table
    // entry's *name* only, leaving dimasz/dimexo/dimexe/dimtxt/dimscale at
    // DRW_Dimstyle::reset()'s literal factory-default values, and libdwgr
    // never populates the $DIM* header variables either -- a real gap in
    // that vendored file, not something to patch there silently (see
    // CLAUDE.md). So for a DWG (this project's own teste.dwg included), the
    // resolved style is indistinguishable from a genuine 0.18-unit-scale
    // drawing, sized for a completely different drawing than the one
    // actually being viewed.
    //
    // hasReliableDimHeaderVars_ (set in addHeader) tells the two cases
    // apart: no header $DIM* vars at all is itself the DWG-gap signature
    // (a real DXF's header always carries them, defaults or not). When
    // that's true and the resolved style is bit-for-bit those same factory
    // defaults, every dimension in the file -- not just this one -- shares
    // the same unreliable-scale problem, so they all fall back to the SAME
    // uniformFallbackDimStyle() (cached from the first dimension's own
    // `referenceLength`, its measured length/radius) instead of each
    // computing its own from its own measurement, which looked wildly
    // inconsistent from one dimension to the next (e.g. a 500-unit and a
    // 150-unit dimension elsewhere in the same drawing getting very
    // differently sized arrows).
    //
    // A trustworthy result is then multiplied by dimScale_ ($DIMSCALE) --
    // applied here rather than folded into dimStyles_/the header-fallback
    // members themselves, since it's a document-wide multiplier on top of
    // WHATEVER style ends up in effect, not specific to either source (a
    // named style's own per-style dimscale field is exactly the kind of
    // often-left-at-1.0 value this whole function distrusts -- e.g. this
    // project's own teste.dxf, where the "ISO-25" DIMSTYLE table entry
    // carries no dimscale override at all, yet the header's $DIMSCALE is
    // 12 and very much does apply). Not applied to a fallback result from
    // uniformFallbackDimStyle(), which is already derived from real
    // document-space geometry.
    //
    // Finally, a per-entity "ACAD"/"DSTYLE" XDATA override on `dim` itself
    // (AutoCAD's mechanism for "just this one dimension uses a different
    // DIMSCALE/DIMTXT/etc than its named style") takes precedence over
    // everything above -- see findDstyleXdataOverride. This is common in
    // real files: this project's own teste.dxf has one on every dimension
    // the initial bug report called out by value (500, 150, 930, 1015),
    // each overriding DIMSCALE to 100 (not the header's 12) and DIMTXT to
    // 2.6.
    DimStyleDefaults resolveDimStyle(const DRW_Dimension &dim, double referenceLength);

    // One arrow size shared by every dimension in a file with no
    // trustworthy size data anywhere (see resolveDimStyle) -- cached on
    // first use from whatever referenceLength that first dimension passed
    // in, extension-line/text proportions derived from it the same way as
    // AutoCAD's own factory defaults relate to its factory arrow size.
    DimStyleDefaults uniformFallbackDimStyle(double referenceLength);

    // Pushes a filled triangular arrowhead: tip at `tip`, pointing back
    // along `dirAngleRad` (the direction from the tip toward the rest of
    // the dimension/leader line) -- same standard proportions as most CAD
    // renderers' default arrow (half-angle ~9.46 degrees).
    void addDimensionArrow(Point2D tip, double dirAngleRad, double arrowSize, const RgbColor &color);
    void addDimensionLine(Point2D p1, Point2D p2, const RgbColor &color);
    void addDimensionArcShape(Point2D center, double radius, double startRad, double endRad, const RgbColor &color);
    void addDimensionText(Point2D anchor, double angleRad, double textHeight, const std::string &text,
                           const RgbColor &color);

    // Shared by addDimAlign (theta = direction from p1 to p2) and
    // addDimLinear (theta = the dimension's own rotation angle, code 50):
    // both place a dimension line through `dimLinePt` parallel to `theta`,
    // with extension lines dropped from p1/p2 to meet it.
    void addLinearStyleDimension(const DRW_Dimension &dim, Point2D p1, Point2D p2, Point2D dimLinePt, double thetaRad);

    // Shared by addDimAngular (vertex computed from the two edges'
    // intersection) and addDimAngular3P (vertex given directly): draws the
    // dimension arc between the rays vertex->edgePoint1 and
    // vertex->edgePoint2, picking whichever of the two possible sweeps
    // actually passes through `radiusThroughPoint` (which also sets the
    // arc's radius).
    void addAngularStyleDimension(const DRW_Dimension &dim, Point2D vertex, Point2D edgePoint1, Point2D edgePoint2,
                                   Point2D radiusThroughPoint);

    std::vector<Shape> shapes_;
    BoundingBox bbox_;
    std::string errorMessage_;
    std::unordered_map<std::string, RgbColor> layerColors_;
    std::unordered_map<std::string, std::string> layerLineTypes_;
    std::unordered_map<std::string, std::vector<double>> linePatterns_; // raw, unscaled (code-49 values)
    double globalLtScale_ = 1.0; // $LTSCALE header variable

    // Fallback dimension style defaults, read from the header in
    // addHeader() when present ($DIMASZ/$DIMEXO/$DIMEXE/$DIMTXT, raw --
    // NOT yet times dimScale_, see resolveDimStyle) -- only ever populated
    // for DXF (libdwgr, the DWG reader, never sets these header variables
    // at all). AutoCAD's own factory defaults otherwise. Used by
    // resolveDimStyle() only when the dimension's own named style isn't
    // found in dimStyles_ below, which is the normal, reliable,
    // format-independent source (see addDimStyle).
    double dimArrowSize_ = 0.18;  // $DIMASZ
    double dimExtOffset_ = 0.0625; // $DIMEXO -- gap between the measured point and its extension line
    double dimExtExtend_ = 0.18;  // $DIMEXE -- how far the extension line runs past the dimension line
    double dimTextHeight_ = 0.18; // $DIMTXT

    // $DIMSCALE header variable -- a document-wide multiplier resolveDimStyle
    // applies on top of whichever style (table or header-fallback) it
    // resolves. Only DXF ever sets this (see hasReliableDimHeaderVars_).
    double dimScale_ = 1.0;

    // Whether addHeader actually found $DIMASZ or $DIMSCALE -- see
    // resolveDimStyle's comment for why this is the signal that
    // distinguishes a real (if possibly-factory-default) DXF DIMSTYLE from
    // the DWG reader's parseDwg gap.
    bool hasReliableDimHeaderVars_ = false;

    // Cache for uniformFallbackDimStyle() -- negative means "not yet
    // computed for this file".
    double cachedDimFallbackArrowSize_ = -1.0;

    // DIMSTYLE table entries (name -> raw sizing, NOT yet times dimScale_ --
    // see resolveDimStyle), populated by addDimStyle(). Present for both
    // DXF and DWG (though see resolveDimStyle for why DWG's own numeric
    // fields can't be trusted).
    std::unordered_map<std::string, DimStyleDefaults> dimStyles_;

    std::unordered_map<std::string, BlockDef> blocks_;
    std::vector<PendingInsert> topLevelInserts_;

    // Set while between addBlock() and endBlock(): addShape()/addInsert()
    // route into blocks_[currentBlockName_] instead of the top-level
    // document during that window. libdxfrw's DWG reader special-cases
    // Model Space/Paper Space so their entities are delivered *after*
    // endBlock() (see dwgreader.cpp's "deferredEntityWalk"), so this flag
    // correctly stays false for real top-level drawing content in both DXF
    // and DWG -- only genuinely reusable named blocks get captured here.
    bool insideBlock_ = false;
    std::string currentBlockName_;
};
