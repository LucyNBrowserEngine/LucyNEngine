#include <lucyn/layout/layout.h>
#include <lucyn/paint/software_painter.h>

#include <cctype>
#include <sstream>
#include <unordered_set>

namespace lucyn::layout {
namespace {

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

// No inline style=""/<style> parsing this session (see layout.h); the only
// style-attribute check honoured is a crude, literal "display" and "none"
// search — enough to satisfy the Must-rung "display:none honoured" item
// without pretending to be a CSS parser.
bool hasInlineDisplayNone(const lucyn::dom::Node& el) {
    if (el.hasAttr("hidden")) return true;
    const std::string* style = el.attr("style");
    if (!style) return false;
    std::string s = lower(*style);
    auto pos = s.find("display");
    if (pos == std::string::npos) return false;
    auto colon = s.find(':', pos);
    if (colon == std::string::npos) return false;
    auto valueEnd = s.find(';', colon);
    std::string value = s.substr(colon + 1, valueEnd == std::string::npos ? std::string::npos : valueEnd - colon - 1);
    auto a = value.find_first_not_of(" \t");
    if (a == std::string::npos) return false;
    auto b = value.find_last_not_of(" \t");
    value = value.substr(a, b - a + 1);
    return value == "none";
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

struct BlockLayoutCtx {
    float viewportWidth;
    float fontSize;
};

float layoutBlockChildren(const lucyn::dom::Node& parent, float x, float y, float width,
                           const BlockLayoutCtx& ctx, std::vector<Box>& outChildren);

// Word-wraps `runText` at `width` (measured with the embedded font at
// ctx.fontSize) into one synthetic inline "line" Box per visual line,
// appended to outChildren. Advances cursorY by one lineHeight per line.
void flushInlineRun(std::string& runText, float x, float& cursorY, float width, const BlockLayoutCtx& ctx,
                     std::vector<Box>& outChildren) {
    std::string text = collapseWhitespace(runText);
    runText.clear();
    if (text.empty()) return;

    float lh = lucyn::paint::SoftwarePainter::lineHeight(ctx.fontSize);
    std::istringstream iss(text);
    std::string word;
    std::string line;
    auto pushLine = [&](const std::string& l) {
        if (l.empty()) return;
        Box lineBox;
        lineBox.display = Display::Inline;
        lineBox.text = l;
        lineBox.fontSize = ctx.fontSize;
        lineBox.x = x;
        lineBox.y = cursorY;
        lineBox.width = width;
        lineBox.height = lh;
        outChildren.push_back(std::move(lineBox));
        cursorY += lh;
    };
    while (iss >> word) {
        std::string candidate = line.empty() ? word : line + " " + word;
        float w = lucyn::paint::SoftwarePainter::measureTextWidth(candidate, ctx.fontSize);
        if (w > width && !line.empty()) {
            pushLine(line);
            line = word;
        } else {
            line = candidate;
        }
    }
    pushLine(line);
}

Box layoutElementAsBlock(const lucyn::dom::Node& el, float x, float y, float width, const BlockLayoutCtx& ctx) {
    Box box;
    box.node = &el;
    box.display = Display::Block;
    std::string tag = lower(el.name);
    if (blockMarginTags().count(tag)) {
        box.marginTop = 8;
        box.marginBottom = 8;
    }
    box.x = x + box.marginLeft;
    box.y = y + box.marginTop;
    box.width = width - box.marginLeft - box.marginRight;
    if (box.width < 0) box.width = 0;

    float contentX = box.x + box.paddingLeft + box.borderWidth;
    float contentY = box.y + box.paddingTop + box.borderWidth;
    float contentWidth = box.width - box.paddingLeft - box.paddingRight - 2 * box.borderWidth;
    if (contentWidth < 0) contentWidth = 0;

    float consumedHeight = layoutBlockChildren(el, contentX, contentY, contentWidth, ctx, box.children);
    box.height = consumedHeight + box.paddingTop + box.paddingBottom + 2 * box.borderWidth;
    return box;
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

        std::string tag = lower(child->name);
        Display d = uaDisplayForTag(tag);
        if (hasInlineDisplayNone(*child)) d = Display::None;
        if (d == Display::None) continue;

        if (d == Display::Block) {
            flush();
            Box b = layoutElementAsBlock(*child, x, cursorY, width, ctx);
            cursorY = b.y + b.height + b.marginBottom;
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

void collectPaint(const Box& box, lucyn::paint::DisplayList& out) {
    using namespace lucyn::paint;
    if (box.borderWidth > 0) {
        out.push_back(DrawBorder{box.x, box.y, box.width, box.height, box.borderWidth, Color::grey(), false, {}});
    }
    if (!box.text.empty()) {
        float lh = lucyn::paint::SoftwarePainter::lineHeight(box.fontSize);
        // Baseline sits most of the way down the line box, not at its very top.
        out.push_back(DrawText{box.x, box.y + lh * 0.8f, box.text, box.fontSize, Color::black(), false, {}});
    }
    for (const auto& c : box.children) collectPaint(c, out);
}

}  // namespace

Display uaDisplayForTag(const std::string& tagLowerIn) {
    std::string t = lower(tagLowerIn);
    if (noneTags().count(t)) return Display::None;
    if (inlineTags().count(t)) return Display::Inline;
    return Display::Block;  // div/p/h1-h6/ul/li/body, and any unknown tag (HTML5 default)
}

Box layoutDocument(const lucyn::dom::Document& doc, const LayoutOptions& opts) {
    const lucyn::dom::Node* startNode = doc.body();
    if (!startNode) startNode = doc.documentElement();
    if (!startNode) startNode = &doc.root();

    BlockLayoutCtx ctx{opts.viewportWidth, opts.defaultFontSize};
    Box root;
    root.node = startNode;
    root.display = Display::Block;
    root.x = 0;
    root.y = 0;
    root.width = opts.viewportWidth;
    root.height = layoutBlockChildren(*startNode, 0, 0, opts.viewportWidth, ctx, root.children);
    return root;
}

lucyn::paint::DisplayList paint(const Box& root) {
    lucyn::paint::DisplayList list;
    collectPaint(root, list);
    return list;
}

}  // namespace lucyn::layout
