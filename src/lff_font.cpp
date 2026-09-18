#include "lff_font.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>

namespace {

// One raw (unflattened) point from a stroke line, e.g. "1.6125,8,A-0.8":
// x, y, and an optional bulge (the "Abulge" suffix) for the segment ending
// at this point -- see LffGlyph's comment on the bulge convention.
struct RawPoint {
    double x = 0.0;
    double y = 0.0;
    bool hasBulge = false;
    double bulge = 0.0;
};

bool parsePoint(const std::string &token, RawPoint &out) {
    const size_t comma1 = token.find(',');
    if (comma1 == std::string::npos) return false;
    const size_t comma2 = token.find(',', comma1 + 1);
    try {
        out.x = std::stod(token.substr(0, comma1));
        if (comma2 == std::string::npos) {
            out.y = std::stod(token.substr(comma1 + 1));
            out.hasBulge = false;
            return true;
        }
        out.y = std::stod(token.substr(comma1 + 1, comma2 - comma1 - 1));
        const std::string bulgeToken = token.substr(comma2 + 1);
        if (!bulgeToken.empty() && (bulgeToken[0] == 'A' || bulgeToken[0] == 'a')) {
            out.bulge = std::stod(bulgeToken.substr(1));
            out.hasBulge = true;
        }
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

// Same chord+bulge -> center/radius/start+end-angle conversion as
// viewer_widget.cpp's bulgeToArc(), adapted to Point2D since this project's
// model-layer files (this one included, see lff_font.h) stay Qt-free.
// Deliberately not shared with viewer_widget.cpp's QPointF version --
// pulling a document-geometry helper into a font parser (or vice versa) for
// one small trig function isn't worth the cross-module coupling.
bool bulgeToArc(Point2D p1, Point2D p2, double bulge, Point2D &center, double &radius,
                 double &startAngle, double &endAngle) {
    if (std::abs(bulge) < 1e-9) return false;
    const double dx = p2.x - p1.x;
    const double dy = p2.y - p1.y;
    const double chordLen = std::hypot(dx, dy);
    if (chordLen < 1e-9) return false;

    const double sign = bulge >= 0.0 ? 1.0 : -1.0;
    const double halfAngle = 2.0 * std::atan(std::abs(bulge));
    radius = (chordLen / 2.0) / std::sin(halfAngle);

    const Point2D mid{(p1.x + p2.x) / 2.0, (p1.y + p2.y) / 2.0};
    const Point2D perp = sign > 0.0 ? Point2D{-dy, dx} : Point2D{dy, -dx};
    const double perpLen = std::hypot(perp.x, perp.y);
    const double distToCenter = radius * std::cos(halfAngle);
    center = {mid.x + perp.x / perpLen * distToCenter, mid.y + perp.y / perpLen * distToCenter};

    startAngle = std::atan2(p1.y - center.y, p1.x - center.x);
    endAngle = std::atan2(p2.y - center.y, p2.x - center.x);
    if (sign > 0.0) {
        if (endAngle < startAngle) endAngle += 2 * M_PI;
    } else {
        if (endAngle > startAngle) endAngle -= 2 * M_PI;
    }
    return true;
}

// Appends the flattened segment from p1 to p2 to `out` (p1 itself excluded
// -- the caller's vector already ends with it): a straight line if `bulge`
// is ~0, otherwise a sampled arc.
void appendSegment(std::vector<Point2D> &out, Point2D p1, Point2D p2, double bulge) {
    Point2D center;
    double radius = 0.0, startAngle = 0.0, endAngle = 0.0;
    if (!bulgeToArc(p1, p2, bulge, center, radius, startAngle, endAngle)) {
        out.push_back(p2);
        return;
    }
    const double sweep = endAngle - startAngle;
    const int segments = std::clamp(static_cast<int>(std::ceil(std::abs(sweep) / (M_PI / 24.0))), 2, 64);
    for (int i = 1; i <= segments; ++i) {
        const double t = startAngle + sweep * i / segments;
        out.push_back({center.x + radius * std::cos(t), center.y + radius * std::sin(t)});
    }
}

// Parses one stroke line (semicolon-separated points) into a flattened
// polyline. Malformed tokens are skipped rather than aborting the whole
// glyph -- a single bad point in a hand-edited font file shouldn't blank
// out an otherwise-fine character.
std::vector<Point2D> parseStroke(const std::string &line) {
    std::vector<RawPoint> raw;
    size_t start = 0;
    while (start <= line.size()) {
        const size_t semi = line.find(';', start);
        const std::string token = line.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
        RawPoint p;
        if (parsePoint(token, p)) raw.push_back(p);
        if (semi == std::string::npos) break;
        start = semi + 1;
    }

    std::vector<Point2D> flat;
    if (raw.empty()) return flat;
    flat.push_back({raw[0].x, raw[0].y});
    for (size_t i = 1; i < raw.size(); ++i) {
        appendSegment(flat, {raw[i - 1].x, raw[i - 1].y}, {raw[i].x, raw[i].y},
                      raw[i].hasBulge ? raw[i].bulge : 0.0);
    }
    return flat;
}

// One entry in a glyph's not-yet-resolved definition: either a literal
// flattened stroke, or a "compose from" reference (an LFF line that's just
// "C" + a hex codepoint, e.g. "C0043") -- LibreCAD's accented-Latin glyphs
// (Ç, É, Ã, ...) are defined as their plain base letter plus a couple of
// extra strokes for the diacritic, rather than duplicating the whole
// letter's geometry for every accented variant. A glyph can have any mix,
// in file order, of composition references and its own literal marks (the
// composed base is typically listed first, but nothing here assumes that).
struct RawGlyphEntry {
    bool isComposeRef = false;
    char32_t composeRef = 0;
    std::vector<Point2D> stroke; // valid when !isComposeRef
};

// True if `line` is exactly "C" followed by one or more hex digits and
// nothing else -- the composition-reference form. Every real stroke line
// contains at least one comma (it's "x,y[,Abulge];..."), so checking for
// that first rules out any accidental collision before even trying the
// hex-digit scan.
bool parseComposeRef(const std::string &line, char32_t &codepoint) {
    if (line.size() < 2 || line[0] != 'C' || line.find(',') != std::string::npos) return false;
    for (size_t i = 1; i < line.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(line[i]))) return false;
    }
    try {
        codepoint = static_cast<char32_t>(std::stoul(line.substr(1), nullptr, 16));
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

// Recognizes "# LetterSpacing: 3" / "# WordSpacing: 6.75" /
// "# LineSpacingFactor: 1" header comments (leading/trailing whitespace
// around the key and value tolerated -- real files pad the colon out with
// spaces to align a trailing description column, see resources/fonts/*.lff).
// Any other "#" comment (Format/Creator/Name/Author/License/...) is
// metadata this viewer has no use for and is silently ignored.
void parseHeaderComment(const std::string &rawLine, LffFont &font) {
    size_t pos = 1; // skip leading '#'
    while (pos < rawLine.size() && rawLine[pos] == ' ') ++pos;
    const std::string rest = rawLine.substr(pos);
    const size_t colon = rest.find(':');
    if (colon == std::string::npos) return;
    const std::string key = rest.substr(0, colon);
    try {
        const double value = std::stod(rest.substr(colon + 1));
        if (key == "LetterSpacing") font.letterSpacing = value;
        else if (key == "WordSpacing") font.wordSpacing = value;
        else if (key == "LineSpacingFactor") font.lineSpacingFactor = value;
    } catch (const std::exception &) {
        // Not a numeric header field (e.g. "# Name: ISO 3098-2") -- ignore.
    }
}

} // namespace

namespace {

// Resolves one glyph's final (flattened, composition-expanded) strokes and
// advance, memoizing into `resolved` so a base letter referenced by many
// accented variants (e.g. 'A' under À/Á/Â/Ã/Ä/Å) is only flattened once.
// `stack` guards against a cycle (a composition reference loop, which would
// otherwise recurse forever) -- not expected in any real .lff file, but
// cheap to make safe rather than assume.
const LffGlyph &resolveGlyph(char32_t codepoint, const std::unordered_map<char32_t, std::vector<RawGlyphEntry>> &raw,
                              std::unordered_map<char32_t, LffGlyph> &resolved, std::vector<char32_t> &stack) {
    if (auto it = resolved.find(codepoint); it != resolved.end()) return it->second;

    LffGlyph glyph; // stays empty for an unknown or cyclic reference
    const auto rawIt = raw.find(codepoint);
    const bool cyclic = std::find(stack.begin(), stack.end(), codepoint) != stack.end();
    if (rawIt != raw.end() && !cyclic) {
        stack.push_back(codepoint);
        for (const RawGlyphEntry &entry : rawIt->second) {
            if (entry.isComposeRef) {
                const LffGlyph &base = resolveGlyph(entry.composeRef, raw, resolved, stack);
                glyph.strokes.insert(glyph.strokes.end(), base.strokes.begin(), base.strokes.end());
            } else {
                glyph.strokes.push_back(entry.stroke);
            }
        }
        stack.pop_back();
        for (const auto &stroke : glyph.strokes)
            for (const Point2D &pt : stroke) glyph.advance = std::max(glyph.advance, pt.x);
    }
    return resolved.emplace(codepoint, std::move(glyph)).first->second;
}

} // namespace

std::shared_ptr<const LffFont> LffFont::loadFromFile(const std::string &path) {
    std::ifstream in(path);
    if (!in) return nullptr;

    auto font = std::make_shared<LffFont>();
    std::unordered_map<char32_t, std::vector<RawGlyphEntry>> raw;
    bool inGlyph = false;
    char32_t currentCodepoint = 0;

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back(); // tolerate CRLF files
        if (line.empty()) continue;

        if (line[0] == '#') {
            parseHeaderComment(line, *font);
            continue;
        }
        if (line[0] == '[') {
            const size_t close = line.find(']');
            if (close == std::string::npos) { inGlyph = false; continue; }
            try {
                currentCodepoint = static_cast<char32_t>(std::stoul(line.substr(1, close - 1), nullptr, 16));
                raw[currentCodepoint]; // ensure an (initially empty) entry exists
                inGlyph = true;
            } catch (const std::exception &) {
                inGlyph = false; // malformed section header -- skip its stroke lines below
            }
            continue;
        }
        if (!inGlyph) continue; // stray line before any "[XXXX]" header

        RawGlyphEntry entry;
        if (parseComposeRef(line, entry.composeRef)) {
            entry.isComposeRef = true;
            raw[currentCodepoint].push_back(std::move(entry));
            continue;
        }
        std::vector<Point2D> stroke = parseStroke(line);
        if (!stroke.empty()) {
            entry.stroke = std::move(stroke);
            raw[currentCodepoint].push_back(std::move(entry));
        }
    }

    if (raw.empty()) return nullptr;

    std::vector<char32_t> stack;
    for (const auto &kv : raw) resolveGlyph(kv.first, raw, font->glyphs, stack);
    return font;
}
