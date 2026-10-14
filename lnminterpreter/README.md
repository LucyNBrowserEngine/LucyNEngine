# lnminterpreter/

Reserved folder for the LNM (Large Native Module Language) parser/interpreter — the declarative UI language
described in `LNM_SPEC.md`.

**Status: empty. Not started.** Session 11 built the rendering pipeline LNM will need (`modules/paint`,
`modules/layout`, `modules/ui`), but did not touch LNM itself — confirmed explicitly mid-session that LNM could
not be coded in Session 11, since it depends on those three modules existing first. See `SESSION11.md` and
`SESSION12_PLAN.md` (Part B) for the full reasoning and the suggested order of work.

This folder sits next to `modules/`, not inside it, at the author's request. Whether it becomes `modules/lnm/`
(matching every other module's `include/`/`src/`/`tools/`/`tests/`/own `CMakeLists.txt` shape) is an open
decision for whoever starts this work — not pre-made here.

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
