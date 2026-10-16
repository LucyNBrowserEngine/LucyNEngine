#include <lucyn/layout/layout.h>
#include <lucyn/layout/css.h>
#include <lucyn/paint/software_painter.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <unordered_set>

namespace lucyn::layout {
namespace {

// The active stylesheet for the current layoutDocument() call. Set/cleared
// by layoutDocument() so the existing per-element calls don't have to thread
// the sheet through every helper. nullptr = legacy behaviour (inline style
// only, no <style>/external CSS).
thread_local const Stylesheet* g_sheet = nullptr;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const std::unordered_set<std::string>& inlineTags() {
    static const std::unordered_set<std::string> s = {
        "span", "a", "b", "i", "em", "strong", "small", "code", "label",
        "u", "sub", "sup", "abbr", "cite", "q", "mark", "time", "kbd", "samp"};
    return s;
}
const std::unordered_set<std::string>& noneTags() {
    static const std::unordered_set<std::string> s = {
        "script", "style", "head", "title", "meta", "link", "noscript", "template"};
    return s;
}
// UA default: a little vertical margin, the way real browsers space p/h1..h6/ul/ol.
const std::unordered_set<std::string>& blockMarginTags() {
    static const std::unordered_set<std::string> s = {"p", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "blockquote"};
    return s;
}

std::string collapseWhitespace(const std::string& in) {
    std::string out;
    bool lastWasSpace = true;  // true at start trims leading whitespace
    for (char c : in) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!lastWasSpace) { out += ' '; lastWasSpace = true; }
        } else {
            out += c;
            lastWasSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// An absolutely positioned element met in normal flow. It is not laid out where
// it is found: its containing block's height may not be known yet, so it waits in
// the nearest positioned ancestor's list and is resolved once that box is finished.
struct PendingAbs {
    const lucyn::dom::Node* el;
    float staticX;     // where it would have been in normal flow (border-box edge minus margin)
    float staticY;
    bool visible;      // `visibility` inherited from the DOM parent at the point it was found
    lucyn::paint::Color textColor;   // `color` inherited from the DOM parent at the point it was found
    float fontSize = 16.0f;          // `font-size` inherited from the DOM parent at the point it was found
};

struct BlockLayoutCtx {
    float viewportWidth = 800;
    float viewportHeight = 600;
    float fontSize = 16;
    bool visible = true;                     // inherited `visibility`
    lucyn::paint::Color textColor = lucyn::paint::Color::black();   // inherited `color`
    std::vector<PendingAbs>* abs = nullptr;  // nearest positioned ancestor's waiting list; null = discard (probe layouts)
};

// The resolved margin / padding / border of one element (UA defaults, then style).
struct BoxModel {
    float margin[4] = {0, 0, 0, 0};   // Side order: top, right, bottom, left
    float padding[4] = {0, 0, 0, 0};
    float border = 0;
    float hPad() const { return padding[SideLeft] + padding[SideRight] + 2 * border; }
    float vPad() const { return padding[SideTop] + padding[SideBottom] + 2 * border; }
    float hMargin() const { return margin[SideLeft] + margin[SideRight]; }
    float vMargin() const { return margin[SideTop] + margin[SideBottom]; }
};

BoxModel resolveBoxModel(const lucyn::dom::Node& el, const Style& st) {
    BoxModel m;
    if (blockMarginTags().count(lower(el.name))) {
        m.margin[SideTop] = 8;
        m.margin[SideBottom] = 8;
    }
    for (int i = 0; i < 4; ++i) {
        if (st.margin[i]) m.margin[i] = *st.margin[i];
        if (st.padding[i]) m.padding[i] = *st.padding[i];
    }
    if (st.borderWidth) m.border = *st.borderWidth;
    return m;
}

void shiftBox(Box& b, float dx, float dy) {
    b.x += dx;
    b.y += dy;
    for (auto& c : b.children) shiftBox(c, dx, dy);
}

// Word-wraps `text` (already whitespace-collapsed) at `width` into visual lines.
std::vector<std::string> wrapLines(const std::string& text, float width, float fontSize) {
    std::vector<std::string> lines;
    std::istringstream iss(text);
    std::string word, line;
    while (iss >> word) {
        std::string candidate = line.empty() ? word : line + " " + word;
        float w = lucyn::paint::SoftwarePainter::measureTextWidth(candidate, fontSize);
        if (w > width && !line.empty()) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

float layoutBlockChildren(const lucyn::dom::Node& parent, float x, float y, float width,
                           const BlockLayoutCtx& ctx, std::vector<Box>& outChildren);
Box layoutAbsolute(const PendingAbs& p, float cbX, float cbY, float cbW, float cbH, const BlockLayoutCtx& ctx);

// Word-wraps `runText` at `width` (measured with the embedded font at
// ctx.fontSize) into one synthetic inline "line" Box per visual line,
// appended to outChildren. Advances cursorY by one lineHeight per line.
void flushInlineRun(std::string& runText, float x, float& cursorY, float width, const BlockLayoutCtx& ctx,
                     std::vector<Box>& outChildren) {
    std::string text = collapseWhitespace(runText);
    runText.clear();
    if (text.empty()) return;

    float lh = lucyn::paint::SoftwarePainter::lineHeight(ctx.fontSize);
    for (const std::string& l : wrapLines(text, width, ctx.fontSize)) {
        Box lineBox;
        lineBox.display = Display::Inline;
        lineBox.text = l;
        lineBox.fontSize = ctx.fontSize;
        lineBox.visible = ctx.visible;
        lineBox.textColor = ctx.textColor;
        lineBox.x = x;
        lineBox.y = cursorY;
        lineBox.width = width;
        lineBox.height = lh;
        outChildren.push_back(std::move(lineBox));
        cursorY += lh;
    }
}

// How tall the pending inline run WILL be once flushed (without flushing it).
float pendingRunHeight(const std::string& runText, float width, const BlockLayoutCtx& ctx) {
    std::string text = collapseWhitespace(runText);
    if (text.empty()) return 0;
    return lucyn::paint::SoftwarePainter::lineHeight(ctx.fontSize) *
           static_cast<float>(wrapLines(text, width, ctx.fontSize).size());
}

// Builds one block-level element's box at (x, y) inside a container `availWidth`
// wide. `forceBorderW`/`forceBorderH` (used for absolute boxes) fix the border-box
// size before `width`/`height` are consulted.
Box buildBlockBox(const lucyn::dom::Node& el, const Style& st, float x, float y, float availWidth,
                  const BlockLayoutCtx& ctx, std::optional<float> forceBorderW = std::nullopt,
                  std::optional<float> forceBorderH = std::nullopt) {
    BoxModel m = resolveBoxModel(el, st);

    Box box;
    box.node = &el;
    box.display = Display::Block;
    box.marginTop = m.margin[SideTop];
    box.marginRight = m.margin[SideRight];
    box.marginBottom = m.margin[SideBottom];
    box.marginLeft = m.margin[SideLeft];
    box.paddingTop = m.padding[SideTop];
    box.paddingRight = m.padding[SideRight];
    box.paddingBottom = m.padding[SideBottom];
    box.paddingLeft = m.padding[SideLeft];
    box.borderWidth = m.border;

    box.position = st.position;
    box.hasZIndex = st.zIndex.has_value() && st.position != Position::Static;  // z-index needs a positioned box
    box.zIndex = st.zIndex.value_or(0);
    box.visible = st.visible.value_or(ctx.visible);
    box.clipsChildren = st.clipsOverflow;
    box.textColor = st.color.value_or(ctx.textColor);
    box.fontSize = st.fontSize.value_or(ctx.fontSize);
    if (st.backgroundColor && st.backgroundColor->a > 0) {
        box.hasBackground = true;
        box.background = *st.backgroundColor;
    }
    if (st.borderColor) box.borderColor = *st.borderColor;

    if (el.name == "input") {
        const std::string* value = el.attr("value");
        const std::string* placeholder = el.attr("placeholder");
        box.text = value && !value->empty() ? *value : (placeholder ? *placeholder : "");
        box.fontSize = ctx.fontSize;
        box.textInsetX = 7.0f;
        if (!st.backgroundColor) {
            box.hasBackground = true;
            box.background = lucyn::paint::Color::white();
        }
        if (!st.borderWidth) {
            box.borderWidth = 1.0f;
            box.borderColor = lucyn::paint::Color::grey();
        }
    }

    box.x = x + m.margin[SideLeft];
    box.y = y + m.margin[SideTop];
    if (forceBorderW) {
        box.width = std::max(0.0f, *forceBorderW);
        box.hasExplicitWidth = true;
    } else if (st.width) {
        box.width = *st.width + m.hPad();   // `width` is the content box
        box.hasExplicitWidth = true;
    } else {
        box.width = std::max(0.0f, availWidth - m.hMargin());
    }

    float contentX = box.x + m.padding[SideLeft] + m.border;
    float contentY = box.y + m.padding[SideTop] + m.border;
    float contentWidth = std::max(0.0f, box.width - m.hPad());

    // A positioned box is the containing block for absolute descendants; they wait in
    // its own list and are resolved below, once this box's height is final.
    std::vector<PendingAbs> own;
    BlockLayoutCtx childCtx = ctx;
    childCtx.visible = box.visible;
    childCtx.textColor = box.textColor;
    childCtx.fontSize = box.fontSize;
    const bool isContainingBlock = st.position != Position::Static;
    if (isContainingBlock) childCtx.abs = &own;

    float consumed = layoutBlockChildren(el, contentX, contentY, contentWidth, childCtx, box.children);

    if (forceBorderH) box.height = std::max(0.0f, *forceBorderH);
    else if (st.height) box.height = *st.height + m.vPad();
    else box.height = consumed + m.vPad();

    if (isContainingBlock) {
        // Containing block for absolute descendants = this box's PADDING box.
        const float cbX = box.x + m.border, cbY = box.y + m.border;
        const float cbW = std::max(0.0f, box.width - 2 * m.border);
        const float cbH = std::max(0.0f, box.height - 2 * m.border);
        for (const PendingAbs& p : own) box.children.push_back(layoutAbsolute(p, cbX, cbY, cbW, cbH, childCtx));
    }
    return box;
}

// Right-most extent of a box's content, relative to x = 0, including what sits to the
// right of the content (padding, border, right margin) - a padded child makes its parent
// wider, not just its text. A box with an explicit width contributes exactly that width
// (its own overflow does not widen its parent). Auto-width blocks fill the probe container,
// so only their content counts.
float naturalExtent(const Box& b) {
    if (b.hasExplicitWidth) return b.x + b.width + b.marginRight;
    float r = 0;
    if (!b.text.empty()) r = b.x + lucyn::paint::SoftwarePainter::measureTextWidth(b.text, b.fontSize);
    float kids = 0;
    for (const auto& c : b.children) kids = std::max(kids, naturalExtent(c));
    if (kids > 0) r = std::max(r, kids + b.paddingRight + b.borderWidth + b.marginRight);
    return r;
}

// The width an element's content would take if nothing forced it to wrap: lay it out
// in a very wide container and see how far right the content reaches. Block children
// with auto width fill that huge container, so only text and explicit widths count.
float naturalContentWidth(const lucyn::dom::Node& el, const BlockLayoutCtx& ctx) {
    BlockLayoutCtx probe = ctx;
    probe.abs = nullptr;  // absolute descendants do not contribute to their parent's size
    std::vector<Box> kids;
    layoutBlockChildren(el, 0, 0, 100000.0f, probe, kids);
    float r = 0;
    for (const auto& k : kids) r = std::max(r, naturalExtent(k));
    return r;
}

// Lays out one absolutely positioned box against its containing block (cbX/cbY/cbW/cbH,
// a padding box). The box is built at a provisional origin and then shifted into place,
// because right/bottom anchoring needs the finished width/height first.
Box layoutAbsolute(const PendingAbs& p, float cbX, float cbY, float cbW, float cbH, const BlockLayoutCtx& ctxIn) {
    const lucyn::dom::Node& el = *p.el;
    Style st = styleForElement(el, g_sheet);
    BoxModel m = resolveBoxModel(el, st);

    BlockLayoutCtx ctx = ctxIn;
    ctx.visible = p.visible;
    ctx.textColor = p.textColor;
    ctx.fontSize = p.fontSize;

    const std::optional<float>& oTop = st.offset[SideTop];
    const std::optional<float>& oRight = st.offset[SideRight];
    const std::optional<float>& oBottom = st.offset[SideBottom];
    const std::optional<float>& oLeft = st.offset[SideLeft];

    // ---- border-box width -------------------------------------------------------------
    std::optional<float> forceW;
    if (st.width) {
        forceW = *st.width + m.hPad();
    } else if (oLeft && oRight) {
        forceW = std::max(0.0f, cbW - *oLeft - *oRight - m.hMargin());
    } else {
        float avail = std::max(0.0f, cbW - (oLeft ? *oLeft : 0.0f) - (oRight ? *oRight : 0.0f) - m.hMargin());
        float natural = naturalContentWidth(el, ctx) + m.hPad();   // shrink-to-fit
        forceW = std::min(natural, avail);
    }
    // ---- border-box height (otherwise: from content) ---------------------------------
    std::optional<float> forceH;
    if (!st.height && oTop && oBottom) forceH = std::max(0.0f, cbH - *oTop - *oBottom - m.vMargin());

    Box b = buildBlockBox(el, st, 0, 0, cbW, ctx, forceW, forceH);

    // ---- position: left beats right, top beats bottom (ltr) ----------------------------
    float finalX;
    if (oLeft) finalX = cbX + *oLeft + m.margin[SideLeft];
    else if (oRight) finalX = cbX + cbW - *oRight - m.margin[SideRight] - b.width;
    else finalX = p.staticX + m.margin[SideLeft];

    float finalY;
    if (oTop) finalY = cbY + *oTop + m.margin[SideTop];
    else if (oBottom) finalY = cbY + cbH - *oBottom - m.margin[SideBottom] - b.height;
    else finalY = p.staticY + m.margin[SideTop];

    shiftBox(b, finalX - b.x, finalY - b.y);
    return b;
}

// Lays out `parent`'s element/text children: block children stack vertically
// (flushing any pending inline run first), consecutive inline/text children
// accumulate into one run that word-wraps when flushed. Appends result boxes
// (absolute coordinates) to outChildren. Returns total height consumed.
float layoutBlockChildren(const lucyn::dom::Node& parent, float x, float y, float width,
                           const BlockLayoutCtx& ctx, std::vector<Box>& outChildren) {
    float cursorY = y;
    std::string pendingInline;
    auto flush = [&]() { flushInlineRun(pendingInline, x, cursorY, width, ctx, outChildren); };

    for (const auto& childPtr : parent.children) {
        const lucyn::dom::Node* child = childPtr.get();
        if (!child) continue;

        if (child->type == lucyn::dom::NodeType::Text) {
            pendingInline += child->text;
            continue;
        }
        if (child->type != lucyn::dom::NodeType::Element) continue;  // Comment, etc: skip

        Style st = styleForElement(*child, g_sheet);
        Display d = st.display.value_or(uaDisplayForTag(lower(child->name)));
        if (d == Display::None) continue;

        if (st.position == Position::Absolute) {
            // Out of flow. Remember where it would have been (below any text collected so
            // far) and let its containing block resolve it. No list = a probe layout: skip.
            if (ctx.abs) ctx.abs->push_back({child, x, cursorY + pendingRunHeight(pendingInline, width, ctx), ctx.visible, ctx.textColor, ctx.fontSize});
            continue;
        }

        if (lower(child->name) == "input") {
            // Controls need a box so the UI can paint and hit-test them. This
            // small engine treats inputs as standalone lines until inline
            // control layout has a proper run model.
            flush();
            if (!st.width) st.width = std::max(120.0f, std::min(320.0f, width - 2.0f));
            if (!st.height) st.height = 30.0f;
            Box b = buildBlockBox(*child, st, x, cursorY, width, ctx);
            cursorY = b.y + b.height + b.marginBottom;
            outChildren.push_back(std::move(b));
        } else if (d == Display::Block) {
            flush();
            Box b = buildBlockBox(*child, st, x, cursorY, width, ctx);
            float next = b.y + b.height + b.marginBottom;   // flow slot, BEFORE any relative shift
            if (st.position == Position::Relative) {
                const auto& t = st.offset[SideTop];
                const auto& r = st.offset[SideRight];
                const auto& bo = st.offset[SideBottom];
                const auto& l = st.offset[SideLeft];
                float dx = l ? *l : (r ? -*r : 0.0f);
                float dy = t ? *t : (bo ? -*bo : 0.0f);
                shiftBox(b, dx, dy);
            }
            cursorY = next;
            outChildren.push_back(std::move(b));
        } else {
            // Inline element: fold its subtree's text into the current run.
            // Flattens nested inline markup to plain text — a v0
            // simplification (no per-span styling), documented in layout.h.
            pendingInline += child->textContent();
            pendingInline += " ";
        }
    }
    flush();
    return cursorY - y;
}

// ---------------------------------------------------------------------------------------
// Painting. Order follows the CSS stacking rules (simplified as documented in layout.h).
// ---------------------------------------------------------------------------------------
using lucyn::paint::ClipRect;

struct ClipState {
    bool has = false;
    ClipRect r{};
};

ClipState intersectClip(const ClipState& a, const ClipRect& b) {
    if (!a.has) return {true, b};
    float x0 = std::max(a.r.x, b.x), y0 = std::max(a.r.y, b.y);
    float x1 = std::min(a.r.x + a.r.w, b.x + b.w), y1 = std::min(a.r.y + a.r.h, b.y + b.h);
    return {true, ClipRect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)}};
}

// The clip that applies to `box`'s DESCENDANTS (its own border is never clipped by itself).
ClipState clipForChildren(const Box& box, const ClipState& inherited) {
    if (!box.clipsChildren) return inherited;
    ClipRect pad{box.x + box.borderWidth, box.y + box.borderWidth,
                 std::max(0.0f, box.width - 2 * box.borderWidth),
                 std::max(0.0f, box.height - 2 * box.borderWidth)};
    return intersectClip(inherited, pad);
}

void emitOwn(const Box& box, const ClipState& clip, lucyn::paint::DisplayList& out) {
    using namespace lucyn::paint;
    if (!box.visible) return;   // visibility:hidden: space kept, nothing drawn
    if (box.hasBackground)      // under the border and the text, over whatever was painted before
        out.push_back(DrawRect{box.x, box.y, box.width, box.height, box.background, true, 1.0f, clip.has, clip.r});
    if (box.borderWidth > 0)
        out.push_back(DrawBorder{box.x, box.y, box.width, box.height, box.borderWidth, box.borderColor, clip.has, clip.r});
    if (!box.text.empty()) {
        float lh = SoftwarePainter::lineHeight(box.fontSize);
        // Baseline sits most of the way down the line box, not at its very top.
        float baseline = box.y + lh * 0.8f;
        if (box.node && box.node->name == "input") baseline = box.y + (box.height - lh) * 0.5f + lh * 0.8f;
        out.push_back(DrawText{box.x + box.textInsetX + box.borderWidth, baseline,
                               box.text, box.fontSize, box.textColor, clip.has, clip.r});
    }
}

struct PaintEntry {
    const Box* box;
    ClipState clip;   // clip that applies to the box itself
};
struct Layers {
    std::vector<PaintEntry> neg, zero, pos;
};

// Non-positioned descendants, tree order. Positioned ones are painted from their layer.
void paintFlow(const Box& box, const ClipState& clipForKids, lucyn::paint::DisplayList& out) {
    for (const auto& c : box.children) {
        if (c.position != Position::Static) continue;
        emitOwn(c, clipForKids, out);
        paintFlow(c, clipForChildren(c, clipForKids), out);
    }
}

// Collects the positioned descendants that take part in `box`'s stacking context.
// A z-index:auto positioned box does not start a context, so ITS positioned
// descendants take part in this one too (hence the recursion); a positioned box with
// a z-index is opaque here - it paints its own insides.
void gather(const Box& box, const ClipState& clipForKids, Layers& L) {
    for (const auto& c : box.children) {
        ClipState childClipForKids = clipForChildren(c, clipForKids);
        if (c.position == Position::Static) {
            gather(c, childClipForKids, L);
            continue;
        }
        PaintEntry e{&c, clipForKids};
        if (c.hasZIndex && c.zIndex < 0) L.neg.push_back(e);
        else if (c.hasZIndex && c.zIndex > 0) L.pos.push_back(e);
        else L.zero.push_back(e);
        if (!c.hasZIndex) gather(c, childClipForKids, L);
    }
}

void paintContext(const Box& box, const ClipState& clipAtBox, lucyn::paint::DisplayList& out);

void paintEntry(const PaintEntry& e, lucyn::paint::DisplayList& out) {
    if (e.box->hasZIndex) {
        paintContext(*e.box, e.clip, out);
    } else {
        emitOwn(*e.box, e.clip, out);
        paintFlow(*e.box, clipForChildren(*e.box, e.clip), out);
    }
}

void paintContext(const Box& box, const ClipState& clipAtBox, lucyn::paint::DisplayList& out) {
    emitOwn(box, clipAtBox, out);
    ClipState kids = clipForChildren(box, clipAtBox);
    Layers L;
    gather(box, kids, L);
    auto byZ = [](const PaintEntry& a, const PaintEntry& b) { return a.box->zIndex < b.box->zIndex; };
    std::stable_sort(L.neg.begin(), L.neg.end(), byZ);
    std::stable_sort(L.pos.begin(), L.pos.end(), byZ);
    for (const auto& e : L.neg) paintEntry(e, out);
    paintFlow(box, kids, out);
    for (const auto& e : L.zero) paintEntry(e, out);
    for (const auto& e : L.pos) paintEntry(e, out);
}

}  // namespace

Display uaDisplayForTag(const std::string& tagLowerIn) {
    std::string t = lower(tagLowerIn);
    if (noneTags().count(t)) return Display::None;
    if (inlineTags().count(t)) return Display::Inline;
    return Display::Block;  // div/p/h1-h6/ul/li/body, and any unknown tag (HTML5 default)
}

Box layoutDocument(const lucyn::dom::Document& doc, const LayoutOptions& opts) {
    return layoutDocument(doc, opts, nullptr);
}

Box layoutDocument(const lucyn::dom::Document& doc, const LayoutOptions& opts, const Stylesheet* sheet) {
    struct SheetGuard {
        const Stylesheet* prev;
        SheetGuard(const Stylesheet* s) : prev(g_sheet) { g_sheet = s; }
        ~SheetGuard() { g_sheet = prev; }
    } guard(sheet);

    const lucyn::dom::Node* startNode = doc.body();
    if (!startNode) startNode = doc.documentElement();
    if (!startNode) startNode = &doc.root();

    Style st = styleForElement(*startNode, g_sheet);
    if (st.display && *st.display == Display::None) {
        Box empty;
        empty.node = startNode;
        empty.width = opts.viewportWidth;
        return empty;
    }
    st.position = Position::Static;   // the viewport is the root containing block regardless
    st.zIndex.reset();

    std::vector<PendingAbs> icb;      // absolute boxes with no positioned ancestor
    BlockLayoutCtx ctx;
    ctx.viewportWidth = opts.viewportWidth;
    ctx.viewportHeight = opts.viewportHeight;
    ctx.fontSize = opts.defaultFontSize;
    ctx.abs = &icb;

    Box root = buildBlockBox(*startNode, st, 0, 0, opts.viewportWidth, ctx);
    for (const PendingAbs& p : icb)
        root.children.push_back(layoutAbsolute(p, 0, 0, opts.viewportWidth, opts.viewportHeight, ctx));
    return root;
}

lucyn::paint::DisplayList paint(const Box& root) {
    lucyn::paint::DisplayList list;
    paintContext(root, ClipState{}, list);
    return list;
}

}  // namespace lucyn::layout
