# Session 12 plan (M11 in session labels): CSS, then LNM can actually start

Written 13 Oct 2026 at the end of Session 11, by the AI, from the author's questions mid-session (confirmed live:
no, LNM could not be coded in Session 11 — it needs `layout`/`paint`/`ui`, which Session 11 built). Read
`HANDOFF.md`, `SESSION11.md`, `LNM_SPEC.md`, and `LUCYN_ENGINE_API_SPEC.md` first.

**This is a plan, not an order.** Same as every session boundary so far: the author picks what's next. What
follows is what's actually unblocked and in a sensible order, not a commitment.

---

## Why LNM first, then CSS, or CSS first, then LNM — both are defensible; here's the honest trade-off

`layout`/`paint`/`ui` are built (Session 11), so the literal blocker on starting LNM is gone. But `LNM_SPEC.md`
describes a *declarative* language — views, styles, switches between views — and right now `modules/layout` has
**no style system at all**: no `style=""`, no `<style>` blocks, no way to set a background colour or a width from
outside a box's UA default. An LNM interpreter that compiles LNM views into DOM + inline styles would immediately
need inline-style support in `layout` anyway (LNM styling properties overlap heavily with the README "Should" list
`layout.h` already names: `display`, `width`, `height`, `position`, `visibility`, `z-index`, `overflow`, margin,
padding, border). So:

- **Option A — CSS (`style=""`) first, then LNM.** LNM's style-setting becomes "emit inline styles, let `layout`
  do the rest" — no new layout code path. Safer, less duplicated work, but LNM itself doesn't start until partway
  through the session.
- **Option B — LNM parsing/interpreter skeleton first, CSS in parallel or after.** Gets visible LNM progress
  sooner (a parser, an AST, maybe `.lnm` → DOM compilation with NO styling yet — just structure/views/switches),
  at the cost of the interpreter's styling path being a stub until `layout` grows `style=""` support, which then
  has to be retrofitted in.

**Recommendation (not a decision): Option A.** `layout.h`'s own ladder already names exactly the CSS properties
LNM would need; building that once in `layout` and having LNM simply target it is less total work than building
LNM's own parallel styling path and reconciling the two later. But say so, don't assume — ask the author which
order they want if this plan reaches them before a styling decision is made.

---

## Part A. CSS: `style=""` support in `modules/layout` (the ladder's "Should" rung)

`modules/layout/include/lucyn/layout/layout.h`'s header comment already names the target list, copied here for
convenience: `display`, `width`, `height`, `position: relative|absolute`, `visibility`, `z-index`, `overflow`,
margin, padding, border width.

### Deliverables
- A tiny inline-style parser: `style="key: value; key2: value2"` → a `std::unordered_map<std::string, std::string>`
  (or similar) per element. This does NOT need to be a real CSS tokenizer — `display:none` detection in
  `hasInlineDisplayNone()` (current `layout.cpp`) is already a hand-rolled version of exactly this; generalize
  that function into a real (if minimal) inline-style-string parser, then delete the special-cased
  `hasInlineDisplayNone` in favour of reading `display` out of the general map.
- Wire each parsed property into `layoutElementAsBlock`/`layoutBlockChildren`: explicit `width`/`height` override
  the current "fill available width" / "auto-grow height" behaviour; `margin`/`padding`/`border-width` (numeric,
  `px` suffix only — no `%`, `em`, etc. this session, say so) override the UA defaults; `position: relative`
  shifts a box from its normal-flow position by `top`/`left` without removing it from flow; `position: absolute`
  removes it from flow entirely (positioned against its nearest positioned ancestor, or the viewport if none —
  real CSS positioning semantics, simplified: no `fixed`, no `sticky`); `visibility: hidden` keeps the box's
  space but produces no `DisplayList` output for it (unlike `display: none`, which removes the space too);
  `z-index` affects paint order only when `position` is not `static` (real CSS rule — don't let a `z-index` on a
  static-position box do anything, that is correct, not a bug); `overflow: hidden` becomes a `ClipRect` on the
  box's children's draw items (the `paint` module's clip support already exists for exactly this, built Session
  11 — `collectPaint()` in `layout.cpp` needs the one new line that sets `hasClip`/`clip` when a box's own
  `overflow` is hidden, propagated down to descendants).
- Tests (`layout` suite, extended): each property above gets at least one known-input/known-output case. Reuse
  the existing `layout` test file's style (`CHECK` macro, `findByTag`/`countLines` helpers already there).

### Explicitly not this session (ladder's "Could" rung and beyond)
`<style>` blocks, any selector beyond tag/`.class`/`#id` if `<style>` is even attempted, `%`/`em`/`rem` units,
`calc()`, CSS variables, animations/transitions (README already says these will never render, by design).

---

## Part B. `lnminterpreter/` — starting LNM for real

`LNM_SPEC.md` is the source of truth; read it fully before writing any code, not just the summary below.

### Suggested order (ladder, same "stop when the session runs out" rule as `layout`'s did)
1. **Must:** a real parser for `.lnm` syntax into an AST (views, elements, the switch-between-views construct —
   whatever `LNM_SPEC.md` actually defines as the minimum viable surface; this plan does not re-derive the
   grammar, the spec already has it). Keep the parser in `lnminterpreter/`, separate from `modules/layout` and
   `modules/paint` — LNM *compiles to* DOM + styles that those modules then lay out and paint; it does not
   replace either module or duplicate their logic.
2. **Must:** AST → DOM compilation. An `.lnm` view becomes a real `dom::Document` (or a subtree grafted into one),
   so everything already built — `layout::layoutDocument`, `paint::SoftwarePainter`, `screenshot`, `window` —
   works on LNM-produced pages with zero new rendering code. This is the entire payoff of building `layout`/
   `paint`/`ui` before LNM: LNM's job stops at "produce a DOM," not "also know how to draw."
3. **Should:** the "switch between views" construct — whatever state-driven view-swapping `LNM_SPEC.md`
   describes — wired so that switching re-compiles/re-layouts/re-paints the affected subtree, not the whole page,
   if `LNM_SPEC.md`'s performance expectations call for that; re-rendering the whole page is an acceptable first
   version if time runs out, documented as such.
4. **Could:** whatever `LNM_SPEC.md` lists as advanced/optional in its own document — do not invent scope beyond
   what that file actually asks for.

### A structural note someone will ask about
`lnminterpreter/` sits next to `modules/`, not inside it — the author asked for it as "a new folder, you just make
it lnminterpreter," separate from the usual `modules/<name>/` pattern. Whether it eventually becomes
`modules/lnm/` (matching every other module's `include/`/`src/`/`tools/`/`tests/`/own `CMakeLists.txt` shape) is
the author's call; this plan does not assume that answer and the folder currently has no build wiring of its own
(nothing added it to the root `CMakeLists.txt`). First real decision of Session 12, not pre-made here.

### Engine config
`LUCYN_ENGINE_API_SPEC.md` is still v0.1-alpha. Session 11 did not extend it (no new engine-config surface was
needed for `paint`/`layout`/`ui` beyond what already existed — the font, viewport width, etc. are all
compile-time/call-time parameters, not persistent engine config). If LNM or its view-switching needs new
`engine.config` entries (a default LNM entry-point file, a dev-reload flag, whatever `LNM_SPEC.md` implies),
add them there and say what was added and why, same rule as every session.

---

## Explicitly out of scope for Session 12
Flexbox/grid/floats/tables (unless `LNM_SPEC.md` specifically requires one of them to express a real LNM layout —
check before assuming any of these are needed), streams, canvas, header order, process isolation, the ad blocker.
If CSS work tempts a detour into flexbox, the same "resist" advice Session 11's plan gave about canvas applies
here: flexbox is its own session.

## Notes for the AI (carried over, still true)
- Sandbox: Ubuntu 24, `g++`. `apt-get update` first, then `cmake libcurl4-openssl-dev libgumbo-dev libsdl2-dev`.
  Allowlisted: archive/security.ubuntu.com, github.com, raw.githubusercontent.com, codeload.github.com, pypi, npm,
  crates. Everything else is blocked.
- Test every module standalone (`cmake -S modules/<name> -B b`), not just the full root build — Session 11 found
  a real bug (`lucyn-paint --file` silently missing) that only standalone testing caught; see `SESSION11.md`'s
  "CMake note."
- `mkdir -p a/{b,c}` brace expansion misbehaves in this tool; use explicit `mkdir` calls.
- Prefer one `string(REGEX REPLACE ...)` CMake pass over a per-byte loop for any binary-to-C-array embedding.
- Author's style: chatty and funny replies, casual typos (read generously), "explain like I'm a kid" analogies
  when confused, plain honesty about what is and is not tested.
