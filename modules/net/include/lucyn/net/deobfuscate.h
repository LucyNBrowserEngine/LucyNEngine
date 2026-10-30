// lucyn/net/deobfuscate.h — best-effort JavaScript deobfuscation.
//
// A site serves a minified/obfuscated script; LucyN saves it verbatim (the raw
// view) and runs THIS over it to produce a second view next to it on disk
// (<name>.deobf.js). Text in, text out — the code is NEVER executed.
//
// What it does, in order:
//   1. re-indents the token stream (strings, templates, comments and regex
//      literals are moved as single units, so formatting can't corrupt them),
//   2. decodes string escapes (\xNN, \uNNNN, octal) to literal characters,
//   3. finds `var _0x1234 = ['..','..']` string arrays, follows the classic
//      push/shift rotation IIFE that hides them, and replaces constant index
//      reads (and the `f(a,b)` wrapper functions around them) with literals,
//   4. folds constant string concatenation ("a"+"b" -> "ab"),
//   5. decodes constant atob()/decodeURIComponent()/unescape() calls.
//
// It is a heuristic, not a JS engine: when a pattern is not recognized with
// confidence the original text is left alone. A plain script comes back
// essentially unchanged (just re-indented).
#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace lucyn::net {

// What a deobfuscation attempt found/did. Logged by the engine, asserted on by
// the unit tests.
struct DeobfStats {
    std::size_t inBytes = 0;
    std::size_t outBytes = 0;
    std::size_t stringsDecoded = 0;   // constant index reads replaced by literals
    std::size_t concatsFolded = 0;    // "a"+"b" collapsed to "ab"
    std::size_t escapesDecoded = 0;   // \xNN / \uNNNN / octal escapes made literal
    std::size_t base64Decoded = 0;    // constant atob(...) results inlined
    std::size_t arraysFound = 0;      // string-array declarations recognized
    bool lookedObfuscated = false;    // arrays found (decodable or not)
};

// Returns the (best-effort) deobfuscated source. `stats` is optional.
std::string deobfuscate(std::string_view js, DeobfStats* stats = nullptr);

}  // namespace lucyn::net
