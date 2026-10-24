# Session 18 — the headless pivot: no window, no pixels, no SDL, no LNM

## What the author asked for

LucyN is a **headless browser engine**. No window, no rendering, no pixels, no SDL, no LNM chrome. Think Lightpanda —
fetch, DOM, JS execution, interact, extract. That is the entire product. `screenshot` stays: a headless framebuffer to
PNG, no window ever opens. No CDP, no WebSockets, no new protocol — the existing JSON stdin/stdout interface is it.

**Removed completely:** `modules/ui`, `lnminterpreter/` (the folder Session 12 created at the repo root), the untracked
`apps/lucyn-browser` and its `examples/browser-demo`, `examples/lnm-demo/`, `LNM_SPEC.md`, `logger_module.md`, all SDL
dependencies, all LNM chrome code in the controller, the `--show` flag, and `--ghostful`.

**Kept (and made excellent):** net, dom, js, shell, security, search, adblock, profile, core, controller, server,
logger — plus, per the author's decision, `modules/paint` and `modules/layout`: they have no SDL, no window, no chrome;
they are the pure offscreen rasteriser (DOM → box tree / litehtml → DisplayList → SoftwarePainter → Framebuffer → PNG)
that backs the `screenshot` command. Removing them would have deleted `screenshot`; keeping them keeps the promise
"headless framebuffer to PNG only".

## What changed

### Deleted
- `modules/ui/` (SDL2 window, Ghostful Mode, the UI thread of Session 16) — tracked, gone from git's view.
- `lnminterpreter/` (parser, compiler, App runtime, `lucyn-lnm` CLI), `LNM_SPEC.md`, `logger_module.md`.
- `apps/lucyn-browser/` and `examples/browser-demo/` — **untracked in git: deleting them is permanent.**
- `examples/lnm-demo/`, and with it the last consumer of `lnm.entry`.
- The logger's `Level::lnm` / `log.lnm()` (last callers were the controller's chrome code).

### `modules/controller` (2734 → 1393 lines)
- The whole outer `#ifdef LUCYN_CONTROLLER_HAVE_UI` world is gone: `presentDocument`/`presentCurrentView`/`presentComposed`,
  the composed chrome+page frame, the litehtml chrome renderers and hit-testing, LNM input focus/hover/typing, Esc-to-home,
  `pumpWindowEvents`, the SDL reader thread (`readLineWithWindowEvents` — stdin is a plain `std::getline` again),
  `pushVisibilityToPage`, `cmdWindow` (and its built-without-SDL2 fallback).
- `frame` and `hit` (lucyn-browser's offscreen pixel view and pixel-click navigation) are gone with the app.
- The tab subsystem (Tab struct, stash/activate/newTab, history back/forward/reload, `recordHistory`, `suppressHistory_`)
  went too: it only ever existed to serve the LNM tab strip and chrome runjs functions; no JSON command exposed it.
  `loadPageFollowing` is now the single navigation funnel with no history side effects.
- Kept and untouched in behaviour: `screenshot` (still guarded by `LUCYN_CONTROLLER_HAVE_PAINT`, still uses litehtml
  when present and the native block layout otherwise, still has the blank-page render guard), `visit`/`search`/`html`/
  `extract`/`extractAll`/`eval`/`waitFor`/`waitIdle`/`click`/`profile`/`cookies`/`setCookie`/`clearCookies`/`download`/
  `moonbug`/`captures`/`info`/`quit`.
- `ControllerOptions`: `showWindow`, `ghostfulWindow`, `lnmEntry`, `windowWidth/Height/Title` removed; the header's
  command docs and its SDL paragraph updated.

### `apps/lucyn/main.cpp`
`--show` and `--ghostful` removed (now "unknown option"); `--config` reads only `search.default` and
`search.searxng_url` (the `lnm.entry` and `window.*` parsers are deleted); usage text updated.

### JS visibility (the honest-lies problem, solved by deletion)
`window.__lucynSetHidden` and `__lucynHidden` are gone from the prelude. LucyN has no window, so the one truthful state
for a page is "foreground tab of a real browser": `document.hidden === false`, `document.visibilityState === 'visible'`,
`document.hasFocus() === true`. Verified end to end through the real engine (below). Ghostful Mode's problem —
consistency between a hidden window and what the page believes — no longer exists, because there is nothing to be
hidden behind.

### Build wiring
Root `CMakeLists.txt`: `LUCYN_MODULE_UI`, `LUCYN_MODULE_LNM` options, their `add_subdirectory`s and their
controller link blocks removed, `apps/lucyn-browser` unwired. `LUCYN_LITEHTML` and the paint/layout/screenshot
wiring stay (they are the screenshot path). `third_party/litehtml` also supplies the bundled Gumbo fallback for `dom`.

### Client and docs
`client/src/index.js`: `ghostful`/`show` options, `setWindow()`, `setVisible()` removed; screenshot docs updated.
`README.md`, `HANDOFF.md`, `AGENTS.md`, `ORG_README.md`, `LUCYN_ENGINE_API_SPEC.md`, `THIRD_PARTY_NOTICES.md`,
`client/README.md` updated: the "Why LNM" pitch, the Ghostful Mode section (replaced by "Headless by design"), every
SDL row/flag/command, and the dead `window.*`/`lnm.entry` config sections are gone. Historical `SESSION*.md` and
`CHANGES.md` files were left untouched — they record what happened, not what is.

## Verification (all on this machine, real libcurl-impersonate v2.2.3)

- Clean configure + full build from scratch: **zero compiler errors, no new warnings** (`-Wall -Wextra` retained).
- `ctest --test-dir build`: **21/21 suites pass** (the removed `ui`, `lnm_parser`, `lnm_app` suites went with their
  modules; everything else, including `controller`, `controller_net`, `controller_download`, `cli_download`,
  `js`, `js_net`, `paint`, `layout`, `layout_style`, is green).
- End-to-end through the real binary against a local HTTP server, driven exactly like the JS client drives it
  (NDJSON on stdin): `info` → profile `chrome146`, `impersonating: true`; `visit` → 200, scripted `<title>` present;
  `extract("h1")` → text; `eval` → `false/visible/true` (hidden/visibilityState/hasFocus — the fix, live); `click("#btn")`
  → inline `onclick` fired (`data-clicked="yes"`); `screenshot` → 12,330-byte PNG; `window` and `frame` →
  `unknown command`; `lucyn --show` → `unknown option: --show`.
- The screenshot PNG was **looked at**: 800×150, dark `#102030` body background from the page's `<style>` block, cyan
  heading, paragraph and the "Click me" button all rendered — real CSS through the litehtml path, offscreen, no window.

## Known limits / notes

- `modules/paint`'s and `modules/layout`'s own READMEs still describe their full internal design (Session 11/12); their
  role is now screenshot-only, which the root README and HANDOFF state clearly.
- `document.hidden` is now a constant `false` accessor and `hasFocus()` constant `true`; nothing ever dispatches
  `visibilitychange`/`focus`/`blur` (a real browser rarely does either for a foreground tab either).
- The `Lightpanda/` research notes, `dashboard/`, and the vendored `stb`/`fonts` (paint's font + image decoding) all
  remain — `stb` is still needed by the screenshot path.
- The controller's `screenshot` still uses `chooseRenderDocument`'s blank-page guard and the 600px default viewport
  height; no behaviour change there.

## After pulling this

`rm -rf build && cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure`.
Unpack libcurl-impersonate into `third_party/curl-impersonate/` first if it is missing — it is now a hard requirement
(SESSION17 already removed the stock-libcurl fallback).

## Opening message for Session 19

> "Read HANDOFF.md and SESSION18.md first. Then: [paste what you want next]."
