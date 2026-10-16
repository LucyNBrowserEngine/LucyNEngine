// lucyn/layout/css.h - minimal CSS stylesheet + selector matcher for layout.
//
// Built on top of style.h (declaration parsing is reused). What this file adds:
//   Stylesheet: a list of (selector, declarations) rules, with simple selector
//   matching and CSS custom property (`--var`) resolution.
//
// What is supported (deliberately small; "good enough for VitePress"):
//   - rule blocks of the form  selectorList { decl; decl; ... }
//   - selectorList: comma-separated list of compound selectors with optional
//     descendant combinators (whitespace). `>`, `+`, `~` are NOT supported
//     (treated as whitespace so `.a > .b` matches like `.a .b`).
//   - compound selector: zero or more of: `*` (universal), tag (e.g. `div`),
//     `.class`, `#id`, `[attr]` or `[attr=value]` (only `=` operator),
//     `:not(simple)`. Pseudo-elements (`::before`) and most pseudo-classes
//     (`:hover`, `:nth-child`, ...) are dropped from the selector — the rule
//     still matches as if those weren't there. `:root` matches `<html>`.
//   - declarations: same parser as style.h (so `width`, `color`, etc. are
//     understood). `var(--x)` resolves against custom properties seen on
//     `:root` / `.dark` (we don't actually toggle `.dark`; we keep both sets
//     and prefer `.dark` if the html element has it).
//   - source order = cascade order; later rules win. `!important` beats
//     non-`!important`. ID specificity beats class beats tag, but only
//     when both rules are non-important (or both important); otherwise the
//     important flag dominates.
//
// What is NOT supported: @media queries are flattened (treated as matching
// always), @keyframes / @font-face / @import / @charset / @namespace are
// skipped (the controller fetches @import'ed stylesheets itself, so the
// CSS parser doesn't need to). No flexbox, grid, animations, transforms,
// transitions, etc. - layout only knows what layout knows.
#pragma once
#include <lucyn/dom/dom.h>
#include <lucyn/layout/style.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace lucyn::layout {

// One compound selector. A rule's full selector is a sequence of these
// (descendant combinator between them).
struct SimpleSelector {
    std::string tag;                 // lowercase, "" = any
    std::vector<std::string> classes;
    std::string id;                  // "" = none
    bool universal = false;          // `*`
    std::vector<std::pair<std::string, std::string>> attrs;  // [name, value]; value="" means just [name]
    // pseudo-classes that survive (currently only :not(...) is consumed, others dropped)
};

// A selector is a chain of compound selectors. Matching walks the DOM
// ancestor chain: the last compound must match the element; each earlier
// compound must match some ancestor (descendant combinator).
using CompoundChain = std::vector<SimpleSelector>;

struct CssRule {
    CompoundChain selector;
    std::vector<Declaration> decls;  // source order; `!important` already extracted
    // Specificity for cascade: (id_count, class_count, tag_count). Higher wins.
    int specA = 0, specB = 0, specC = 0;
};

// A whole stylesheet. Custom properties (CSS variables) live in a flat map
// keyed by name; resolution prefers `.dark` variants when `html.dark` is set
// (VitePress and most frameworks toggle a `dark` class on <html>).
struct Stylesheet {
    std::vector<CssRule> rules;
    // CSS custom properties collected from `:root` (and bare `:root`-like rules).
    std::unordered_map<std::string, std::string> vars;
    // Same but from `.dark` rules. Empty unless the document's <html> carries
    // a `dark` class at apply time.
    std::unordered_map<std::string, std::string> darkVars;
    // True once we've seen at least one `.dark` rule — enables dark var lookups.
    bool hasDarkRules = false;

    // Parse a CSS text blob into this sheet. `baseUrl` is informational only
    // (kept for debugging notes; @import is not chased here). Idempotent —
    // multiple calls accumulate rules in source order, exactly like a real
    // browser that loads several stylesheets in order.
    void add(const std::string& cssText);
};

// Parses a single selector list (e.g. "h1, h2, .title") into one chain per
// comma-separated selector. Empty on parse failure (the rule is dropped).
std::vector<CompoundChain> parseSelectorList(const std::string& text);

// True if `chain` matches `el`, using the ancestor chain for descendant
// combinators. Walks up the DOM; `el` itself is tested against the last
// compound in the chain.
bool matches(const CompoundChain& chain, const lucyn::dom::Node& el);

// Computes the style for `el`: starts with UA defaults (`hidden` attribute),
// applies all matching rules from `sheet` (cascade by specificity + order,
// `!important` wins), then overlays the inline `style=""` attribute. `sheet`
// may be nullptr to skip stylesheet matching (legacy callers).
Style styleForElement(const lucyn::dom::Node& el, const Stylesheet* sheet);

// Resolves `var(--name)` references inside a value. Looks `--name` up in
// `sheet->vars` (and `sheet->darkVars` if `darkActive` is true). Recursively
// resolves `var(var(--x))` up to a small depth. Returns the original string
// (with `var(...)` still in it) on resolution failure.
std::string resolveVars(const std::string& value, const Stylesheet& sheet, bool darkActive);

}  // namespace lucyn::layout
