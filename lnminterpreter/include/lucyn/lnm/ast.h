// lucyn/lnm/ast.h - what a parsed .lnm file looks like (LNM_SPEC.md v0.2).
//
// The AST is deliberately flat and boring: a Program is the `grab` lines plus a
// list of Rules. A Rule is "for elements matching SELECTOR [when EVENT happens]:
// these style declarations and these actions". LNM has no variables, no
// conditionals and no loops (spec: "What LNM Does NOT Do"), so nothing here is
// recursive.
//
// A nested event block (spec section 9)
//     form#login-form { :onsubmit { runjs: handleLogin() } }
// is flattened at parse time into the equivalent
//     form#login-form:onsubmit { runjs: handleLogin() }
// i.e. into a Rule with event "onsubmit". Source order is preserved.
#pragma once
#include <string>
#include <variant>
#include <vector>

namespace lucyn::lnm {

struct SourceLoc {
    int line = 1;   // 1-based
    int col = 1;    // 1-based, in bytes
};

struct Diagnostic {
    enum class Severity { Warning, Error };
    Severity severity = Severity::Error;
    SourceLoc loc;
    std::string message;
};

// grab(./shell.html)  /  grab.js(./api.js)
struct Grab {
    enum class Kind { Html, Js };
    Kind kind = Kind::Html;
    std::string path;      // exactly as written, quotes stripped
    SourceLoc loc;
};

// One `property: value` line inside a rule. Names are lowercased; values are kept
// verbatim (trimmed). Whether the name is a real LNM property is the compiler's
// business, not the parser's.
struct StyleDecl {
    std::string name;
    std::string value;
    SourceLoc loc;
};

enum class SwitchMode { Replace, Overlay, Tooltip };
enum class TooltipPosition { None, Top, Bottom, Left, Right, Cursor };

// switch.replace: ./home.html   switch.overlay: ./settings.html   switch.tooltip(top): ./tip.html
struct SwitchAction {
    SwitchMode mode = SwitchMode::Replace;
    TooltipPosition position = TooltipPosition::None;   // required for Tooltip, None otherwise
    std::string target;                                  // path as written
    SourceLoc loc;
};

// runjs: setTheme("dark")
struct RunJs {
    std::string function;                // "setTheme"
    std::vector<std::string> args;       // raw JS source text of each argument: "\"dark\""
    std::string source;                  // the whole call as written: setTheme("dark")
    SourceLoc loc;
};

using Action = std::variant<SwitchAction, RunJs>;

struct Rule {
    std::string selector;                // CSS selector (list), whitespace-collapsed, WITHOUT the :event suffix
    std::string event;                   // "" = a static style rule; else "onclick", "onhover", ... (no colon)
    std::vector<StyleDecl> style;        // in source order
    std::vector<Action> actions;         // in source order
    SourceLoc loc;
};

struct Program {
    std::vector<Grab> grabs;             // source order
    std::vector<Rule> rules;             // source order
};

const char* toString(SwitchMode m);
const char* toString(TooltipPosition p);

}  // namespace lucyn::lnm
