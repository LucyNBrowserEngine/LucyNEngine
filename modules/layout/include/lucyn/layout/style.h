// lucyn/layout/style.h - inline `style=""` parsing (SESSION12_PLAN.md Part A).
//
// NOT a CSS tokenizer. It is the smallest thing that gives real CSS *semantics*
// for the handful of properties layout understands:
//
//   display, width, height, margin(-side), padding(-side), border / border-width,
//   position (static|relative|absolute), top/right/bottom/left, visibility,
//   z-index, overflow(-x/-y)
//   and, because LNM's first two properties need somewhere to land,
//   background-color / background, color, border-color (colours: see parseColor)
//
// Rules this parser follows (all deliberate, all tested):
//   - declarations are applied in SOURCE ORDER (so `margin:10px; margin-top:0`
//     and `margin-top:0; margin:10px` differ, as in real CSS); a later
//     declaration of the same property wins; `!important` beats any non-important
//     declaration regardless of order
//   - `/* comments */` are stripped; `;` inside quotes or parentheses does not split
//   - property names and values are matched case-insensitively
//   - a declaration whose value we cannot understand is IGNORED (the earlier value
//     or the UA default stays) - exactly what a real browser does with an invalid
//     declaration
//   - lengths are `px` only (a bare `0` is fine). `%`, `em`, `rem`, `calc()`, `var()`
//     are unsupported this session, so those declarations are ignored
//   - `margin: auto` is unsupported (ignored); `position: fixed|sticky` are
//     unsupported (ignored, the box stays `static`)
#pragma once
#include <lucyn/dom/dom.h>
#include <lucyn/paint/displaylist.h>

#include <optional>
#include <string>
#include <vector>

namespace lucyn::layout {

enum class Display { Block, Inline, None };
enum class Position { Static, Relative, Absolute };

// One `name: value` pair, in source order. Name and value are lowercased and
// trimmed; `!important` is stripped from the value and recorded in `important`.
struct Declaration {
    std::string name;
    std::string value;
    bool important = false;
};

// Splits a style-attribute string into declarations (source order). Never
// throws; garbage in gives fewer declarations out.
std::vector<Declaration> parseDeclarations(const std::string& styleText);

// Parses "12px", "-3.5px", "0", "+4" into `out`. Returns false (and leaves `out`
// alone) for anything else: "10%", "2em", "auto", "", "px", "1e3px", ...
bool parsePx(const std::string& token, float& out);

// Parses a CSS colour: #rgb, #rgba, #rrggbb, #rrggbbaa, rgb(r,g,b), rgba(r,g,b,a)
// (comma or space separated; channels are 0-255 or percentages, alpha is 0-1 or a
// percentage), `transparent`, and a small table of named colours (the 16 classic
// HTML names plus orange, pink, brown, gold, cyan, magenta, grey/gray spellings and
// a few darks/lights - not the full 148). Anything else: nullopt.
std::optional<lucyn::paint::Color> parseColor(const std::string& text);

// Side order everywhere below: top, right, bottom, left (the CSS order).
enum Side { SideTop = 0, SideRight = 1, SideBottom = 2, SideLeft = 3 };

// The result of applying a style attribute. An unset optional means "the style
// did not say": layout falls back to the UA default (or, for width/height and
// the offsets, to `auto`).
struct Style {
    std::optional<Display> display;
    std::optional<float> width, height;                // content-box size, px, >= 0
    std::optional<float> margin[4];                    // px, may be negative
    std::optional<float> padding[4];                   // px, >= 0
    std::optional<float> borderWidth;                  // px, >= 0 (ONE width for all sides, see layout.h)
    Position position = Position::Static;
    std::optional<float> offset[4];                    // top/right/bottom/left, px, may be negative
    std::optional<bool> visible;                       // unset = inherit
    std::optional<int> zIndex;                         // unset = auto
    bool clipsOverflow = false;                        // overflow: hidden | scroll | auto | clip
    std::optional<lucyn::paint::Color> backgroundColor;  // not inherited
    std::optional<lucyn::paint::Color> color;            // text colour; INHERITED by block descendants
    std::optional<lucyn::paint::Color> borderColor;      // one colour for all four sides
    std::optional<float> fontSize;                       // px; INHERITED by block descendants
};

// Applies already-parsed declarations (normal ones first, then `!important`).
Style styleFromDeclarations(const std::vector<Declaration>& decls);

// parseDeclarations + styleFromDeclarations.
Style styleFromText(const std::string& styleText);

// The style for one DOM element: the UA `hidden` attribute (display:none, which
// an author `display:` can override, same as `[hidden]{display:none}` in a real
// UA sheet) and then its `style=""` attribute. Non-element nodes give a default Style.
Style styleForElement(const lucyn::dom::Node& el);

}  // namespace lucyn::layout
