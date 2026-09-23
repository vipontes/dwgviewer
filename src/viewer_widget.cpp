#include "viewer_widget.h"

#include "lff_font.h"

#include <QBrush>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>

namespace {
// Converts a DXF/DWG polyline vertex "bulge" (tan(includedAngle/4), signed:
// positive = the arc sweeps CCW from p1 to p2, negative = CW) into the
// center/radius/angle form ViewerWidget already knows how to sample for the
// Arc case. Returns false for a (near-)zero bulge, meaning the segment is a
// straight line. Standard bulge->arc conversion (see e.g. ezdxf's
// bulge_to_arc / the DXF group-42 spec).
bool bulgeToArc(QPointF p1, QPointF p2, double bulge,
                 QPointF &center, double &radius,
                 double &startAngle, double &endAngle) {
    if (std::abs(bulge) < 1e-9) return false;
    const double dx = p2.x() - p1.x();
    const double dy = p2.y() - p1.y();
    const double chordLen = std::hypot(dx, dy);
    if (chordLen < 1e-9) return false;

    const double sign = bulge >= 0.0 ? 1.0 : -1.0;
    const double halfAngle = 2.0 * std::atan(std::abs(bulge)); // = includedAngle / 2
    radius = (chordLen / 2.0) / std::sin(halfAngle);

    const QPointF mid((p1.x() + p2.x()) / 2.0, (p1.y() + p2.y()) / 2.0);
    // Chord direction rotated +/-90 degrees (CCW for a positive bulge).
    const QPointF perpUnit = (sign > 0.0 ? QPointF(-dy, dx) : QPointF(dy, -dx)) / chordLen;
    // Negative for an included angle > 180 degrees (bulge magnitude > 1),
    // which correctly places the center on the far side of the chord.
    const double distToCenter = radius * std::cos(halfAngle);
    center = mid + perpUnit * distToCenter;

    // startAngle must stay p1's angle and endAngle must stay p2's angle --
    // sampling has to run p1 -> p2 in that order so the path stays
    // connected to whatever comes before/after this segment. Direction
    // (CCW/CW) is encoded by which way endAngle gets normalized relative
    // to startAngle, not by swapping which point is "start".
    startAngle = std::atan2(p1.y() - center.y(), p1.x() - center.x());
    endAngle = std::atan2(p2.y() - center.y(), p2.x() - center.x());
    if (sign > 0.0) {
        if (endAngle < startAngle) endAngle += 2 * M_PI;
    } else {
        if (endAngle > startAngle) endAngle -= 2 * M_PI;
    }
    return true;
}

// Appends points sampling the segment from p1 to p2 (p1 itself excluded --
// the caller's vector already ends with it) -- a straight line if bulge is
// ~0, otherwise a sampled arc. Mirrors the Arc case's cos/sin sampling (same
// "don't use QPainter's angle convention under our Y-flipped transform"
// reasoning noted there).
void sampleSegmentPoints(std::vector<QPointF> &pts, QPointF p1, QPointF p2, double bulge) {
    QPointF center;
    double radius = 0.0, startAngle = 0.0, endAngle = 0.0;
    if (!bulgeToArc(p1, p2, bulge, center, radius, startAngle, endAngle)) {
        pts.push_back(p2);
        return;
    }
    // Signed: negative for a clockwise (negative-bulge) sweep. The sample
    // loop below still lands exactly on endAngle (hence p2) at i==segments
    // regardless of sign.
    const double sweep = endAngle - startAngle;
    const int segments = std::clamp(static_cast<int>(std::ceil(std::abs(sweep) / (M_PI / 24.0))), 2, 64);
    for (int i = 1; i <= segments; ++i) {
        const double t = startAngle + sweep * i / segments;
        pts.emplace_back(center.x() + radius * std::cos(t), center.y() + radius * std::sin(t));
    }
}

// Appends a filled straight trapezoid from p1 to p2 -- half-width w1/2 at
// p1, w2/2 at p2 (w1==0 or w2==0 collapses that end to a point, the standard
// "wide polyline as arrowhead" technique, e.g. this project's own
// polyline_with_width_test.dwg) -- as its own closed subpath of `path`.
// Callers that need a curved (bulged) band pre-sample it into straight
// sub-chords first (see drawWidthAwarePolyline) rather than this function
// special-casing arcs itself, the same "sample to straight sub-chords"
// approach the rest of this file already uses for dashing an Arc/bulged
// Polyline (see the class comment on drawStroke).
void appendStraightBand(QPainterPath &path, QPointF p1, QPointF p2, double w1, double w2) {
    const double dx = p2.x() - p1.x();
    const double dy = p2.y() - p1.y();
    const double len = std::hypot(dx, dy);
    if (len < 1e-9) return;
    const QPointF perp(-dy / len, dx / len);
    const QPointF left1 = p1 + perp * (w1 / 2.0);
    const QPointF left2 = p2 + perp * (w2 / 2.0);
    const QPointF right2 = p2 - perp * (w2 / 2.0);
    const QPointF right1 = p1 - perp * (w1 / 2.0);
    path.moveTo(left1);
    path.lineTo(left2);
    path.lineTo(right2);
    path.lineTo(right1);
    path.closeSubpath();
}

// Strokes a polyline (pts.size() >= 2), optionally closed (an implicit last
// segment from pts.back() back to pts.front()). A solid dashPattern is the
// common case and stays a single QPainterPath / drawPath call; a dashed one
// is walked manually, alternating dash-length/gap-length from dashPattern
// (cyclically) as cumulative distance advances along the polyline, emitting
// a moveTo+lineTo subpath per "on" (dash) interval and nothing for "off"
// (gap) ones. This has to happen in document space (i.e. on the geometry
// before documentToScreen_ is applied) rather than via QPen::setDashPattern,
// because that pen-width-relative API doesn't have a well-defined document-
// space unit for our cosmetic (always-0-width) pens -- see the class
// comment on ViewerWidget::documentToScreen_.
void drawStroke(QPainter &painter, const std::vector<QPointF> &pts, bool closed,
                 const std::vector<double> &dashPattern) {
    if (pts.size() < 2) return;

    if (dashPattern.empty()) {
        QPainterPath path;
        path.moveTo(pts[0]);
        for (size_t i = 1; i < pts.size(); ++i) path.lineTo(pts[i]);
        if (closed) path.closeSubpath();
        painter.drawPath(path);
        return;
    }

    QPainterPath path;
    size_t patternIdx = 0;
    double remaining = dashPattern[0]; // distance left in the current dash/gap
    bool on = true;                    // dashPattern[0] is always a dash (see resolveEntityLineType)

    const size_t segCount = closed ? pts.size() : pts.size() - 1;
    for (size_t i = 0; i < segCount; ++i) {
        const QPointF a = pts[i];
        const QPointF b = pts[(i + 1) % pts.size()];
        const double segLen = QLineF(a, b).length();
        if (segLen < 1e-12) continue;
        double segPos = 0.0; // distance walked along this segment so far
        while (segPos < segLen) {
            const double step = std::min(remaining, segLen - segPos);
            const double t0 = segPos / segLen;
            const double t1 = (segPos + step) / segLen;
            if (on) {
                path.moveTo(a + (b - a) * t0);
                path.lineTo(a + (b - a) * t1);
            }
            segPos += step;
            remaining -= step;
            if (remaining <= 1e-9) {
                patternIdx = (patternIdx + 1) % dashPattern.size();
                remaining = dashPattern[patternIdx];
                on = !on;
            }
        }
    }
    painter.drawPath(path);
}

// Draws a Polyline Shape that carries per-segment width data
// (Shape::startWidths/endWidths), combining width tapering with the dash
// pattern in a single pass so a dashed *and* widthed polyline (a real
// combination -- see this project's own teste.dxf, which has LWPOLYLINEs on
// a "Tracejada" (dashed) layer that also carry a nonzero constant width)
// renders dashed filled bands, rather than either a solid bar (width
// honored, dash ignored) or a thin dashed line (dash honored, width
// ignored). Both the dash phase and the width taper are carried as one
// continuous distance parameter across the whole polyline -- same reasoning
// as drawStroke()'s dash phase staying continuous across vertices, extended
// here to also keep the width taper continuous across a dash on/off
// boundary. A zero-width dash "on" interval still draws as a plain cosmetic
// line (not a degenerate zero-area fill), so a polyline mixing thin and
// wide segments (e.g. a thin shaft with one wide tapered arrowhead segment)
// keeps its thin segments crisp.
void drawWidthAwarePolyline(QPainter &painter, const Shape &s) {
    const size_t n = s.points.size();
    const bool hasBulges = s.bulges.size() == n;
    const size_t segCount = s.closed ? n : n - 1;
    const std::vector<double> &dash = s.dashPattern;

    QPainterPath strokePath; // zero-width dash "on" intervals
    QPainterPath fillPath;   // nonzero-width dash "on" intervals
    fillPath.setFillRule(Qt::WindingFill);

    size_t patternIdx = 0;
    double remaining = dash.empty() ? 0.0 : dash[0];
    bool on = true; // dash[0] is always a dash, see resolveEntityLineType

    for (size_t i = 0; i < segCount; ++i) {
        const QPointF p1(s.points[i].x, s.points[i].y);
        const QPointF p2(s.points[(i + 1) % n].x, s.points[(i + 1) % n].y);
        const double bulge = hasBulges ? s.bulges[i] : 0.0;
        const double w1 = s.startWidths[i];
        const double w2 = s.endWidths[i];

        std::vector<QPointF> pts{p1};
        sampleSegmentPoints(pts, p1, p2, bulge);
        const int subCount = static_cast<int>(pts.size()) - 1;

        for (int k = 0; k < subCount; ++k) {
            const QPointF a = pts[k];
            const QPointF b = pts[k + 1];
            const double segLen = QLineF(a, b).length();
            if (segLen < 1e-12) continue;
            // Width at the endpoints of this straight sub-chord, lerped
            // from the segment's own w1/w2 by the sub-chord's position
            // within the (possibly arc-sampled) segment.
            const double ta = static_cast<double>(k) / subCount;
            const double tb = static_cast<double>(k + 1) / subCount;
            const double wa = w1 + (w2 - w1) * ta;
            const double wb = w1 + (w2 - w1) * tb;

            double segPos = 0.0;
            while (segPos < segLen) {
                const double step = dash.empty() ? segLen : std::min(remaining, segLen - segPos);
                const double u0 = segPos / segLen;
                const double u1 = (segPos + step) / segLen;
                if (on) {
                    const QPointF sa = a + (b - a) * u0;
                    const QPointF sb = a + (b - a) * u1;
                    const double swid = wa + (wb - wa) * u0;
                    const double ewid = wa + (wb - wa) * u1;
                    if (swid == 0.0 && ewid == 0.0) {
                        strokePath.moveTo(sa);
                        strokePath.lineTo(sb);
                    } else {
                        appendStraightBand(fillPath, sa, sb, swid, ewid);
                    }
                }
                segPos += step;
                if (dash.empty()) break;
                remaining -= step;
                if (remaining <= 1e-9) {
                    patternIdx = (patternIdx + 1) % dash.size();
                    remaining = dash[patternIdx];
                    on = !on;
                }
            }
        }
    }

    if (!strokePath.isEmpty()) painter.drawPath(strokePath);
    if (!fillPath.isEmpty()) painter.fillPath(fillPath, QBrush(QColor(s.color.r, s.color.g, s.color.b)));
}

// Samples one HatchLoop into a closed point ring, reusing the same
// bulge->arc sampling as the Polyline case (a hatch loop is the same
// points+bulges representation, just always implicitly closed).
std::vector<QPointF> sampleHatchLoop(const HatchLoop &loop) {
    const size_t n = loop.points.size();
    std::vector<QPointF> pts;
    if (n < 2) return pts;
    const bool hasBulges = loop.bulges.size() == n;
    pts.reserve(n);
    pts.emplace_back(loop.points[0].x, loop.points[0].y);
    for (size_t i = 1; i < n; ++i) {
        sampleSegmentPoints(pts, QPointF(loop.points[i - 1].x, loop.points[i - 1].y),
                            QPointF(loop.points[i].x, loop.points[i].y),
                            hasBulges ? loop.bulges[i - 1] : 0.0);
    }
    sampleSegmentPoints(pts, QPointF(loop.points[n - 1].x, loop.points[n - 1].y),
                        QPointF(loop.points[0].x, loop.points[0].y),
                        hasBulges ? loop.bulges[n - 1] : 0.0);
    return pts;
}

// Builds the filled region for a hatch: the union of all its boundary
// loops under an even-odd fill rule, so an island loop automatically reads
// as a hole regardless of which winding direction either loop happens to
// use -- AutoCAD/LibreCAD don't guarantee loop winding for HATCH boundary
// data the way a well-formed nonzero-rule polygon set would need.
QPainterPath buildHatchPath(const Shape &s) {
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    for (const HatchLoop &loop : s.hatchLoops) {
        const std::vector<QPointF> pts = sampleHatchLoop(loop);
        if (pts.size() < 2) continue;
        QPainterPath sub;
        sub.moveTo(pts[0]);
        for (size_t i = 1; i < pts.size(); ++i) sub.lineTo(pts[i]);
        sub.closeSubpath();
        path.addPath(sub);
    }
    return path;
}

// Draws one HATCH pattern definition line's full family of repeats
// (base point + k*offset, for every k whose line crosses `bounds`) as
// infinite-looking rays -- actual visibility is left entirely to the
// caller's clip (see the Pattern case in ViewerWidget::paintEvent), so
// this only needs to reach past `bounds` in both directions, not compute
// exact polygon intersections itself.
void drawHatchPatternLine(QPainter &painter, const HatchPatternLine &pl, const QRectF &bounds) {
    const QPointF dir(std::cos(pl.angleRad), std::sin(pl.angleRad));
    const QPointF perp(-dir.y(), dir.x());
    const QPointF base(pl.basePoint.x, pl.basePoint.y);
    const QPointF offset(pl.offset.x, pl.offset.y);

    // Decompose the repeat offset into perpendicular pitch (which row a
    // repeat lands on) and along-line phase shift (dash-pattern stagger
    // between rows) -- both are dot products against the unit dir/perp
    // axes above.
    const double spacing = QPointF::dotProduct(offset, perp);
    const double phaseShift = QPointF::dotProduct(offset, dir);
    if (std::abs(spacing) < 1e-9) return; // degenerate pattern data -- nothing sane to repeat

    const QPointF corners[4] = {bounds.topLeft(), bounds.topRight(), bounds.bottomLeft(), bounds.bottomRight()};
    double minPerp = std::numeric_limits<double>::max(), maxPerp = std::numeric_limits<double>::lowest();
    double minAlong = std::numeric_limits<double>::max(), maxAlong = std::numeric_limits<double>::lowest();
    for (const QPointF &c : corners) {
        const QPointF rel = c - base;
        minPerp = std::min(minPerp, QPointF::dotProduct(rel, perp));
        maxPerp = std::max(maxPerp, QPointF::dotProduct(rel, perp));
        minAlong = std::min(minAlong, QPointF::dotProduct(rel, dir));
        maxAlong = std::max(maxAlong, QPointF::dotProduct(rel, dir));
    }

    int kMin = static_cast<int>(std::floor(minPerp / spacing)) - 1;
    int kMax = static_cast<int>(std::ceil(maxPerp / spacing)) + 1;
    if (kMin > kMax) std::swap(kMin, kMax);
    if (static_cast<std::int64_t>(kMax) - kMin > 100000) return; // corrupt/degenerate spacing -- bail rather than hang

    // Raw DXF code-49 values: positive=dash, negative=gap, 0=dot (sized
    // relative to the pattern's own total length, same reasoning as
    // DwgDocument::resolveEntityLineType's dotLen).
    double patternTotal = 0.0;
    for (double d : pl.dashPattern) patternTotal += std::abs(d);
    const double dotLen = std::max(patternTotal * 0.02, 1e-6);

    QPainterPath path;
    for (int k = kMin; k <= kMax; ++k) {
        const QPointF rowBase = base + perp * (k * spacing);
        const QPointF p0 = rowBase + dir * minAlong;
        const QPointF p1 = rowBase + dir * maxAlong;
        const double segLen = maxAlong - minAlong;
        if (segLen <= 1e-9) continue;

        if (pl.dashPattern.empty()) {
            path.moveTo(p0);
            path.lineTo(p1);
            continue;
        }

        // Phase: distance from p0 to this row's own base point (base +
        // k*offset), projected along dir, wrapped into [0, patternTotal).
        const double rowBaseAlong = k * phaseShift; // relative to `base`'s own along-position
        double phase = std::fmod(minAlong - rowBaseAlong, patternTotal);
        if (phase < 0.0) phase += patternTotal;

        size_t idx = 0;
        double acc = 0.0;
        for (; idx + 1 < pl.dashPattern.size(); ++idx) {
            const double len = pl.dashPattern[idx] == 0.0 ? dotLen : std::abs(pl.dashPattern[idx]);
            if (phase < acc + len) break;
            acc += len;
        }
        double remaining = acc + (pl.dashPattern[idx] == 0.0 ? dotLen : std::abs(pl.dashPattern[idx])) - phase;
        bool on = pl.dashPattern[idx] >= 0.0;

        double pos = 0.0;
        while (pos < segLen) {
            const double step = std::min(remaining, segLen - pos);
            if (on) {
                path.moveTo(p0 + dir * pos);
                path.lineTo(p0 + dir * (pos + step));
            }
            pos += step;
            remaining -= step;
            if (remaining <= 1e-9) {
                idx = (idx + 1) % pl.dashPattern.size();
                remaining = pl.dashPattern[idx] == 0.0 ? dotLen : std::abs(pl.dashPattern[idx]);
                on = pl.dashPattern[idx] >= 0.0;
            }
        }
    }
    painter.drawPath(path);
}

// --- Resource directories ---------------------------------------------------
// Both resources/fonts (.lff) and resources/patterns (.dxf) ship next to the
// executable rather than as Qt resources (see CMakeLists.txt's post-build
// copy), and are looked up by lowercased file stem.

// Lowercased stem -> absolute path of every file in `dir` matching
// `nameFilter`. Empty if `dir` doesn't exist.
std::unordered_map<std::string, QString> indexDirByStem(const QString &dir, const QString &nameFilter) {
    std::unordered_map<std::string, QString> m;
    const QFileInfoList entries = QDir(dir).entryInfoList(QStringList{nameFilter}, QDir::Files);
    for (const QFileInfo &fi : entries) {
        m[fi.completeBaseName().toLower().toStdString()] = fi.absoluteFilePath();
    }
    return m;
}

// --- Hatch pattern library --------------------------------------------------
// A HATCH that names a pattern but carries no definition lines of its own
// (every DWG hatch, see Shape::hatchPatternName) is drawn from
// resources/patterns/<name>.dxf. Each file is LibreCAD's pattern format: one
// small drawing (LINE/ARC/CIRCLE/LWPOLYLINE, occasionally solid HATCH dots)
// whose own bounding box is the tile that repeats in both directions -- the
// files' $EXTMIN/$EXTMAX header values are unreliable (several carry a
// +/-1e20 "unset" sentinel), so the pitch is measured from the geometry.

// One pattern file, flattened to two paths in tile space (unscaled, unrotated,
// tile origin wherever the file drew it). Per-entity colors/linetypes in the
// pattern file are deliberately ignored: the whole tile takes the using
// hatch's color and strokes solid.
struct HatchTile {
    QPainterPath strokes;
    QPainterPath fills; // solid-filled regions (e.g. the dots in gost_*.dxf)
    QRectF bounds;      // the repeat cell; width/height are the pitch
    int elementCount = 0;
};

const QString &hatchPatternsDir() {
    static const QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/resources/patterns");
    return dir;
}

std::shared_ptr<const HatchTile> loadHatchTile(const QString &path) {
    DwgDocument doc;
    if (!doc.loadFile(path.toStdString())) return nullptr;

    auto tile = std::make_shared<HatchTile>();
    for (const Shape &s : doc.shapes()) {
        switch (s.kind) {
            case ShapeKind::Line:
                if (s.points.size() != 2) break;
                tile->strokes.moveTo(s.points[0].x, s.points[0].y);
                tile->strokes.lineTo(s.points[1].x, s.points[1].y);
                break;
            case ShapeKind::Circle:
                tile->strokes.addEllipse(QPointF(s.center.x, s.center.y), s.radius, s.radius);
                break;
            case ShapeKind::Arc: {
                // Same cos/sin sampling as paintEvent's Arc case (not
                // QPainterPath::arcTo -- see CLAUDE.md on Qt's angle convention).
                const double start = s.startAngleRad;
                double end = s.endAngleRad;
                if (end < start) end += 2 * M_PI;
                constexpr int kSegments = 48;
                for (int i = 0; i <= kSegments; ++i) {
                    const double t = start + (end - start) * i / kSegments;
                    const QPointF p(s.center.x + s.radius * std::cos(t), s.center.y + s.radius * std::sin(t));
                    if (i == 0) tile->strokes.moveTo(p); else tile->strokes.lineTo(p);
                }
                break;
            }
            case ShapeKind::Polyline: {
                const size_t n = s.points.size();
                if (n < 2) break;
                const bool hasBulges = s.bulges.size() == n;
                std::vector<QPointF> pts;
                pts.emplace_back(s.points[0].x, s.points[0].y);
                for (size_t i = 1; i < n; ++i) {
                    sampleSegmentPoints(pts, QPointF(s.points[i - 1].x, s.points[i - 1].y),
                                        QPointF(s.points[i].x, s.points[i].y), hasBulges ? s.bulges[i - 1] : 0.0);
                }
                if (s.closed) {
                    sampleSegmentPoints(pts, QPointF(s.points[n - 1].x, s.points[n - 1].y),
                                        QPointF(s.points[0].x, s.points[0].y), hasBulges ? s.bulges[n - 1] : 0.0);
                }
                tile->strokes.moveTo(pts[0]);
                for (size_t i = 1; i < pts.size(); ++i) tile->strokes.lineTo(pts[i]);
                break;
            }
            case ShapeKind::Hatch:
                if (s.hatchFillKind == Shape::HatchFillKind::Solid) tile->fills.addPath(buildHatchPath(s));
                break;
            case ShapeKind::Text:
                break; // no pattern uses text
        }
    }

    const bool hasStrokes = !tile->strokes.isEmpty(), hasFills = !tile->fills.isEmpty();
    if (!hasStrokes && !hasFills) return nullptr;
    tile->bounds = !hasFills ? tile->strokes.boundingRect()
                 : !hasStrokes ? tile->fills.boundingRect()
                               : tile->strokes.boundingRect().united(tile->fills.boundingRect());
    if (tile->bounds.width() < 1e-9 || tile->bounds.height() < 1e-9) return nullptr; // no 2D repeat cell
    tile->elementCount = tile->strokes.elementCount() + tile->fills.elementCount();
    return tile;
}

// Resolves a HATCH pattern name (e.g. "ANSI31") to a loaded, cached tile --
// nullptr for a name with no file in resources/patterns (a "_USER" pattern,
// an AutoCAD-only name, ...), which the caller draws as nothing. The cache
// stores nullptr results too, so an unknown name doesn't re-hit the disk on
// every repaint.
std::shared_ptr<const HatchTile> hatchTileFor(const std::string &patternName) {
    static const std::unordered_map<std::string, QString> index =
        indexDirByStem(hatchPatternsDir(), QStringLiteral("*.dxf"));
    static std::unordered_map<std::string, std::shared_ptr<const HatchTile>> cache;

    const std::string key = QString::fromStdString(patternName).trimmed().toLower().toStdString();
    if (auto cached = cache.find(key); cached != cache.end()) return cached->second;

    std::shared_ptr<const HatchTile> tile;
    if (auto it = index.find(key); it != index.end()) tile = loadHatchTile(it->second);
    cache[key] = tile;
    return tile;
}

// Tiles `s`'s library pattern across `boundary` (already the hatch's
// even-odd path, in document space). `painter` must currently carry
// `documentToScreen`; it's restored on return.
//
// Tile (i, j) is the pattern file's own drawing shifted by (i*w, j*h), all
// under scale (Shape::hatchPatternScale) -> rotate (hatchPatternAngleRad) ->
// translate (hatchPatternOrigin) into document space. Only the tiles that
// overlap the visible part of the boundary are drawn.
void drawLibraryHatchPattern(QPainter &painter, const Shape &s, const QPainterPath &boundary,
                             const QTransform &documentToScreen, const QRect &viewport) {
    const std::shared_ptr<const HatchTile> tile = hatchTileFor(s.hatchPatternName);
    if (!tile || !(s.hatchPatternScale > 0.0)) return;

    bool invertible = false;
    const QTransform screenToDocument = documentToScreen.inverted(&invertible);
    if (!invertible) return;
    const QRectF visible = boundary.boundingRect().intersected(screenToDocument.mapRect(QRectF(viewport)));
    if (visible.isEmpty()) return;

    QTransform tileToDocument;
    tileToDocument.translate(s.hatchPatternOrigin.x, s.hatchPatternOrigin.y);
    tileToDocument.rotateRadians(s.hatchPatternAngleRad);
    tileToDocument.scale(s.hatchPatternScale, s.hatchPatternScale);
    const QRectF needed = tileToDocument.inverted().mapRect(visible);

    const double w = tile->bounds.width(), h = tile->bounds.height();

    // A pattern too fine to resolve (tiles a couple of pixels wide, or so
    // many strokes it would stall the repaint) reads as a solid fill anyway
    // -- which is also what AutoCAD does when a hatch is too dense. The
    // pitch check comes first: past it, the tile indices below span at most
    // the viewport / kMinPitchPx, so they can't overflow an int (a hatch
    // with a near-zero scale would otherwise ask for billions of tiles).
    constexpr double kMinPitchPx = 3.0;
    constexpr std::int64_t kMaxElements = 400000;
    const double pixelsPerUnit = std::hypot(documentToScreen.m11(), documentToScreen.m12());
    const double pitchPx = std::min(w, h) * s.hatchPatternScale * pixelsPerUnit;
    const QColor color(s.color.r, s.color.g, s.color.b);
    if (!(pitchPx >= kMinPitchPx)) { // also catches NaN
        painter.fillPath(boundary, color);
        return;
    }

    const int i0 = static_cast<int>(std::floor((needed.left() - tile->bounds.left()) / w));
    const int i1 = static_cast<int>(std::floor((needed.right() - tile->bounds.left()) / w));
    const int j0 = static_cast<int>(std::floor((needed.top() - tile->bounds.top()) / h));
    const int j1 = static_cast<int>(std::floor((needed.bottom() - tile->bounds.top()) / h));
    const std::int64_t tileCount = static_cast<std::int64_t>(i1 - i0 + 1) * (j1 - j0 + 1);
    if (tileCount * tile->elementCount > kMaxElements) {
        painter.fillPath(boundary, color);
        return;
    }

    painter.save();
    painter.setClipPath(boundary, Qt::IntersectClip);
    for (int j = j0; j <= j1; ++j) {
        for (int i = i0; i <= i1; ++i) {
            painter.setTransform(QTransform::fromTranslate(i * w, j * h) * tileToDocument * documentToScreen);
            painter.drawPath(tile->strokes);
            if (!tile->fills.isEmpty()) painter.fillPath(tile->fills, color);
        }
    }
    painter.restore();
}

// --- LFF stroke-font resolution -------------------------------------------
// TEXT/MTEXT's actual font, per the file's own STYLE table (see
// DwgDocument::addTextStyle / Shape::fontFile), is looked up here rather
// than in DwgDocument since finding/parsing a font *file on disk* is a
// rendering concern, not a document-model one -- dwg_document.h stays
// Qt/filesystem-free (see Shape::fontFile's comment).

// "<exe_dir>/resources/fonts" -- fonts ship next to the executable, not
// bundled as Qt resources, so they can be added/replaced without rebuilding.
const QString &lffFontsDir() {
    static const QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/resources/fonts");
    return dir;
}

// Lowercased font stem (no directory, no extension) -> absolute .lff path,
// built once from whatever's actually in resources/fonts/. Matching by
// stem alone (rather than exact filename) means the lookup works
// regardless of the filesystem's own case sensitivity, and regardless of
// which extension a STYLE table's font name carries.
const std::unordered_map<std::string, QString> &lffFontIndex() {
    static const std::unordered_map<std::string, QString> index =
        indexDirByStem(lffFontsDir(), QStringLiteral("*.lff"));
    return index;
}

// Strips any directory and extension from a STYLE table font name (e.g.
// "romans.shx" -> "romans"), lowercased for lffFontIndex() lookup.
std::string lffFontStem(const std::string &fontFile) {
    const size_t slash = fontFile.find_last_of("/\\");
    std::string base = slash == std::string::npos ? fontFile : fontFile.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    std::transform(base.begin(), base.end(), base.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return base;
}

// Resolves a Shape::fontFile to a loaded, cached LffFont -- nullptr if
// `fontFile` is empty (no STYLE override known) or names a font this
// project has no shipped .lff for (e.g. a TTF style name like "Arial", or
// an SHX name with no bundled equivalent -- see CLAUDE.md). Callers treat
// nullptr identically: fall back to Qt's system font, matching this
// viewer's behavior before LFF support existed. The cache also stores
// nullptr results so an unresolvable style doesn't re-scan the directory
// index on every repaint.
std::shared_ptr<const LffFont> lffFontFor(const std::string &fontFile) {
    if (fontFile.empty()) return nullptr;
    static std::unordered_map<std::string, std::shared_ptr<const LffFont>> cache;
    const std::string stem = lffFontStem(fontFile);
    if (auto cached = cache.find(stem); cached != cache.end()) return cached->second;

    std::shared_ptr<const LffFont> font;
    if (auto it = lffFontIndex().find(stem); it != lffFontIndex().end()) {
        font = LffFont::loadFromFile(it->second.toStdString());
    }
    cache[stem] = font;
    return font;
}

// LibreCAD ships "unicode.lff" as a broad-coverage fallback for glyphs a
// narrower stroke font (most of resources/fonts/*.lff are Latin-only)
// doesn't define -- used below whenever the entity's own resolved font is
// missing a codepoint, before giving up and leaving a blank advance.
const LffFont *lffFallbackFont() {
    static const std::shared_ptr<const LffFont> fallback = lffFontFor("unicode.lff");
    return fallback.get();
}

// Looks up `cp` in `font`, then `fallback`, writing this character's
// advance (font design units, already includes `font`'s own LetterSpacing)
// into `advance` regardless of whether a glyph was found -- an undefined
// codepoint (most commonly a space, which no .lff glyph section defines)
// still needs to move the pen, via `font`'s WordSpacing, so later
// characters on the line don't overlap it.
const LffGlyph *lffStepGlyph(const LffFont &font, const LffFont *fallback, char32_t cp, double &advance) {
    const LffGlyph *glyph = font.findGlyph(cp);
    if (!glyph && fallback) glyph = fallback->findGlyph(cp);
    advance = (glyph ? glyph->advance : font.wordSpacing) + font.letterSpacing;
    return glyph;
}

double lffLineWidth(const LffFont &font, const LffFont *fallback, const QString &line) {
    double width = 0.0;
    for (const uint cp : line.toUcs4()) {
        double advance = 0.0;
        lffStepGlyph(font, fallback, static_cast<char32_t>(cp), advance);
        width += advance;
    }
    return width;
}

// AutoCAD/LibreCAD's MTEXT line-spacing-factor 1.0 corresponds to roughly
// 5/3 of the text height between baselines ("exact" spacing) -- there's no
// per-font metric for this in the .lff format itself (unlike LetterSpacing/
// WordSpacing), so this is a fixed approximation shared by every LFF font,
// same spirit as this project's other documented "reasonable
// approximation" choices (see CLAUDE.md's HATCH gradient note).
constexpr double kLffLineSpacingRatio = 5.0 / 3.0;

// Draws `lines` (already split on '\n') using `font` (falling back to
// `fallback` per-glyph, see lffStepGlyph) into the QPainter's *current*
// local transform -- caller has already translated/rotated to the text
// entity's anchor exactly like the QFont path below, so this only needs to
// place glyphs relative to that origin. `capHeightPx` is the on-screen
// pixel size of the font's 9-design-unit cap height (see LffGlyph's
// comment), the LFF equivalent of the QFont path's pixelHeight.
void drawLffTextLines(QPainter &painter, const QStringList &lines, const LffFont &font,
                       const LffFont *fallback, double capHeightPx, TextHAlign hAlign, TextVAlign vAlign) {
    const double scale = capHeightPx / 9.0;
    const double linePitchPx = capHeightPx * font.lineSpacingFactor * kLffLineSpacingRatio;
    const double blockHeight = linePitchPx * lines.size();

    double firstBaselineY;
    switch (vAlign) {
        case TextVAlign::Top:      firstBaselineY = capHeightPx; break;
        case TextVAlign::Middle:   firstBaselineY = capHeightPx - blockHeight / 2.0; break;
        case TextVAlign::Bottom:   firstBaselineY = capHeightPx - blockHeight; break;
        case TextVAlign::Baseline: default: firstBaselineY = 0.0;
    }

    double y = firstBaselineY;
    for (const QString &line : lines) {
        double startX = 0.0;
        if (hAlign == TextHAlign::Center) startX = -lffLineWidth(font, fallback, line) * scale / 2.0;
        else if (hAlign == TextHAlign::Right) startX = -lffLineWidth(font, fallback, line) * scale;

        double penX = 0.0;
        for (const uint rawCp : line.toUcs4()) {
            const char32_t cp = static_cast<char32_t>(rawCp);
            double advance = 0.0;
            const LffGlyph *glyph = lffStepGlyph(font, fallback, cp, advance);
            if (glyph) {
                // Glyph coordinates are Y-up (baseline at y=0, caps extend
                // to y=+9, see LffGlyph's comment) but this local transform
                // still follows QPainter's own Y-down screen convention
                // (only documentToScreen_ carries a flip, and Text
                // deliberately never composes with it -- see CLAUDE.md
                // point #6) -- so each point's Y must be negated here, the
                // same "flip glyphs explicitly, don't inherit one" rule
                // that convention already calls out.
                for (const std::vector<Point2D> &stroke : glyph->strokes) {
                    for (size_t i = 1; i < stroke.size(); ++i) {
                        const Point2D &a = stroke[i - 1];
                        const Point2D &b = stroke[i];
                        painter.drawLine(QPointF(startX + (penX + a.x) * scale, y - a.y * scale),
                                          QPointF(startX + (penX + b.x) * scale, y - b.y * scale));
                    }
                }
            }
            penX += advance;
        }
        y += linePitchPx;
    }
}
} // namespace

ViewerWidget::ViewerWidget(QWidget *parent) : QWidget(parent) {
    setMinimumSize(200, 200);
    setMouseTracking(false);
}

void ViewerWidget::setDocument(DwgDocument doc) {
    document_ = std::move(doc);
    hasFitOnce_ = false;
    zoomFit();
    update();
}

void ViewerWidget::zoomFit() {
    const BoundingBox &bbox = document_.boundingBox();
    if (!bbox.isValid()) return;

    constexpr double marginPx = 20.0;
    const double availW = std::max(1.0, width() - 2 * marginPx);
    const double availH = std::max(1.0, height() - 2 * marginPx);

    double bboxW = bbox.maxX - bbox.minX;
    double bboxH = bbox.maxY - bbox.minY;
    // Guard degenerate drawings (a single point, or a perfectly
    // horizontal/vertical line) so we don't divide by zero.
    if (bboxW < 1e-9) bboxW = bboxH > 1e-9 ? bboxH : 1.0;
    if (bboxH < 1e-9) bboxH = bboxW;

    const double scale = std::min(availW / bboxW, availH / bboxH);
    const double offsetX = marginPx + (availW - bboxW * scale) / 2.0;
    const double offsetY = marginPx + (availH + bboxH * scale) / 2.0;

    QTransform t;
    t.translate(offsetX, offsetY);
    t.scale(scale, -scale); // flip Y: drawing is Y-up, screen is Y-down
    t.translate(-bbox.minX, -bbox.minY);
    documentToScreen_ = t;
    hasFitOnce_ = true;
}

void ViewerWidget::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    // Simple policy for this scaffold: always re-fit on resize. A real
    // viewer would preserve the user's current pan/zoom instead.
    zoomFit();
}

void ViewerWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    // Dark canvas, matching LibreCAD's default drawing-view background
    // (RS_Settings::BACKGROUND == "Black").
    painter.fillRect(rect(), Qt::black);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (document_.shapes().empty() || !hasFitOnce_) {
        painter.setPen(Qt::gray);
        painter.drawText(rect(), Qt::AlignCenter, "No drawing loaded");
        return;
    }

    QPen pen;
    pen.setWidth(0); // cosmetic: always 1 device pixel, regardless of zoom
    painter.setTransform(documentToScreen_);

    for (const Shape &s : document_.shapes()) {
        pen.setColor(QColor(s.color.r, s.color.g, s.color.b));
        painter.setPen(pen);
        switch (s.kind) {
            case ShapeKind::Line: {
                if (s.points.size() != 2) break;
                const QPointF p0(s.points[0].x, s.points[0].y);
                const QPointF p1(s.points[1].x, s.points[1].y);
                if (s.dashPattern.empty()) {
                    painter.drawLine(p0, p1); // fast path: overwhelming majority of entities are solid
                } else {
                    drawStroke(painter, {p0, p1}, false, s.dashPattern);
                }
                break;
            }
            case ShapeKind::Circle: {
                if (s.dashPattern.empty()) {
                    painter.drawEllipse(QPointF(s.center.x, s.center.y), s.radius, s.radius); // exact, no sampling
                    break;
                }
                // Same sampling density as the Arc case: 48 segments over a
                // full 2*pi sweep is that case's "segments per radian" rate.
                constexpr int kSegments = 48;
                std::vector<QPointF> pts;
                pts.reserve(kSegments);
                for (int i = 0; i < kSegments; ++i) {
                    const double t = 2 * M_PI * i / kSegments;
                    pts.emplace_back(s.center.x + s.radius * std::cos(t), s.center.y + s.radius * std::sin(t));
                }
                drawStroke(painter, pts, /*closed=*/true, s.dashPattern);
                break;
            }
            case ShapeKind::Arc: {
                // Deliberately NOT using QPainter::drawArc(): its angle
                // convention is defined in the painter's local coordinate
                // system, and under our Y-flipped transform (scale_y < 0)
                // that silently mirrors which half of the circle gets
                // drawn. Sampling points with plain trigonometry uses the
                // exact same (x, y) math already validated by the line
                // and circle cases above, so there's no separate
                // convention to get wrong.
                double start = s.startAngleRad;
                double end = s.endAngleRad;
                if (end < start) end += 2 * M_PI;
                constexpr int kSegments = 48;
                std::vector<QPointF> pts;
                pts.reserve(kSegments + 1);
                for (int i = 0; i <= kSegments; ++i) {
                    const double t = start + (end - start) * i / kSegments;
                    pts.emplace_back(s.center.x + s.radius * std::cos(t), s.center.y + s.radius * std::sin(t));
                }
                drawStroke(painter, pts, /*closed=*/false, s.dashPattern);
                break;
            }
            case ShapeKind::Polyline: {
                if (s.points.size() < 2) break;
                const size_t n = s.points.size();
                const bool hasBulges = s.bulges.size() == n;
                const bool hasWidths = s.startWidths.size() == n && s.endWidths.size() == n;

                if (!hasWidths) {
                    std::vector<QPointF> pts;
                    pts.reserve(n);
                    pts.emplace_back(s.points[0].x, s.points[0].y);
                    for (size_t i = 1; i < n; ++i) {
                        sampleSegmentPoints(pts, QPointF(s.points[i - 1].x, s.points[i - 1].y),
                                            QPointF(s.points[i].x, s.points[i].y),
                                            hasBulges ? s.bulges[i - 1] : 0.0);
                    }
                    if (s.closed) {
                        sampleSegmentPoints(pts, QPointF(s.points[n - 1].x, s.points[n - 1].y),
                                            QPointF(s.points[0].x, s.points[0].y),
                                            hasBulges ? s.bulges[n - 1] : 0.0);
                    }
                    // The closing edge, if any, is already sampled into pts
                    // above (correctly, as an arc when it has a bulge) --
                    // pass closed=false so drawStroke doesn't also add its
                    // own implicit (always-straight) closing segment on top.
                    drawStroke(painter, pts, /*closed=*/false, s.dashPattern);
                    break;
                }

                drawWidthAwarePolyline(painter, s);
                break;
            }
            case ShapeKind::Text: {
                if (s.text.empty()) break;

                // documentToScreen_'s linear part is always a uniform scale
                // with a Y flip (zoomFit()/wheelEvent() only ever scale by
                // (k, -k)), so m11() alone gives the current doc-units ->
                // pixels scale.
                const double pixelsPerUnit = std::abs(documentToScreen_.m11());
                if (pixelsPerUnit <= 0.0) break;

                // Drawing glyphs through documentToScreen_ directly would
                // mirror them (same class of bug as QPainter::drawArc()
                // noted above): the transform's Y flip that keeps polylines
                // reading correctly turns readable letterforms backwards.
                // So each text shape gets its own screen-space transform,
                // built from scratch (translate + rotate(-angle) + uniform
                // positive scale, no flip) instead of composing with
                // documentToScreen_.
                const QPointF originScreen =
                    documentToScreen_.map(QPointF(s.center.x, s.center.y));
                double capHeightPx = s.textHeightDoc * pixelsPerUnit;
                if (capHeightPx < 1.0) capHeightPx = 1.0;

                const QStringList lines = QString::fromUtf8(s.text.c_str()).split(QLatin1Char('\n'));

                painter.save();
                painter.resetTransform();
                painter.translate(originScreen);
                painter.rotate(-s.textAngleRad * 180.0 / M_PI);
                // Width factor (DXF/DWG code 41) stretches/condenses glyphs
                // along the text's own reading direction only, never its
                // height -- applied here, after the rotate, so it scales the
                // local (already-rotated) x-axis rather than document X.
                // Cosmetic (always-0-width) pens stay constant-width in
                // device pixels under a non-uniform QPainter scale, so this
                // doesn't distort LFF glyph stroke thickness. A malformed
                // file with a non-positive width factor falls back to 1.0
                // rather than drawing zero-width or mirrored text.
                const double widthFactor = s.textWidthFactor > 0.0 ? s.textWidthFactor : 1.0;
                painter.scale(widthFactor, 1.0);

                // Only entities whose STYLE table names a font this project
                // actually ships a .lff for take this path (see
                // lffFontFor) -- everything else (no STYLE override, or one
                // naming e.g. a TTF font) falls through to the Qt path
                // below exactly as before LFF support existed.
                if (const std::shared_ptr<const LffFont> lffFont = lffFontFor(s.fontFile)) {
                    drawLffTextLines(painter, lines, *lffFont, lffFallbackFont(), capHeightPx,
                                      s.textHAlign, s.textVAlign);
                } else {
                    QFont font = painter.font();
                    font.setPixelSize(static_cast<int>(std::round(capHeightPx)));
                    QFontMetricsF metrics(font);

                    const double linePitch = metrics.height();
                    const double blockHeight = linePitch * lines.size();

                    double firstBaselineY;
                    switch (s.textVAlign) {
                        case TextVAlign::Top:      firstBaselineY = metrics.ascent(); break;
                        case TextVAlign::Middle:   firstBaselineY = metrics.ascent() - blockHeight / 2.0; break;
                        case TextVAlign::Bottom:   firstBaselineY = metrics.ascent() - blockHeight; break;
                        case TextVAlign::Baseline: default: firstBaselineY = 0.0; break;
                    }

                    painter.setFont(font);
                    double y = firstBaselineY;
                    for (const QString &line : lines) {
                        double x = 0.0;
                        if (s.textHAlign == TextHAlign::Center) {
                            x = -metrics.horizontalAdvance(line) / 2.0;
                        } else if (s.textHAlign == TextHAlign::Right) {
                            x = -metrics.horizontalAdvance(line);
                        }
                        painter.drawText(QPointF(x, y), line);
                        y += linePitch;
                    }
                }
                painter.restore();
                break;
            }
            case ShapeKind::Hatch: {
                if (s.hatchLoops.empty()) break;
                const QPainterPath path = buildHatchPath(s);
                if (path.isEmpty()) break;

                switch (s.hatchFillKind) {
                    case Shape::HatchFillKind::Solid:
                        painter.fillPath(path, QColor(s.color.r, s.color.g, s.color.b));
                        break;
                    case Shape::HatchFillKind::Gradient: {
                        const QRectF bounds = path.boundingRect();
                        const double halfDiag = 0.5 * std::hypot(bounds.width(), bounds.height());
                        if (halfDiag < 1e-9) {
                            painter.fillPath(path, QColor(s.color.r, s.color.g, s.color.b));
                            break;
                        }
                        const QPointF center = bounds.center();
                        const QPointF dir(std::cos(s.hatchGradientAngleRad), std::sin(s.hatchGradientAngleRad));
                        QLinearGradient grad(center - dir * halfDiag, center + dir * halfDiag);
                        grad.setColorAt(0.0, QColor(s.color.r, s.color.g, s.color.b));
                        grad.setColorAt(1.0, QColor(s.hatchColor2.r, s.hatchColor2.g, s.hatchColor2.b));
                        painter.fillPath(path, QBrush(grad));
                        break;
                    }
                    case Shape::HatchFillKind::Pattern: {
                        if (s.hatchPatternLines.empty()) {
                            // No lines in the file itself -- use the named
                            // pattern from resources/patterns, if any.
                            if (!s.hatchPatternName.empty()) {
                                drawLibraryHatchPattern(painter, s, path, documentToScreen_, rect());
                            }
                            break;
                        }
                        painter.save();
                        painter.setClipPath(path, Qt::IntersectClip);
                        const QRectF bounds = path.boundingRect();
                        for (const HatchPatternLine &pl : s.hatchPatternLines) {
                            drawHatchPatternLine(painter, pl, bounds);
                        }
                        painter.restore();
                        break;
                    }
                }
                break;
            }
        }
    }
}

void ViewerWidget::wheelEvent(QWheelEvent *event) {
    if (!hasFitOnce_) return;

    const double factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
    const QPointF cursorScreen = event->position();
    const QPointF cursorDoc = documentToScreen_.inverted().map(cursorScreen);

    // Rescale around the cursor: keep the drawing point under the cursor
    // fixed on screen while the zoom level changes.
    QTransform t = documentToScreen_;
    t.translate(cursorDoc.x(), cursorDoc.y());
    t.scale(factor, factor);
    t.translate(-cursorDoc.x(), -cursorDoc.y());
    documentToScreen_ = t;

    update();
    event->accept();
}

void ViewerWidget::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::MiddleButton) {
        panning_ = true;
        lastMousePos_ = event->pos();
        setCursor(Qt::ClosedHandCursor);
    }
}

void ViewerWidget::mouseMoveEvent(QMouseEvent *event) {
    if (!panning_) return;
    const QPoint delta = event->pos() - lastMousePos_;
    lastMousePos_ = event->pos();
    // Right-multiply: QTransform composes as a row-vector transform, where
    // (A * B).map(p) == B.map(A.map(p)) -- so A is applied FIRST. Putting
    // the new screen-space shift on the right (documentToScreen_ * Translate)
    // means a point is mapped through the existing document->screen
    // transform first and the raw pixel delta is added on top of that
    // result, giving an exact 1:1 screen-space follow regardless of the
    // current zoom scale. The previous left-multiplied order applied delta
    // *before* documentToScreen_ instead (in document space, pre-Y-flip),
    // which both inverted vertical drag direction (the flip landed after
    // the shift) and scaled pan speed by the current zoom factor (very
    // sluggish when zoomed out, since zoomFit's scale is usually << 1 for
    // a real drawing).
    documentToScreen_ = documentToScreen_ * QTransform::fromTranslate(delta.x(), delta.y());
    update();
}

void ViewerWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::MiddleButton) {
        panning_ = false;
        unsetCursor();
    }
}
