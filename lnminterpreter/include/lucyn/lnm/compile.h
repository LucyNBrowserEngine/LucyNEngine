// lucyn/lnm/compile.h - Program -> styled DOM (SESSION12_PLAN.md Part B, rung 2).
//
// LNM does not draw and does not lay out. It COMPILES to a dom::Document whose elements carry
// inline `style=""` attributes, and everything already built (layout::layoutDocument,
// paint::SoftwarePainter, screenshot, window) works on that document with zero new rendering
// code.
//
// Property translation (LNM_SPEC.md section 2 -> the CSS layout understands):
//     color      -> background-color          textcolor -> color
//     radius     -> border-radius             opacity   -> opacity
//     scale      -> transform: scale(v)       shadow    -> box-shadow
//     transition -> transition: all <v>       padding / margin / border -> same name
// layout renders background-color, color, padding, margin and border today. radius, opacity,
// scale, shadow and transition are emitted (so they are in the DOM the moment the painter
// learns them) but layout ignores them: there is no rounded-corner/alpha-layer/transform/
// animation support yet. Any OTHER property (width, display, position, ...) is passed through
// to layout verbatim with a warning, because LNM v0.2 lists only ten properties but a real
// view needs to size things.
//
// Cascade: rules are applied by SPECIFICITY (ids > classes/attributes > tags), then source
// order; an element's own `style=""` attribute always wins over every rule (as in CSS).
#pragma once
#include <lucyn/dom/dom.h>
#include <lucyn/lnm/ast.h>

#include <string>
#include <utility>
#include <vector>

namespace lucyn::lnm {

using CssDecl = std::pair<std::string, std::string>;   // name, value

// One LNM declaration -> zero or more CSS declarations. Appends warnings to `diags` (nullable).
std::vector<CssDecl> translateStyle(const StyleDecl& d, std::vector<Diagnostic>* diags = nullptr);

// "a, b > c, d" -> {"a", "b > c", "d"} (top-level commas only).
std::vector<std::string> splitSelectorList(const std::string& selectorList);

// CSS specificity of ONE selector (no commas): ids*10000 + (classes, attributes, pseudo-classes)*100 + types.
int specificity(const std::string& selector);

// Applies every static (event-less) rule to `doc` by writing inline styles. When `within` is
// given, only elements inside that subtree are touched (used for a freshly inserted tooltip).
// Call it ONCE per element: it prepends rule styles to whatever `style` the element already has.
void applyStaticRules(lucyn::dom::Document& doc, const Program& program, std::vector<Diagnostic>* diags = nullptr,
                      const lucyn::dom::Node* within = nullptr);

// Appends `decls` to an element's inline style (they win over what is there).
void appendInlineStyle(lucyn::dom::Node& el, const std::vector<CssDecl>& decls);

// True if `el` matches any selector of `selectorList` within `doc`.
bool matchesSelector(lucyn::dom::Document& doc, const std::string& selectorList, const lucyn::dom::Node* el);

}  // namespace lucyn::lnm
