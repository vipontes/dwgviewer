#include "viewer_widget.h"

#include <QBrush>
#include <QColor>
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
#include <cmath>
#include <cstdint>
#include <limits>

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
                // above (correctly, as an arc when it has a bulge) -- pass
                // closed=false so drawStroke doesn't also add its own
                // implicit (always-straight) closing segment on top of it.
                drawStroke(painter, pts, /*closed=*/false, s.dashPattern);
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
                int pixelHeight = static_cast<int>(std::round(s.textHeightDoc * pixelsPerUnit));
                if (pixelHeight < 1) pixelHeight = 1;

                QFont font = painter.font();
                font.setPixelSize(pixelHeight);
                QFontMetricsF metrics(font);

                const QStringList lines = QString::fromUtf8(s.text.c_str()).split(QLatin1Char('\n'));
                const double linePitch = metrics.height();
                const double blockHeight = linePitch * lines.size();

                double firstBaselineY;
                switch (s.textVAlign) {
                    case TextVAlign::Top:      firstBaselineY = metrics.ascent(); break;
                    case TextVAlign::Middle:   firstBaselineY = metrics.ascent() - blockHeight / 2.0; break;
                    case TextVAlign::Bottom:   firstBaselineY = metrics.ascent() - blockHeight; break;
                    case TextVAlign::Baseline: default: firstBaselineY = 0.0; break;
                }

                painter.save();
                painter.resetTransform();
                painter.translate(originScreen);
                painter.rotate(-s.textAngleRad * 180.0 / M_PI);
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
