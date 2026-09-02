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
    Text
};

// Collapses DRW_Text::HAlign/VAlign (TEXT) and DRW_MText's 1-9 attachment
// point (MTEXT, see DwgDocument::addMText) onto the same simplified model:
// HAligned/HMiddle/HFit (TEXT-only, used for fit-to-width variants) fold
// into Center, since a from-scratch viewer has no reason to reshape glyphs
// to hit an exact width.
enum class TextHAlign { Left, Center, Right };
enum class TextVAlign { Baseline, Bottom, Middle, Top };

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

    // --- Everything else in DRW_Interface is a no-op for a pure viewer.
    // Header/table/style callbacks are read but not used; the write*()
    // callbacks are only invoked by libdxfrw when writing (we never call
    // write()), so they're dead code paths here, kept only to satisfy the
    // pure-virtual interface.
    void addDimStyle(const DRW_Dimstyle &) override {}
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
    void addDimAlign(const DRW_DimAligned *) override {}
    void addDimLinear(const DRW_DimLinear *) override {}
    void addDimRadial(const DRW_DimRadial *) override {}
    void addDimDiametric(const DRW_DimDiametric *) override {}
    void addDimAngular(const DRW_DimAngular *) override {}
    void addDimAngular3P(const DRW_DimAngular3p *) override {}
    void addDimOrdinate(const DRW_DimOrdinate *) override {}
    void addDimArc(const DRW_DimArc *) override {}
    void addLeader(const DRW_Leader *) override {}
    void addHatch(const DRW_Hatch *) override {}
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

    std::vector<Shape> shapes_;
    BoundingBox bbox_;
    std::string errorMessage_;
    std::unordered_map<std::string, RgbColor> layerColors_;
    std::unordered_map<std::string, std::string> layerLineTypes_;
    std::unordered_map<std::string, std::vector<double>> linePatterns_; // raw, unscaled (code-49 values)
    double globalLtScale_ = 1.0; // $LTSCALE header variable

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
