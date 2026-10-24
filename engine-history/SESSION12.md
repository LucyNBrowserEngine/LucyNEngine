# Session 12 (M11 in session labels): inline CSS in `layout`, then LNM starts for real

Goal: `SESSION12_PLAN.md` Part A (`style=""` in `modules/layout`) and Part B (`lnminterpreter/`: parser, AST -> DOM,
view switching). The plan recommended **Option A (CSS first, then LNM)**; the author said "get to work" without picking, so
that is what was done.

**Result: 23/23 suites pass (the 20 from Session 11 + `layout_style`, `lnm_parser`, `lnm_app`). `layout`, `style`, and all
LNM code pass clean under ASan/UBSan, build with zero warnings of our own (`-Wall -Wextra`), and build + test standalone
(`cmake -S lnminterpreter -B b`, `cmake -S modules/layout -B b`). Every new test file was mutation-checked: the
implementation was deliberately broken ~10 ways (cascade order, clip propagation, containing-block origin, relative-shift
slot, tooltip anchor, style restore, comment rule...) and the tests failed each time.**

## READ FIRST: the zipped tree did not build (not caused by this session)
`modules/net/include/lucyn/net/http_client.h` includes `<request.h>` (the adblock module's header, added mid-Session-12 era:
root `CMakeLists.txt`, `net/CMakeLists.txt`, `adblock/` are all stamped 14 Oct 15:34-17:32), but `modules/net/CMakeLists.txt`
line 63 links `lucyn::adblock` as **`PRIVATE`**. A PRIVATE link does not propagate include directories, and `http_client.h` is a
public header, so every consumer of `net` (net tests, `lucyn-fetch`, shell, js, controller, ...) fails with
`fatal error: request.h: No such file or directory`.
**Fix (one word, NOT applied, because adblock/net wiring was out of scope and is the author's in-progress work):**
change `PRIVATE lucyn::adblock` to `PUBLIC lucyn::adblock` in `modules/net/CMakeLists.txt`. (Also: `http_client.h` includes it as
`<request.h>`, a very generic name; `<lucyn/adblock/request.h>` would be safer long-term.)
All numbers in this file were produced with `cmake -S . -B b -DCMAKE_CXX_FLAGS="-I<repo>/modules/adblock/include"` instead.

## What changed

### `modules/layout/` — Part A (+ colours)
New: `include/lucyn/layout/style.h`, `src/style.cpp`, `tests/test_style.cpp`. Rewritten: `src/layout.cpp`. Extended: `layout.h`
(`Box`, `LayoutOptions`), `tests/test_layout.cpp`, `CMakeLists.txt` (+ `layout_style` test).
`hasInlineDisplayNone()` is gone, as the plan said; `display` now comes out of the general parser.

- **Parser** (`parseDeclarations`/`styleFromDeclarations`): a vector of `Declaration` in SOURCE ORDER rather than the plan's
  `unordered_map` — order is what makes `margin:10px; margin-top:0` and the reverse behave differently, as in real CSS. Handles
  comments, `;` inside quotes/parens, case-insensitivity, `!important`. An invalid value is ignored (earlier value / UA default stays).
- **Properties:** `display` (block/inline/none; `flex`/`grid`/`table` render as block, `inline-block` flattens to inline),
  `width`/`height` (CONTENT-box, so `Box::width` = width + padding + 2*border), `margin`/`padding` (+ side longhands, 1-4 value
  shorthands, negative margins ok), `border`/`border-width` (ONE width for all sides), `position`, `top/right/bottom/left`,
  `visibility`, `z-index`, `overflow(-x/-y)`, and (added for LNM) `background-color`/`background`, `color`, `border-color`.
- **`position: relative`:** shifted by top/left (or -bottom/-right; top beats bottom, left beats right) after layout; its flow slot is unchanged.
- **`position: absolute`:** out of flow; resolved after its containing block's height is known (padding box of the nearest positioned
  ancestor, else the viewport — new `LayoutOptions::viewportHeight`, default 600). Handles left/right/top/bottom/width/height
  over-constraint, margins, static position (incl. below text collected before it), **shrink-to-fit width** (min(natural, available)),
  nested absolute boxes, absolute inside a shifted relative box. It becomes a child of its CONTAINING BLOCK's `Box`, which is what makes
  clipping and stacking come out right.
- **`visibility`:** keeps the space, draws nothing for the box; inherited, a descendant can say `visible` again.
- **`z-index`:** only on positioned boxes (a z-index on a static box does nothing; tested as correct, not a bug). Real stacking contexts:
  own decoration, negative contexts, in-flow content, z:auto/0 positioned in tree order, positive contexts; a z-indexed positioned box
  isolates its children, a `z-index:auto` one does not (both tested).
- **`overflow: hidden`** (also scroll/auto/clip) clips descendants to the padding box; nested clips intersect; an absolute box is clipped only
  if its containing block is (or is inside) the clipper.
- **Colours:** `parseColor` (#rgb/#rgba/#rrggbb/#rrggbbaa, rgb()/rgba() with commas or spaces, percentages, `transparent`, ~30 names — NOT
  the full 148). `background-color` -> a `DrawRect` under the border; `color` inherited by descendant blocks; border colour. Inline
  elements are still flattened, so a coloured `<span>` is not coloured.
- **`<body>`/`<html>` style** is honoured (margin/padding/border/width/height/overflow/visibility/colours); `position` on them is ignored.
- **A real bug found by the LNM demo and fixed:** shrink-to-fit ignored the right padding/border/margin of nested blocks, so a
  tooltip containing a padded box wrapped needlessly. Regression test in `test_layout.cpp`. (Parts of the test suite had passed
  on the buggy code: only the visual render showed it. Worth remembering.)
- **Not done (plan said so):** `%`/`em`/`rem`/`calc()`/`var()`, `margin:auto`, `fixed`/`sticky`, `<style>` blocks, selectors, flexbox, margin collapsing.

### `lnminterpreter/` — Part B (new; see its `README.md` for the full table, decisions and open questions)
- `include/lucyn/lnm/{ast,parser,compile,app}.h`, `src/{parser,compile,app}.cpp`, `tools/lucyn-lnm.cpp`, `tests/test_lnm_{parser,app}.cpp`, `CMakeLists.txt`.
- **Parser:** the spec's section 11 example parses verbatim; every spec snippet is a test; ~25 error cases each report line:col and the
  parser carries on; 20 000 nested `#a{` and a pile of truncated input terminate without crashing.
- **Compile:** LNM `color`->`background-color`, `textcolor`->`color`, `radius`, `opacity`, `scale`->`transform`, `shadow`, `transition`,
  `padding`/`margin`/`border` -> inline styles on the matching elements, by specificity then source order, the element's own `style=""` last (wins).
- **`App`:** load (multi-grab), `trigger`/`clearEventStyles` (hover etc.), `restyle` (= `LNM.style`), `switchReplace`, `switchOverlay`
  (z-index layer), `switchTooltip` (anchored to the element's real layout box: top/bottom/left/right/cursor), `dismiss*`,
  `registerFunction`/`setRunJsHandler`/`validateRunJs` (spec section 7 dev/prod messages).
- **Wired into the root build** behind `LUCYN_MODULE_LNM` (default ON); the folder stays `lnminterpreter/` (shaped like a module so
  `modules/lnm/` is a rename).
- **`lucyn-lnm render app.lnm out.png --do "#settings-btn" onclick`** renders an LNM app (after firing events) to a PNG with no SDL.

## Decisions (details in `lnminterpreter/README.md`)
Comment-vs-`#id`-vs-`#hex` rule; event suffix taken from the end of the selector text; extra grabs append to the first one's body; rules
are global (a switched-in view is styled by them too); overlay/tooltip as absolute layers with z-index 100+/1000; non-spec properties
pass through with a warning; whole-page rebuild on switch; `dismissOverlay`/`dismissTooltip` added (not in the spec).

## Engine config
`LUCYN_ENGINE_API_SPEC.md` (v0.2): **no additions needed.** `engine.config.lnm.entry` already names the entry file. The viewport size LNM
needs is a runtime parameter (`ViewOptions`), like the font and viewport width in Session 11, not persistent config. If the author wants a
dev hot-reload flag, that is the first thing that would go there.

## NOT done / NOT verified (do not claim otherwise)
- **No input path.** Nothing delivers hover/click to `App::trigger()` yet: `ui::Window` has no mouse events or hit-testing; the controller's
  `click` goes through the JS DOM, not LNM. Tests and `lucyn-lnm --do` call `trigger()` directly.
- **`runjs` does not run JS.** The seam is built and tested (`setRunJsHandler`, `registerFunction`, `jsFiles()`, dev/prod validation); the
  `LNM` JS global, `LNM.register()`, `LNM.switch.*`, `LNM.style()` and loading `grab.js` into QuickJS are not wired. Next step: `js` module.
- **`radius`, `opacity`, `scale`, `shadow`, `transition` are compiled but have no visible effect** (no rounded corners/alpha layers/transforms/animation anywhere).
- **Prod mode** (compile `.lnm` to a header, embed minified assets) and **dev hot reload** are not built.
- `screenshot`/the controller still use the default viewport height (600) for absolute `bottom:` anchoring; `lucyn-paint --file` likewise.
  Passing a `height` through the controller's `screenshot` command is a few lines if wanted.
- The Session 11 caveats still stand (no real internet in the sandbox; `ui` only tested under the dummy SDL driver; curl-impersonate not re-verified).
- Verified only against the sandbox's `libgumbo` 'dom' parser: how `<style>`/`<head>` content in a grabbed file behaves is "ignored by layout", not designed.

## Next: Session 13 (the author's call, as always)
Candidates, in the order the dependencies suggest: (1) fix the `net`/adblock PRIVATE-link; (2) wire LNM into the engine (`engine.config.lnm.entry` ->
`App`) and into the `window` path so something real shows an LNM app; (3) mouse input in `ui` (move/click/hit-test -> `trigger`/`clearEventStyles`);
(4) `js` <-> LNM (`runjs`, `LNM.register`, `LNM.*`, `grab.js`); (5) decide the CSS direction (see the README's open questions: real `.css`
files + `<style>` + selectors would need a real CSS parser in `layout`); (6) painter effects (radius/opacity/transition); (7) flexbox.

## Opening message for Session 13
> "Read HANDOFF.md, SESSION12.md, lnminterpreter/README.md, LNM_SPEC.md, and LUCYN_ENGINE_API_SPEC.md first. Then: [paste what you want next]."
