#include "dwg_document.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

namespace {
// Looks up a DOUBLE-typed header variable under either its DXF-style
// "$NAME" key or the DWG reader's bare "NAME" key (see the $LTSCALE
// comment below), writing into `out` and returning whether it was found.
bool findDoubleHeaderVar(const DRW_Header &data, const char *dollarName, const char *bareName, double &out) {
    for (const char *key : {dollarName, bareName}) {
        auto it = data.vars.find(key);
        if (it != data.vars.end() && it->second && it->second->type() == DRW_Variant::DOUBLE) {
            out = it->second->d_val();
            return true;
        }
    }
    return false;
}
} // namespace

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

    // $DIMSCALE is a global multiplier AutoCAD applies on top of $DIMASZ/
    // $DIMEXO/$DIMEXE/$DIMTXT -- and, per resolveDimStyle's comment, on top
    // of a named DIMSTYLE table entry's own values too, not just these
    // header-fallback ones (a drawing set up at a non-1:1 plot scale
    // routinely sets only this, not each DIMSTYLE's own variables, so
    // skipping it would leave arrows/text sized for a completely different
    // scale -- potentially sub-pixel and invisible, as very nearly
    // happened with this project's own test file). Applied in
    // resolveDimStyle, not here, since it's a multiplier on WHATEVER style
    // ends up in effect, not specifically these header values.
    const bool hasDimScale = findDoubleHeaderVar(*data, "$DIMSCALE", "DIMSCALE", dimScale_);
    if (dimScale_ <= 0.0) dimScale_ = 1.0;

    const bool hasDimAsz = findDoubleHeaderVar(*data, "$DIMASZ", "DIMASZ", dimArrowSize_);
    findDoubleHeaderVar(*data, "$DIMEXO", "DIMEXO", dimExtOffset_);
    findDoubleHeaderVar(*data, "$DIMEXE", "DIMEXE", dimExtExtend_);
    findDoubleHeaderVar(*data, "$DIMTXT", "DIMTXT", dimTextHeight_);

    // See resolveDimStyle(): a DWG's header never carries either of these
    // (libdwgr doesn't populate them), so finding one here is itself the
    // signal that this file's dimension sizing is trustworthy.
    hasReliableDimHeaderVars_ = hasDimAsz || hasDimScale;
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

// Transforms a pure direction/displacement (no translation) -- used for
// HatchPatternLine::offset, which is a spacing vector between repeats, not
// a position.
Point2D applyLinear(const Transform2D &t, const Point2D &v) {
    return {t.a * v.x + t.c * v.y, t.b * v.x + t.d * v.y};
}

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

    for (HatchLoop &loop : out.hatchLoops) {
        for (auto &p : loop.points) p = applyTransform(t, p);
        if (mirrored) {
            for (double &bulge : loop.bulges) bulge = -bulge;
        }
    }
    out.hatchGradientAngleRad = s.hatchGradientAngleRad + rotation;
    for (HatchPatternLine &line : out.hatchPatternLines) {
        line.basePoint = applyTransform(t, line.basePoint);
        line.offset = applyLinear(t, line.offset);
        line.angleRad = line.angleRad + rotation;
        for (double &d : line.dashPattern) d *= scale; // scale is never negative -- sign (dash/gap) is preserved
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

namespace {
bool hasAnyBulge(const std::vector<double> &bulges) {
    return std::any_of(bulges.begin(), bulges.end(),
                        [](double b) { return b != 0.0; });
}

// Builds one HatchLoop from a DRW_HatchLoop. Returns false for a loop this
// viewer can't represent -- an edge-list loop containing anything besides
// LINE/ARC (ELLIPSE and SPLINE boundary edges aren't implemented, same gap
// as those entity types elsewhere -- see CLAUDE.md) -- so the caller can
// drop the whole hatch rather than fill a boundary with a silently-patched
// gap where an unsupported edge should have been.
bool buildHatchLoop(const DRW_HatchLoop &loop, HatchLoop &out) {
    constexpr int kPolylineFlag = 2;
    if (loop.type & kPolylineFlag) {
        if (loop.objlist.empty()) return false;
        auto *pline = dynamic_cast<DRW_LWPolyline *>(loop.objlist.front().get());
        if (!pline) return false;
        out.points.reserve(pline->vertlist.size());
        out.bulges.reserve(pline->vertlist.size());
        for (const auto &v : pline->vertlist) {
            out.points.push_back({v->x, v->y});
            out.bulges.push_back(v->bulge);
        }
        if (!hasAnyBulge(out.bulges)) out.bulges.clear();
        return out.points.size() >= 2;
    }

    // Edge-list loop: each edge contributes its start point (its end point
    // is the next edge's start, or closes back to this loop's first point),
    // exactly like a polyline vertex -- LINE edges are straight (bulge 0),
    // ARC edges get the equivalent bulge for their sweep so the existing
    // bulge->arc sampling (ViewerWidget::sampleSegmentPoints) draws them
    // identically to a polyline-sourced boundary.
    for (const auto &edge : loop.objlist) {
        if (edge->eType == DRW::LINE) {
            const auto *e = static_cast<const DRW_Line *>(edge.get());
            out.points.push_back({e->basePoint.x, e->basePoint.y});
            out.bulges.push_back(0.0);
        } else if (edge->eType == DRW::ARC) {
            const auto *e = static_cast<const DRW_Arc *>(edge.get());
            double start = e->staangle;
            double end = e->endangle;
            double bulge;
            if (e->isccw) {
                if (end < start) end += 2.0 * M_PI;
                bulge = std::tan((end - start) / 4.0);
            } else {
                if (end > start) end -= 2.0 * M_PI;
                bulge = -std::tan((start - end) / 4.0);
            }
            out.points.push_back({e->basePoint.x + e->radious * std::cos(start),
                                   e->basePoint.y + e->radious * std::sin(start)});
            out.bulges.push_back(bulge);
        } else {
            return false; // ELLIPSE/SPLINE/etc. -- not supported yet
        }
    }
    if (!hasAnyBulge(out.bulges)) out.bulges.clear();
    return out.points.size() >= 2;
}

// Expands `loops` (bulge-aware, same reasoning as addShape's Polyline case)
// into `bbox` -- shared so both the top-level and future block-local
// bounding logic stay in sync with how the loops actually render.
void expandHatchBounds(const std::vector<HatchLoop> &loops, BoundingBox &bbox) {
    for (const HatchLoop &loop : loops) {
        for (const auto &p : loop.points) bbox.expand(p);
        const size_t n = loop.points.size();
        if (loop.bulges.size() != n || n == 0) continue;
        auto expandSegment = [&](size_t i, size_t j) {
            Point2D center;
            double radius = 0.0;
            if (bulgeArcExtent(loop.points[i], loop.points[j], loop.bulges[i], center, radius)) {
                bbox.expand(center.x - radius, center.y - radius);
                bbox.expand(center.x + radius, center.y + radius);
            }
        };
        for (size_t i = 0; i + 1 < n; ++i) expandSegment(i, i + 1);
        expandSegment(n - 1, 0); // loops are always implicitly closed
    }
}
} // namespace

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
        case ShapeKind::Hatch:
            expandHatchBounds(shape.hatchLoops, bbox_);
            break;
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

void DwgDocument::addHatch(const DRW_Hatch *data) {
    if (!data) return;

    Shape s;
    s.kind = ShapeKind::Hatch;
    s.color = resolveEntityColor(*data);

    s.hatchLoops.reserve(data->looplist.size());
    for (const auto &loopPtr : data->looplist) {
        HatchLoop loop;
        if (!buildHatchLoop(*loopPtr, loop)) return; // unsupported edge -- draw nothing, don't guess
        s.hatchLoops.push_back(std::move(loop));
    }
    if (s.hatchLoops.empty()) return;

    if (data->isGradient && data->gradColors.size() >= 2) {
        s.hatchFillKind = Shape::HatchFillKind::Gradient;
        // Stops aren't guaranteed to arrive in ascending `value` order.
        auto stops = data->gradColors;
        std::sort(stops.begin(), stops.end(),
                  [](const DRW_Hatch::GradientStop &a, const DRW_Hatch::GradientStop &b) {
                      return a.value < b.value;
                  });
        s.color = resolveDirectColor(stops.front().aciColor, stops.front().rgb);
        s.hatchColor2 = resolveDirectColor(stops.back().aciColor, stops.back().rgb);
        s.hatchGradientAngleRad = data->gradAngle;
    } else if (data->solid) {
        s.hatchFillKind = Shape::HatchFillKind::Solid;
    } else {
        s.hatchFillKind = Shape::HatchFillKind::Pattern;
        s.hatchPatternLines.reserve(data->patternLines.size());
        for (const DRW_Hatch::PatternLine &pl : data->patternLines) {
            HatchPatternLine line;
            line.angleRad = pl.angle * M_PI / 180.0;
            line.basePoint = {pl.baseX, pl.baseY};
            line.offset = {pl.offsetX, pl.offsetY};
            line.dashPattern = pl.dashList;
            s.hatchPatternLines.push_back(std::move(line));
        }
    }

    addShape(std::move(s));
}

namespace {
// Trims a fixed-precision measurement string down to its shortest exact
// form ("12.50" -> "12.5", "12.00" -> "12") -- this viewer has no access to
// the file's own $DIMDEC precision setting, so it always formats at a fixed
// precision and trims, rather than showing trailing zeros that weren't in
// the original DIMSTYLE.
std::string formatDimensionMeasurement(double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", value);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

// Builds the dimension's text: the user's own override (code 1) if set,
// substituting it for a literal "<>" placeholder when present (a common
// real-world pattern, e.g. "<>+/-0.05"); otherwise `prefix` +
// the auto-measured value + `suffix` (e.g. "R" / an angle's degree sign).
std::string formatDimensionText(const DRW_Dimension &dim, double measured, const std::string &prefix,
                                 const std::string &suffix) {
    const std::string autoText = prefix + formatDimensionMeasurement(measured) + suffix;
    const std::string override = dim.getText();
    if (override.empty() || override == "<>") return autoText;
    const size_t pos = override.find("<>");
    if (pos == std::string::npos) return override;
    std::string result = override;
    result.replace(pos, 2, autoText);
    return result;
}

double normalizeAngle2Pi(double a) {
    while (a < 0.0) a += 2.0 * M_PI;
    while (a >= 2.0 * M_PI) a -= 2.0 * M_PI;
    return a;
}

// A dimension line's direction is only meaningful up to +/- pi -- the line
// itself looks identical whichever of its two definition points a file
// happened to store as "first" -- so rendering dimension text at that raw
// angle would render it upside-down (reads as mirrored) whenever the
// backing entity's points were stored back-to-front relative to a
// left-to-right reading of theta. Flips by pi whenever the raw angle falls
// in the "upside-down" half, leaving it in (-pi/2, pi/2].
double uprightTextAngle(double angleRad) {
    double a = normalizeAngle2Pi(angleRad); // [0, 2*pi)
    if (a > M_PI / 2.0 && a < 3.0 * M_PI / 2.0) a -= M_PI;
    return a;
}

// Picks which of the two possible CCW sweeps between dir1/dir2 (both
// measured from the same vertex) actually contains `throughAngle` -- the
// raw direction values alone don't say whether the dimension spans the
// "short way" or "long way" around; the dimension's own arc-location point
// (`throughAngle`) is what disambiguates it.
void resolveAngularSweep(double dir1, double dir2, double throughAngle, double &start, double &end) {
    const double sweep1 = normalizeAngle2Pi(dir2 - dir1); // CCW sweep from dir1 to dir2
    const double rel = normalizeAngle2Pi(throughAngle - dir1);
    if (rel <= sweep1) {
        start = dir1;
        end = dir1 + sweep1;
    } else {
        start = dir2;
        end = dir2 + (2.0 * M_PI - sweep1);
    }
}

// Reads one numeric DIMSTYLE override from an entity's XDATA -- AutoCAD's
// mechanism for overriding specific dimension variables on a single
// dimension without a full named DIMSTYLE (visible in DXF as an "ACAD"/
// "DSTYLE" extended-data group: 1001 "ACAD", 1000 "DSTYLE", 1002 "{", then
// (1070 dimvar-group-code)/(1040 value) pairs, 1002 "}"). `dimVarCode` is
// the DIMSTYLE group code identifying which variable (40=dimscale,
// 41=dimasz, 42=dimexo, 44=dimexe, 140=dimtxt) -- all double-valued, hence
// only handling a 1040-coded value here. A pair naming some other
// variable (e.g. 340, a text-style handle) is simply skipped, not
// misread, since the search is keyed on the group code, not position.
bool findDstyleXdataOverride(const std::vector<std::shared_ptr<DRW_Variant>> &extData, int dimVarCode, double &out) {
    for (size_t i = 0; i + 1 < extData.size(); ++i) {
        const DRW_Variant *appName = extData[i].get();
        if (!appName || appName->code() != 1001 || appName->type() != DRW_Variant::STRING) continue;
        if (std::string(appName->c_str()) != "ACAD") continue;
        const DRW_Variant *marker = extData[i + 1].get();
        if (!marker || marker->code() != 1000 || marker->type() != DRW_Variant::STRING) continue;
        if (std::string(marker->c_str()) != "DSTYLE") continue;

        for (size_t j = i + 2; j + 1 < extData.size(); ++j) {
            const DRW_Variant *v = extData[j].get();
            if (!v) continue;
            if (v->code() == 1002 && v->type() == DRW_Variant::STRING && std::string(v->c_str()) == "}") break;
            if (v->code() != 1070 || v->type() != DRW_Variant::INTEGER || v->i_val() != dimVarCode) continue;
            const DRW_Variant *value = extData[j + 1].get();
            if (value && value->code() == 1040 && value->type() == DRW_Variant::DOUBLE) {
                out = value->d_val();
                return true;
            }
        }
        return false; // found the DSTYLE group but not this specific override
    }
    return false;
}
} // namespace

DwgDocument::DimStyleDefaults DwgDocument::uniformFallbackDimStyle(double referenceLength) {
    if (cachedDimFallbackArrowSize_ < 0.0) {
        cachedDimFallbackArrowSize_ = std::max(referenceLength, 1e-6) * 0.03;
    }
    const double arrow = cachedDimFallbackArrowSize_;
    return {arrow, arrow * 0.25, arrow * 0.7, arrow * 1.1};
}

DwgDocument::DimStyleDefaults DwgDocument::resolveDimStyle(const DRW_Dimension &dim, double referenceLength) {
    auto it = dimStyles_.find(dim.getStyle());
    if (it == dimStyles_.end()) it = dimStyles_.find("Standard");

    DimStyleDefaults style;
    bool trustworthy;
    if (it != dimStyles_.end()) {
        style = it->second;
        // Bit-for-bit DRW_Dimstyle::reset()'s literal factory-default
        // values -- the signature DRW_Dimstyle::parseDwg (DWG only) leaves
        // behind by skipping these fields entirely, not a real file
        // coincidentally wanting all four at once (see resolveDimStyle's
        // declaration comment).
        const bool looksUnparsed = style.arrowSize == 0.18 && style.extOffset == 0.0625 &&
                                    style.extExtend == 0.18 && style.textHeight == 0.18;
        if (looksUnparsed && hasReliableDimHeaderVars_) {
            // The named style's own fields weren't really parsed, but the
            // header's $DIM* values ARE reliable here (DRW_Header::parseDwg
            // -- drw_header.cpp -- does populate these for DWG, unlike the
            // DIMSTYLE table): use those instead of the table's bogus
            // defaults, not the defaults themselves. Previously this branch
            // only decided *whether* to trust the result, still returning
            // the unparsed 0.18 table value even when reliable header data
            // was sitting right there to use instead -- arrows (and text,
            // whenever no XDATA override supplied a real dimtxt) came out
            // sized for a 0.18-unit-scale drawing regardless of the actual
            // one.
            style = {dimArrowSize_, dimExtOffset_, dimExtExtend_, dimTextHeight_};
        }
        trustworthy = !looksUnparsed || hasReliableDimHeaderVars_;
    } else {
        style = {dimArrowSize_, dimExtOffset_, dimExtExtend_, dimTextHeight_};
        trustworthy = hasReliableDimHeaderVars_;
    }

    if (!trustworthy) return uniformFallbackDimStyle(referenceLength);

    // A per-entity XDATA override (see findDstyleXdataOverride) replaces
    // the corresponding raw component before $DIMSCALE is applied --
    // AutoCAD's own precedence (most specific to this one dimension wins).
    double effectiveScale = dimScale_;
    findDstyleXdataOverride(dim.extData, 40, effectiveScale);
    findDstyleXdataOverride(dim.extData, 41, style.arrowSize);
    findDstyleXdataOverride(dim.extData, 42, style.extOffset);
    findDstyleXdataOverride(dim.extData, 44, style.extExtend);
    findDstyleXdataOverride(dim.extData, 140, style.textHeight);

    // $DIMSCALE (or this entity's own XDATA override of it) applies on top
    // of whichever style is in effect -- see the declaration comment for
    // why this isn't folded into dimStyles_/the header-fallback members
    // themselves.
    style.arrowSize *= effectiveScale;
    style.extOffset *= effectiveScale;
    style.extExtend *= effectiveScale;
    style.textHeight *= effectiveScale;
    return style;
}

void DwgDocument::addDimStyle(const DRW_Dimstyle &data) {
    dimStyles_[data.name] = {data.dimasz, data.dimexo, data.dimexe, data.dimtxt};
}

void DwgDocument::addDimensionArrow(Point2D tip, double dirAngleRad, double arrowSize, const RgbColor &color) {
    constexpr double kHalfAngle = 0.165; // radians (~9.46 degrees) -- standard arrowhead proportions
    const double side = arrowSize / std::cos(kHalfAngle);
    Shape s;
    s.kind = ShapeKind::Hatch;
    s.hatchFillKind = Shape::HatchFillKind::Solid;
    s.color = color;
    HatchLoop loop;
    loop.points.push_back(tip);
    // Back corners are offset from `tip` *toward* dirAngleRad (the
    // direction from tip toward the rest of the dimension/leader line),
    // not away from it -- the tip is the single point that touches the
    // measured location, and the wide base sits inside the dimension
    // line's span, overlapping it, the way every CAD dimension arrowhead
    // is drawn. Flipping this sign (as an earlier version of this
    // function did) still puts the tip in the right place -- the bug was
    // invisible on an arrow that's small relative to a long dimension line
    // -- but the base then pokes out past the measured point instead of
    // into the line, which becomes obvious wherever the arrow is a
    // significant fraction of the line's length, or another dimension's
    // own reference line sits right where the base incorrectly lands.
    loop.points.push_back({tip.x + side * std::cos(dirAngleRad + kHalfAngle),
                            tip.y + side * std::sin(dirAngleRad + kHalfAngle)});
    loop.points.push_back({tip.x + side * std::cos(dirAngleRad - kHalfAngle),
                            tip.y + side * std::sin(dirAngleRad - kHalfAngle)});
    s.hatchLoops.push_back(std::move(loop));
    addShape(std::move(s));
}

void DwgDocument::addDimensionLine(Point2D p1, Point2D p2, const RgbColor &color) {
    Shape s;
    s.kind = ShapeKind::Line;
    s.points = {p1, p2};
    s.color = color;
    addShape(std::move(s));
}

void DwgDocument::addDimensionArcShape(Point2D center, double radius, double startRad, double endRad,
                                        const RgbColor &color) {
    Shape s;
    s.kind = ShapeKind::Arc;
    s.center = center;
    s.radius = radius;
    s.startAngleRad = startRad;
    s.endAngleRad = endRad;
    s.color = color;
    addShape(std::move(s));
}

void DwgDocument::addDimensionText(Point2D anchor, double angleRad, double textHeight, const std::string &text,
                                    const RgbColor &color) {
    if (text.empty()) return;
    Shape s;
    s.kind = ShapeKind::Text;
    s.text = text;
    s.center = anchor;
    s.textAngleRad = angleRad;
    s.textHeightDoc = textHeight;
    s.textHAlign = TextHAlign::Center;
    s.textVAlign = TextVAlign::Middle;
    s.color = color;
    addShape(std::move(s));
}

void DwgDocument::addLinearStyleDimension(const DRW_Dimension &dim, Point2D p1, Point2D p2, Point2D dimLinePt,
                                           double thetaRad) {
    const double dirX = std::cos(thetaRad), dirY = std::sin(thetaRad);
    const double normX = -dirY, normY = dirX;

    // The dimension line is the infinite line through dimLinePt in
    // direction theta; foot1/foot2 are p1/p2 *projected* onto it (not
    // translated by a shared offset -- that only happens to stay parallel
    // to theta when p2-p1 is already parallel to theta, true for an
    // Aligned dimension but NOT for a Linear/rotated one, where theta is
    // an independently-specified angle: a "horizontal" or "vertical"
    // DIMLINEAR measuring two points at different heights/offsets would
    // otherwise come out as a skewed line matching the p1-p2 direction
    // instead of a true horizontal/vertical one).
    const double s1 = (p1.x - dimLinePt.x) * dirX + (p1.y - dimLinePt.y) * dirY;
    const double s2 = (p2.x - dimLinePt.x) * dirX + (p2.y - dimLinePt.y) * dirY;
    const Point2D foot1{dimLinePt.x + s1 * dirX, dimLinePt.y + s1 * dirY};
    const Point2D foot2{dimLinePt.x + s2 * dirX, dimLinePt.y + s2 * dirY};
    const double measure = std::abs(s2 - s1);
    const DimStyleDefaults style = resolveDimStyle(dim, measure);
    const RgbColor color = resolveEntityColor(dim);

    // Each extension line's own perpendicular offset from the dimension
    // line -- computed per-point (not shared) since p1/p2 need not be
    // equidistant from it.
    const double t1 = (p1.x - dimLinePt.x) * normX + (p1.y - dimLinePt.y) * normY;
    const double t2 = (p2.x - dimLinePt.x) * normX + (p2.y - dimLinePt.y) * normY;
    const double extSign1 = (t1 <= 0.0) ? 1.0 : -1.0;
    const double extSign2 = (t2 <= 0.0) ? 1.0 : -1.0;

    addDimensionLine(foot1, foot2, color);
    addDimensionLine({p1.x + normX * extSign1 * style.extOffset, p1.y + normY * extSign1 * style.extOffset},
                      {foot1.x + normX * extSign1 * style.extExtend, foot1.y + normY * extSign1 * style.extExtend},
                      color);
    addDimensionLine({p2.x + normX * extSign2 * style.extOffset, p2.y + normY * extSign2 * style.extOffset},
                      {foot2.x + normX * extSign2 * style.extExtend, foot2.y + normY * extSign2 * style.extExtend},
                      color);

    // Default (unflipped): each arrow's tip sits at its own foot, pointing
    // inward along the dimension line toward the other foot -- which of
    // thetaRad/thetaRad+pi that is depends on whether foot2 or foot1 is
    // further along theta (s2 vs s1), NOT a fixed assignment: for an
    // Aligned dimension theta is defined as the p1->p2 direction, so foot2
    // is always the one further along it (s2>=s1 always) and a fixed
    // assignment happens to work -- but for a Linear/rotated dimension
    // theta is independently specified, so def1 can land on either side of
    // dimLinePt (this file has real examples of both). Getting this
    // backwards for one foot doesn't move its tip -- the tip is still
    // exactly at that foot -- but its body then extends away from the
    // other foot instead of toward it, i.e. outside the dimension line
    // instead of inside it.
    const bool foot2Ahead = s2 >= s1;
    const double foot1Inward = foot2Ahead ? thetaRad : thetaRad + M_PI;
    const double foot2Inward = foot2Ahead ? thetaRad + M_PI : thetaRad;
    addDimensionArrow(foot1, dim.getFlipArrow1() ? foot1Inward + M_PI : foot1Inward, style.arrowSize, color);
    addDimensionArrow(foot2, dim.getFlipArrow2() ? foot2Inward + M_PI : foot2Inward, style.arrowSize, color);

    const Point2D textAnchor{dim.getTextPoint().x, dim.getTextPoint().y};
    // Upright, not literally theta: theta is only defined up to +/- pi (a
    // dimension line reads the same whichever end p1/p2 happens to be), so
    // using it as-is would render the text upside-down (looking mirrored)
    // whenever def1/def2 happened to be stored "backwards" relative to a
    // left-to-right reading of theta.
    addDimensionText(textAnchor, uprightTextAngle(thetaRad), style.textHeight,
                      formatDimensionText(dim, measure, "", ""), color);
}

void DwgDocument::addAngularStyleDimension(const DRW_Dimension &dim, Point2D vertex, Point2D edgePoint1,
                                            Point2D edgePoint2, Point2D radiusThroughPoint) {
    const double radius = std::hypot(radiusThroughPoint.x - vertex.x, radiusThroughPoint.y - vertex.y);
    if (radius < 1e-9) return; // degenerate -- arc-location point sits on the vertex itself

    const DimStyleDefaults style = resolveDimStyle(dim, radius);
    const double dir1 = std::atan2(edgePoint1.y - vertex.y, edgePoint1.x - vertex.x);
    const double dir2 = std::atan2(edgePoint2.y - vertex.y, edgePoint2.x - vertex.x);
    const double throughAngle = std::atan2(radiusThroughPoint.y - vertex.y, radiusThroughPoint.x - vertex.x);
    double start = 0.0, end = 0.0;
    resolveAngularSweep(dir1, dir2, throughAngle, start, end);

    const RgbColor color = resolveEntityColor(dim);
    addDimensionArcShape(vertex, radius, start, end, color);

    // Extension lines run from the edge's own definition point out past the
    // arc, along that same ray from the vertex -- an approximation (real
    // AutoCAD leaves a small gap near the vertex rather than using the
    // definition point directly), but a defensible one since that point is
    // itself a real point on the measured edge.
    auto extensionLine = [&](Point2D edgePoint, double dirAngle) {
        const Point2D outer{vertex.x + std::cos(dirAngle) * (radius + style.extExtend),
                             vertex.y + std::sin(dirAngle) * (radius + style.extExtend)};
        addDimensionLine(edgePoint, outer, color);
    };
    extensionLine(edgePoint1, dir1);
    extensionLine(edgePoint2, dir2);

    // Arrow tangent direction points "into" the swept arc: +90 degrees from
    // this ray if it's the sweep's start (increasing angle goes into the
    // arc), -90 degrees if it's the end (decreasing angle goes into the
    // arc).
    auto arrowAt = [&](double dirAngle, bool flip) {
        const Point2D pt{vertex.x + radius * std::cos(dirAngle), vertex.y + radius * std::sin(dirAngle)};
        const bool isStart = std::abs(normalizeAngle2Pi(dirAngle - start)) < 1e-6;
        const double intoArc = isStart ? dirAngle + M_PI / 2.0 : dirAngle - M_PI / 2.0;
        addDimensionArrow(pt, flip ? intoArc + M_PI : intoArc, style.arrowSize, color);
    };
    arrowAt(dir1, dim.getFlipArrow1());
    arrowAt(dir2, dim.getFlipArrow2());

    const Point2D textAnchor{dim.getTextPoint().x, dim.getTextPoint().y};
    const double measureDeg = (end - start) * 180.0 / M_PI;
    addDimensionText(textAnchor, 0.0, style.textHeight, formatDimensionText(dim, measureDeg, "", "\xC2\xB0"), color);
}

void DwgDocument::addDimAlign(const DRW_DimAligned *data) {
    if (!data) return;
    const Point2D p1{data->getDef1Point().x, data->getDef1Point().y};
    const Point2D p2{data->getDef2Point().x, data->getDef2Point().y};
    const Point2D dimLinePt{data->getDimPoint().x, data->getDimPoint().y};
    const double theta = std::atan2(p2.y - p1.y, p2.x - p1.x);
    addLinearStyleDimension(*data, p1, p2, dimLinePt, theta);
}

void DwgDocument::addDimLinear(const DRW_DimLinear *data) {
    if (!data) return;
    const Point2D p1{data->getDef1Point().x, data->getDef1Point().y};
    const Point2D p2{data->getDef2Point().x, data->getDef2Point().y};
    const Point2D dimLinePt{data->getDimPoint().x, data->getDimPoint().y};
    const double theta = data->getAngle() * M_PI / 180.0;
    addLinearStyleDimension(*data, p1, p2, dimLinePt, theta);
}

void DwgDocument::addDimRadial(const DRW_DimRadial *data) {
    if (!data) return;
    const Point2D center{data->getCenterPoint().x, data->getCenterPoint().y};
    const Point2D onCircle{data->getDiameterPoint().x, data->getDiameterPoint().y};
    const Point2D textAnchor{data->getTextPoint().x, data->getTextPoint().y};
    const double measure = std::hypot(onCircle.x - center.x, onCircle.y - center.y);
    const RgbColor color = resolveEntityColor(*data);
    const DimStyleDefaults style = resolveDimStyle(*data, measure);

    addDimensionLine(onCircle, textAnchor, color);
    double dirAngle = std::atan2(textAnchor.y - onCircle.y, textAnchor.x - onCircle.x);
    if (data->getFlipArrow1()) dirAngle += M_PI;
    addDimensionArrow(onCircle, dirAngle, style.arrowSize, color);
    addDimensionText(textAnchor, 0.0, style.textHeight, formatDimensionText(*data, measure, "R", ""), color);
}

void DwgDocument::addDimDiametric(const DRW_DimDiametric *data) {
    if (!data) return;
    const Point2D p1{data->getDiameter1Point().x, data->getDiameter1Point().y};
    const Point2D p2{data->getDiameter2Point().x, data->getDiameter2Point().y};
    const Point2D textAnchor{data->getTextPoint().x, data->getTextPoint().y};
    const double measure = std::hypot(p2.x - p1.x, p2.y - p1.y);
    const RgbColor color = resolveEntityColor(*data);
    const DimStyleDefaults style = resolveDimStyle(*data, measure);

    addDimensionLine(p1, textAnchor, color);
    double dirAngle = std::atan2(textAnchor.y - p1.y, textAnchor.x - p1.x);
    if (data->getFlipArrow1()) dirAngle += M_PI;
    addDimensionArrow(p1, dirAngle, style.arrowSize, color);
    addDimensionText(textAnchor, 0.0, style.textHeight, formatDimensionText(*data, measure, "\xC3\x98", ""), color);
}

void DwgDocument::addDimAngular(const DRW_DimAngular *data) {
    if (!data) return;
    const Point2D p1a{data->getFirstLine1().x, data->getFirstLine1().y};
    const Point2D p1b{data->getFirstLine2().x, data->getFirstLine2().y};
    const Point2D p2a{data->getSecondLine1().x, data->getSecondLine1().y};
    const Point2D p2b{data->getSecondLine2().x, data->getSecondLine2().y};

    // Vertex = intersection of line (p1a,p1b) and line (p2a,p2b).
    const double denom = (p1b.x - p1a.x) * (p2b.y - p2a.y) - (p1b.y - p1a.y) * (p2b.x - p2a.x);
    if (std::abs(denom) < 1e-9) return; // parallel edges -- no well-defined vertex
    const double s = ((p2a.x - p1a.x) * (p2b.y - p2a.y) - (p2a.y - p1a.y) * (p2b.x - p2a.x)) / denom;
    const Point2D vertex{p1a.x + s * (p1b.x - p1a.x), p1a.y + s * (p1b.y - p1a.y)};

    addAngularStyleDimension(*data, vertex, p1b, p2b, {data->getDimPoint().x, data->getDimPoint().y});
}

void DwgDocument::addDimAngular3P(const DRW_DimAngular3p *data) {
    if (!data) return;
    const Point2D vertex{data->getVertexPoint().x, data->getVertexPoint().y};
    const Point2D p1{data->getFirstLine().x, data->getFirstLine().y};
    const Point2D p2{data->getSecondLine().x, data->getSecondLine().y};
    const Point2D throughPt{data->getDimPoint().x, data->getDimPoint().y};
    addAngularStyleDimension(*data, vertex, p1, p2, throughPt);
}

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
    dimArrowSize_ = 0.18;
    dimExtOffset_ = 0.0625;
    dimExtExtend_ = 0.18;
    dimTextHeight_ = 0.18;
    dimScale_ = 1.0;
    hasReliableDimHeaderVars_ = false;
    cachedDimFallbackArrowSize_ = -1.0;
    dimStyles_.clear();
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
