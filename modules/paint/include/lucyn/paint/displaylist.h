// lucyn/paint/displaylist.h - the dumb "draw this here" list from README's
// Rendering Pipeline / Painter interface section, with the small additions
// Session 11 decided on (SESSION11_PLAN.md Part B):
//   - an rgba colour on DrawRect/DrawText
//   - fill vs stroke on DrawRect (or use DrawBorder for a border-only box)
//   - a clip rectangle, so overflow:hidden can be expressed
// layout never draws: it builds a DisplayList. Exactly one backend
// (SoftwarePainter, for now) turns that into pixels. Colours are mostly
// ignored by LucyN's "looks like sh*t but renders" promise, but the painter
// still needs *some* colour to be readable: default is black text on a white
// background with 1px grey borders, so boxes are visible.
#pragma once
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace lucyn::paint {

// Plain RGBA, 0-255 per channel. Default is opaque black.
struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;

    static constexpr Color rgba(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_ = 255) {
        return Color{r_, g_, b_, a_};
    }
    static constexpr Color white() { return Color{255, 255, 255, 255}; }
    static constexpr Color black() { return Color{0, 0, 0, 255}; }
    static constexpr Color grey()  { return Color{128, 128, 128, 255}; }  // default border colour
    static constexpr Color transparent() { return Color{0, 0, 0, 0}; }
};

// A clip rectangle in the same coordinate space as the draw commands.
// hasClip == false on an item means "no clipping" (the common case).
struct ClipRect {
    float x = 0, y = 0, w = 0, h = 0;
};

struct DrawRect {
    float x = 0, y = 0, w = 0, h = 0;
    Color color = Color::white();
    bool  fill  = true;          // true: filled rect. false: stroke only (box outline)
    float strokeWidth = 1.0f;    // used only when fill == false
    bool     hasClip = false;
    ClipRect clip{};
};

// A border drawn as four strokes (top/right/bottom/left), used instead of
// DrawRect{fill:false} when per-side widths ever differ; for now layout emits
// one width for all four sides, same as the README Box struct.
struct DrawBorder {
    float x = 0, y = 0, w = 0, h = 0;
    float width = 1.0f;
    Color color = Color::grey();
    bool     hasClip = false;
    ClipRect clip{};
};

struct DrawText {
    float x = 0, y = 0;          // baseline-left origin, matches stb_truetype's convention
    std::string text;
    float size = 16.0f;          // pixel size (ascent+descent), not "pt"
    Color color = Color::black();
    bool     hasClip = false;
    ClipRect clip{};
};

// imageId indexes into a Painter-specific image table (see ImageTable in
// software_painter.h). -1 (or an id the table does not have) draws nothing:
// a broken image is a blank box, never a crash.
struct DrawImage {
    float x = 0, y = 0, w = 0, h = 0;
    int imageId = -1;
    bool     hasClip = false;
    ClipRect clip{};
};

using DisplayItem = std::variant<DrawRect, DrawBorder, DrawText, DrawImage>;
using DisplayList = std::vector<DisplayItem>;

}  // namespace lucyn::paint
