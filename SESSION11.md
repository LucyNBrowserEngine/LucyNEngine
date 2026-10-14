# Session 11 (M10 in session labels): the painter and the UI

Goal: `SESSION11_PLAN.md` Parts B, C, D — `modules/paint`, `modules/layout`, `modules/ui`, plus the `screenshot`/`window`
controller commands and Ghostful Mode's visibility consistency. Part A (`download`) was already done in the
`SESSION10.md` addendum — not touched this session.

**Result: all three modules built, wired into the root build, and tested. 20/20 suites pass (17 unchanged +
`paint` + `layout` + `ui`). `paint` passes clean under ASan/UBSan. `screenshot` and `window` work end-to-end
against a real HTTP server, including `document.hidden`/`visibilityState`/`hasFocus()`/focus/blur all agreeing
with the real (dummy-driver) window's visible/hidden state.**

**Sandbox note:** same restrictions as every prior session — no real network beyond the GitHub/package-registry
allowlist, no real display. `cmake`, `libgumbo-dev`, `libcurl4-openssl-dev`, `libsdl2-dev` were installed fresh via
`apt-get` (allowlisted `archive.ubuntu.com`/`security.ubuntu.com`). The `ui` suite runs under `SDL_VIDEODRIVER=dummy`
(set automatically by `ctest`, via each test's `ENVIRONMENT` property). Real HTML was fetched from a local
`python3 -m http.server` loopback for the end-to-end checks below — there is no real internet in this sandbox, so
`visit("https://example.com")` itself was never exercised here; only the rendering pipeline behind it was.

## What changed

### `third_party/stb/` (new)
Vendored `stb_truetype.h`, `stb_image.h`, `stb_image_write.h` from `nothings/stb` (`master`, fetched 13 Oct 2026 —
the sandbox could not resolve an exact commit SHA via `api.github.com` this run; re-pin next time it's touched).
Public domain / MIT dual license. See `VENDORED.txt`.

### `third_party/fonts/` (new)
Vendored `PTSans-Regular.ttf` (PT Sans Regular, ParaType Ltd., SIL OFL 1.1) instead of the plan's suggested DejaVu:
`dejavu-fonts/dejavu-fonts`'s GitHub repo ships only build sources (SFD + fontforge scripts), no prebuilt `.ttf`,
in the commit this sandbox could reach. `google/fonts`' OFL mirror on GitHub had a real static (non-variable) TTF
with a clear permissive license — confirmed as a valid `0x00010000` sfnt file, and the OFL text was read (not
assumed) before committing: embedding in software is explicitly allowed, the only restriction is not selling the
font file itself under the reserved name. See `VENDORED.txt` for the full reasoning and the file-swap note for
whoever wants DejaVu specifically later.

### `modules/paint/` (new) — Part B
- `include/lucyn/paint/{displaylist,painter,framebuffer,software_painter}.h`, `src/{software_painter,font_data}.cpp`,
  `tools/lucyn-paint.cpp`, `tests/test_paint.cpp`, `embed_font.cmake`, `CMakeLists.txt`, `README.md`.
- **Decisions (README left these open, as the plan said to document):**
  - `DrawRect` carries an rgba `Color`, a `fill`/`stroke` flag (`strokeWidth` only matters when `fill==false`);
    `DrawBorder` is the four-sided convenience `layout` actually emits for element borders.
  - Every draw item carries an optional `ClipRect` (`hasClip` gates it) — `overflow:hidden` becomes a clip rect
    on the children's draw items; nesting (intersecting multiple clip rects) is `layout`'s job, not `paint`'s —
    `paint` only ever sees one rect per item.
  - Defaults: black text on white, 1px grey borders — "looks like sh*t but renders."
- **Text:** `stb_truetype`, one embedded font, ASCII only (`c >= 0x80` is skipped, not mojibake'd or crashed on).
  `measureTextWidth()`/`lineHeight()` are `static` on `SoftwarePainter` so `layout` can use them for word-wrap
  without needing a `Framebuffer` or a `Painter` instance.
- **Images:** `stb_image` (`STBI_ONLY_PNG/JPEG/BMP`) via `ImageTable::load(path)` → an id, or -1 on failure; a
  `DrawImage` with no matching id draws nothing (a blank box, never a crash).
- **Font embedding:** `embed_font.cmake` turns the vendored `.ttf` into a C++ byte array at *configure* time, same
  habit as `modules/js/embed_prelude.cmake` for the JS prelude, but binary instead of text: `file(READ ... HEX)`
  then one `string(REGEX REPLACE "(..)" "0x\\1," ...)` pass (not a per-byte CMake loop — that would have been
  minutes instead of the ~320ms this takes on a 442,960-byte file). Compiled once in `font_data.cpp`.
- **Pixel format:** `Framebuffer::pixels` is `uint32_t`, packed `(r<<24)|(g<<16)|(b<<8)|a`. This mattered later for
  `ui` (see below) — written down there, not duplicated here.
- **`lucyn-paint --demo out.png`**: hard-coded display list (header bar, bordered card, wrapped-looking text,
  a clipped red square). **Looked at the PNG with the `view` tool**: header bar renders, text is legible (PT Sans,
  correctly anti-aliased), the bordered box outline is crisp, and the clip rect genuinely clips — the red square
  (drawn 300×300) renders as a 150×150 square, matching its `ClipRect{480,100,150,150}`. First real visual proof
  the painter works, same as `SESSION11_PLAN.md` asked for.

### `modules/layout/` (new) — Part C
- `include/lucyn/layout/layout.h`, `src/layout.cpp`, `tests/test_layout.cpp`, `CMakeLists.txt`.
- **Ladder: reached "Must," stopped there on purpose** (documented in `layout.h`'s header comment, not just here):
  - Box tree built from `modules/dom`, walked recursively.
  - UA default display by tag (`uaDisplayForTag()`): `div/p/h1-h6/ul/li/body`/unknown → block; `span/a/b/i/em/
    strong/small/code/label/u/sub/sup/abbr/cite/q/mark/time/kbd/samp` → inline; `script/style/head/title/meta/
    link/noscript/template` → none. Unknown tags default to block, matching real HTML5 behaviour.
  - `display:none` honoured two ways: the UA `none` list above, and a crude (not a real CSS parser) literal
    search for `display` + `none` inside a `style=""` attribute, plus the `hidden` attribute. Good enough for
    the Must-rung item; `layout.cpp` says plainly this is not CSS parsing.
  - Block boxes stack vertically; UA default top/bottom margin (8px) on `p/h1-h6/ul/ol/blockquote`, 0 elsewhere.
  - Inline text (and inline elements' text content, flattened — nested inline markup loses its own styling this
    session) accumulates into a run that word-wraps at the container width using `paint::SoftwarePainter::
    measureTextWidth()`, one synthetic "line" `Box` per visual line.
  - Line-height from the embedded font's real metrics (`SoftwarePainter::lineHeight()`), not a guessed constant.
  - `paint(root)` walks the finished `Box` tree into a `DisplayList` — the one place a box tree becomes draw
    commands, same "layout never draws" rule the painter module itself follows.
- **Not reached this session** (ladder items 2–4, explicitly deferred, not attempted and not faked):
  - inline `style=""` parsing (display/width/height/position/visibility/z-index/overflow/margin/padding/border
    — the README "Should" list)
  - `<style>` blocks with tag/class/id selectors ("Could")
  - flexbox, grid, floats, tables (explicitly Session 12+ per the plan)
- **`getBoundingClientRect`/`offsetWidth` wiring into `modules/js`: not attempted.** The plan said "wire last and
  only if easy"; given the ladder only reached block/inline with no CSS box model beyond UA defaults, wiring real
  numbers into `modules/js` felt likely to either be misleadingly thin (numbers that ignore padding/border the
  moment `style=""` support lands next session) or to need touching `js`'s own 12/12-passing suite under time
  pressure — skipped rather than rushed. Still returns whatever `modules/js` returned before this session (check
  that file's own notes; not re-verified here since nothing in `layout` or `js` changed that contract).
- **`lucyn-paint --file page.html out.png`**: wired via a late-binding CMake trick (see "CMake note" below), runs
  `dom::parseHtml` → `layout::layoutDocument` → `layout::paint` → `SoftwarePainter`. **Looked at the PNG** of a
  5-paragraph test page with a `display:none` div, a `<div><p>` nesting, and a long sentence: the hidden div
  produced zero output, the nested `<p>`'s 8px UA margin visibly pushed it down from its parent `<div>`'s content
  origin, and — initially this looked like a wrapping bug (one very long sentence rendered as a single line at
  800px width) until actually measuring it: `SoftwarePainter::measureTextWidth()` put that exact sentence at
  796.76px, under the 800px viewport, so one line was correct. Re-ran the same page at 300px width afterward and
  got a real 3-line wrap, confirming the word-wrap logic itself was never broken — the first render just happened
  to almost exactly fill the line. Worth remembering: "doesn't look wrapped" is not the same bug as "didn't wrap."

#### CMake note: the late-binding trick, twice
`modules/paint`'s own `CMakeLists.txt` cannot see `lucyn::layout` to wire `lucyn-paint --file` itself: when
`modules/layout/CMakeLists.txt` does `add_subdirectory(../paint)`, paint's CMakeLists runs and finishes *before*
`layout`'s own `add_library(lucyn_layout ...)` line further down defines the target — chicken and egg. Same shape
of problem the root `CMakeLists.txt` already had wiring `lucyn::js` into `lucyn-shell`. Fixed the same way: `paint`
leaves `--file` support out of its own file, and whoever genuinely has both targets by the time its own CMakeLists
finishes — `modules/layout/CMakeLists.txt` (for a standalone `cmake -S modules/layout` build) and the root
`CMakeLists.txt` (for the full build) — does the `target_link_libraries`/`target_compile_definitions` wiring
after the fact. Caught this by actually testing `--file` standalone (`cmake -S modules/layout -B ...`) before
assuming the root build would be fine; it silently built an `lucyn-paint` with no `--file` support at all until
this was fixed. Lesson for Session 12: test every module standalone, not just the full root build.

### `modules/ui/` (new) — Part D
- `include/lucyn/ui/window.h`, `src/window.cpp`, `tests/test_ui.cpp`, `CMakeLists.txt`, `README.md`.
- **`ui::providingUI()` is a hardcoded `true`.** `ui::Window` always creates a real `SDL_CreateWindow(...,
  SDL_WINDOW_HIDDEN, ...)`; `setVisible()` only calls `SDL_ShowWindow`/`SDL_HideWindow`. `present()` still blits
  into the window's surface while hidden, so `setVisible(true)` later shows real content immediately, not a blank
  window — Ghostful Mode's whole point.
- **SDL2 decisions, as the plan asked, written down:**
  1. **Optional dependency.** `modules/ui/CMakeLists.txt` does `find_package(SDL2 QUIET)` (with a pkg-config
     fallback), and on failure prints a `STATUS` message and `return()`s — nothing else (`paint`, `layout`,
     `screenshot`) is affected. Root `LUCYN_MODULE_UI` option, same style as the others.
  2. **Event loop vs. stdin loop: picked (a).** `controller.h`'s "no threads" sentence stays, with one sentence
     added explaining the trade-off. `Window::pollEvents()` is pumped once per stdin command line, on the same
     thread, not on a timer. **Honest trade-off:** a window event (close button, resize) is only noticed when the
     next JSON command arrives — a human staring at the window between commands sees it lag. A `Quit` event
     (close button) hides the window (`setVisible(false)` + push the new state into the page) rather than ending
     the whole process; only EOF or an explicit `"quit"` command ends the controller. Fallback (b) — a reader
     thread queuing stdin lines so SDL can be pumped independently — is documented as the thing to reach for if
     the stdin-cadence trade-off ever stops being acceptable; not built this session.
  3. **Tests run under `SDL_VIDEODRIVER=dummy`**, set automatically by each test's `ENVIRONMENT` property so a
     bare `ctest` is enough. They check creation, hidden-by-default, resize, `present()` not crashing (hidden and
     visible), and `pollEvents()` not crashing on both a real and a never-`create()`d window. **What they cannot
     verify** (see "NOT verified" below): an actual window on a real desktop, real mouse/keyboard input, real
     click-and-drag resize.
- **Pixel format bug almost shipped.** `Framebuffer`'s packed `uint32_t` is, in little-endian memory, byte order
  A,B,G,R (not R,G,B,A) — `SDL_PIXELFORMAT_ABGR32`, not the more obvious-looking `SDL_PIXELFORMAT_RGBA32` (which
  SDL endianness-normalises to always mean R,G,B,A in memory, the *opposite* of what this buffer actually has).
  Getting this wrong would not crash anything — the window would just show blue skies as orange, silently. Worked
  through it by hand before writing the code, then verified with a real present() call under the dummy driver
  that nothing crashed (the dummy driver doesn't let you compare colours, so this is checked by reasoning, not by
  a pixel-level test — a real display is needed to be fully sure; see "NOT verified").
- **`window_` refcounting**: a small `std::atomic<int>` global tracks how many `Window`s have called `SDL_Init
  Subsystem(SDL_INIT_VIDEO)`, so repeated `create()`/destroy() in the test suite (several `Window` objects across
  several test blocks) does not call `SDL_QuitSubSystem` out from under a sibling `Window`. Caught a bug in my
  first version of this (double-decrementing on a `Window` that was *constructed* but never successfully
  `create()`d) with a dedicated `sdlOwned_` flag before shipping it.

### `modules/controller/` — `screenshot`, `window`, and Ghostful visibility consistency
- **`screenshot`** (`cmdScreenshot`, guarded by `LUCYN_CONTROLLER_HAVE_PAINT`): `layout::layoutDocument` →
  `layout::paint` → `SoftwarePainter` → PNG into `nothing-data/screenshots/`, reusing the existing `download`
  command's `sanitizeFileName`/`uniqueDownloadPath` helpers so screenshot file names get the same hostile-name
  treatment downloads already had. Works with **no SDL and no display**, as the plan required — verified by
  running it against a build where `ui` never even links in (paint/layout alone are enough). If a window exists
  (even hidden), also `present()`s the same framebuffer into it, so `screenshot` and `window` never show a
  different page.
- **`window`** (`cmdWindow`, guarded by `LUCYN_CONTROLLER_HAVE_UI`): creates the window lazily on first use
  (1024×768 default, or the given width/height), or resizes/shows/hides an existing one. **Always a recognized
  command** — a build without SDL2 still answers `"window"`, just with `ok:false` and a clear message, never
  `"unknown command: window"`, exactly as the plan asked.
- **`showWindow`/`ghostfulWindow` `ControllerOptions`, `--show`/`--ghostful` CLI flags**: the engine's own default
  is to not create any SDL window at all until something asks for one (zero SDL overhead for pure scripting use).
  `--show` creates one visible at startup; `--ghostful` creates one hidden at startup — matching the client's
  promised `ghostful: true` constructor option more literally ("a real window from the first instant", not
  "a window gets created the first time you ask for one"). This is a deliberate divergence from reading README's
  Ghostful Mode section as "every run always has a hidden window": documented here as a choice, not an oversight,
  and it's one `if` away from being flipped if the author wants the eager-every-run behaviour instead.
- **"Consistent lies while hidden"** (README, Part D): grepped `modules/js` first per the plan's instruction
  rather than assuming. Found `document.hidden` (hardcoded `false`), `document.visibilityState` (hardcoded
  `'visible'`), `hasFocus()` (hardcoded `true`) — and **no** `focus`/`blur` dispatching and **no**
  `requestAnimationFrame` at all. Made the three accessors read one flag (`__lucynHidden`, default `true` — no
  window exists until asked for, so "hidden" is the honest starting state) instead of being hardcoded, and added
  `window.__lucynSetHidden(bool)` (in `p3_window.js`, next to where `window` first becomes an `EventTarget`) as
  the single place that changes the flag and fires `visibilitychange`/`focus`/`blur` — so no two of these signals
  can ever disagree with each other. The controller calls this via `jsHost_->eval()` (the exact same code path
  the `"eval"` command already uses) from three places: every `cmdWindow` call, a `Quit` SDL event, and right
  after a fresh `visit()` creates a new JS context (so a page loaded while the window is already visible doesn't
  briefly default to "hidden" before anything asks). **`requestAnimationFrame` itself: not implemented, so there
  is no cadence to leak.** Adding a real rAF implementation felt like meaningful scope creep beyond "make the
  signals that already exist agree with each other" — flagged for whichever session adds rAF to also make its
  cadence Ghostful-aware from day one, rather than bolted on after the fact.
- **Verified end-to-end** (not just unit-tested) against a real local HTTP server: `visit` → `eval` showed
  `{hidden:true, state:'hidden', focus:false}` by default; `window visible:true` flipped it to
  `{hidden:false,'visible',true}`; `window visible:false` flipped it back. A second run with
  `addEventListener('visibilitychange'/'focus'/'blur')` recorded the exact sequence `["vis:visible","focus",
  "vis:hidden","blur"]` across a show-then-hide — the events fire, in the right order, not just the properties.

### `client/src/index.js`, `client/README.md`
- `screenshot(name, opts)`, `setWindow(opts)`, `setVisible(visible)` (convenience for `setWindow({visible})`,
  matching the `engine.setVisible(false)` examples already in `README.md`), and the `ghostful` constructor option
  (→ `--ghostful`). Documented in the commands table and the constructor example.
- **Not touched:** the pre-existing, already-unwired `cookies`/`headers` constructor options README's example
  shows (`cookies: true`, `headers: {...}`) — those were already just accepted-and-ignored before this session
  (`persistCookies` exists as a `ControllerOptions` field and a CLI concept, but the JS client's constructor
  never threaded `opts.cookies` into anything). Out of scope for this session; flagged for whoever picks it up.

### `apps/lucyn/main.cpp`
`--show`, `--ghostful` flags; usage text updated.

### `modules/controller/include/lucyn/controller/controller.h`
Command-list doc comment: `screenshot`, `window`. `ControllerOptions`: `showWindow`, `ghostfulWindow`. The "no
threads" sentence got one more sentence after it explaining the SDL-pump trade-off (see Part D above) rather than
being changed or removed — the rule itself still holds.

### `third_party/`, `modules/*/README.md`, `CMakeLists.txt` (root)
New `LUCYN_MODULE_PAINT`/`LUCYN_MODULE_LAYOUT`/`LUCYN_MODULE_UI` options (all default `ON`); `modules/paint`,
`modules/layout` add unconditionally (layout pulls in dom+paint itself if not already present), `modules/ui` only
if `LUCYN_MODULE_UI` and `LUCYN_MODULE_PAINT`. See the CMake note above for the two places that do late-binding.

## Tests

| Suite | What it covers |
|---|---|
| `paint` (new) | known-pixel rect placement; rects off every edge of the framebuffer (ASan/UBSan clean); text bounding-box sanity + empty/huge string; clip-rect behaviour; PNG round trip (write → `stb_image` read-back → compare); a golden-hash tripwire (printed, not blindly asserted — see the test file's own note on why) |
| `layout` (new) | UA tag classification (including case-insensitivity and the unknown-tag-defaults-to-block rule); two stacked `<div>`s land at the right x/y and stack in order; `display:none` (both the `style=""` and `hidden`-attribute forms) produces zero boxes; the same paragraph wraps into >1 line at 100px and into fewer lines at 2000px; nested `<div><p>` margins add up (the `<p>`'s own 8px top margin, verified against the `<div>`'s content origin); `layout::paint()` produces a non-empty `DisplayList` |
| `ui` (new) | `providingUI()` is `true`; create/destroy/resize/title against the dummy driver; `present()` doesn't crash hidden or visible; `pollEvents()` doesn't crash on a real or a never-created `Window` — **everything that can be checked with no real display** |
| `controller`, `controller_net`, `controller_download`, `cli_download` | unchanged, still passing (screenshot/window are additive; nothing existing was touched except the one new sentence in `controller.h` and the three new fields on `ControllerOptions`) |
| `core`, `profile`, `identity`, `security`, `net`, `moonbug`, `net_request`, `server`, `dom`, `dom_mutate`, `shell`, `js`, `js_net` | unchanged, still passing |

**20/20 suites pass.** ASan/UBSan run on `paint` (per the plan's explicit ask) — clean, including the
off-every-edge-of-the-framebuffer stress test. **`layout` was not separately run under ASan/UBSan** — it has no
raw pointer arithmetic of its own (everything is `std::vector`/`std::string`), and its one real memory hazard
(`Box::node` pointing into the `dom::Document` it was built from) is exercised by every test that calls
`layoutDocument` and holds the result past the point the `Document` could theoretically be freed, which none of
the current tests do — this is a real gap if `layout` ever outlives its `Document` in practice (the controller's
`screenshot` command, as written, always finishes using the `Box` tree before the next `visit()` could tear the
old `jsHost_`/`lastResult` down, but this was reasoned through, not stress-tested).

## Verified (AI, in the sandbox, 13 Oct 2026)
- `paint` and `layout` standalone builds (`cmake -S modules/paint -B ...`, same for `layout`) and the full root
  build, each from a clean directory.
- `lucyn-paint --demo out.png` and `lucyn-paint --file page.html out.png`: both PNGs **looked at directly** with
  the `view` tool (not just "the command exited 0") — see the per-module notes above for what was actually seen.
- The real `lucyn` binary, built from the full root `CMakeLists.txt`, driven over its real stdin/stdout JSON
  protocol against a local HTTP server: `visit`, `screenshot` (producing a real PNG of real rendered/wrapped
  HTML, including a correctly-excluded `display:none` div), `window` (create/resize/show/hide, all under
  `SDL_VIDEODRIVER=dummy`), and the full visibility-consistency chain (`document.hidden`/`visibilityState`/
  `hasFocus()` plus real `visibilitychange`/`focus`/`blur` event firing, in order).
- `paint` under ASan/UBSan (`-fsanitize=address,undefined`): build and full test suite clean.

## NOT verified
- **Any real window on a real desktop.** Everything SDL-related ran under `SDL_VIDEODRIVER=dummy` — window
  creation, `present()`, `pollEvents()` all succeed against the dummy driver, which proves the *code paths* work,
  not that pixels land correctly on a real X11/Wayland/Windows/macOS window. **Author's one-line check:**
  `./bin/lucyn --show` then pipe `{"cmd":"visit","url":"https://example.com"}` — a window should appear showing
  the page.
- **The SDL pixel-format reasoning (`SDL_PIXELFORMAT_ABGR32`) is worked out by hand, not confirmed against a real
  screen.** The dummy driver accepts any format without complaint; a wrong format there would not fail a test. If
  colours look swapped (blue showing as orange, etc.) on a real run, check `modules/ui/src/window.cpp`'s
  `present()` first.
- **Real network.** As in every prior session, `visit("https://example.com")` itself was never run here — only
  against a local loopback HTTP server serving a hand-written test page. The rendering pipeline (layout → paint →
  screenshot) is the same regardless of where the HTML came from, but the fetch path (`net`/`profile`) was not
  re-exercised this session; it was already covered by prior sessions' real-network checks.
- **Font rendering quality on anything but Linux/GNU's FreeType-adjacent rasterization path.** `stb_truetype` does
  its own rasterization (no FreeType dependency at all), so this should be identical cross-platform by
  construction, but was only actually run on this one Linux sandbox.
- **`getBoundingClientRect`/`offsetWidth` real-number wiring** — explicitly not attempted this session (see the
  `layout` section above).
- **`requestAnimationFrame`** — not implemented, so nothing to verify.
- **Multi-window, or a window outliving the controller process in some embedded-app scenario** — this engine
  creates at most one `ui::Window` for its whole lifetime; untested (and not designed for) anything else.

## Decisions (collected, cross-referenced to where they're explained above)
- `DisplayList` colour/fill/stroke/clip design — `modules/paint` section above, and `modules/paint/README.md`.
- PT Sans over DejaVu — `third_party/fonts/VENDORED.txt`.
- `embed_font.cmake`'s single-regex-pass approach over a per-byte CMake loop — `modules/paint` section above.
- Late-binding CMake wiring (twice: paint→layout's `--file`, and controller→paint/layout/ui) — "CMake note" above.
- SDL2 optional dependency, stdin-cadence event pumping (not a thread), `SDL_VIDEODRIVER=dummy` for tests —
  `modules/ui` section above, and `modules/ui/README.md`.
- `SDL_PIXELFORMAT_ABGR32` (not `RGBA32`) for `Framebuffer`'s packed format — `modules/ui` section above.
- Window created lazily by default, `--show`/`--ghostful`/`ghostfulWindow` for eager creation — `controller`
  section above (a deliberate divergence from "every run has a hidden window by default," written down as such).
- `__lucynHidden` single-source-of-truth flag, pushed via the existing `eval()` path rather than new plumbing —
  "Consistent lies while hidden" section above.
- Layout ladder stopped at "Must" (no CSS parsing); `getBoundingClientRect` wiring skipped entirely rather than
  wired thin — `modules/layout` section above.

## Next: Session 12

See **`SESSION12_PLAN.md`** — written at the end of this session, same as `SESSION11_PLAN.md` was written at the
end of Session 10. Short version: **no, LNM could not be coded this session** (confirmed to the author mid-session,
before any code was written) — it needs `layout`/`paint`/`ui` to exist first, which is exactly what this session
built. Session 12 is positioned to start it, but is the author's call, same as every session boundary so far.

## Opening message for Session 12

> "Read HANDOFF.md, SESSION11.md and SESSION12_PLAN.md in the zip. paint/layout/ui are built and tested (20/20
> suites). Do not redo them. Pick up where SESSION12_PLAN.md says to start."
