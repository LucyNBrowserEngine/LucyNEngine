// Tests for lucyn/layout/style.h: the inline `style=""` parser and its value rules.
#include <cmath>
#include <cstdio>
#include <lucyn/dom/dom.h>
#include <lucyn/layout/style.h>

using namespace lucyn::layout;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

namespace {
bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
bool is(const std::optional<float>& o, float v) { return o && near(*o, v); }
}  // namespace

int main() {
    // ---- parseDeclarations: shape -----------------------------------------------------
    {
        auto d = parseDeclarations("color:red; Width : 10PX ;;  bogus ; a:b:c ;:novalue; novalue2: ;");
        CHECK(d.size() == 3);
        CHECK(d.size() >= 1 && d[0].name == "color" && d[0].value == "red");
        CHECK(d.size() >= 2 && d[1].name == "width" && d[1].value == "10px");   // lowercased + trimmed
        CHECK(d.size() >= 3 && d[2].name == "a" && d[2].value == "b:c");        // split at the FIRST colon
        CHECK(parseDeclarations("").empty());
        CHECK(parseDeclarations("   ;;; ").empty());
    }
    // comments, quotes, parentheses
    {
        auto d = parseDeclarations("width:/* ten */10px; /* gone: 1px */ height:5px");
        CHECK(d.size() == 2 && d[0].value == "10px" && d[1].name == "height" && d[1].value == "5px");
        auto e = parseDeclarations("background:url(data:image/png;base64,AAA);width:5px");
        CHECK(e.size() == 2 && e[1].name == "width");                           // ';' inside () did not split
        auto f = parseDeclarations("content:\"a;b\";width:5px");
        CHECK(f.size() == 2 && f[1].name == "width");                           // ';' inside quotes did not split
        auto g = parseDeclarations("width:5px; /* unterminated comment height:9px");
        CHECK(g.size() == 1 && g[0].name == "width");
    }
    // !important
    {
        auto d = parseDeclarations("width:5px ! important; height:6px !IMPORTANT; margin:1px");
        CHECK(d.size() == 3);
        CHECK(d[0].important && d[0].value == "5px");
        CHECK(d[1].important && d[1].value == "6px");
        CHECK(!d[2].important);
    }

    // ---- parsePx ------------------------------------------------------------------------
    {
        float v = 99;
        CHECK(parsePx("10px", v) && near(v, 10));
        CHECK(parsePx("-3.5px", v) && near(v, -3.5f));
        CHECK(parsePx("0", v) && near(v, 0));
        CHECK(parsePx(".5px", v) && near(v, 0.5f));
        CHECK(parsePx("+4", v) && near(v, 4));
        CHECK(parsePx("  12PX ", v) && near(v, 12));
        v = 99;
        CHECK(!parsePx("10%", v));
        CHECK(!parsePx("2em", v));
        CHECK(!parsePx("1.5rem", v));
        CHECK(!parsePx("auto", v));
        CHECK(!parsePx("", v));
        CHECK(!parsePx("px", v));
        CHECK(!parsePx("7.", v));
        CHECK(!parsePx("-", v));
        CHECK(!parsePx(".", v));
        CHECK(!parsePx("1e3px", v));
        CHECK(!parsePx("10 px", v));
        CHECK(!parsePx("calc(1px+2px)", v));
        CHECK(near(v, 99));                              // failures never write `out`
        CHECK(parsePx("99999999999px", v) && v <= 1.0e6f);  // absurd values are clamped
    }

    // ---- box shorthands: 1 / 2 / 3 / 4 values -----------------------------------------
    {
        Style s = styleFromText("margin:1px 2px 3px 4px");
        CHECK(is(s.margin[SideTop], 1) && is(s.margin[SideRight], 2) && is(s.margin[SideBottom], 3) && is(s.margin[SideLeft], 4));
        s = styleFromText("margin:5px 10px");
        CHECK(is(s.margin[SideTop], 5) && is(s.margin[SideRight], 10) && is(s.margin[SideBottom], 5) && is(s.margin[SideLeft], 10));
        s = styleFromText("margin:1px 2px 3px");
        CHECK(is(s.margin[SideTop], 1) && is(s.margin[SideRight], 2) && is(s.margin[SideBottom], 3) && is(s.margin[SideLeft], 2));
        s = styleFromText("padding:7px");
        for (int i = 0; i < 4; ++i) CHECK(is(s.padding[i], 7));
        s = styleFromText("margin:-4px");                       // negative margins are legal CSS
        CHECK(is(s.margin[SideLeft], -4));
        s = styleFromText("padding:-4px");                      // negative padding is not
        CHECK(!s.padding[SideLeft]);
        s = styleFromText("margin:1px 2px 3px 4px 5px");        // five values: invalid, ignored whole
        CHECK(!s.margin[SideTop]);
        s = styleFromText("margin:10% 5px");                    // one bad token ignores the whole declaration
        CHECK(!s.margin[SideTop] && !s.margin[SideLeft]);
        s = styleFromText("margin:auto");
        CHECK(!s.margin[SideLeft]);                             // auto margins: unsupported, ignored
        s = styleFromText("");
        CHECK(!s.margin[0] && !s.padding[0] && !s.width && !s.height && !s.display && !s.visible && !s.zIndex);
        CHECK(s.position == Position::Static && !s.clipsOverflow);
    }
    // source order, longhand vs shorthand, and !important
    {
        Style s = styleFromText("margin:10px; margin-top:0");
        CHECK(is(s.margin[SideTop], 0) && is(s.margin[SideBottom], 10));
        s = styleFromText("margin-top:0; margin:10px");           // later shorthand wins
        CHECK(is(s.margin[SideTop], 10));
        s = styleFromText("margin-top:0 !important; margin:10px");
        CHECK(is(s.margin[SideTop], 0) && is(s.margin[SideBottom], 10));
        s = styleFromText("width:50px; width:10%");              // invalid later value: the earlier one stays
        CHECK(is(s.width, 50));
        s = styleFromText("width:50px; width:auto");            // auto resets
        CHECK(!s.width);
        s = styleFromText("width:-5px");                         // negative size: invalid
        CHECK(!s.width);
        s = styleFromText("WIDTH:12PX;HEIGHT:3px");
        CHECK(is(s.width, 12) && is(s.height, 3));
    }

    // ---- border ---------------------------------------------------------------------------
    {
        CHECK(is(styleFromText("border:1px solid red").borderWidth, 1));
        CHECK(is(styleFromText("border:2px dashed #f00").borderWidth, 2));
        CHECK(is(styleFromText("border:solid").borderWidth, 3));          // style with no width: `medium`
        CHECK(is(styleFromText("border:thick solid").borderWidth, 5));
        CHECK(is(styleFromText("border:1px none").borderWidth, 0));       // style none beats a width
        CHECK(is(styleFromText("border:0").borderWidth, 0));
        CHECK(!styleFromText("border:banana").borderWidth);               // nothing recognisable: ignored
        CHECK(is(styleFromText("border-width:4px 1px").borderWidth, 4));  // ONE width: the first value
        CHECK(!styleFromText("border-width:-1px").borderWidth);
        CHECK(is(styleFromText("border:1px solid; border-width:6px").borderWidth, 6));
    }

    // ---- display --------------------------------------------------------------------------
    {
        CHECK(styleFromText("display:block").display == Display::Block);
        CHECK(styleFromText("display:inline").display == Display::Inline);
        CHECK(styleFromText("display:none").display == Display::None);
        CHECK(styleFromText("DISPLAY: NONE").display == Display::None);
        CHECK(styleFromText("display:flex").display == Display::Block);          // flexbox is not built: a plain block
        CHECK(styleFromText("display:inline-block").display == Display::Inline);
        CHECK(!styleFromText("display:bogus").display);
        CHECK(styleFromText("display:none; display:bogus").display == Display::None);
    }

    // ---- position + offsets -------------------------------------------------------------
    {
        CHECK(styleFromText("position:relative").position == Position::Relative);
        CHECK(styleFromText("position:absolute").position == Position::Absolute);
        CHECK(styleFromText("position:fixed").position == Position::Static);      // unsupported: ignored
        CHECK(styleFromText("position:sticky").position == Position::Static);
        CHECK(styleFromText("position:absolute; position:fixed").position == Position::Absolute);
        Style s = styleFromText("top:-5px; left:auto; right:3px; bottom:0");
        CHECK(is(s.offset[SideTop], -5) && !s.offset[SideLeft] && is(s.offset[SideRight], 3) && is(s.offset[SideBottom], 0));
        s = styleFromText("left:10%");
        CHECK(!s.offset[SideLeft]);
    }

    // ---- visibility, z-index, overflow --------------------------------------------------
    {
        CHECK(styleFromText("visibility:hidden").visible == false);
        CHECK(styleFromText("visibility:collapse").visible == false);
        CHECK(styleFromText("visibility:visible").visible == true);
        CHECK(!styleFromText("visibility:bogus").visible);

        CHECK(styleFromText("z-index:7").zIndex == 7);
        CHECK(styleFromText("z-index:-2").zIndex == -2);
        CHECK(styleFromText("z-index:+3").zIndex == 3);
        CHECK(!styleFromText("z-index:abc").zIndex);
        CHECK(!styleFromText("z-index:1.5").zIndex);
        CHECK(!styleFromText("z-index:3; z-index:auto").zIndex);

        CHECK(styleFromText("overflow:hidden").clipsOverflow);
        CHECK(styleFromText("overflow:scroll").clipsOverflow);
        CHECK(styleFromText("overflow:auto").clipsOverflow);
        CHECK(!styleFromText("overflow:visible").clipsOverflow);
        CHECK(!styleFromText("overflow:hidden; overflow:visible").clipsOverflow);
        CHECK(styleFromText("overflow-x:hidden").clipsOverflow);
        CHECK(styleFromText("overflow-y:hidden").clipsOverflow);
        CHECK(!styleFromText("overflow:bogus").clipsOverflow);
        CHECK(styleFromText("overflow:visible hidden").clipsOverflow);            // two-value form
    }


    // ---- colours -----------------------------------------------------------------------------------
    {
        using lucyn::paint::Color;
        auto eq = [](const std::optional<Color>& c, int r, int g, int b, int a) {
            return c && c->r == r && c->g == g && c->b == b && c->a == a;
        };
        CHECK(eq(parseColor("#fff"), 255, 255, 255, 255));
        CHECK(eq(parseColor("#1a1a1a"), 0x1a, 0x1a, 0x1a, 255));
        CHECK(eq(parseColor("#F00"), 255, 0, 0, 255));                       // case-insensitive, nibble doubling
        CHECK(eq(parseColor("#0f08"), 0, 255, 0, 0x88));
        CHECK(eq(parseColor("#11223344"), 0x11, 0x22, 0x33, 0x44));
        CHECK(eq(parseColor("  #abc  "), 0xaa, 0xbb, 0xcc, 255));
        CHECK(eq(parseColor("rgb(255, 0, 0)"), 255, 0, 0, 255));
        CHECK(eq(parseColor("rgb(1 2 3)"), 1, 2, 3, 255));                  // space separated
        CHECK(eq(parseColor("rgba(0, 0, 0, 0.5)"), 0, 0, 0, 128));
        CHECK(eq(parseColor("RGBA(10,20,30,1)"), 10, 20, 30, 255));
        CHECK(eq(parseColor("rgb(100%, 0%, 50%)"), 255, 0, 128, 255));
        CHECK(eq(parseColor("rgba(0,0,0,50%)"), 0, 0, 0, 128));
        CHECK(eq(parseColor("rgb(300, -5, 0)"), 255, 0, 0, 255));           // clamped, like a browser
        CHECK(eq(parseColor("red"), 255, 0, 0, 255));
        CHECK(eq(parseColor("Transparent"), 0, 0, 0, 0));
        CHECK(eq(parseColor("grey"), 128, 128, 128, 255) && eq(parseColor("gray"), 128, 128, 128, 255));
        CHECK(!parseColor(""));
        CHECK(!parseColor("#"));
        CHECK(!parseColor("#ff"));
        CHECK(!parseColor("#fffff"));
        CHECK(!parseColor("#ggg"));
        CHECK(!parseColor("rgb(1,2)"));
        CHECK(!parseColor("rgb(1,2,3"));
        CHECK(!parseColor("rgb(a,b,c)"));
        CHECK(!parseColor("notacolour"));
        CHECK(!parseColor("10px"));

        Style s = styleFromText("background-color:#123; color:rgb(1,2,3); border-color:blue");
        CHECK(eq(s.backgroundColor, 0x11, 0x22, 0x33, 255));
        CHECK(eq(s.color, 1, 2, 3, 255));
        CHECK(eq(s.borderColor, 0, 0, 255, 255));
        s = styleFromText("background:url(x.png) no-repeat #abcdef");        // shorthand: the colour token wins, image ignored
        CHECK(eq(s.backgroundColor, 0xab, 0xcd, 0xef, 255));
        s = styleFromText("background:none");
        CHECK(!s.backgroundColor);
        s = styleFromText("background-color:red; background-color:bogus");   // invalid: earlier value stays
        CHECK(eq(s.backgroundColor, 255, 0, 0, 255));
        s = styleFromText("color:#f00; color:#00f");
        CHECK(eq(s.color, 0, 0, 255, 255));                                  // later wins
        s = styleFromText("border:2px solid #0f0");                          // width AND colour from the shorthand
        CHECK(is(s.borderWidth, 2) && eq(s.borderColor, 0, 255, 0, 255));
        s = styleFromText("border:1px solid red; border-color:blue");
        CHECK(is(s.borderWidth, 1) && eq(s.borderColor, 0, 0, 255, 255));
        s = styleFromText("border-color:red blue green yellow");            // 4 sides, one colour: the first
        CHECK(eq(s.borderColor, 255, 0, 0, 255));
        s = styleFromText("border-color:red bogus");                         // any bad token ignores the declaration
        CHECK(!s.borderColor);
        s = styleFromText("");
        CHECK(!s.backgroundColor && !s.color && !s.borderColor);
    }

    // ---- font-size ------------------------------------------------------------------------
    {
        CHECK(is(styleFromText("font-size:12px").fontSize, 12));
        CHECK(is(styleFromText("font-size: 0.5px").fontSize, 0.5f));
        CHECK(is(styleFromText("font-size:xx-small").fontSize, 9));
        CHECK(is(styleFromText("font-size:medium").fontSize, 16));
        CHECK(!styleFromText("font-size:larger").fontSize);  // unsupported relative sizes: ignore
        CHECK(!styleFromText("font-size:bogus").fontSize);
        CHECK(is(styleFromText("font-size:10px; font-size:20px").fontSize, 20));
    }

    // ---- styleForElement: UA `hidden` attribute vs author style -------------------------
    {
        auto doc = lucyn::dom::parseHtml(
            "<body><div id=a hidden></div><div id=b hidden style=\"display:block\"></div>"
            "<div id=c style=\"width:30px\"></div><span id=d></span></body>");
        auto styleOf = [&](const char* id) { return styleForElement(*doc->body()->querySelector(id)); };
        CHECK(styleOf("#a").display == Display::None);
        CHECK(styleOf("#b").display == Display::Block);    // author display overrides the UA [hidden] rule
        CHECK(!styleOf("#c").display && is(styleOf("#c").width, 30));
        CHECK(!styleOf("#d").display);
        CHECK(!styleForElement(doc->root()).width);        // a non-element gets an empty style
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("style: all tests passed\n");
    return 0;
}
