# lnminterpreter/ — LNM (Lucy Native Markup)

The parser and runtime for the declarative UI language in `LNM_SPEC.md` (v0.2, the source of truth).
**Status (Session 12): the Must and Should rungs are built and tested; the Could rung is not.**

```
.lnm text --parse--> Program (AST) --compile--> dom::Document with inline style="" --layout--> Box tree --paint--> pixels
                                       ^ App: events, switch.replace/overlay/tooltip, runjs seam
```

LNM never draws and never lays out. It produces a `dom::Document`; `modules/layout` + `modules/paint` do the rest, so
every rendering feature those modules gain, LNM gets for free.

## Build and try it
```
cmake -S lnminterpreter -B b && cmake --build b -j && ctest --test-dir b --output-on-failure     # standalone (pulls dom/paint/layout)
b/lucyn-lnm ast    app.lnm                                    # print the AST + diagnostics
b/lucyn-lnm check  app.lnm                                    # load every grabbed file, report problems
b/lucyn-lnm render app.lnm out.png --width 520 --height 260 --do "#settings-btn" onclick
```
Root build: `LUCYN_MODULE_LNM` (default ON, needs layout). The folder is shaped like a module (`include/lucyn/lnm`, `src`,
`tools`, `tests`, own `CMakeLists.txt`, target `lucyn::lnm`); moving it to `modules/lnm/` is a rename. Left as `lnminterpreter/`
because the author asked for that folder.

## What exists
| Piece | Where | State |
|---|---|---|
| Parser: grab, grab.js, rules, `:on*` events, nested event blocks, `switch.*`, `runjs`, diagnostics with line:col, error recovery | `parser.h/.cpp` | done, tested (spec section 11 verbatim + error paths + hostile input) |
| Compile: LNM props -> CSS, specificity-aware cascade, inline style always wins | `compile.h/.cpp` | done |
| `App`: load, multi-grab, trigger/clearEventStyles, `LNM.style` (restyle), switch.replace / overlay / tooltip (anchored from the anchor's real layout box), dismiss, runjs seam + register + dev/prod validation | `app.h/.cpp` | done |
| `lucyn-lnm` CLI (ast / check / render) | `tools/` | done |
| Prod mode: compile `.lnm` into an embedded header; minified embedded assets | — | **not built** (a `FileReader` serving embedded bytes is the seam) |
| Hot reload (dev) | — | not built |
| Real `runjs` execution, `LNM.register`, the `LNM` JS global, `grab.js` loading | — | **not built**: needs the `js` module wired in. `App` exposes `setRunJsHandler` / `registerFunction` / `jsFiles()` for exactly that |
| Input: something must call `trigger()` (mouse hover/click -> element) | — | not built: `ui::Window` has no mouse-move/hit-test yet |
| Painter-side effects: `radius`, `opacity`, `scale`, `shadow`, `transition` | — | compiled into the DOM but **ignored by layout/paint** (no rounded corners, alpha layers, transforms or animation exist yet) |
| Re-render after a switch | — | whole-page rebuild (the plan's accepted first version), not a subtree update |

## Decisions the spec left open (all tested; change them freely, they are one place each)
1. **Comments vs `#id` vs `#hex`.** `#` is a comment only at a statement start / after whitespace AND followed by whitespace, EOL, EOF or another `#`.
   So `# Styles`, `## ---` and `color: #fff   # note` work; `#nav-bar {` and `color: #fff` are not comments. A comment written `#note` (no space) would be read as a selector.
2. **Event suffix** comes from the end of the whole selector text: `#a, #b:onclick` = both, on click. Unknown `:onfoo` is accepted with a warning.
3. **Several `grab(...)`**: the first html file is the page; the others' `<body>` contents are appended to its body.
4. **Rules are global to the app**: after `switch.replace`, the same rules style the new view. Overlay and tooltip content is styled too.
5. **`switch.replace` replaces everything** (including the shell the button lived in), as the spec says "entirely".
6. **Overlay** = a `position:absolute` viewport-sized layer, `z-index` 100, 101, ... **Tooltip** = a `position:absolute` layer, `z-index` 1000, one at a time, placed from the anchor's laid-out box
   (`top`: bottom edge on the anchor's top, left-aligned; `bottom`; `left`; `right`; `cursor`: at the mouse point).
7. **Closing overlays/tooltips** is not in the spec; `dismissOverlay()` / `dismissTooltip()` exist because a view you cannot close is not usable. When and how to call them is the UI's job.
8. **Properties**: the ten in the spec are translated; any other `name: value` is passed through to layout as CSS **with a warning** (so `width`, `display`, `position` work).
9. **Cascade**: by CSS specificity (id > class/attribute > tag), then source order; an element's own `style=""` beats every rule. No `!important`.
10. **`runjs` dev/prod** messages follow spec section 7 word for word (`validateRunJs()`); an unregistered call at runtime is skipped with a warning either way.

## Open questions for the author
- **Direction.** The stray text at the bottom of this README (pasted from a chat) says LNM may become "glue" that points at real `.css` files
  (`:hover`, `transition`, ...) with `LNM_SPEC.md` section 2 "stale". `LNM_SPEC.md` v0.2 as it stands still defines LNM-side styling, and that is what is built. If the
  direction really changed, the parser/AST/App/switching are unaffected; what changes is `compile.cpp` (emit/attach CSS instead of translating properties) and `layout` needs a real CSS parser (`<style>`, selectors, `.css` files).
- Transitions: the LNM spec (section 10) says the painter animates; `README.md` says CSS transitions never render, by design. They contradict; nothing animates today.
- Should a switched-in view get the app's rules (decision 4) or only its own?

Read before writing any code here:
1. `LNM_SPEC.md` (repo root) — the actual language spec. Source of truth; this README is not a substitute for it.
2. `LUCYN_ENGINE_API_SPEC.md` (repo root) — the engine config/API surface LNM may need to extend.
3. `SESSION12_PLAN.md` Part B — suggested ladder (parser → AST → DOM compilation → view-switching), and the
   reasoning for why LNM compiling *to* a `dom::Document` (rather than LNM knowing how to draw itself) is the
   point of building `layout`/`paint` first: every rendering feature those modules gain, LNM gets for free.




   Good, still here. Let me write this as a proper README that sits alongside the full spec — capturing the philosophy shift plainly so whoever (including future-you) opens it understands the "why" before the syntax.That's the README — drop it into the repo next to `LNM_SPEC.md`. The core of it:

- **LNM is glue, not a language.** No new colors, no new async, no new HTML — it just says "grab this HTML, load this CSS, when X happens apply Y or call Z." That's the whole job.
- **CSS owns color/animation**, the normal way (`transition`, etc.) — LNM just decides when a style block becomes active.
- **LNM only builds LucyN's own chrome** — about page, settings page, any internal UI. The moment a real website gets visited, LNM is nowhere near it; that still goes through the exact dom → layout → paint → window pipeline from this session, untouched.
- Flagged the real follow-on costs honestly: section 2 of the old spec (inline hex colors) is now stale and contradicts this, `:hover` needs mouse-move tracking that `ui::Window` doesn't have yet, and "real CSS" means `layout` needs an actual CSS parser, not just the `style=""` support `SESSION12_PLAN.md` had scoped.

That last point is worth sitting with for a second: you've quietly upgraded Session 12/13 from "add inline style support" to "write a real CSS parser," since LNM now needs to point at genuine `.css` files. Not a bad call — it's the honest cost of not reinventing CSS — but it's a bigger chunk of work than the smaller `style=""` plan was.
