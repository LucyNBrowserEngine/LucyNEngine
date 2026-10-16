// lucyn/lnm/parser.h - .lnm source text -> Program (LNM_SPEC.md v0.2).
//
// Grammar (newline-significant; `;` is an optional separator):
//
//   file      := { trivia | grab | rule }
//   grab      := "grab" "(" path ")"  |  "grab.js" "(" path ")"
//   rule      := selector [ ":on<event>" ] "{" body "}"
//   body      := { trivia | decl | action | ":on<event>" "{" body "}" }
//   decl      := property ":" value
//   action    := "switch.replace" ":" path
//              | "switch.overlay" ":" path
//              | "switch.tooltip" "(" top|bottom|left|right|cursor ")" ":" path
//              | "runjs" ":" function "(" [ args ] ")"
//
// Choices the spec leaves open (all are tested; all are listed in lnminterpreter/README.md):
//   - COMMENTS. The spec says comments use `#`, but `#` also starts id selectors and hex
//     colours. Rule: a `#` is a comment only when it begins a statement or follows
//     whitespace, AND is followed by whitespace, end of line, end of file, or another `#`.
//     So `# Styles` and `## ---` are comments; `#nav-bar {` and `color: #fff` are not;
//     `color: #fff   # trailing` has a trailing comment.
//   - EVENT SUFFIX. The `:on<name>` suffix is taken from the END of the whole selector text,
//     so `#a, #b:onclick` means "#a and #b, on click".
//   - UNKNOWN EVENTS (`:onfoo`) are accepted with a warning (spec: "More events will be added").
//   - UNKNOWN PROPERTIES are not the parser's problem: it keeps any `name: value`.
//   - A block that contains only nested event blocks produces just those event rules (no
//     empty static rule is emitted for it).
//
// The parser never throws and never stops at the first problem: it records a Diagnostic,
// skips to the next line (or past the offending block), and carries on, so one run reports
// every mistake in the file.
#pragma once
#include <lucyn/lnm/ast.h>

#include <string>
#include <vector>

namespace lucyn::lnm {

struct ParseResult {
    Program program;
    std::vector<Diagnostic> diagnostics;
    bool ok() const;   // true when there is no Error-severity diagnostic (warnings are fine)
};

ParseResult parse(const std::string& source);

// The events LNM_SPEC.md lists: onclick onhover ondoubleclick onsubmit onfocus onblur onrightclick.
const std::vector<std::string>& knownEvents();

// "file:line:col: error: message" (the compiler-style line a terminal or editor understands).
std::string formatDiagnostic(const Diagnostic& d, const std::string& file = "");

// A stable, human-readable dump of the AST. Used by `lucyn-lnm ast` and by the tests.
std::string dump(const Program& p);

}  // namespace lucyn::lnm
