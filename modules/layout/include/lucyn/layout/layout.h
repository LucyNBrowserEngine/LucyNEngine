// lucyn/layout/layout.h - "just enough layout" (SESSION11_PLAN.md Part C).
// Block and inline only. What is built this session (the ladder's "Must" rung):
//   - a box tree built from the dom module
//   - UA default rules by tag: div/p/h1-h6/ul/li/body block; span/a/b/i/em/strong/
//     small/code/label inline; script/style/head/title/meta/link/noscript none;
//     display:none honoured
//   - block boxes stack vertically
//   - inline text wraps at the container width (word wrap, via
//     paint::SoftwarePainter::measureTextWidth)
//   - margin/padding/border widths (UA defaults only; no inline `style=""`,
//     no `<style>` blocks — those are the "Should"/"Could" rungs, not reached
//     this session, see SESSION11.md)
//   - line-height from the embedded font's own metrics
//   - produces a paint::DisplayList (layout never draws itself, same README rule)
//
// NOT this session (explicitly out of scope, see SESSION11_PLAN.md): flexbox,
// grid, floats, tables, inline `style=""`/`<style>` parsing, getBoundingClientRect
// wiring into modules/js.
#pragma once
#include <lucyn/dom/dom.h>
#include <lucyn/paint/displaylist.h>

#include <string>
#include <vector>

namespace lucyn::layout {

enum class Display { Block, Inline, None };

// One box in the tree. `node` is the DOM element this box came from, or
// nullptr for an anonymous box (a wrapped line of inline text has no single
// owning element). Coordinates are absolute (same space the whole page is
// painted in), not relative to the parent — simplest thing that lets
// paint() walk the tree without carrying an offset.
struct Box {
    const lucyn::dom::Node* node = nullptr;
    Display display = Display::Block;
    float x = 0, y = 0, width = 0, height = 0;
    float marginTop = 0, marginBottom = 0, marginLeft = 0, marginRight = 0;
    float paddingTop = 0, paddingRight = 0, paddingBottom = 0, paddingLeft = 0;
    float borderWidth = 0;
    float fontSize = 16;             // only meaningful on a text-run box
    std::string text;               // non-empty only on a synthetic inline "line" box
    std::vector<Box> children;
};

struct LayoutOptions {
    float viewportWidth = 800;
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

// Turns a laid-out Box tree into a DisplayList a Painter can draw. This is
// the one place a Box tree becomes draw commands ("layout never draws").
lucyn::paint::DisplayList paint(const Box& root);

}  // namespace lucyn::layout
