# Session 11 plan (M10 in session labels): the painter and the UI

Written 13 Oct 2026 at the end of Session 10, by the AI, from the author's request. Read `HANDOFF.md`, `SESSION10.md`, `LNM_SPEC.md`, and `LUCYN_ENGINE_API_SPEC.md` first.

**Author's decision:** the next session builds the **painter and the UI**. Streams, header order and canvas wait.
The small human-requested **download command is already built** (Part A below is just the status). Do B, C, D.

Rule for this session, same as every one: say plainly what is and is not tested. The sandbox has no real network (only the
allowlist: github.com, raw.githubusercontent.com, pypi, npm, crates, ubuntu mirrors) and no real display.

---

## Part A. The `download` command: DONE (built right after Session 10, see the addendum in `SESSION10.md`)

`./bin/lucyn --download <url> [--name f] [--overwrite] [--max-bytes N] [--referer R]`, the controller `download` command and the client
`a.download(url, opts)` all exist, with streaming to disk, hidden `.part` + atomic rename, hostile-name sanitising, collision handling, 17/17
suites (`controller_download`, `cli_download` are new) and a real-HTTPS check against `curl`. **Do not redo this.** What is still open:

- **Author's check on their own machine** (the AI had only the GitHub allowlist): run it on the 10,340,250-byte file from your log and compare with curl:
  ```bash
  cd build && ./bin/lucyn --download <your url>            # prints the saved path
  curl -s -o /dev/null -w "%{size_download}\n" <your url>    # byte counts must match
  ```
  Also worth one try with libcurl-impersonate built in (the AI never had it).
- Optional: resume (`Range`), progress events, `Content-Type`-based extension guessing when a URL has none.

### Phase 2 (optional, not started): real `blob:` URLs
The author's wording was "download blob url". Today's meaning is "fetch a URL, get bytes". The literal browser meaning is `blob:` URLs from
`URL.createObjectURL(blob)`, which pages use for generated downloads (`<a href="blob:..." download="x.csv">`). To support it:
- `URL.createObjectURL` / `revokeObjectURL` in `modules/js` (a registry: `blob:` + page origin + uuid -> the Blob's bytes).
- `click()` on an `<a download>` (or `a.click()` from script) emits `{"event":"download","name":"x.csv","path":"..."}` and saves into `downloads/`.
- `download` command accepts a `blob:` URL when a page is loaded and asks the JS host for the bytes.

---

## Part B. The painter (`modules/paint`)

README already designs this: **layout never draws; it makes a display list, and exactly one backend turns that into pixels.**

### Deliverables
- `include/lucyn/paint/`: `displaylist.h` (`DrawRect`, `DrawText`, `DrawImage`, `DisplayList` as in README), `painter.h` (`Painter` interface), `framebuffer.h`, `software_painter.h`.
- **Decide and document** the small additions README leaves out. Suggested, keep it dumb: a `uint32_t rgba` colour on `DrawRect` and `DrawText`, a `fill` vs `stroke` flag (or a separate `DrawBorder`), and a clip rectangle so `overflow: hidden` works. README says colours are mostly ignored; the painter still needs *some* colour to be readable (default: black text on white, 1px grey borders so boxes are visible; that is the "looks like sh*t but renders" promise).
- `SoftwarePainter` draws into a `Framebuffer { int w, h; std::vector<uint32_t> pixels; }`. **No SDL anywhere in this module**, so it builds and tests on any machine.
- Text: `stb_truetype.h`. Images: `stb_image.h` (decode) and `stb_image_write.h` (PNG out). Vendor them as single headers into `third_party/stb/` with a `VENDORED.txt` (source URL, version/commit, licence), same habit as quickjs and mongoose. They are on GitHub (`nothings/stb`), which the sandbox can reach.
- **A font.** stb_truetype needs a `.ttf`. Vendor one small, permissively licensed font into `third_party/fonts/` (a DejaVu Sans or Liberation Sans style licence is fine; **read the actual licence text before committing and record it in `VENDORED.txt`**, do not guess). Embed it with the same CMake trick as `embed_prelude.cmake` so the binary stays self-contained; no font search on the host (host fonts would also be a fingerprint, see README: font list comes from the identity).
- `tools/lucyn-paint.cpp`: tiny CLI, e.g. `lucyn-paint --demo out.png` draws a hard-coded display list. Easiest first proof the painter works.

### Tests (`paint` suite)
- a rect of known colour at a known place: exact pixels inside, untouched pixels outside
- rects hanging off every edge of the framebuffer do not write out of bounds (build once with ASan/UBSan, like `dom` was)
- text renders non-blank pixels, only inside its box; empty string and a huge string do not crash
- clip rect clips
- PNG round trip: write, read back with `stb_image`, compare pixels
- one tiny golden image (hash of the pixel buffer) so accidental rendering changes get noticed
- **The AI can look at output PNGs with the `view` tool. Do that, and say what it saw.** It is the only real visual check available in the sandbox.

---

## Part C. Just enough layout (`modules/layout`)

The painter has nothing to paint without a display list, and `layout` is empty. Keep this **small and honest**: block and inline only.

Ladder (stop when the session runs out, and record how far you got):
1. **Must:** box tree from the `dom` module; UA default rules by tag (`div/p/h1-h6/ul/li/body` block, `span/a/b/i` inline, `script/style/head/title` none, `display:none` honoured); block boxes stack vertically; inline text wraps at the container width; margin/padding/border widths; text line-height from the font metrics; produces a `DisplayList`.
2. **Should:** inline `style=""` for the README's "parsed" list (`display`, `width`, `height`, `position: relative|absolute`, `visibility`, `z-index`, `overflow`, margin, padding, border width).
3. **Could:** `<style>` blocks with simple selectors (tag, `.class`, `#id`).
4. **Not this session:** flexbox (Session 12), grid, floats, tables.

Also: once boxes exist, `getBoundingClientRect` / `offsetWidth` etc. in `modules/js` can start returning real numbers (README: "real values where layout exists, plausible and consistent elsewhere"). Wire that **last and only if it is easy**; check first what `modules/js` returns today and do not break the 15 passing suites.

Tests (`layout` suite): known tiny HTML in, exact box x/y/w/h out; wrapping at a known width; `display:none` produces no boxes; nested margins/padding add up.

---

## Part D. The UI (`modules/ui`)

README + `HANDOFF.md` section 3 item 9 set the design (author's): the **`ui` module always reports "I am providing a UI"; the `controller` decides whether it is shown.** That is where "always headless" comes from, and where Ghostful Mode lives.

### Deliverables
- `ui::Window`: wraps SDL2. `create(w, h)`, `setVisible(bool)`, `present(const paint::Framebuffer&)`, `pollEvents()` (quit, resize, mouse wheel to scroll, click position), `title`.
- `ui::providingUI()` (or similar) is **always true**, regardless of visibility. A hidden window is still a real window: real display, real size, real surface. That is the whole stealth idea.
- **Ghostful Mode:** `setVisible(false)` creates/keeps the window with `SDL_WINDOW_HIDDEN`; it still renders to its surface. Default for the CLI: **hidden** (existing scripts and the 15 suites must behave as before). `lucyn --show` shows the window. (Author can flip the default; it is one line.)
- **Consistent lies while hidden** (README "Honest note"): `document.visibilityState`, `document.hidden`, focus/blur, `requestAnimationFrame` cadence must not give the curtains away. First `grep` what `modules/js` already does for these (do not assume), then make them agree with the window state.
- **New controller commands** (add to `controller.h`'s list, the client, and the client README):
  - `{"cmd":"screenshot","name":"page.png"}` -> renders the current page through layout + `SoftwarePainter` into `nothing-data/screenshots/` and replies with the path. **This must work with no SDL and no display** (offscreen framebuffer), so the AI can test it and look at the PNG.
  - `{"cmd":"window","visible":true,"width":1280,"height":720}` -> show/hide/resize (needs SDL; replies `ok:false` with a clear message if built without it).
  - client: `await a.screenshot(name)`, and the constructor option `ghostful: true` that README already promises.
- `lucyn-shell` or a new `lucyn-paint --url/--file page.html out.png` for a no-controller screenshot.

### SDL2 decisions to make and write down in `SESSION11.md`
1. **Optional dependency.** Recommended: `ui` builds only if SDL2 is found; otherwise CMake prints a clear message, skips `ui`, and **everything else (including `paint`, `layout`, `screenshot`) still builds and tests**. Keeps the "tiny, small deps" promise and keeps the sandbox usable. Root option `LUCYN_MODULE_UI`, plus `LUCYN_MODULE_PAINT` and `LUCYN_MODULE_LAYOUT`, same style as the existing ones. System package: `libsdl2-dev` (README already lists SDL2 as a system dependency).
2. **The event loop vs the stdin loop.** `controller.h` says: *synchronous, one command at a time, does NOT spawn threads*, and it blocks reading stdin. SDL wants its events pumped regularly, **on the main thread** (strict on macOS and Windows, lenient on Linux). Options: (a) while the window is visible, `poll()` stdin with a ~16 ms timeout and pump SDL between polls (simple on Linux/macOS, awkward for Windows pipes); (b) a reader thread that queues stdin lines for the main thread (works everywhere, but breaks the "no threads" sentence, so change that sentence on purpose). The author runs Linux (their prompt is `peaseernest:~/dev/lucyn/build$`) and the repo has a `default.windows.identity.json`, so Windows matters eventually. Pick one, justify it, and note the other as the fallback. Hidden/offscreen mode needs no pumping at all.
3. **Tests without a display:** `SDL_VIDEODRIVER=dummy` for the `ui` suite (window creates, presents, resizes, hide/show flags flip, `providingUI()` stays true). Real on-screen behaviour **cannot** be verified by the AI. List it under "NOT verified" and give the author a one-line check: `./bin/lucyn --show` then `{"cmd":"visit","url":"https://example.com"}`, and a window should show the page.

---

## Order of work and what "done" means

1. ~~**A**: `download`~~ done.
2. **B**: `paint` + stb vendoring + font + `lucyn-paint --demo`. View the PNG.
3. **C**: `layout` ladder step 1 (then 2, 3 if time). `lucyn-paint --file page.html out.png`. View the PNG.
4. **D**: `ui` + controller `screenshot` and `window` + Ghostful flag + JS visibility consistency.
5. **Docs**, before finishing: write `SESSION11.md` (what changed, decisions, honest limits), and update `HANDOFF.md` (status table: `layout`/`paint`/`ui` rows, one-line state, next steps) and `README.md` (roadmap rows, Downloads section: remove "planned" where built, project-layout tags).

**Done =** the 17 existing suites still pass + new suites for `paint`, `layout`, `ui` (dummy driver) pass, with ASan/UBSan run on `paint` and `layout`; `screenshot` demonstrably works; the AI has *looked at* at least one rendered PNG of a real HTML page; and the docs say plainly which parts only the author can verify (real window, real network download, anything curl-impersonate).

## Explicitly out of scope for Session 11
Streams, header order / `CURLOPT_HTTPBASEHEADER`, canvas, `WebSocket` `binaryType:'blob'`, flexbox, Skia, ad blocker, Dear ImGui chrome (URL bar and buttons). All still on the list, none of them next. If the painter work tempts a detour into canvas, resist: canvas stays a stub until after the painter is real.

## Notes for the AI (carried over)
- Sandbox: Ubuntu 24, `g++`. `apt-get update` first, then `cmake libcurl4-openssl-dev libgumbo-dev libsdl2-dev`. Allowlisted: archive/security.ubuntu.com, github.com, raw.githubusercontent.com, pypi, npm, crates. Everything else is blocked.
- `mkdir -p a/{b,c}` brace expansion misbehaves in this tool; use explicit `mkdir` calls.
- Author's style: chatty and funny replies, casual typos (read generously), "explain like I'm a kid" analogies when confused, plain honesty about what is and is not tested.
