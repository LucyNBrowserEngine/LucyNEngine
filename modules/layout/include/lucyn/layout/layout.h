// lucyn/layout/layout.h - "just enough layout" (SESSION11_PLAN.md Part C, extended by
// SESSION12_PLAN.md Part A).
// Block and inline only. What is built:
//   Session 11 (the ladder's "Must" rung)
//   - a box tree built from the dom module
//   - UA default rules by tag: div/p/h1-h6/ul/li/body block; span/a/b/i/em/strong/
//     small/code/label inline; script/style/head/title/meta/link/noscript none
//   - block boxes stack vertically
//   - inline text wraps at the container width (word wrap, via
//     paint::SoftwarePainter::measureTextWidth)
//   - margin/padding/border widths, line-height from the embedded font's own metrics
//   - produces a paint::DisplayList (layout never draws itself, same README rule)
//
//   Session 12 (the ladder's "Should" rung): inline `style=""` (see style.h)
//   - display (block | inline | none), width, height, margin, padding, border width
//       * `width`/`height` are CONTENT-box sizes (CSS box-sizing: content-box); a Box's
//         own `width`/`height` are the BORDER-box sizes: width + padding + 2*border
//       * `px` only. No %, em, rem, calc(), var(). Unsupported values are ignored.
//       * ONE border width for all four sides (Box::borderWidth); `border-width: 1px 2px`
//         uses the first value. Per-side border longhands are ignored.
//       * margins do not collapse (Session 11 behaviour, unchanged)
//   - position: static | relative | absolute  (no fixed, no sticky)
//       * relative: laid out in normal flow, then shifted by top/left (or -bottom/-right);
//         its flow slot is unchanged
//       * absolute: removed from flow. Containing block = padding box of the nearest
//         positioned ancestor, else the viewport (LayoutOptions::viewportWidth/Height).
//         left/right/top/bottom/width/height resolve like CSS (left beats right, top
//         beats bottom when over-constrained). Auto width is shrink-to-fit
//         (min(content's natural width, available)); auto height is the content height
//         unless top AND bottom are both set. With no offsets it sits at its static position.
//       * position/size/offset on an INLINE element are ignored (inline content is
//         flattened to text, see below) except that `position: absolute` blockifies it
//   - visibility: hidden keeps the box's space but emits no draw items for the box
//     itself; the property inherits, and a descendant may say `visible` again
//   - z-index: only honoured on a positioned box (a z-index on a static box does
//     nothing, as in CSS). A positioned box with a z-index starts a stacking context;
//     painting order inside one: its own border/text, negative-z contexts,
//     non-positioned descendants in tree order, z-index:auto/0 positioned boxes in
//     tree order, then positive-z contexts (ascending, ties in tree order)
//   - overflow: hidden (also scroll/auto/clip) clips descendants' draw items to the
//     box's padding box; nested clips intersect. There is no scrolling.
//   - colours: `background-color` (or the colour in `background`) fills the border box
//     (under the border); `color` sets the text colour and is inherited by descendant
//     BLOCKS (inline elements are flattened, so a coloured <span> is not coloured);
//     `border-color`/the colour in `border` colours the one stroke. Colour syntax: see
//     style.h's parseColor.
//   `<body>` / `<html>` style is honoured for margin/padding/border/width/height/
//   overflow/visibility; position on them is ignored (the viewport is the root
//   containing block either way).
//
// STILL NOT: flexbox (`display:flex` renders as a block), grid, floats, tables,
// `<style>` blocks, selectors, background images, opacity, border-radius, shadows,
// transforms, getBoundingClientRect wiring into
// modules/js. Inline elements are flattened to plain text, so any style on a
// <span>/<a>/... other than display:none/block is ignored (a v0 simplification).
#pragma once
#include <lucyn/dom/dom.h>
#include <lucyn/layout/style.h>
#include <lucyn/layout/css.h>
#include <lucyn/paint/displaylist.h>

#include <string>
#include <vector>

namespace lucyn::layout {

// (enum class Display and Position live in style.h, included above.)

// One box in the tree. `node` is the DOM element this box came from, or
// nullptr for an anonymous box (a wrapped line of inline text has no single
// owning element). Coordinates are absolute (same space the whole page is
// painted in), not relative to the parent — simplest thing that lets
// paint() walk the tree without carrying an offset. An absolutely positioned box
// is a child of its CONTAINING BLOCK's box (not of its DOM parent), which is what
// makes clipping and stacking come out right.
struct Box {
    const lucyn::dom::Node* node = nullptr;
    Display display = Display::Block;
    float x = 0, y = 0, width = 0, height = 0;   // border box
    float marginTop = 0, marginBottom = 0, marginLeft = 0, marginRight = 0;
    float paddingTop = 0, paddingRight = 0, paddingBottom = 0, paddingLeft = 0;
    float borderWidth = 0;
    float fontSize = 16;             // only meaningful on a text-run box
    std::string text;               // non-empty only on a synthetic inline "line" box
    float textInsetX = 0;            // form-control text inset from the border box

    // Session 12
    Position position = Position::Static;
    bool hasZIndex = false;          // true only when z-index was set AND position != static
    int  zIndex = 0;
    bool visible = true;             // false: keep the space, draw nothing for this box
    bool clipsChildren = false;      // overflow:hidden-ish: descendants clip to the padding box
    bool hasExplicitWidth = false;   // width came from `width:` (or an absolute box's computed width)

    // Colours (Session 12, added because LNM's `color`/`textcolor` need a target)
    bool hasBackground = false;      // background-color was set (and is not fully transparent)
    lucyn::paint::Color background = lucyn::paint::Color::transparent();
    lucyn::paint::Color textColor = lucyn::paint::Color::black();   // inherited; used by this box's text and its descendants'
    lucyn::paint::Color borderColor = lucyn::paint::Color::grey();

    std::vector<Box> children;
};

struct LayoutOptions {
    float viewportWidth = 800;
    float viewportHeight = 600;      // only used as the containing block of absolute boxes with no positioned ancestor
    float defaultFontSize = 16;
};

// Classifies one element's UA-default display by tag name (lowercased).
// Exposed because the controller/tests may want it without a full layout.
Display uaDisplayForTag(const std::string& tagLower);

// Builds the box tree for `doc` and lays it out (block stacking + inline word
// wrap) within a viewport `opts.viewportWidth` wide. Lays out starting from
// <body> if present, else <html>, else the document root. Returns the root
// box; root.height is the full document (content) height.
Box layoutDocument(const lucyn::dom::Document& doc, const LayoutOptions& opts = {});

// Same as above, but with an external stylesheet (built by the controller
// from <style> blocks and <link rel=stylesheet> CSS files). Pass nullptr to
// get the legacy behaviour (inline `style=""` only).
Box layoutDocument(const lucyn::dom::Document& doc, const LayoutOptions& opts, const Stylesheet* sheet);

// Turns a laid-out Box tree into a DisplayList a Painter can draw. This is
// the one place a Box tree becomes draw commands ("layout never draws").
lucyn::paint::DisplayList paint(const Box& root);

}  // namespace lucyn::layout
