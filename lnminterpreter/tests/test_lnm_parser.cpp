// Parser tests: LNM_SPEC.md v0.2 examples verbatim, then each syntax rule, then errors.
#include <cstdio>
#include <lucyn/lnm/parser.h>

using namespace lucyn::lnm;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
#define CHECKEQ(a, b) do { std::string _a = (a), _b = (b); if (_a != _b) { std::printf("FAIL %s:%d\n--- got ---\n%s\n--- want ---\n%s\n", __FILE__, __LINE__, _a.c_str(), _b.c_str()); ++failures; } } while (0)

namespace {
int errors(const ParseResult& r) {
    int n = 0;
    for (auto& d : r.diagnostics) if (d.severity == Diagnostic::Severity::Error) ++n;
    return n;
}
int warnings(const ParseResult& r) { return static_cast<int>(r.diagnostics.size()) - errors(r); }
bool anyMessageHas(const ParseResult& r, const std::string& needle) {
    for (auto& d : r.diagnostics) if (d.message.find(needle) != std::string::npos) return true;
    return false;
}
}  // namespace

int main() {
    // ---- the spec's section 11 full example, verbatim ------------------------------------------
    {
        const char* src = R"(grab(./shell.html)
grab.js(./api.js)

# Styles
#nav-bar {
    color: #111111
    padding: 10px
    radius: 0px
}

.nav-item {
    textcolor: #aaaaaa
    padding: 4px 12px
    transition: 100ms
}

.nav-item:onhover {
    textcolor: #ffffff
    scale: 1.03
}

# Navigation
#home-btn:onclick {
    switch.replace: ./home.html
}

#settings-btn:onclick {
    switch.overlay: ./settings.html
}

#tooltip-icon:onhover {
    switch.tooltip(top): ./tooltip.html
}

# Forms
form#search-bar {
    :onsubmit {
        runjs: handleSearch()
    }
}
)";
        ParseResult r = parse(src);
        CHECK(r.ok());
        CHECK(r.diagnostics.empty());
        CHECKEQ(dump(r.program),
                "grab html ./shell.html\n"
                "grab js ./api.js\n"
                "rule \"#nav-bar\" static\n"
                "  style color: #111111\n"
                "  style padding: 10px\n"
                "  style radius: 0px\n"
                "rule \".nav-item\" static\n"
                "  style textcolor: #aaaaaa\n"
                "  style padding: 4px 12px\n"
                "  style transition: 100ms\n"
                "rule \".nav-item\" on:onhover\n"
                "  style textcolor: #ffffff\n"
                "  style scale: 1.03\n"
                "rule \"#home-btn\" on:onclick\n"
                "  switch.replace -> ./home.html\n"
                "rule \"#settings-btn\" on:onclick\n"
                "  switch.overlay -> ./settings.html\n"
                "rule \"#tooltip-icon\" on:onhover\n"
                "  switch.tooltip(top) -> ./tooltip.html\n"
                "rule \"form#search-bar\" on:onsubmit\n"      // nested block flattened; no empty static rule
                "  runjs handleSearch args=[]\n");
    }

    // ---- comments: `#` is a comment only before whitespace / EOL / EOF / another # --------------
    {
        ParseResult r = parse("# a comment\n#id { color: #fff }\n## banner ##\n.c { color: #abc   # trailing\n padding: 2px }\n#");
        CHECK(r.ok());
        CHECKEQ(dump(r.program),
                "rule \"#id\" static\n  style color: #fff\n"
                "rule \".c\" static\n  style color: #abc\n  style padding: 2px\n");
    }

    // ---- separators, one-liners, line endings, BOM -------------------------------------------------
    {
        ParseResult r = parse("#a { color: red; padding: 1px; margin: 2px }\r\n#b {\r\n  color: blue;\r\n}\r\n");
        CHECK(r.ok());
        CHECKEQ(dump(r.program),
                "rule \"#a\" static\n  style color: red\n  style padding: 1px\n  style margin: 2px\n"
                "rule \"#b\" static\n  style color: blue\n");
        ParseResult b = parse("\xEF\xBB\xBF#a { color: red }");
        CHECK(b.ok() && b.program.rules.size() == 1);
    }

    // ---- grab forms ------------------------------------------------------------------------------------
    {
        ParseResult r = parse("grab(./a.html)\ngrab(\"./b b.html\")\ngrab.js( ./x.js )\ngrab ( ./c.html )  # note\n");
        CHECK(r.ok());
        CHECK(r.program.grabs.size() == 4);
        CHECK(r.program.grabs.size() == 4 && r.program.grabs[1].path == "./b b.html");   // quotes stripped, spaces kept
        CHECK(r.program.grabs.size() == 4 && r.program.grabs[2].kind == Grab::Kind::Js && r.program.grabs[2].path == "./x.js");
        CHECK(r.program.grabs.size() == 4 && r.program.grabs[3].path == "./c.html");
        // `grab` as a selector-looking word without parens is just a rule
        ParseResult t = parse("grab { color: red }");
        CHECK(t.ok() && t.program.grabs.empty() && t.program.rules.size() == 1);
    }

    // ---- selectors ----------------------------------------------------------------------------------------
    {
        ParseResult r = parse("#a, .b,\n div > p.x:onclick { color: red }\nul   li  a { padding: 1px }");
        CHECK(r.ok());
        CHECK(r.program.rules.size() == 2);
        CHECK(r.program.rules.size() == 2 && r.program.rules[0].selector == "#a, .b, div > p.x");   // list kept, event stripped
        CHECK(r.program.rules.size() == 2 && r.program.rules[0].event == "onclick");
        CHECK(r.program.rules.size() == 2 && r.program.rules[1].selector == "ul li a");             // whitespace collapsed
    }

    // ---- every event in the spec, and an unknown one (warning, still parsed) ---------------------------
    {
        for (const char* ev : {"onclick", "onhover", "ondoubleclick", "onsubmit", "onfocus", "onblur", "onrightclick"}) {
            ParseResult r = parse(std::string("#x:") + ev + " { color: red }");
            CHECK(r.ok() && r.diagnostics.empty());
            CHECK(r.program.rules.size() == 1 && r.program.rules[0].event == ev);
        }
        ParseResult r = parse("#x:onwiggle { color: red }");
        CHECK(r.ok() && warnings(r) == 1);
        CHECK(r.program.rules.size() == 1 && r.program.rules[0].event == "onwiggle");
        // a CSS pseudo-class that is not an event stays part of the selector
        ParseResult p = parse("a:first-child { color: red }");
        CHECK(p.ok() && p.program.rules.size() == 1 && p.program.rules[0].selector == "a:first-child" && p.program.rules[0].event.empty());
    }

    // ---- nested event blocks: mixed with static declarations, several per rule ----------------------
    {
        ParseResult r = parse("form#f {\n  padding: 4px\n  :onfocus { color: red }\n  :onsubmit {\n    runjs: go()\n  }\n}");
        CHECK(r.ok());
        CHECKEQ(dump(r.program),
                "rule \"form#f\" static\n  style padding: 4px\n"
                "rule \"form#f\" on:onfocus\n  style color: red\n"
                "rule \"form#f\" on:onsubmit\n  runjs go args=[]\n");
    }

    // ---- switch actions -----------------------------------------------------------------------------------
    {
        ParseResult r = parse("#b:onclick {\n switch.tooltip(bottom): ./d.html\n switch.tooltip( Cursor ): ./c.html\n"
                              " switch.tooltip(left): ./l.html\n switch.tooltip(right): ./r.html\n switch.replace: \"./q q.html\"\n}");
        CHECK(r.ok());
        CHECKEQ(dump(r.program),
                "rule \"#b\" on:onclick\n"
                "  switch.tooltip(bottom) -> ./d.html\n"
                "  switch.tooltip(cursor) -> ./c.html\n"
                "  switch.tooltip(left) -> ./l.html\n"
                "  switch.tooltip(right) -> ./r.html\n"
                "  switch.replace -> ./q q.html\n");
    }

    // ---- runjs: arguments are raw JS text, split at top-level commas only ---------------------------------
    {
        ParseResult r = parse("#d:onclick { runjs: setTheme(\"dark\") }\n#e:onclick { runjs: f(1, \"a,b\", [1,2], {k: (3)}, g(4,5)) }\n#g:onclick { runjs: ns.go() }");
        CHECK(r.ok());
        CHECKEQ(dump(r.program),
                "rule \"#d\" on:onclick\n  runjs setTheme args=[\"dark\"]\n"
                "rule \"#e\" on:onclick\n  runjs f args=[1|\"a,b\"|[1,2]|{k: (3)}|g(4,5)]\n"
                "rule \"#g\" on:onclick\n  runjs ns.go args=[]\n");
        auto* j = std::get_if<RunJs>(&r.program.rules[0].actions[0]);
        CHECK(j && j->source == "setTheme(\"dark\")");
    }

    // ---- values keep their punctuation ---------------------------------------------------------------------
    {
        ParseResult r = parse("#a { color: rgba(0, 0, 0, 0.5)\n shadow: 0 2px 4px rgba(0,0,0,.2); border: 1px solid #ccc }");
        CHECK(r.ok());
        CHECK(r.program.rules[0].style.size() == 3);
        CHECK(r.program.rules[0].style[0].value == "rgba(0, 0, 0, 0.5)");
        CHECK(r.program.rules[0].style[1].value == "0 2px 4px rgba(0,0,0,.2)");
        CHECK(r.program.rules[0].style[2].value == "1px solid #ccc");
        ParseResult q = parse("#a { Color : RED }");     // names are lowercased; values are not touched
        CHECK(q.program.rules[0].style[0].name == "color" && q.program.rules[0].style[0].value == "RED");
    }

    // ---- source locations -------------------------------------------------------------------------------------
    {
        ParseResult r = parse("grab(./a.html)\n\n#x {\n    color: red\n}\n");
        CHECK(r.program.grabs[0].loc.line == 1 && r.program.grabs[0].loc.col == 1);
        CHECK(r.program.rules[0].loc.line == 3 && r.program.rules[0].loc.col == 1);
        CHECK(r.program.rules[0].style[0].loc.line == 4 && r.program.rules[0].style[0].loc.col == 5);
    }

    // ---- errors: each is reported, parsing continues, later rules still arrive ------------------------------
    {
        ParseResult r = parse("#a { color red }\n#b { color: blue }\n");
        CHECK(!r.ok() && errors(r) == 1);
        CHECK(anyMessageHas(r, "expected ':' after 'color red'"));
        CHECK(r.program.rules.size() == 2 && r.program.rules[1].style.size() == 1);   // #b survived
    }
    {
        ParseResult r = parse("#a { color: }\n#b { padding: 1px }");
        CHECK(errors(r) == 1 && anyMessageHas(r, "missing value for 'color'"));
        CHECK(r.program.rules.size() == 2 && r.program.rules[1].style.size() == 1);
    }
    {
        ParseResult r = parse("#a:onclick { switch.tooltip: ./t.html }");
        CHECK(errors(r) == 1 && anyMessageHas(r, "needs a position"));
        r = parse("#a:onclick { switch.tooltip(middle): ./t.html }");
        CHECK(errors(r) == 1 && anyMessageHas(r, "unknown tooltip position 'middle'"));
        r = parse("#a:onclick { switch.replace(top): ./t.html }");
        CHECK(errors(r) == 1 && anyMessageHas(r, "takes no position"));
        r = parse("#a:onclick { switch.popup: ./t.html }");
        CHECK(errors(r) == 1 && anyMessageHas(r, "unknown switch mode"));
        r = parse("#a:onclick { switch.tooltip(top: ./t.html }");
        CHECK(errors(r) >= 1);
    }
    {
        ParseResult r = parse("#a:onclick { runjs: handleLogin }");
        CHECK(errors(r) == 1 && anyMessageHas(r, "runjs expects a call"));
        r = parse("#a:onclick { runjs: 9bad() }");
        CHECK(errors(r) == 1);
        r = parse("#a:onclick { runjs: f(1,,2) }");
        CHECK(errors(r) == 1 && anyMessageHas(r, "empty argument"));
    }
    {
        ParseResult r = parse("#a { color: red\n#b { color: blue }\n");   // missing '}' for #a: the rest becomes its body
        CHECK(!r.ok());
        CHECK(anyMessageHas(r, "nested rules") || anyMessageHas(r, "missing '}'"));
        r = parse("#a {\n color: red\n");
        CHECK(!r.ok() && anyMessageHas(r, "missing '}' for rule '#a'"));
        CHECK(r.diagnostics[0].loc.line == 1);                                   // reported at the rule that never closed
        r = parse("#a { color: red }\n}\n");
        CHECK(!r.ok() && anyMessageHas(r, "unmatched '}'") && r.diagnostics[0].loc.line == 2);
        r = parse("#a\n#b { color: red }");
        CHECK(errors(r) == 1 && anyMessageHas(r, "expected '{' after selector '#a'"));
        CHECK(r.program.rules.size() == 1 && r.program.rules[0].selector == "#b");   // recovered
        r = parse("{ color: red }");
        CHECK(errors(r) == 1 && anyMessageHas(r, "empty selector"));
        r = parse(":onclick { color: red }");
        CHECK(errors(r) == 1 && anyMessageHas(r, "needs a selector"));
    }
    {
        ParseResult r = parse("#a { .b { color: red } color: blue }");
        CHECK(errors(r) == 1 && anyMessageHas(r, "nested rules are not supported"));
        CHECK(r.program.rules.size() == 1 && r.program.rules[0].style.size() == 1 && r.program.rules[0].style[0].value == "blue");
        r = parse("#a:onclick { :onhover { color: red } }");
        CHECK(errors(r) == 1 && anyMessageHas(r, "cannot be nested inside an event rule"));
        r = parse("#a { :wiggle { color: red } }");
        CHECK(errors(r) == 1 && anyMessageHas(r, "is not an event"));
        r = parse("grab(./a.html\n#x { color: red }");
        CHECK(errors(r) == 1 && anyMessageHas(r, "missing ')'") && r.program.rules.size() == 1);
        r = parse("grab()");
        CHECK(errors(r) == 1 && anyMessageHas(r, "needs a file path"));
        r = parse("grab(./a.html) junk");
        CHECK(errors(r) == 1 && anyMessageHas(r, "unexpected text after grab"));
        r = parse("#a { bad name: red }");
        CHECK(r.ok() && r.program.rules[0].style[0].name == "badname");   // inner spaces are dropped from names (like `switch. replace`)
        r = parse("#a { bad$name: red }");
        CHECK(errors(r) == 1 && anyMessageHas(r, "invalid property name"));
    }

    // ---- hostile / degenerate input never crashes or hangs -----------------------------------------------------
    {
        const char* nasty[] = {"", " ", "\n\n\n", "{", "}", "{}", "{{{{", "}}}}", "#", "##", "grab", "grab(", "grab.js(", ":", "::", "#a{", "#a{:", "#a{:onclick",
                               "#a{:onclick{", "#a{x", "#a{x:", "#a{runjs:", "#a{runjs:(", "#a{switch.:x}", "\"", "'", "#a{x:\"unterminated\n}", "#a{x:(((}",
                               "\xff\xfe\x00 junk"};
        for (const char* n : nasty) {
            ParseResult r = parse(std::string(n));
            (void)r;
        }
        std::string deep;
        for (int i = 0; i < 20000; ++i) deep += "#a{";
        ParseResult r = parse(deep);                 // must terminate (errors are fine)
        CHECK(!r.ok());
        CHECK(parse("").ok());
    }

    // ---- formatDiagnostic ----------------------------------------------------------------------------------------
    {
        Diagnostic d{Diagnostic::Severity::Error, SourceLoc{3, 7}, "boom"};
        CHECK(formatDiagnostic(d, "app.lnm") == "app.lnm:3:7: error: boom");
        d.severity = Diagnostic::Severity::Warning;
        CHECK(formatDiagnostic(d) == "3:7: warning: boom");
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("lnm parser: all tests passed\n");
    return 0;
}
