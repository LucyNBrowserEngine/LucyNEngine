#include <cmath>
#include <cstdio>
#include <lucyn/dom/dom.h>
#include <lucyn/layout/layout.h>
#include <lucyn/paint/software_painter.h>
#include <variant>

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

const Box* findById(const Box& b, const std::string& id) {
    if (b.node && b.node->isElement() && b.node->id() == id) return &b;
    for (auto& c : b.children) {
        if (const Box* r = findById(c, id)) return r;
    }
    return nullptr;
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }
#define CHECKF(a, b) do { if (!near((a), (b))) { std::printf("FAIL %s:%d  %s (=%g) != %s (=%g)\n", __FILE__, __LINE__, #a, (double)(a), #b, (double)(b)); ++failures; } } while (0)

// A laid-out document. The boxes point into `doc`, so they travel together.
struct Laid {
    decltype(lucyn::dom::parseHtml("")) doc;
    Box root;
    const Box& id(const std::string& i) const {
        static const Box none;
        const Box* b = findById(root, i);
        if (!b) { std::printf("FAIL no box with id=%s\n", i.c_str()); ++failures; return none; }
        return *b;
    }
};
Laid lay(const std::string& html, float vw = 400, float vh = 600) {
    Laid L;
    L.doc = lucyn::dom::parseHtml(html);
    LayoutOptions o;
    o.viewportWidth = vw;
    o.viewportHeight = vh;
    L.root = layoutDocument(*L.doc, o);
    return L;
}

float LH() { return lucyn::paint::SoftwarePainter::lineHeight(16.0f); }
float TW(const char* s) { return lucyn::paint::SoftwarePainter::measureTextWidth(s, 16.0f); }

// Display-list helpers.
std::vector<std::string> textsOf(const lucyn::paint::DisplayList& l) {
    std::vector<std::string> t;
    for (auto& it : l)
        if (auto* d = std::get_if<lucyn::paint::DrawText>(&it)) t.push_back(d->text);
    return t;
}
std::vector<lucyn::paint::DrawBorder> bordersOf(const lucyn::paint::DisplayList& l) {
    std::vector<lucyn::paint::DrawBorder> b;
    for (auto& it : l)
        if (auto* d = std::get_if<lucyn::paint::DrawBorder>(&it)) b.push_back(*d);
    return b;
}
std::vector<lucyn::paint::DrawText> drawTextsOf(const lucyn::paint::DisplayList& l) {
    std::vector<lucyn::paint::DrawText> t;
    for (auto& it : l)
        if (auto* d = std::get_if<lucyn::paint::DrawText>(&it)) t.push_back(*d);
    return t;
}
std::vector<std::string> paintedTexts(const std::string& html) {
    Laid L = lay(html);
    return textsOf(paint(L.root));
}
using Strs = std::vector<std::string>;
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


    // =====================================================================================
    // Session 12, Part A: inline style="" support
    // =====================================================================================

    // ---- width / height (content-box) -----------------------------------------------------
    {
        Laid L = lay("<body><div id=a style=\"width:100px;height:50px\"></div><div id=b></div></body>");
        CHECKF(L.id("a").width, 100);
        CHECKF(L.id("a").height, 50);
        CHECK(L.id("a").hasExplicitWidth && !L.id("b").hasExplicitWidth);
        CHECKF(L.id("b").y, 50);                 // the next block stacks under the explicit height
        CHECKF(L.id("b").width, 400);            // auto width still fills the container
    }
    {   // padding + border are ADDED to width/height (content-box)
        Laid L = lay("<body><div id=c style=\"width:100px;height:40px;padding:10px;border:2px solid #000\"><div id=d>x</div></div></body>");
        CHECKF(L.id("c").width, 100 + 20 + 4);
        CHECKF(L.id("c").height, 40 + 20 + 4);
        CHECKF(L.id("c").borderWidth, 2);
        CHECKF(L.id("d").x, 12);                 // c.x + padding + border
        CHECKF(L.id("d").y, 12);
        CHECKF(L.id("d").width, 100);            // the child fills the content box
    }
    {   // auto height grows with content; explicit height does not
        Laid L = lay("<body><div id=e style=\"width:200px\"><div style=\"height:30px\"></div><div style=\"height:20px\"></div></div>"
                     "<div id=h style=\"height:5px\"><p>some text</p></div><div id=i></div></body>");
        CHECKF(L.id("e").height, 50);
        CHECKF(L.id("h").height, 5);             // content overflows; the box does not grow
        CHECKF(L.id("i").y, 50 + 5);
    }
    {   // width wider than the viewport is allowed (it just overflows); `auto` resets
        Laid L = lay("<body><div id=w style=\"width:900px\"></div><div id=x style=\"width:50px;width:auto\"></div></body>");
        CHECKF(L.id("w").width, 900);
        CHECKF(L.id("x").width, 400);
    }

    // ---- margin / padding / border override the UA defaults ----------------------------------
    {
        Laid L = lay("<body><p id=p style=\"margin:0\">hi</p><p id=q style=\"margin:3px 5px 7px 9px\">hi</p></body>");
        CHECKF(L.id("p").marginTop, 0);          // UA default for <p> is 8: overridden
        CHECKF(L.id("p").y, 0);
        CHECKF(L.id("q").marginTop, 3);
        CHECKF(L.id("q").marginRight, 5);
        CHECKF(L.id("q").marginBottom, 7);
        CHECKF(L.id("q").marginLeft, 9);
        CHECKF(L.id("q").x, 9);
        CHECKF(L.id("q").width, 400 - 9 - 5);
        CHECKF(L.id("q").y, LH() + 3);           // below p (height LH, margin 0), then its own top margin
    }
    {
        Laid L = lay("<body><div id=n style=\"margin-left:-10px\">x</div></body>");  // negative margins are real CSS
        CHECKF(L.id("n").x, -10);
        CHECKF(L.id("n").width, 410);
    }
    {
        Laid L = lay("<body><div id=pd style=\"padding:10px 20px\">word</div><div id=bw style=\"border-width:3px\"></div></body>");
        const Box& pd = L.id("pd");
        CHECKF(pd.height, LH() + 20);
        CHECK(pd.children.size() == 1);
        if (!pd.children.empty()) {
            CHECKF(pd.children[0].x, 20);        // text starts after the left padding
            CHECKF(pd.children[0].y, 10);
            CHECKF(pd.children[0].width, 400 - 40);
        }
        CHECKF(L.id("bw").borderWidth, 3);       // border-width alone is honoured (one width, grey stroke)
        CHECKF(L.id("bw").height, 6);
    }
    {   // an invalid / unsupported value is ignored: UA default stays
        Laid L = lay("<body><p id=p style=\"margin:10%;padding:-5px;width:50%\">hi</p></body>");
        CHECKF(L.id("p").marginTop, 8);
        CHECKF(L.id("p").paddingLeft, 0);
        CHECKF(L.id("p").width, 400);
    }

    // ---- display -----------------------------------------------------------------------------
    {
        Laid L = lay("<body><span id=s style=\"display:block\">a</span><span id=t>b</span></body>");
        CHECK(L.root.children.size() == 2);
        CHECK(L.root.children.size() == 2 && L.root.children[0].node && L.root.children[0].node->name == "span");
        CHECK(L.root.children.size() == 2 && L.root.children[1].text == "b");   // the plain span folded into a text line
    }
    {   // an inline-styled div is flattened to text like any inline
        Laid L = lay("<body><div id=u style=\"display:inline\">x</div><div id=v style=\"display:inline\">y</div></body>");
        CHECK(L.root.children.size() == 1);
        CHECK(L.root.children.size() == 1 && L.root.children[0].text == "x y");
    }
    {   // display:none removes the space; the [hidden] attribute is overridable by author style
        Laid L = lay("<body><div style=\"height:10px\"></div><div style=\"display:none;height:99px\"></div><div id=c></div>"
                     "<div id=h hidden style=\"display:block;height:7px\"></div></body>");
        CHECKF(L.id("c").y, 10);
        CHECKF(L.id("h").height, 7);
    }
    {   // flexbox is not built: display:flex is a plain block, not a crash and not a row
        Laid L = lay("<body><div id=f style=\"display:flex\"><div style=\"height:10px\"></div><div style=\"height:10px\"></div></div></body>");
        CHECKF(L.id("f").height, 20);
    }

    // ---- position: relative -----------------------------------------------------------------
    {
        Laid L = lay("<body><div style=\"height:20px\"></div>"
                     "<div id=r style=\"position:relative;top:5px;left:7px;height:10px\"><span>hi</span></div>"
                     "<div id=b style=\"height:10px\"></div></body>");
        CHECKF(L.id("r").x, 7);
        CHECKF(L.id("r").y, 25);                 // 20 + top:5
        CHECKF(L.id("b").y, 30);                 // ...but its slot in the flow is unchanged
        CHECK(L.id("r").children.size() == 1);
        if (!L.id("r").children.empty()) {
            CHECKF(L.id("r").children[0].x, 7);  // the shift carried the contents along
            CHECKF(L.id("r").children[0].y, 25);
        }
        CHECK(L.id("r").position == Position::Relative);
    }
    {
        Laid L = lay("<body><div style=\"height:10px\"></div><div id=r style=\"position:relative;bottom:4px;right:6px;height:10px\"></div>"
                     "<div id=t style=\"position:relative;top:5px;bottom:100px;left:1px;right:100px;height:1px\"></div></body>");
        CHECKF(L.id("r").x, -6);                 // right shifts left
        CHECKF(L.id("r").y, 10 - 4);             // bottom shifts up
        CHECKF(L.id("t").y, 20 + 5);             // top beats bottom
        CHECKF(L.id("t").x, 1);                  // left beats right
    }

    // ---- position: absolute -------------------------------------------------------------------
    {   // no positioned ancestor: the viewport is the containing block; out of flow
        Laid L = lay("<body><div style=\"height:10px\"></div>"
                     "<div id=x style=\"position:absolute;left:30px;top:40px;width:50px;height:60px\"></div>"
                     "<div id=b style=\"height:10px\"></div></body>");
        CHECKF(L.id("x").x, 30);
        CHECKF(L.id("x").y, 40);
        CHECKF(L.id("x").width, 50);
        CHECKF(L.id("x").height, 60);
        CHECKF(L.id("b").y, 10);                 // x took no space
        CHECK(L.id("x").position == Position::Absolute);
        CHECKF(L.root.height, 20);               // and did not grow the document
    }
    {   // containing block = the PADDING box of the nearest positioned ancestor
        Laid L = lay("<body><div id=cb style=\"position:relative;margin-left:20px;padding:10px;border:5px solid #000;width:200px;height:100px\">"
                     "<div id=in style=\"position:absolute;left:3px;top:4px;width:10px;height:10px\"></div></div></body>");
        CHECKF(L.id("cb").x, 20);
        CHECKF(L.id("in").x, 20 + 5 + 3);        // inside the border, ignoring the padding
        CHECKF(L.id("in").y, 5 + 4);
    }
    {   // a NON-positioned wrapper in between is skipped
        Laid L = lay("<body><div id=cb style=\"position:relative;height:100px\"><div id=mid style=\"margin-left:50px\">"
                     "<div id=in style=\"position:absolute;left:0;top:0;width:5px;height:5px\"></div></div></div></body>");
        CHECKF(L.id("mid").x, 50);
        CHECKF(L.id("in").x, 0);
        // the abs box hangs off its containing block, not its DOM parent
        CHECK(findById(L.id("cb"), "in") != nullptr);
        CHECK(findById(L.id("mid"), "in") == nullptr);
    }
    {   // right / bottom anchoring, and over-constraint (left beats right, top beats bottom)
        const char* cb = "<body><div style=\"position:relative;width:200px;height:100px\">";
        Laid L = lay(std::string(cb) + "<div id=a style=\"position:absolute;right:10px;bottom:20px;width:30px;height:15px\"></div></div></body>");
        CHECKF(L.id("a").x, 200 - 10 - 30);
        CHECKF(L.id("a").y, 100 - 20 - 15);
        L = lay(std::string(cb) + "<div id=a style=\"position:absolute;left:10px;right:10px;top:7px;bottom:7px;width:50px;height:20px\"></div></div></body>");
        CHECKF(L.id("a").x, 10);
        CHECKF(L.id("a").width, 50);
        CHECKF(L.id("a").y, 7);
        CHECKF(L.id("a").height, 20);
        L = lay(std::string(cb) + "<div id=a style=\"position:absolute;left:10px;right:20px;top:10px;bottom:30px\"></div></div></body>");
        CHECKF(L.id("a").width, 170);            // stretched between left and right
        CHECKF(L.id("a").height, 60);            // stretched between top and bottom
        L = lay(std::string(cb) + "<div id=a style=\"position:absolute;left:10px;top:10px;margin:5px;width:20px;height:20px\"></div></div></body>");
        CHECKF(L.id("a").x, 15);                 // margins offset the box from its anchors
        CHECKF(L.id("a").y, 15);
    }
    {   // static position: where it would have been in normal flow
        Laid L = lay("<body><div style=\"height:10px\"></div><div id=x style=\"position:absolute;width:5px;height:5px\"></div></body>");
        CHECKF(L.id("x").x, 0);
        CHECKF(L.id("x").y, 10);
        L = lay("<body>hello<div id=x style=\"position:absolute;width:5px;height:5px\"></div></body>");
        CHECKF(L.id("x").y, LH());               // below the text collected before it
        L = lay("<body><div style=\"padding:10px\"><div id=x style=\"position:absolute;width:5px;height:5px\"></div></div></body>");
        CHECKF(L.id("x").x, 10);
        CHECKF(L.id("x").y, 10);
    }
    {   // auto width is shrink-to-fit
        Laid L = lay("<body><div id=x style=\"position:absolute;left:0;top:0\">short</div></body>");
        CHECKF(L.id("x").width, TW("short"));
        CHECKF(L.id("x").height, LH());
        L = lay("<body><div id=x style=\"position:absolute;left:0;top:0;padding:5px\">short</div></body>");
        CHECKF(L.id("x").width, TW("short") + 10);
        L = lay("<body><div id=x style=\"position:absolute;left:30px;top:0\">the quick brown fox jumps over the lazy dog</div></body>", 100);
        CHECKF(L.id("x").width, 70);             // min(natural, viewport - left)
        CHECK(countLines(L.id("x")) > 1);        // so it wraps
    }
    {   // shrink-to-fit counts the padding/border/margin of NESTED blocks, not just their text
        Laid L = lay("<body><div id=x style=\"position:absolute;left:0;top:0\"><div id=in style=\"padding:6px;border:1px solid #000;margin-right:3px\">some words here</div></div></body>");
        CHECKF(L.id("x").width, TW("some words here") + 2 * 6 + 2 * 1 + 3);
        CHECK(countLines(L.id("x")) == 1);                      // so the text does NOT wrap
        CHECKF(L.id("in").width, TW("some words here") + 2 * 6 + 2 * 1);
        // an explicit-width child contributes exactly its width + margin, whatever it contains
        Laid M = lay("<body><div id=x style=\"position:absolute;left:0;top:0\"><div style=\"width:50px;margin-right:4px\">a much much longer text than fifty pixels</div></div></body>");
        CHECKF(M.id("x").width, 54);
    }
    {   // auto height is the content height
        Laid L = lay("<body><div id=x style=\"position:absolute;left:0;top:0;width:100px\"><p style=\"margin:0\">a</p><p style=\"margin:0\">b</p></div></body>");
        CHECKF(L.id("x").height, 2 * LH());
    }
    {   // position:absolute on an inline element blockifies it and takes it out of the text flow
        Laid L = lay("<body><span id=s style=\"position:absolute;left:5px;top:6px;width:10px;height:10px\">hi</span></body>");
        CHECK(L.root.children.size() == 1);
        CHECKF(L.id("s").x, 5);
        CHECKF(L.id("s").y, 6);
        CHECKF(L.id("s").width, 10);
    }
    {   // an absolute box is the containing block of its own absolute descendants
        Laid L = lay("<body><div id=o style=\"position:absolute;left:100px;top:100px;width:50px;height:50px\">"
                     "<div id=i style=\"position:absolute;left:5px;top:5px;width:1px;height:1px\"></div></div></body>");
        CHECKF(L.id("i").x, 105);
        CHECKF(L.id("i").y, 105);
    }
    {   // absolute children travel with a shifted relative parent; they do not size their parent
        Laid L = lay("<body><div id=p style=\"position:relative;top:10px;left:10px;height:50px\">"
                     "<div id=i style=\"position:absolute;left:0;top:0;width:5px;height:500px\"></div></div></body>");
        CHECKF(L.id("i").x, 10);
        CHECKF(L.id("i").y, 10);
        CHECKF(L.id("p").height, 50);
        Laid M = lay("<body><div id=p><div style=\"position:absolute;height:500px\"></div></div></body>");
        CHECKF(M.id("p").height, 0);
    }
    {   // the viewport HEIGHT is the root containing block for bottom: anchoring
        Laid L = lay("<body><div id=x style=\"position:absolute;bottom:0;right:0;width:10px;height:10px\"></div></body>", 400, 300);
        CHECKF(L.id("x").x, 390);
        CHECKF(L.id("x").y, 290);
    }
    {   // display:none wins over position:absolute
        Laid L = lay("<body><div id=x style=\"display:none;position:absolute;width:5px;height:5px\"></div></body>");
        CHECK(findById(L.root, "x") == nullptr);
    }

    // ---- visibility ----------------------------------------------------------------------------
    {
        Laid L = lay("<body><div id=a style=\"visibility:hidden;border:1px solid #000;height:20px\">secret</div><div id=b>after</div></body>");
        CHECKF(L.id("a").height, 22);
        CHECKF(L.id("b").y, 22);                 // hidden: the space is KEPT
        CHECK(!L.id("a").visible && L.id("b").visible);
        auto list = paint(L.root);
        CHECK(textsOf(list) == Strs{"after"});   // ...but nothing is drawn for it
        CHECK(bordersOf(list).empty());
        Laid N = lay("<body><div id=a style=\"display:none;height:20px\"></div><div id=b>after</div></body>");
        CHECKF(N.id("b").y, 0);                  // display:none: the space is GONE
    }
    {   // inherited, and a descendant may opt back in
        CHECK(paintedTexts("<body><div style=\"visibility:hidden\"><p style=\"visibility:visible\">shown</p><p>gone</p></div></body>") == Strs{"shown"});
        CHECK(paintedTexts("<body><div style=\"visibility:hidden\"><div><div>deep</div></div></div><div>ok</div></body>") == Strs{"ok"});
    }

    // ---- z-index / paint order ------------------------------------------------------------------
    {
        const std::string abs = "position:absolute;";
        CHECK(paintedTexts("<body><div style=\"" + abs + "z-index:2\">A</div><div style=\"" + abs + "z-index:1\">B</div><div style=\"" + abs + "\">C</div></body>")
              == (Strs{"C", "B", "A"}));         // auto (layer 0), then z=1, then z=2 - not source order
        CHECK(paintedTexts("<body>flow<div style=\"" + abs + "z-index:-1\">D</div></body>")
              == (Strs{"D", "flow"}));           // negative z paints BELOW normal-flow content
        CHECK(paintedTexts("<body><div style=\"" + abs + "z-index:1\">A</div><div style=\"" + abs + "z-index:1\">B</div></body>")
              == (Strs{"A", "B"}));              // ties: tree order
        CHECK(paintedTexts("<body><div style=\"" + abs + "z-index:0\">A</div><div style=\"" + abs + "\">B</div></body>")
              == (Strs{"A", "B"}));              // z-index:0 and auto share layer 0, in tree order
    }
    {   // z-index on a STATIC box does nothing (correct CSS, not a bug)
        CHECK(paintedTexts("<body><div style=\"z-index:5\">one</div><div>two</div></body>") == (Strs{"one", "two"}));
        CHECK(paintedTexts("<body><div>one</div><div style=\"z-index:-5\">two</div><div style=\"position:relative;z-index:-5\">three</div></body>")
              == (Strs{"three", "one", "two"})); // only the positioned one moved
        Laid L = lay("<body><div id=s style=\"z-index:9\">x</div><div id=r style=\"position:relative;z-index:9\">y</div></body>");
        CHECK(!L.id("s").hasZIndex && L.id("r").hasZIndex);
    }
    {   // positioned (z auto) paints after in-flow non-positioned content
        CHECK(paintedTexts("<body><div style=\"position:relative\">rel</div><div>static</div></body>") == (Strs{"static", "rel"}));
    }
    {   // a positioned z-index starts a stacking context: its child cannot climb above the next sibling
        const std::string a = "position:absolute;";
        CHECK(paintedTexts("<body><div style=\"" + a + "z-index:1\">P<div style=\"" + a + "z-index:99\">PC</div></div>"
                           "<div style=\"" + a + "z-index:2\">Q</div></body>") == (Strs{"P", "PC", "Q"}));
        // ...but with z-index:auto there is no context, so the child DOES take part in the outer one
        CHECK(paintedTexts("<body><div style=\"" + a + "\">P<div style=\"" + a + "z-index:99\">PC</div></div>"
                           "<div style=\"" + a + "z-index:2\">Q</div></body>") == (Strs{"P", "Q", "PC"}));
    }

    // ---- overflow: hidden -> clip ------------------------------------------------------------------
    {
        Laid L = lay("<body><div id=o style=\"overflow:hidden;width:50px;height:20px;border:2px solid #000\">long text that overflows the little box</div>"
                     "<div id=free>free</div></body>");
        CHECK(L.id("o").clipsChildren && !L.id("free").clipsChildren);
        auto list = paint(L.root);
        auto borders = bordersOf(list);
        CHECK(borders.size() == 1);
        if (borders.size() == 1) CHECK(!borders[0].hasClip);       // a box's own border is not clipped by itself
        int clipped = 0, unclipped = 0;
        for (auto& t : drawTextsOf(list)) {
            if (t.hasClip) {
                ++clipped;
                // padding box of o: inside the 2px border, 50 x 20
                CHECKF(t.clip.x, 2); CHECKF(t.clip.y, 2); CHECKF(t.clip.w, 50); CHECKF(t.clip.h, 20);
            } else {
                ++unclipped;
                CHECK(t.text == "free");
            }
        }
        CHECK(clipped > 1 && unclipped == 1);
    }
    {   // nested clips intersect
        Laid L = lay("<body><div style=\"overflow:hidden;width:100px;height:100px\">"
                     "<div style=\"overflow:hidden;width:200px;height:30px;margin-left:50px\">txt</div></div></body>");
        auto texts = drawTextsOf(paint(L.root));
        CHECK(texts.size() == 1);
        if (texts.size() == 1) {
            CHECK(texts[0].hasClip);
            CHECKF(texts[0].clip.x, 50); CHECKF(texts[0].clip.y, 0); CHECKF(texts[0].clip.w, 50); CHECKF(texts[0].clip.h, 30);
        }
    }
    {   // an absolute box is clipped by overflow only if the overflow box is (or is inside) its containing block
        const std::string inner = "<div id=esc style=\"position:absolute;left:0;top:0;width:100px;height:10px;border:1px solid #000\"></div></div></div></body>";
        Laid L = lay("<body><div style=\"position:relative;width:300px;height:100px\"><div style=\"overflow:hidden;width:20px;height:20px\">" + inner);
        auto b = bordersOf(paint(L.root));
        CHECK(b.size() == 1);
        if (b.size() == 1) CHECK(!b[0].hasClip);                     // its containing block is OUTSIDE the clipper: it escapes
        Laid M = lay("<body><div style=\"position:relative;width:300px;height:100px\"><div style=\"position:relative;overflow:hidden;width:20px;height:20px\">" + inner);
        auto c = bordersOf(paint(M.root));
        CHECK(c.size() == 1);
        if (c.size() == 1) {
            CHECK(c[0].hasClip);                                     // the clipper IS its containing block: clipped
            CHECKF(c[0].clip.w, 20); CHECKF(c[0].clip.h, 20);
        }
        Laid N = lay("<body><div style=\"overflow:hidden;width:20px;height:20px\">hi</div></body>");
        CHECK(drawTextsOf(paint(N.root))[0].hasClip);
        Laid O = lay("<body><div style=\"overflow:visible;width:20px;height:20px\">hi</div></body>");
        CHECK(!drawTextsOf(paint(O.root))[0].hasClip);
    }


    // ---- colours (background-color / color / border-color) ---------------------------------------------
    {
        using namespace lucyn::paint;
        Laid L = lay("<body><div id=a style=\"background-color:#102030;border:2px solid #ff0000;color:blue;width:100px;height:20px\">hello</div></body>");
        auto list = paint(L.root);
        CHECK(list.size() == 3);                                         // rect, border, text - in THAT order
        if (list.size() == 3) {
            auto* r = std::get_if<DrawRect>(&list[0]);
            auto* b = std::get_if<DrawBorder>(&list[1]);
            auto* t = std::get_if<DrawText>(&list[2]);
            CHECK(r && b && t);
            if (r && b && t) {
                CHECK(r->fill && r->color.r == 0x10 && r->color.g == 0x20 && r->color.b == 0x30 && r->color.a == 255);
                CHECKF(r->x, 0); CHECKF(r->y, 0); CHECKF(r->w, 100 + 4); CHECKF(r->h, 20 + 4);   // the BORDER box
                CHECK(b->color.r == 255 && b->color.g == 0 && b->color.b == 0);
                CHECK(t->color.r == 0 && t->color.g == 0 && t->color.b == 255);
            }
        }
        CHECK(L.id("a").hasBackground);
    }
    {   // defaults are unchanged: no background item, black text, grey border
        using namespace lucyn::paint;
        Laid L = lay("<body><div style=\"border:1px solid\">x</div></body>");
        auto list = paint(L.root);
        CHECK(list.size() == 2);
        if (list.size() == 2) {
            auto* b = std::get_if<DrawBorder>(&list[0]);
            auto* t = std::get_if<DrawText>(&list[1]);
            CHECK(b && b->color.r == 128 && b->color.g == 128 && b->color.b == 128);
            CHECK(t && t->color.r == 0 && t->color.g == 0 && t->color.b == 0);
        }
        Laid M = lay("<body><div style=\"background-color:transparent\">x</div><div style=\"background-color:rgba(0,0,0,0)\">y</div></body>");
        bool anyRect = false;
        for (auto& it : paint(M.root)) if (std::holds_alternative<DrawRect>(it)) anyRect = true;
        CHECK(!anyRect);                                                 // fully transparent: nothing to draw
        Laid N = lay("<body><div style=\"background-color:rgba(255,0,0,0.5)\">x</div></body>");
        auto nl = paint(N.root);
        CHECK(!nl.empty() && std::holds_alternative<DrawRect>(nl[0]));
        if (!nl.empty()) if (auto* r = std::get_if<DrawRect>(&nl[0])) CHECK(r->color.a == 128);   // translucent kept for the painter to blend
    }
    {   // `color` is inherited by descendant blocks and overridable; `background-color` is not inherited
        using namespace lucyn::paint;
        Laid L = lay("<body><div style=\"color:#00ff00;background-color:#eeeeee\">outer<div>inner</div><div style=\"color:red\">redtext</div></div></body>");
        auto texts = drawTextsOf(paint(L.root));
        CHECK(texts.size() == 3);
        if (texts.size() == 3) {
            CHECK(texts[0].text == "outer"   && texts[0].color.g == 255 && texts[0].color.r == 0);
            CHECK(texts[1].text == "inner"   && texts[1].color.g == 255 && texts[1].color.r == 0);   // inherited
            CHECK(texts[2].text == "redtext" && texts[2].color.r == 255 && texts[2].color.g == 0);   // overridden
        }
        int rects = 0;
        for (auto& it : paint(L.root)) if (std::holds_alternative<DrawRect>(it)) ++rects;
        CHECK(rects == 1);                                                // only the outer div has a background
    }
    {   // an absolutely positioned box inherits colour from its DOM PARENT (not from its containing block)
        using namespace lucyn::paint;
        Laid L = lay("<body><div style=\"color:#00ff00\"><div id=x style=\"position:absolute;left:0;top:0\">abs</div></div></body>");
        auto texts = drawTextsOf(paint(L.root));
        CHECK(texts.size() == 1);
        if (texts.size() == 1) CHECK(texts[0].color.g == 255 && texts[0].color.r == 0);
        Laid M = lay("<body style=\"color:#0000ff\"><div>x</div></body>");  // <body> colour reaches everything
        auto mt = drawTextsOf(paint(M.root));
        CHECK(mt.size() == 1);
        if (mt.size() == 1) CHECK(mt[0].color.b == 255 && mt[0].color.r == 0);
    }
    {   // backgrounds respect visibility, and are clipped by an overflow ancestor
        using namespace lucyn::paint;
        Laid L = lay("<body><div style=\"visibility:hidden;background-color:red\">x</div></body>");
        CHECK(paint(L.root).empty());
        Laid M = lay("<body><div style=\"overflow:hidden;width:30px;height:30px\"><div style=\"background-color:red;width:200px;height:200px\"></div></div></body>");
        bool found = false;
        for (auto& it : paint(M.root))
            if (auto* r = std::get_if<DrawRect>(&it)) { found = true; CHECK(r->hasClip); CHECKF(r->clip.w, 30); CHECKF(r->clip.h, 30); CHECKF(r->w, 200); }
        CHECK(found);
    }

    // ---- <body> / <html> style ------------------------------------------------------------------------
    {
        Laid L = lay("<body style=\"padding:10px;border:1px solid #000\"><div id=a>x</div></body>");
        CHECKF(L.root.paddingLeft, 10);
        CHECKF(L.root.borderWidth, 1);
        CHECKF(L.id("a").x, 11);
        CHECKF(L.id("a").width, 400 - 22);
        CHECKF(L.root.height, LH() + 22);
        CHECK(bordersOf(paint(L.root)).size() == 1);                 // the body's own border is drawn
    }
    {
        Laid L = lay("<body style=\"position:absolute;left:50px;top:50px\"><div id=a>x</div></body>");
        CHECKF(L.root.x, 0);                                         // position on <body> is ignored
        CHECKF(L.root.y, 0);
        CHECKF(L.id("a").y, 0);
        Laid N = lay("<body style=\"display:none\"><div>x</div></body>");
        CHECK(N.root.children.empty());
        CHECKF(N.root.height, 0);
    }
    {   // plain documents are unchanged by all of this
        Laid L = lay("<body><h1>Title</h1><p>Some text here.</p></body>");
        CHECKF(L.root.x, 0);
        CHECKF(L.root.y, 0);
        CHECKF(L.root.width, 400);
        CHECK(textsOf(paint(L.root)) == (Strs{"Title", "Some text here."}));
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("layout: all tests passed\n");
    return 0;
}
