#pragma once

// Parses LibreCAD's ".lff" stroke-font format (plain text, one section per
// glyph keyed by a hex Unicode codepoint, each glyph a set of pen-up-
// separated strokes of line/arc segments) into glyph outlines ViewerWidget
// can stroke directly with QPainter -- see CLAUDE.md's arc/dash-pattern
// conventions for why this project always samples/strokes geometry itself
// rather than trusting a black-box text API's own rendering. Kept
// independent of Qt (same reasoning as dwg_document.h's Shape/Point2D) even
// though its only current caller is Qt code, since parsing a font file has
// nothing to do with painting one.

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "dwg_document.h" // Point2D

// One glyph's geometry: independent strokes (pen-up between them), each
// already flattened to a plain polyline in the font's own design units --
// an "Abulge" suffix on a raw .lff point (same tan(includedAngle/4)
// convention as Shape::bulges) means the segment ending at that point is an
// arc, sampled once here at load time rather than per-paint like
// ViewerWidget does for document geometry, since a font's glyphs are parsed
// once and reused for every occurrence of that character.
struct LffGlyph {
    std::vector<std::vector<Point2D>> strokes;

    // This glyph's own natural width (max X across all its flattened
    // points), NOT yet plus the font's LetterSpacing -- callers add that
    // separately between consecutive glyphs (see ViewerWidget's text
    // layout), matching how LibreCAD's own .lff renderer separates "how
    // wide is this glyph" from "how much gap goes between glyphs".
    double advance = 0.0;
};

// One parsed .lff font. Glyph coordinates are in the format's own design
// units, where a capital letter's cap height is normalized to 9 units --
// matches every sampled glyph in resources/fonts/iso.lff (e.g. 'A' spans
// y=[0,9], baseline at y=0) -- so callers scale by textHeightDoc / 9.0.
// Like Shape, this is deliberately a plain struct with public fields: there
// is no invariant to protect once a font is loaded, and every consumer
// needs direct access to letterSpacing/wordSpacing/lineSpacingFactor for
// text layout, not just glyph lookup.
struct LffFont {
    std::unordered_map<char32_t, LffGlyph> glyphs;

    // Header metadata read from the file's own "# LetterSpacing:" /
    // "# WordSpacing:" / "# LineSpacingFactor:" comment lines, already in
    // the same design units as glyph coordinates. Defaults match what a
    // font with no such comment (or with an unparseable value) should fall
    // back to.
    double letterSpacing = 3.0;
    double wordSpacing = 6.75;
    double lineSpacingFactor = 1.0;

    const LffGlyph *findGlyph(char32_t codepoint) const {
        auto it = glyphs.find(codepoint);
        return it == glyphs.end() ? nullptr : &it->second;
    }

    // Returns nullptr if `path` can't be opened or contains no parseable
    // glyph -- callers treat that identically to "no such font" and fall
    // back to whatever they'd otherwise use (see ViewerWidget::lffFontFor).
    static std::shared_ptr<const LffFont> loadFromFile(const std::string &path);
};
