#include <cstdio>
#include <lucyn/dom/dom.h>
#include <lucyn/layout/layout.h>

using namespace lucyn::layout;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

namespace {
// Finds the first box (depth-first) whose node has this tag, or nullptr.
const Box* findByTag(const Box& b, const std::string& tag) {
    if (b.node && b.node->isElement() && b.node->name == tag) return &b;
    for (auto& c : b.children) {
        if (const Box* r = findByTag(c, tag)) return r;
    }
    return nullptr;
}
int countLines(const Box& b) {
    int n = b.text.empty() ? 0 : 1;
    for (auto& c : b.children) n += countLines(c);
    return n;
}
}  // namespace

int main() {
    // 1. UA default display classification.
    CHECK(uaDisplayForTag("div") == Display::Block);
    CHECK(uaDisplayForTag("P") == Display::Block);     // case-insensitive
    CHECK(uaDisplayForTag("span") == Display::Inline);
    CHECK(uaDisplayForTag("a") == Display::Inline);
    CHECK(uaDisplayForTag("script") == Display::None);
    CHECK(uaDisplayForTag("made-up-tag") == Display::Block);  // unknown -> block (HTML5 default)

    // 2. Known tiny HTML in, exact box x/y/w/h out: two stacked divs with fixed
    // heights via nested block children (no inline style this session, so we
    // check stacking order/position, not a specific declared height).
    {
        auto doc = lucyn::dom::parseHtml("<body><div>one</div><div>two</div></body>");
        LayoutOptions opts; opts.viewportWidth = 400;
        Box root = layoutDocument(*doc, opts);
        CHECK(root.children.size() == 2);
        CHECK(root.children[0].x == 0 && root.children[0].y == 0);
        CHECK(root.children[0].width == 400);
        // second div stacks below the first (y grows downward)
        CHECK(root.children[1].y >= root.children[0].y + root.children[0].height);
    }

    // 3. display:none produces no boxes.
    {
        auto doc = lucyn::dom::parseHtml("<body><div>shown</div><div style=\"display:none\">hidden</div><div hidden>also hidden</div></body>");
        LayoutOptions opts; opts.viewportWidth = 400;
        Box root = layoutDocument(*doc, opts);
        CHECK(root.children.size() == 1);  // only the shown div produced a box
    }

    // 4. Wrapping at a known width: a long paragraph wraps into more than one line
    // at a narrow width, and into fewer (ideally one) line at a wide width.
    {
        std::string html = "<body><p>" 
            "the quick brown fox jumps over the lazy dog again and again and again"
            "</p></body>";
        auto doc = lucyn::dom::parseHtml(html);

        LayoutOptions narrow; narrow.viewportWidth = 100;
        Box narrowRoot = layoutDocument(*doc, narrow);
        const Box* pNarrow = findByTag(narrowRoot, "p");
        CHECK(pNarrow != nullptr);
        int narrowLines = pNarrow ? countLines(*pNarrow) : 0;

        LayoutOptions wide; wide.viewportWidth = 2000;
        Box wideRoot = layoutDocument(*doc, wide);
        const Box* pWide = findByTag(wideRoot, "p");
        CHECK(pWide != nullptr);
        int wideLines = pWide ? countLines(*pWide) : 0;

        CHECK(narrowLines > 1);       // must wrap at 100px
        CHECK(wideLines <= narrowLines);
        CHECK(wideLines >= 1);
    }

    // 5. Nested margins/padding add up: a <p> inside a <div> inside <body> —
    // the p's own UA top margin pushes it down from the div's content origin.
    {
        auto doc = lucyn::dom::parseHtml("<body><div><p>hi</p></div></body>");
        LayoutOptions opts; opts.viewportWidth = 400;
        Box root = layoutDocument(*doc, opts);
        CHECK(root.children.size() == 1);
        const Box& div = root.children[0];
        CHECK(div.children.size() == 1);
        const Box& p = div.children[0];
        CHECK(p.marginTop == 8);           // UA default for <p>
        CHECK(p.y == div.y + 8);           // margin pushed it down from the div's origin
    }

    // 6. Produces a DisplayList (layout never draws itself — paint() is the
    // one place a Box tree becomes draw commands).
    {
        auto doc = lucyn::dom::parseHtml("<body><h1>Title</h1><p>Some text here.</p></body>");
        LayoutOptions opts; opts.viewportWidth = 400;
        Box root = layoutDocument(*doc, opts);
        auto list = paint(root);
        CHECK(!list.empty());
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("layout: all tests passed\n");
    return 0;
}
