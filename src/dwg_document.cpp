#include "dwg_document.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "libdxfrw.h"
#include "libdwgr.h"

void BoundingBox::expand(double x, double y) {
    minX = std::min(minX, x);
    minY = std::min(minY, y);
    maxX = std::max(maxX, x);
    maxY = std::max(maxY, y);
}

namespace {
// ACI 7 is nominally black ({0,0,0} in DRW::dxfColors), but this viewer's
// canvas is black, so a literal ACI-7 entity would be invisible. LibreCAD's
// renderer swaps pure black for its foreground/white when it would match
// the background (lc_graphicviewrenderer.cpp); replicate that here.
RgbColor aciToColor(int aci) {
    if (aci <= 0 || aci > 255) aci = 7;
    if (aci == 7) return RgbColor{255, 255, 255};
    const unsigned char *rgb = DRW::dxfColors[aci];
    return RgbColor{rgb[0], rgb[1], rgb[2]};
}

// True-color (code 420) takes priority over the ACI index when set, same
// as rs_filterdxfrw::numberToColor()/attributesToPen().
RgbColor resolveDirectColor(int aci, int color24) {
    if (color24 >= 0) {
        return RgbColor{
            static_cast<unsigned char>((color24 >> 16) & 0xFF),
            static_cast<unsigned char>((color24 >> 8) & 0xFF),
            static_cast<unsigned char>(color24 & 0xFF)};
    }
    return aciToColor(aci);
}

// Same bulge->arc conversion as ViewerWidget::bulgeToArc, duplicated here in
// plain doubles/Point2D (rather than shared) so the document model stays
// Qt-free -- used only to widen the auto-fit bounding box enough that a
// strongly bulged segment doesn't get clipped off-screen on first load.
bool bulgeArcExtent(const Point2D &p1, const Point2D &p2, double bulge,
                     Point2D &center, double &radius) {
    if (bulge == 0.0) return false;
    const double dx = p2.x - p1.x;
    const double dy = p2.y - p1.y;
    const double chordLen = std::hypot(dx, dy);
    if (chordLen < 1e-9) return false;
    const double sign = bulge >= 0.0 ? 1.0 : -1.0;
    const double halfAngle = 2.0 * std::atan(std::abs(bulge));
    radius = (chordLen / 2.0) / std::sin(halfAngle);
    const double midx = (p1.x + p2.x) / 2.0;
    const double midy = (p1.y + p2.y) / 2.0;
    const double perpx = (sign > 0.0 ? -dy : dy) / chordLen;
    const double perpy = (sign > 0.0 ? dx : -dx) / chordLen;
    const double distToCenter = radius * std::cos(halfAngle);
    center = {midx + perpx * distToCenter, midy + perpy * distToCenter};
    return true;
}
} // namespace

RgbColor DwgDocument::resolveEntityColor(const DRW_Entity &data) const {
    // BYLAYER (256) and BYBLOCK (0) both resolve to the entity's layer
    // color here: this viewer doesn't track block-insert nesting, so there
    // is no separate "inherited from the inserting block" color to apply
    // for BYBLOCK -- falling back to the layer color is the closest
    // approximation.
    if (data.color24 < 0 &&
        (data.color == DRW::ColorByLayer || data.color == DRW::ColorByBlock)) {
        auto it = layerColors_.find(data.layer);
        if (it != layerColors_.end()) return it->second;
        return RgbColor{255, 255, 255};
    }
    return resolveDirectColor(data.color, data.color24);
}

std::vector<double> DwgDocument::resolveEntityLineType(const DRW_Entity &data) const {
    // Case-insensitive: DXF convention is upper-case ("BYLAYER",
    // "CONTINUOUS"), but don't fail on a writer that didn't bother.
    auto ieq = [](const std::string &a, const char *b) {
        if (a.size() != std::strlen(b)) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != b[i]) return false;
        return true;
    };

    std::string name = data.lineType;
    if (ieq(name, "bylayer") || ieq(name, "byblock")) {
        // Same approximation as resolveEntityColor(): no block-insert
        // linetype inheritance is tracked, so BYBLOCK falls back to the
        // entity's own layer too.
        auto it = layerLineTypes_.find(data.layer);
        if (it == layerLineTypes_.end()) return {};
        name = it->second;
    }
    if (ieq(name, "continuous") || name.empty()) return {};

    auto patIt = linePatterns_.find(name);
    if (patIt == linePatterns_.end() || patIt->second.empty()) return {}; // unknown/complex linetype -- render solid rather than guess

    const double scale = globalLtScale_ * (data.ltypeScale > 0.0 ? data.ltypeScale : 1.0);
    if (scale <= 0.0) return {};

    // A "dot" (raw value 0) has no length of its own; DXF/AutoCAD render it
    // as a short mark rather than nothing. Size it relative to the
    // pattern's own total length so it looks proportionate at any drawing
    // scale, with a tiny absolute floor as a last resort.
    double totalAbs = 0.0;
    for (double v : patIt->second) totalAbs += std::abs(v);
    const double dotLen = std::max(totalAbs * 0.02, 1e-6);

    std::vector<double> resolved;
    resolved.reserve(patIt->second.size());
    for (double raw : patIt->second) {
        const double len = (raw == 0.0) ? dotLen : std::abs(raw);
        resolved.push_back(len * scale);
    }
    return resolved;
}

void DwgDocument::addLayer(const DRW_Layer &data) {
    layerColors_[data.name] = resolveDirectColor(data.color, data.color24);
    layerLineTypes_[data.name] = data.lineType;
}

void DwgDocument::addLType(const DRW_LType &data) {
    linePatterns_[data.name] = data.path;
}

void DwgDocument::addHeader(const DRW_Header *data) {
    if (!data) return;
    // DXF parsing keys header vars by the literal group-9 name, which
    // always includes the leading '$' (e.g. "$LTSCALE"); the DWG binary
    // reader instead hardcodes the bare name ("LTSCALE"). Check both so
    // this works for either source format.
    for (const char *key : {"$LTSCALE", "LTSCALE"}) {
        auto it = data->vars.find(key);
        if (it != data->vars.end() && it->second && it->second->type() == DRW_Variant::DOUBLE) {
            globalLtScale_ = it->second->d_val();
            break;
        }
    }
}

namespace {
// Old-style "%%x" text codes (pre-dating Unicode escapes, still emitted by
// some CAD tools for TEXT and carried verbatim into MTEXT content).
std::string expandPercentCodes(const std::string &raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '%' && i + 2 < raw.size() && raw[i + 1] == '%') {
            switch (std::tolower(static_cast<unsigned char>(raw[i + 2]))) {
                case 'd': out += "\xC2\xB0"; i += 2; continue; // degree sign
                case 'p': out += "\xC2\xB1"; i += 2; continue; // plus-minus
                case 'c': out += "\xC3\x98"; i += 2; continue; // diameter (Ø)
                case '%': out += '%';        i += 2; continue;
                // Underline/overline are toggle codes, not literal
                // characters -- this viewer doesn't render underline/
                // overline runs, so just drop the code instead of leaving
                // the literal "%%u"/"%%o" in the visible text.
                case 'u': case 'o': i += 2; continue;
                default: break;
            }
        }
        out += raw[i];
    }
    return out;
}

// MTEXT content mixes literal text with inline formatting codes (\P for a
// paragraph break, {\C1;...} for color runs, \fFont|...; for font changes,
// \H/\W/\Q/\T/\A/\S for height/width/oblique/tracking/alignment/stacking,
// grouping braces, and \\ \{ \} escapes). This viewer has no rich-text
// rendering, so strip formatting down to plain text instead of showing the
// raw codes: \P becomes a newline, escapes are unescaped, and any other
// backslash code is skipped up to its terminating ';' (or the next
// backslash/brace, for codes with no argument).
std::string sanitizeMTextContent(const std::string &raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '\\' && i + 1 < raw.size()) {
            char next = raw[i + 1];
            if (next == 'P' || next == 'p') {
                out += '\n';
                ++i;
                continue;
            }
            if (next == '\\' || next == '{' || next == '}') {
                out += next;
                ++i;
                continue;
            }
            // Unknown formatting code: skip the code letter and its
            // ';'-terminated argument, if any.
            ++i;
            while (i + 1 < raw.size() && raw[i + 1] != ';' && raw[i + 1] != '\\' &&
                   raw[i + 1] != '{' && raw[i + 1] != '}') {
                ++i;
            }
            if (i + 1 < raw.size() && raw[i + 1] == ';') ++i;
            continue;
        }
        if (c == '{' || c == '}') continue; // grouping braces, not literal text
        out += c;
    }
    return expandPercentCodes(out);
}
} // namespace

Shape DwgDocument::makeTextShape(const DRW_Text &data) const {
    Shape s;
    s.kind = ShapeKind::Text;
    s.text = expandPercentCodes(data.text);
    s.textHeightDoc = data.height;
    s.textAngleRad = data.angle * M_PI / 180.0;
    s.color = resolveEntityColor(data);

    switch (data.alignH) {
        case DRW_Text::HCenter:
        case DRW_Text::HMiddle:
        case DRW_Text::HAligned:
            s.textHAlign = TextHAlign::Center;
            break;
        case DRW_Text::HRight:
            s.textHAlign = TextHAlign::Right;
            break;
        default:
            s.textHAlign = TextHAlign::Left;
    }
    switch (data.alignV) {
        case DRW_Text::VBottom: s.textVAlign = TextVAlign::Bottom; break;
        case DRW_Text::VMiddle: s.textVAlign = TextVAlign::Middle; break;
        case DRW_Text::VTop:    s.textVAlign = TextVAlign::Top;    break;
        default:                s.textVAlign = TextVAlign::Baseline;
    }
    // DXF group 72 == 4 ("Middle", HAlign::HMiddle) is a distinct, older
    // justification code documented as only meaningful "if VAlign==0"
    // (drw_entities.h) -- it's CAD software's single "Middle" text-anchor
    // option, a true 2D center, not "horizontally centered baseline text".
    // Files pairing it with the nominal VAlign==0 (VBaseLine) therefore
    // still mean vertically-centered, not baseline; without this override
    // such text sits on its baseline instead of centered on the anchor.
    if (data.alignH == DRW_Text::HMiddle) s.textVAlign = TextVAlign::Middle;

    // DXF: the insertion point (code 10) is only the anchor for default
    // alignment (left, baseline); any other alignment anchors on the
    // "second alignment point" (code 11) instead.
    const bool isDefaultAlign =
        data.alignH == DRW_Text::HLeft && data.alignV == DRW_Text::VBaseLine;
    const DRW_Coord &anchor = isDefaultAlign ? data.basePoint : data.secPoint;
    s.center = {anchor.x, anchor.y};
    return s;
}

Shape DwgDocument::makeMTextShape(const DRW_MText &data) const {
    Shape s;
    s.kind = ShapeKind::Text;
    s.text = sanitizeMTextContent(data.text);
    s.textHeightDoc = data.height;
    s.textAngleRad = data.angle * M_PI / 180.0;
    s.color = resolveEntityColor(data);
    s.center = {data.basePoint.x, data.basePoint.y};

    // MTEXT's attachment point (DXF group 71) already combines
    // horizontal+vertical alignment and names the insertion point's role
    // directly, unlike TEXT's separate alignH/alignV -- see the comment on
    // DRW_MText::parseDwg for why it round-trips through the inherited
    // `textgen` field instead of a dedicated one.
    switch (data.textgen) {
        case DRW_MText::TopLeft:      s.textHAlign = TextHAlign::Left;   s.textVAlign = TextVAlign::Top;    break;
        case DRW_MText::TopCenter:    s.textHAlign = TextHAlign::Center; s.textVAlign = TextVAlign::Top;    break;
        case DRW_MText::TopRight:     s.textHAlign = TextHAlign::Right;  s.textVAlign = TextVAlign::Top;    break;
        case DRW_MText::MiddleLeft:   s.textHAlign = TextHAlign::Left;   s.textVAlign = TextVAlign::Middle; break;
        case DRW_MText::MiddleCenter: s.textHAlign = TextHAlign::Center; s.textVAlign = TextVAlign::Middle; break;
        case DRW_MText::MiddleRight:  s.textHAlign = TextHAlign::Right;  s.textVAlign = TextVAlign::Middle; break;
        case DRW_MText::BottomLeft:   s.textHAlign = TextHAlign::Left;   s.textVAlign = TextVAlign::Bottom; break;
        case DRW_MText::BottomCenter: s.textHAlign = TextHAlign::Center; s.textVAlign = TextVAlign::Bottom; break;
        case DRW_MText::BottomRight:  s.textHAlign = TextHAlign::Right;  s.textVAlign = TextVAlign::Bottom; break;
        default:                      s.textHAlign = TextHAlign::Left;   s.textVAlign = TextVAlign::Top;
    }
    return s;
}

// ATTRIB/ATTDEF are DRW_Text subclasses (see drw_entities.h) carrying the
// same position/height/alignment fields, plus an optional R2010+ MTEXT-style
// payload (`mtext`, non-null iff attVersion > 0) for multi-line attribute
// values that the plain `text` field can't represent.
Shape DwgDocument::makeAttribShape(const DRW_Attrib &attrib) const {
    return attrib.mtext ? makeMTextShape(*attrib.mtext) : makeTextShape(attrib);
}

void DwgDocument::addText(const DRW_Text &data) {
    Shape s = makeTextShape(data);
    if (!s.text.empty()) addShape(std::move(s));
}

void DwgDocument::addMText(const DRW_MText &data) {
    Shape s = makeMTextShape(data);
    if (!s.text.empty()) addShape(std::move(s));
}

void DwgDocument::addAttDef(const DRW_Attdef &data) {
    // Only a CONSTANT attribute definition (DXF group 70 bit 2) belongs in
    // the block's own static geometry: it has no per-instance ATTRIB, so
    // every insertion must show this same fixed text. A regular (editable)
    // ATTDEF is just the in-block-editor template -- the actual per-insert
    // value arrives separately via DRW_Insert::attlist (see addInsert /
    // instantiateInsert), and rendering the template here too would show
    // stale placeholder text alongside the real value.
    constexpr std::uint8_t kConstantFlag = 2;
    constexpr std::uint8_t kInvisibleFlag = 1;
    if ((data.attribFlags & kConstantFlag) == 0) return;
    if (data.attribFlags & kInvisibleFlag) return;

    Shape s = makeAttribShape(data);
    if (!s.text.empty()) addShape(std::move(s));
}

void DwgDocument::addBlock(const DRW_Block &data) {
    insideBlock_ = true;
    currentBlockName_ = data.name;
    BlockDef &block = blocks_[currentBlockName_]; // creates it if new
    block.basePoint = {data.basePoint.x, data.basePoint.y};
}

void DwgDocument::endBlock() {
    insideBlock_ = false;
    currentBlockName_.clear();
}

void DwgDocument::addInsert(const DRW_Insert &data) {
    PendingInsert ins;
    ins.blockName = data.name;
    ins.insertionPoint = {data.basePoint.x, data.basePoint.y};
    ins.xscale = data.xscale;
    ins.yscale = data.yscale;
    ins.angleRad = data.angle; // DRW_Insert::angle is already radians (code 50)
    ins.colCount = std::max(1, data.colcount);
    ins.rowCount = std::max(1, data.rowcount);
    ins.colSpace = data.colspace;
    ins.rowSpace = data.rowspace;

    // Unlike the block's own geometry, ATTRIB coordinates (and height) are
    // already baked to their final placement by whatever wrote the file --
    // AutoCAD computes them from the ATTDEF template plus this specific
    // insert's position/scale/rotation once, at insert time, rather than
    // storing them in block-local space for re-transformation on every
    // reference. So these must NOT go through instantiateInsert()'s
    // transform like block.shapes does -- add them the same way any other
    // directly-parsed entity would be (addShape() already routes correctly
    // into the current block's own local shapes if this INSERT is itself
    // nested inside another block's definition, or straight into the
    // document otherwise).
    constexpr std::uint8_t kInvisibleFlag = 1;
    for (const auto &attrib : data.attlist) {
        if (!attrib || (attrib->attribFlags & kInvisibleFlag)) continue;
        Shape s = makeAttribShape(*attrib);
        if (!s.text.empty()) addShape(std::move(s));
    }

    if (insideBlock_) {
        // A block referencing another block -- resolved recursively by
        // instantiateInsert() when the outer block itself gets placed.
        blocks_[currentBlockName_].inserts.push_back(std::move(ins));
    } else {
        topLevelInserts_.push_back(std::move(ins));
    }
}

namespace {
Point2D applyTransform(const Transform2D &t, const Point2D &p) {
    return {t.a * p.x + t.c * p.y + t.e, t.b * p.x + t.d * p.y + t.f};
}

// Applies `child` first, then `parent` -- i.e. the result maps a point the
// same as applyTransform(parent, applyTransform(child, p)). Used to combine
// a nested block's own INSERT transform with the transform already placing
// its containing block.
Transform2D composeTransform(const Transform2D &parent, const Transform2D &child) {
    Transform2D r;
    r.a = parent.a * child.a + parent.c * child.b;
    r.b = parent.b * child.a + parent.d * child.b;
    r.c = parent.a * child.c + parent.c * child.d;
    r.d = parent.b * child.c + parent.d * child.d;
    r.e = parent.a * child.e + parent.c * child.f + parent.e;
    r.f = parent.b * child.e + parent.d * child.f + parent.f;
    return r;
}

// T(insertionPoint) * R(angleRad) * S(sx,sy) * T(-blockBasePoint), collapsed
// to a single affine transform: entities are defined relative to the
// block's own basePoint, so that point is what lands exactly on
// insertionPoint once scale/rotation are applied.
Transform2D makeInsertTransform(Point2D insertionPoint, double sx, double sy,
                                 double angleRad, Point2D blockBasePoint) {
    const double c = std::cos(angleRad);
    const double s = std::sin(angleRad);
    Transform2D t;
    t.a = sx * c;
    t.b = sx * s;
    t.c = -sy * s;
    t.d = sy * c;
    t.e = insertionPoint.x - blockBasePoint.x * t.a - blockBasePoint.y * t.c;
    t.f = insertionPoint.y - blockBasePoint.x * t.b - blockBasePoint.y * t.d;
    return t;
}

// Circles/arcs/text carry a single scalar radius/height, so non-uniform
// (sx != sy) scale can only be approximated: sqrt(|determinant|) is the
// "equal-area" equivalent uniform scale for the 2x2 linear part.
double effectiveScale(const Transform2D &t) {
    const double det = t.a * t.d - t.b * t.c;
    return std::sqrt(std::abs(det));
}

// Rotation implied by the transform's linear part, as applied to angles
// measured from local +X (matches how Arc start/end and Text rotation are
// stored). Exact when the transform has no non-uniform scale; a reasonable
// approximation otherwise -- same tradeoff as effectiveScale().
double effectiveRotation(const Transform2D &t) { return std::atan2(t.b, t.a); }

Shape transformShape(const Shape &s, const Transform2D &t) {
    Shape out = s;
    const double scale = effectiveScale(t);
    const double rotation = effectiveRotation(t);
    const bool mirrored = (t.a * t.d - t.b * t.c) < 0.0;

    for (auto &p : out.points) p = applyTransform(t, p);
    out.center = applyTransform(t, s.center);
    out.radius = s.radius * scale;
    out.startAngleRad = s.startAngleRad + rotation;
    out.endAngleRad = s.endAngleRad + rotation;
    out.textHeightDoc = s.textHeightDoc * scale;
    out.textAngleRad = s.textAngleRad + rotation;
    for (double &d : out.dashPattern) d *= scale;
    if (mirrored) {
        // A mirrored insert (negative xscale/yscale) flips which way each
        // bulge segment curves.
        for (double &bulge : out.bulges) bulge = -bulge;
    }
    return out;
}
} // namespace

void DwgDocument::instantiateInsert(const PendingInsert &insert, const Transform2D &parentTransform, int depth) {
    constexpr int kMaxInsertDepth = 32; // guards a malformed/cyclic block reference
    if (depth > kMaxInsertDepth) return;

    auto it = blocks_.find(insert.blockName);
    if (it == blocks_.end()) return; // unresolved/xref block -- nothing to draw
    const BlockDef &block = it->second;

    for (int row = 0; row < insert.rowCount; ++row) {
        for (int col = 0; col < insert.colCount; ++col) {
            const Point2D origin = {insert.insertionPoint.x + col * insert.colSpace,
                                     insert.insertionPoint.y + row * insert.rowSpace};
            const Transform2D localTransform =
                makeInsertTransform(origin, insert.xscale, insert.yscale, insert.angleRad, block.basePoint);
            const Transform2D combined = composeTransform(parentTransform, localTransform);

            for (const Shape &s : block.shapes) addShape(transformShape(s, combined));
            for (const PendingInsert &nested : block.inserts) instantiateInsert(nested, combined, depth + 1);
        }
    }
}

void DwgDocument::resolveInserts() {
    const Transform2D identity;
    for (const PendingInsert &insert : topLevelInserts_) instantiateInsert(insert, identity, 0);
}

void DwgDocument::addShape(Shape shape) {
    if (insideBlock_) {
        // Block-local coordinates aren't placed yet -- keep them out of
        // shapes_/bbox_ until resolveInserts() transforms and drops in a
        // copy per INSERT (or discards them entirely if the block is never
        // referenced, which is exactly the "unused block shows up anyway"
        // bug this branch fixes).
        blocks_[currentBlockName_].shapes.push_back(std::move(shape));
        return;
    }
    switch (shape.kind) {
        case ShapeKind::Line:
            for (const auto &p : shape.points) bbox_.expand(p);
            break;
        case ShapeKind::Polyline: {
            for (const auto &p : shape.points) bbox_.expand(p);
            const size_t n = shape.points.size();
            if (shape.bulges.size() == n) {
                auto expandSegment = [&](size_t i, size_t j) {
                    Point2D center;
                    double radius = 0.0;
                    if (bulgeArcExtent(shape.points[i], shape.points[j], shape.bulges[i], center, radius)) {
                        bbox_.expand(center.x - radius, center.y - radius);
                        bbox_.expand(center.x + radius, center.y + radius);
                    }
                };
                for (size_t i = 0; i + 1 < n; ++i) expandSegment(i, i + 1);
                if (shape.closed && n > 0) expandSegment(n - 1, 0);
            }
            break;
        }
        case ShapeKind::Circle:
        case ShapeKind::Arc:
            bbox_.expand(shape.center.x - shape.radius, shape.center.y - shape.radius);
            bbox_.expand(shape.center.x + shape.radius, shape.center.y + shape.radius);
            break;
        case ShapeKind::Text: {
            // No font metrics available at parse time (this model stays
            // Qt-free) -- rough size estimate, just enough that zoomFit()
            // doesn't ignore text-only drawings. Not alignment/rotation
            // aware; that's fine for an auto-fit margin.
            double longestLine = 0.0;
            double lineCount = 1.0;
            size_t start = 0;
            while (start <= shape.text.size()) {
                size_t nl = shape.text.find('\n', start);
                size_t end = (nl == std::string::npos) ? shape.text.size() : nl;
                longestLine = std::max(longestLine, static_cast<double>(end - start));
                if (nl == std::string::npos) break;
                start = nl + 1;
                lineCount += 1.0;
            }
            const double w = longestLine * shape.textHeightDoc * 0.6;
            const double h = lineCount * shape.textHeightDoc * 1.5;
            bbox_.expand(shape.center.x, shape.center.y);
            bbox_.expand(shape.center.x + w, shape.center.y + h);
            break;
        }
    }
    shapes_.push_back(std::move(shape));
}

void DwgDocument::addLine(const DRW_Line &data) {
    Shape s;
    s.kind = ShapeKind::Line;
    s.points.push_back({data.basePoint.x, data.basePoint.y});
    s.points.push_back({data.secPoint.x, data.secPoint.y});
    s.color = resolveEntityColor(data);
    s.dashPattern = resolveEntityLineType(data);
    addShape(std::move(s));
}

void DwgDocument::addCircle(const DRW_Circle &data) {
    Shape s;
    s.kind = ShapeKind::Circle;
    s.center = {data.basePoint.x, data.basePoint.y};
    s.radius = data.radious;
    s.color = resolveEntityColor(data);
    s.dashPattern = resolveEntityLineType(data);
    addShape(std::move(s));
}

void DwgDocument::addArc(const DRW_Arc &data) {
    Shape s;
    s.kind = ShapeKind::Arc;
    s.center = {data.basePoint.x, data.basePoint.y};
    s.radius = data.radious;
    s.startAngleRad = data.staangle;
    s.endAngleRad = data.endangle;
    s.color = resolveEntityColor(data);
    s.dashPattern = resolveEntityLineType(data);
    addShape(std::move(s));
}

namespace {
bool hasAnyBulge(const std::vector<double> &bulges) {
    return std::any_of(bulges.begin(), bulges.end(),
                        [](double b) { return b != 0.0; });
}
} // namespace

void DwgDocument::addLWPolyline(const DRW_LWPolyline &data) {
    Shape s;
    s.kind = ShapeKind::Polyline;
    s.closed = (data.flags & 1) != 0;
    s.points.reserve(data.vertlist.size());
    s.bulges.reserve(data.vertlist.size());
    for (const auto &v : data.vertlist) {
        s.points.push_back({v->x, v->y});
        s.bulges.push_back(v->bulge);
    }
    if (!hasAnyBulge(s.bulges)) s.bulges.clear();
    s.color = resolveEntityColor(data);
    s.dashPattern = resolveEntityLineType(data);
    if (!s.points.empty()) addShape(std::move(s));
}

void DwgDocument::addPolyline(const DRW_Polyline &data) {
    Shape s;
    s.kind = ShapeKind::Polyline;
    s.closed = (data.flags & 1) != 0;
    s.points.reserve(data.vertlist.size());
    s.bulges.reserve(data.vertlist.size());
    for (const auto &v : data.vertlist) {
        s.points.push_back({v->basePoint.x, v->basePoint.y});
        s.bulges.push_back(v->bulge);
    }
    if (!hasAnyBulge(s.bulges)) s.bulges.clear();
    s.color = resolveEntityColor(data);
    s.dashPattern = resolveEntityLineType(data);
    if (!s.points.empty()) addShape(std::move(s));
}

namespace {
std::string lowerExt(const std::string &path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    return ext;
}
} // namespace

bool DwgDocument::loadFile(const std::string &path) {
    shapes_.clear();
    bbox_ = BoundingBox{};
    errorMessage_.clear();
    layerColors_.clear();
    layerLineTypes_.clear();
    linePatterns_.clear();
    globalLtScale_ = 1.0;
    blocks_.clear();
    topLevelInserts_.clear();
    insideBlock_ = false;
    currentBlockName_.clear();

    const std::string ext = lowerExt(path);
    bool ok = false;

    if (ext == "dxf") {
        dxfRW reader(path.c_str());
        ok = reader.read(this, false);
    } else if (ext == "dwg") {
        dwgRW reader(path.c_str());
        ok = reader.read(this, false);
    } else {
        errorMessage_ = "Unrecognized file extension (expected .dxf or .dwg): " + path;
        return false;
    }

    if (!ok) {
        errorMessage_ = "libdxfrw failed to read: " + path;
        return false;
    }

    // Defensive: a well-formed file always balances addBlock()/endBlock(),
    // but don't let a malformed one leave addShape() routing resolved
    // INSERT geometry into blocks_ instead of the document below.
    insideBlock_ = false;

    // Deferred until here (rather than resolved inline in addInsert()):
    // block definition order in the file isn't guaranteed, so a top-level
    // INSERT can reference a block that hadn't been parsed yet when the
    // INSERT callback fired.
    resolveInserts();
    return true;
}
