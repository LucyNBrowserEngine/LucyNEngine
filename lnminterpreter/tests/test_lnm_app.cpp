// compile + App tests: LNM -> styled DOM -> layout -> display list, with an in-memory file system.
#include <cmath>
#include <cstdio>
#include <lucyn/lnm/app.h>
#include <lucyn/paint/software_painter.h>
#include <map>
#include <variant>

using namespace lucyn::lnm;
using lucyn::dom::Node;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
#define CHECKEQ(a, b) do { std::string _a = (a), _b = (b); if (_a != _b) { std::printf("FAIL %s:%d\n  got:  %s\n  want: %s\n", __FILE__, __LINE__, _a.c_str(), _b.c_str()); ++failures; } } while (0)
#define CHECKF(a, b) do { double _a = (a), _b = (b); if (std::fabs(_a - _b) > 1e-3) { std::printf("FAIL %s:%d  %s (=%g) != %s (=%g)\n", __FILE__, __LINE__, #a, _a, #b, _b); ++failures; } } while (0)

namespace {
using FS = std::map<std::string, std::string>;
FileReader memReader(FS& fs) {
    return [&fs](const std::string& p) -> std::optional<std::string> {
        auto it = fs.find(p);
        if (it == fs.end()) return std::nullopt;
        return it->second;
    };
}
std::string styleOf(App& app, const char* sel) {
    Node* n = app.document()->querySelector(sel);
    if (!n) return "<no such element>";
    const std::string* s = n->attr("style");
    return s ? *s : "";
}
const lucyn::layout::Box* boxFor(const lucyn::layout::Box& b, const std::string& id) {
    if (b.node && b.node->isElement() && b.node->id() == id) return &b;
    for (auto& c : b.children)
        if (auto* r = boxFor(c, id)) return r;
    return nullptr;
}
bool hasDiag(const App& a, Diagnostic::Severity sev, const std::string& needle) {
    for (auto& d : a.diagnostics())
        if (d.severity == sev && d.message.find(needle) != std::string::npos) return true;
    return false;
}
const auto Err = Diagnostic::Severity::Error;
const auto Warn = Diagnostic::Severity::Warning;
}  // namespace

int main() {
    // ---- translateStyle ------------------------------------------------------------------------------
    {
        auto t = [](const char* n, const char* v) { std::vector<Diagnostic> d; StyleDecl s; s.name = n; s.value = v; auto r = translateStyle(s, &d); return std::make_pair(r, d.size()); };
        auto c = t("color", "#111");           CHECK(c.first.size() == 1 && c.first[0] == CssDecl("background-color", "#111") && c.second == 0);
        auto x = t("textcolor", "red");        CHECK(x.first[0] == CssDecl("color", "red") && x.second == 0);
        auto r = t("radius", "4px");           CHECK(r.first[0] == CssDecl("border-radius", "4px"));
        auto s = t("scale", "1.05");           CHECK(s.first[0] == CssDecl("transform", "scale(1.05)"));
        auto h = t("shadow", "0 1px 2px #000");CHECK(h.first[0] == CssDecl("box-shadow", "0 1px 2px #000"));
        auto n = t("transition", "150ms");     CHECK(n.first[0] == CssDecl("transition", "all 150ms"));
        for (const char* same : {"padding", "margin", "border", "opacity"}) { auto q = t(same, "1px"); CHECK(q.first[0] == CssDecl(same, "1px") && q.second == 0); }
        auto bad = t("color", "notacolour");   CHECK(bad.second == 1);                      // still emitted, with a warning
        auto unk = t("width", "10px");         CHECK(unk.first[0] == CssDecl("width", "10px") && unk.second == 1);  // pass-through + warning
    }

    // ---- selector helpers --------------------------------------------------------------------------------
    {
        CHECK(specificity("div") == 1);
        CHECK(specificity(".a") == 100);
        CHECK(specificity("#a") == 10000);
        CHECK(specificity("div.a#b") == 10101);
        CHECK(specificity("ul li a") == 3);
        CHECK(specificity("div > p.x.y") == 202);
        CHECK(specificity("a[href]") == 101);
        CHECK(specificity("a:first-child") == 101);
        CHECK(specificity("*") == 0);
        CHECK(specificity("#a .b") > specificity(".a .b .c .d"));
        auto parts = splitSelectorList("a, b > c ,\n d[x=\"1,2\"]");
        CHECK(parts.size() == 3 && parts[0] == "a" && parts[1] == "b > c" && parts[2] == "d[x=\"1,2\"]");
    }

    // ---- cascade: specificity, then source order; the element's own style wins ----------------------------------
    {
        FS fs{{"a.html", "<body><div id=x class=c>hi</div><p id=p style=\"color: blue\">p</p></body>"}};
        App app(memReader(fs));
        CHECK(app.load("grab(./a.html)\n"
                       "#x { padding: 1px }\n.c { padding: 2px; margin: 3px }\ndiv { padding: 4px; margin: 5px }\n#x { margin: 6px }\n") == false);   // './a.html' != 'a.html'
        fs["./a.html"] = fs["a.html"];
        CHECK(app.load("grab(./a.html)\n#x { padding: 1px }\n.c { padding: 2px; margin: 3px }\ndiv { padding: 4px; margin: 5px }\n#x { margin: 6px }\n"
                       "p { textcolor: red }\n"));
        // applied low -> high specificity (later declaration wins): div(1), .c(100), #x(10000: both #x rules in source order)
        CHECKEQ(styleOf(app, "#x"), "padding: 4px; margin: 5px; padding: 2px; margin: 3px; padding: 1px; margin: 6px");
        CHECKEQ(styleOf(app, "#p"), "color: red; color: blue");     // the element's own style comes LAST, so it wins
        // and layout agrees: #x ends up padding 1px / margin 6px
        auto root = app.layoutView();
        const auto* bx = boxFor(root, "x");
        CHECK(bx != nullptr);
        if (bx) { CHECKF(bx->paddingLeft, 1); CHECKF(bx->marginLeft, 6); }
        const auto* bp = boxFor(root, "p");
        if (bp) CHECKF(bp->textColor.b, 255);                          // inline blue beat the rule's red
    }
    {   // comma lists, descendant selectors, and rules that match nothing are harmless
        FS fs{{"./a.html", "<body><div id=a><span id=s>x</span></div><div id=b></div></body>"}};
        App app(memReader(fs));
        CHECK(app.load("grab(./a.html)\n#a, #b { padding: 2px }\n#a span { margin: 1px }\n.nothing { padding: 9px }\n"));
        CHECKEQ(styleOf(app, "#a"), "padding: 2px");
        CHECKEQ(styleOf(app, "#b"), "padding: 2px");
        CHECKEQ(styleOf(app, "#s"), "margin: 1px");
    }

    // ---- loading: grabs, errors ---------------------------------------------------------------------------
    {
        FS fs{{"./shell.html", "<body><div id=nav>n</div></body>"}, {"./side.html", "<body><div id=side>s</div></body>"}};
        App app(memReader(fs));
        CHECK(app.load("grab(./shell.html)\ngrab(./side.html)\ngrab.js(./api.js)\ngrab.js(./utils.js)\n#side { padding: 5px }"));
        CHECK(app.document()->querySelector("#nav") && app.document()->querySelector("#side"));   // second grab joined the first's body
        CHECKEQ(styleOf(app, "#side"), "padding: 5px");
        CHECK(app.jsFiles().size() == 2 && app.jsFiles()[0] == "./api.js" && app.jsFiles()[1] == "./utils.js");
        CHECK(app.currentViewPath() == "./shell.html");

        App m(memReader(fs));
        CHECK(!m.load("grab(./missing.html)"));
        CHECK(hasDiag(m, Err, "cannot read './missing.html'"));
        CHECK(m.document() == nullptr);

        App e(memReader(fs));
        CHECK(e.load("grab(./shell.html)\ngrab(./gone.html)"));          // a missing EXTRA grab is reported, not fatal
        CHECK(hasDiag(e, Err, "cannot read './gone.html'") && e.document() != nullptr);

        App p(memReader(fs));
        CHECK(!p.load("grab(./shell.html)\n#a { color red }"));          // parse error: nothing loads
        CHECK(!p.diagnostics().empty() && p.document() == nullptr);

        App n(memReader(fs));
        CHECK(!n.load("#a { color: red }") && hasDiag(n, Err, "no grab(...)"));
        CHECK(!n.load("grab.js(./a.js)") && hasDiag(n, Err, "no grab(...)"));
    }

    // ---- end to end: LNM -> DOM -> layout -> display list ------------------------------------------------------
    {
        FS fs{{"./shell.html", "<body><div id=nav-bar>menu</div></body>"}};
        App app(memReader(fs), ViewOptions{400, 300});
        CHECK(app.load("grab(./shell.html)\n#nav-bar {\n  color: #111111\n  textcolor: #ffffff\n  padding: 10px\n  border: 2px solid #ff0000\n}\n"));
        auto root = app.layoutView();
        const auto* nav = boxFor(root, "nav-bar");
        CHECK(nav != nullptr);
        if (nav) {
            CHECKF(nav->paddingLeft, 10);
            CHECKF(nav->borderWidth, 2);
            CHECK(nav->hasBackground && nav->background.r == 0x11);
            CHECK(nav->textColor.r == 255 && nav->textColor.g == 255);
            CHECKF(nav->height, 10 + 10 + 4 + lucyn::paint::SoftwarePainter::lineHeight(16.0f));
        }
        auto list = lucyn::layout::paint(root);
        bool rect = false, border = false, text = false;
        for (auto& it : list) {
            if (auto* r = std::get_if<lucyn::paint::DrawRect>(&it)) rect = rect || (r->color.r == 0x11 && r->color.b == 0x11);
            if (auto* b = std::get_if<lucyn::paint::DrawBorder>(&it)) border = border || (b->color.r == 255 && b->color.g == 0);
            if (auto* t = std::get_if<lucyn::paint::DrawText>(&it)) text = text || (t->text == "menu" && t->color.g == 255);
        }
        CHECK(rect && border && text);                                   // the LNM styles reached the painter's input
    }

    // ---- events: style changes apply, clear, and count ------------------------------------------------------------
    {
        FS fs{{"./a.html", "<body><div id=a class=item style=\"margin: 1px\">a</div><div id=b class=item>b</div></body>"}};
        App app(memReader(fs));
        CHECK(app.load("grab(./a.html)\n.item { padding: 2px }\n.item:onhover { textcolor: #fff\n scale: 1.05\n transition: 150ms }\n#a:onclick { padding: 9px }\n"));
        Node* a = app.document()->querySelector("#a");
        Node* b = app.document()->querySelector("#b");
        CHECKEQ(styleOf(app, "#a"), "padding: 2px; margin: 1px");
        CHECK(app.trigger(a, "onhover") == 1);
        CHECKEQ(styleOf(app, "#a"), "padding: 2px; margin: 1px; color: #fff; transform: scale(1.05); transition: all 150ms");
        CHECKEQ(styleOf(app, "#b"), "padding: 2px");                     // only the hovered element changed
        app.clearEventStyles(a);
        CHECKEQ(styleOf(app, "#a"), "padding: 2px; margin: 1px");         // restored exactly
        CHECK(app.trigger(a, "onclick") == 1 && styleOf(app, "#a").find("padding: 9px") != std::string::npos);
        CHECK(app.trigger(b, "onclick") == 0);                           // #b matches no :onclick rule
        CHECK(app.trigger(b, "onfocus") == 0);
        CHECK(app.trigger(nullptr, "onclick") == 0);
        app.clearEventStyles(b);                                         // never changed: harmless
        CHECKEQ(styleOf(app, "#b"), "padding: 2px");
        // an element with no original style attribute gets it removed again, not left as ""
        app.trigger(b, "onhover");
        app.clearEventStyles(b);
        CHECKEQ(styleOf(app, "#b"), "padding: 2px");
    }
    {
        FS fs{{"./a.html", "<body><div id=x>x</div></body>"}};
        App app(memReader(fs));
        CHECK(app.load("grab(./a.html)\n#x:onclick { color: red }\n"));
        Node* x = app.document()->querySelector("#x");
        CHECK(x->attr("style") == nullptr);
        app.trigger(x, "onclick");
        CHECK(x->attr("style") && *x->attr("style") == "background-color: red");
        app.clearEventStyles(x);
        CHECK(x->attr("style") == nullptr);                              // the attribute is gone again
    }

    // ---- LNM.style: restyle at runtime -------------------------------------------------------------------------------
    {
        FS fs{{"./a.html", "<body><div id=x class=k>x</div><div class=k>y</div></body>"}};
        App app(memReader(fs));
        CHECK(app.load("grab(./a.html)"));
        CHECK(app.restyle(".k", {{"color", "#fff"}, {"radius", "4px"}}) == 2);
        CHECKEQ(styleOf(app, "#x"), "background-color: #fff; border-radius: 4px");
        CHECK(app.restyle(".nope", {{"color", "red"}}) == 0);
    }

    // ---- switch.replace -------------------------------------------------------------------------------------------------
    {
        FS fs{{"./shell.html", "<body><button id=home-btn>home</button><div id=old>old</div></body>"},
              {"./home.html", "<body><div id=welcome class=card>welcome</div></body>"}};
        App app(memReader(fs));
        CHECK(app.load("grab(./shell.html)\n.card { padding: 7px }\n#home-btn:onclick { switch.replace: ./home.html }\n"));
        Node* btn = app.document()->querySelector("#home-btn");
        CHECK(app.trigger(btn, "onclick") == 1);
        CHECK(app.currentViewPath() == "./home.html");
        CHECK(app.document()->querySelector("#welcome") && !app.document()->querySelector("#old"));   // replaced ENTIRELY
        CHECKEQ(styleOf(app, "#welcome"), "padding: 7px");                // the same rules style the new view
        // a failing switch keeps the current view and reports
        CHECK(!app.switchReplace("./nope.html"));
        CHECK(app.currentViewPath() == "./home.html" && app.document()->querySelector("#welcome"));
        CHECK(hasDiag(app, Err, "switch.replace: cannot read './nope.html'"));
    }

    // ---- switch.overlay ----------------------------------------------------------------------------------------------------
    {
        FS fs{{"./shell.html", "<body><div id=base>base</div></body>"},
              {"./settings.html", "<body><div id=panel class=card>settings</div></body>"},
              {"./about.html", "<body><div id=about>about</div></body>"}};
        App app(memReader(fs), ViewOptions{400, 300});
        CHECK(app.load("grab(./shell.html)\n.card { padding: 3px }"));
        Node* base = app.document()->querySelector("#base");
        CHECK(app.switchOverlay("./settings.html"));
        CHECK(app.overlayCount() == 1);
        CHECK(app.document()->querySelector("#base") && app.document()->querySelector("#panel"));    // both visible: it is a layer ON TOP
        (void)base;                                                       // (the old pointer is dead after a rebuild, by contract)
        Node* layer = app.document()->querySelector("[data-lnm-layer=overlay]");
        CHECK(layer != nullptr);
        if (layer) CHECKEQ(*layer->attr("style"), "position:absolute;left:0;top:0;width:400px;height:300px;z-index:100");
        CHECKEQ(styleOf(app, "#panel"), "padding: 3px");                  // overlay content is styled by the same rules
        auto root = app.layoutView();
        const auto* lb = boxFor(root, "panel");
        CHECK(lb != nullptr);
        if (lb) { CHECKF(lb->x, 0); CHECKF(lb->y, 0); }                   // the layer sits at the viewport origin, out of flow
        const auto* bb = boxFor(root, "base");
        if (bb) CHECKF(bb->y, 0);                                         // and did not push the base view down

        CHECK(app.switchOverlay("./about.html"));
        CHECK(app.overlayCount() == 2);
        auto layers = app.document()->querySelectorAll("[data-lnm-layer=overlay]");
        CHECK(layers.size() == 2);
        if (layers.size() == 2) {
            CHECK(layers[0]->attr("style")->find("z-index:100") != std::string::npos);
            CHECK(layers[1]->attr("style")->find("z-index:101") != std::string::npos);                // later overlay stacks above
        }
        // painting order: base first, then overlay 1, then overlay 2 (z-index decides, not tree order alone)
        std::vector<std::string> texts;
        for (auto& it : lucyn::layout::paint(app.layoutView()))
            if (auto* t = std::get_if<lucyn::paint::DrawText>(&it)) texts.push_back(t->text);
        CHECK(texts.size() == 3 && texts[0] == "base" && texts[1] == "settings" && texts[2] == "about");
        CHECK(app.dismissOverlay() && app.overlayCount() == 1 && !app.document()->querySelector("#about"));
        CHECK(app.dismissOverlay() && app.overlayCount() == 0 && !app.document()->querySelector("#panel"));
        CHECK(!app.dismissOverlay());                                     // nothing left to close
        CHECK(!app.switchOverlay("./missing.html") && app.overlayCount() == 0);
        // replace clears overlays
        app.switchOverlay("./settings.html");
        app.switchReplace("./about.html");
        CHECK(app.overlayCount() == 0 && app.document()->querySelector("#about") && !app.document()->querySelector("#panel"));
    }

    // ---- switch.tooltip: anchored from the anchor's REAL laid-out box --------------------------------------------------
    {
        FS fs{{"./shell.html", "<body><div id=anchor style=\"margin-left:100px;margin-top:50px;width:60px;height:20px\">a</div></body>"},
              {"./tip.html", "<body><div id=tip class=t style=\"width:30px;height:10px\"></div></body>"}};
        App app(memReader(fs), ViewOptions{400, 300});
        CHECK(app.load("grab(./shell.html)\n.t { padding: 0px }\n"));
        Node* anchor = app.document()->querySelector("#anchor");
        auto tipBox = [&]() { static lucyn::layout::Box root; root = app.layoutView(); return boxFor(root, "tip"); };

        CHECK(app.switchTooltip("./tip.html", TooltipPosition::Top, anchor));
        CHECK(app.hasTooltip() && app.document()->querySelector("#anchor") == anchor);       // NO rebuild: pointers stay valid
        const auto* t = tipBox();
        CHECK(t != nullptr);
        if (t) { CHECKF(t->x, 100); CHECKF(t->y, 50 - 10); CHECKF(t->width, 30); }           // bottom edge touches the anchor's top
        CHECKEQ(styleOf(app, "#tip"), "padding: 0px; width:30px;height:10px");               // tooltip content got the rules too (once)
        CHECKEQ(styleOf(app, "#anchor"), "margin-left:100px;margin-top:50px;width:60px;height:20px");   // base not re-styled

        CHECK(app.switchTooltip("./tip.html", TooltipPosition::Bottom, anchor));
        CHECK(app.document()->querySelectorAll("[data-lnm-layer=tooltip]").size() == 1);     // one tooltip at a time
        t = tipBox();
        if (t) { CHECKF(t->x, 100); CHECKF(t->y, 70); }
        CHECK(app.switchTooltip("./tip.html", TooltipPosition::Left, anchor));
        t = tipBox();
        if (t) { CHECKF(t->x, 100 - 30); CHECKF(t->y, 50); }
        CHECK(app.switchTooltip("./tip.html", TooltipPosition::Right, anchor));
        t = tipBox();
        if (t) { CHECKF(t->x, 160); CHECKF(t->y, 50); }
        CHECK(app.switchTooltip("./tip.html", TooltipPosition::Cursor, nullptr, Point{123, 77}));
        t = tipBox();
        if (t) { CHECKF(t->x, 123); CHECKF(t->y, 77); }
        CHECK(app.dismissTooltip() && !app.hasTooltip() && !app.document()->querySelector("#tip"));
        CHECK(!app.dismissTooltip());

        CHECK(!app.switchTooltip("./tip.html", TooltipPosition::Top, nullptr));              // no anchor -> clear error, no tooltip
        CHECK(hasDiag(app, Err, "anchor element has no box"));
        CHECK(!app.switchTooltip("./tip.html", TooltipPosition::None, anchor));
        CHECK(!app.switchTooltip("./missing.html", TooltipPosition::Top, anchor));
        CHECK(!app.hasTooltip());
    }
    {   // a tooltip from a RULE anchors to the element that fired it; an overlay switch dismisses it
        FS fs{{"./shell.html", "<body><div id=icon style=\"margin-left:20px;width:10px;height:10px\">i</div></body>"},
              {"./tip.html", "<body><div id=tip style=\"width:5px;height:5px\"></div></body>"},
              {"./o.html", "<body><div id=o>o</div></body>"}};
        App app(memReader(fs), ViewOptions{400, 300});
        CHECK(app.load("grab(./shell.html)\n#icon:onhover { switch.tooltip(right): ./tip.html }\n#icon:onclick { switch.overlay: ./o.html }\n"));
        Node* icon = app.document()->querySelector("#icon");
        CHECK(app.trigger(icon, "onhover") == 1 && app.hasTooltip());
        auto root = app.layoutView();
        const auto* tip = boxFor(root, "tip");
        if (tip) { CHECKF(tip->x, 30); CHECKF(tip->y, 0); }
        CHECK(app.trigger(icon, "onclick") == 1);
        CHECK(!app.hasTooltip() && app.overlayCount() == 1);
    }

    // ---- runjs: registration, dispatch, dev/prod behaviour (spec section 7) -----------------------------------------------
    {
        FS fs{{"./a.html", "<body><button id=dark-btn>d</button></body>"}};
        App app(memReader(fs));
        CHECK(app.load("grab(./a.html)\ngrab.js(./settings.js)\n#dark-btn:onclick { runjs: setTheme(\"dark\", 2) }\n"));
        std::vector<std::string> calls;
        app.setRunJsHandler([&](const RunJs& j, const Node* el) { calls.push_back(j.function + "(" + j.args[0] + "|" + j.args[1] + ")@" + el->name); });

        Node* btn = app.document()->querySelector("#dark-btn");
        app.trigger(btn, "onclick");
        CHECK(calls.empty());                                            // not registered: the element does nothing
        CHECK(hasDiag(app, Warn, "setTheme() was called but is not registered"));

        auto dev = app.validateRunJs();
        CHECK(dev.size() == 1 && dev[0].severity == Err);
        CHECK(dev.size() == 1 && dev[0].message.find("runjs: setTheme() \xE2\x80\x94 function not registered in ./settings.js") == 0);
        CHECK(dev.size() == 1 && dev[0].message.find("Register it with LNM.register({ setTheme }) and rebuild.") != std::string::npos);

        app.setMode(BuildMode::Prod);
        auto prod = app.validateRunJs();
        CHECK(prod.size() == 1 && prod[0].severity == Warn);
        CHECK(prod.size() == 1 && prod[0].message.find("setTheme() was called on #dark-btn:onclick but is not registered.") == 0);
        CHECK(prod.size() == 1 && prod[0].message.find("#dark-btn will not respond as expected.") != std::string::npos);

        app.registerFunction("setTheme");
        CHECK(app.validateRunJs().empty());
        app.trigger(btn, "onclick");
        CHECK(calls.size() == 1 && calls[0] == "setTheme(\"dark\"|2)@button");
    }
    {   // a style change AND an action on the same rule both happen; actions run in source order
        FS fs{{"./a.html", "<body><div id=x>x</div></body>"}};
        App app(memReader(fs));
        CHECK(app.load("grab(./a.html)\n#x:onclick {\n  textcolor: red\n  runjs: first()\n  runjs: second()\n}\n"));
        std::vector<std::string> order;
        app.registerFunction("first");
        app.registerFunction("second");
        app.setRunJsHandler([&](const RunJs& j, const Node*) { order.push_back(j.function); });
        Node* x = app.document()->querySelector("#x");
        app.trigger(x, "onclick");
        CHECK(order.size() == 2 && order[0] == "first" && order[1] == "second");
        CHECKEQ(styleOf(app, "#x"), "color: red");
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("lnm app: all tests passed\n");
    return 0;
}
