# LucyN: handoff for the next session

**Latest: `SESSION19_CEF_MIGRATION.md` (the CEF migration — Chromium replaced QuickJS/libcurl; steps 1-10 are done,
including step 10, the deletion of the dead modules).** Then `SESSION18.md` (the headless pivot: `modules/ui`, `lnminterpreter/`, `apps/lucyn-browser`, every SDL dependency, the `window`/`frame`/`hit` commands and `--show`/`--ghostful` are GONE; pages report themselves visible; `screenshot` is the only rendering output).** Then `SESSION17.md` (pages can navigate so JS challenges finish, `<noscript>` not rendered, curl-impersonate only from `third_party/`). Then `SESSION12.md` (M11: inline `style=""` + colours in `layout`; **read its first section: the zipped tree needed a one-word `net`/adblock CMake fix to build**). Then `SESSION11.md` (M10: `paint` + `layout` — the painter and block/inline layout; the SDL window and Ghostful Mode it built were removed in Session 18), `SESSION10.md` (M9: `Blob`/`File`, real `FormData` file parts, `Response`/`Request.blob()`, XHR `responseType: 'blob'`, plus the `download` command addendum), `SESSION9.md` (M8: inline `on*` attribute compilation + full WebSocket), `SESSION8.md` (M7: cookies, fetch/XHR rebuilt, identity + browser-context headers, the POST hang fix), `SESSION7.md` (M6: eval/waitFor/waitIdle/click), `SESSION6.md` (M5: Moonbug + NULLBYTE + TinyElysia, incl. licensing note), `SESSION5.md` (M4 controller/client), `SESSION4.md` (JS design rules; `modules/js/README.md` went with the JS module in Session 19), then `README.md` (spec/vision). Sessions 13-17 built the LNM interpreter, the SDL window UI and the `lucyn-browser` app; all of that was deliberately deleted in Session 18 - those SESSION files are history, not current state.

**One-line state (Session 19 — the engine is CEF):** identity/profile switching, and **real Chromium** doing the browser work. Controller commands: `visit`, `html`, `eval`, `extract`, `extractAll`, `click`, `fill`, `submit`, `waitFor`, `waitIdle`, `cookies`/`setCookie`/`clearCookies`, `download`, `screenshot`, `search`, `profile`, `info`, `moonbug`/`captures`, `quit` — navigation, page JS (V8), forms, cookies, TLS and rendering are Chromium's, not ours. Moonbug still records every capture (fed from CEF's resource handler, headers and all) and the capture server/dashboard still serve them. Still headless: no window, no SDL, no chrome, no LNM; `screenshot` writes a PNG from Chromium's own offscreen renderer. **10/10 ctest suites pass** (`core, search, profile, identity, security, moonbug, server, cef_host, controller, cli_download`) and the build is clean with zero warnings. The old engine was **deleted in Session 19 (step 10)**: `modules/{dom,shell,js,layout,paint}`, `third_party/{quickjs,stb,fonts,litehtml,curl-impersonate}` and every QuickJS/libcurl code path are gone; `modules/net` survives only as the Moonbug wiretap (HttpClient is gone — Chromium is the HTTP stack). See `SESSION19_CEF_MIGRATION.md`.

---

## 1. What LucyN is (30 seconds)

A from-scratch browser engine in C++17, **headless-first** ("a useful v0.4 exists before a single pixel is drawn"),
built for the author's own project, *Nothing Browser*. The author is **not** copying Nothing Browser's code (it is a
Qt/Chromium app that patches a real browser with injected JS); LucyN owns its HTTP stack, DOM and JS engine, so it can do
things natively instead of patching them. `README.md` has the full vision, roadmap (M1..M9) and API ideas.

## 2. Build and test

```bash
sudo apt install build-essential cmake      # no SDL2, no libcurl, no libgumbo: Chromium is the whole stack
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
# 10 suites: core, search, profile, identity, security, moonbug, server, cef_host, controller, cli_download
```

Each surviving module also builds alone: `cmake -S modules/profile -B b`, `cmake -S modules/core -B b`,
`cmake -S modules/net -B b` (Moonbug only, self-contained).
Root options: `LUCYN_MODULE_CORE|LOGGER|PROFILE|NET|SECURITY|SERVER|SEARCH|CEF_HOST|CONTROLLER`, `LUCYN_WITH_CEF`,
`LUCYN_BUILD_TESTS`, `LUCYN_BUILD_TOOLS`.

**CEF** (Chromium Embedded Framework) is a prebuilt dependency and a **hard build requirement** — the binary
distribution must sit at `third_party/cef/` (headers, `Release/libcef.so`, `libcef_dll/` wrapper, `Resources/` with
`icudtl.dat` + `*.pak` + `locales/`). `cmake/FindCEF.cmake` locates it, links `libcef_dll_wrapper`, sets
`-Wl,-rpath,$ORIGIN` and provides `lucyn_cef_copy_runtime()`, which copies `Release/` and `Resources/` next to the
built binaries. CEF aborts with `Invalid file descriptor to ICU data` if `icudtl.dat` is not findable from the loaded
`libcef.so`, so never move a binary away from those files. There is no libcurl dependency left anywhere.

CLI tools to poke things:
```
build/bin/lucyn-profile  list | info | resolve | switch | identity ...
build/bin/lucyn-search   list | <engine> <query> [--searxng <instance-url>]
build/bin/lucyn          [--download <url> ...] [--profile ...] ...   # the real engine binary (JSON over stdio)
```

## 3. Architecture decisions (keep these)

1. **Modules.** `modules/<name>/` = `include/lucyn/<name>/`, `src/`, `tools/` (CLI), `tests/`, own `CMakeLists.txt`
   that works standalone *and* via `add_subdirectory`. Target alias `lucyn::<name>`. Root CMake only wires modules.
2. **One shared identity.** A single `profile::Switcher` holds the active persona. Modules *follow* it: the controller
   calls `select()` on `visit`/`profile`, and Moonbug records what the wire actually carried. Nobody talks to anybody
   else about identity. The curl-era half (a validator plus a listener that rebuilt the curl handle per persona) left
   with `HttpClient` — there is no handle of ours to rebuild any more, Chromium owns the connection.
3. **Persona vs device.**
   - *Persona* = which browser (a `Profile` such as `safari184`). Everything the browser reports (`Surface`: UA, platform, vendor,
     `sec-ch-ua`, `userAgentData`, WebGL strings, which JS APIs exist) is **derived**, never stored, so TLS and JS cannot disagree.
   - *Device* = the machine (cores, memory, screen, GPU, timezone, languages, noise seeds). Generated once, **stored per
     (profile name, OS class)**, so "the same person comes back tomorrow"; Chrome-on-Mac and Safari-on-Mac share one Mac.
4. **Persona OS comes from the profile registry's table** (the curl-impersonate target table, kept as plain data in
   `modules/profile`; libcurl itself is gone), not from the host. `chrome146` claims macOS Tahoe, so the default
   `"chrome"` persona is a Mac even on a Linux host. The device follows the persona.
5. **Switching is recorded, not yet injected.** `visit ... "profile"` and the `profile` command call
   `profile::Switcher::select()` and that is all: the selected persona's `Surface` is **not yet applied to Chromium's
   request headers** (see the §5 gap). Do not claim that our UA/client hints go on the wire yet.
6. A failed `select()` leaves the old profile active. A corrupt identity file is **reported, never silently replaced**.
7. Lenient by default: unknown target names pass through (the library has the final say). `setStrict(true)` limits to the table.
8. `engine.switch.mode(...)` from the README **cannot compile** (`switch` is a C++ keyword). Rename (`engine.swap`, `engine.profile.set`).
9. **Headless, always (Session 18, unchanged in Session 19).** No window, no SDL, no chrome, no LNM: CEF runs with
   `--headless=new`, GPU off and `--disable-dev-shm-usage` (small `/dev/shm` in containers). Do not reintroduce a window
   or a `--show`-style flag; the product is visit, interact, extract, screenshot (see `SESSION19_CEF_MIGRATION.md`).
10. **Rendering is Chromium's (Session 19).** There is no layout engine, display list, painter or font stack left in the
    tree: `screenshot` reads the BGRA frame CEF paints into `OnPaint` (after a 1.2s settle) and encodes it with the tiny
    dependency-free PNG writer inside `cef_host`. Nothing ever presents to a screen.

## 4. Status

| Module | State | Notes |
|---|---|---|
| `profile` | **done** | Registry (37 browsers), Switcher, JSON, identity generator, Surface, consistency checker, IdentityStore, IdentityManager. Depends only on `core`. |
| `net` | **gutted (Session 19)** | HttpClient, libcurl-impersonate, `lucyn-fetch` and the `test_net`/`test_net_request` suites are **deleted** — Chromium is the HTTP stack. What remains is only the Moonbug wiretap (`Capture`, listener registry, history ring, JSON serialiser), self-contained, no deps. |
| `core` | **done** | `nothing-data` paths. Everything LucyN stores goes there (rule from the author). |
| `dom`, `shell`, `js` | **REMOVED (Session 19, step 10)** | The hand-rolled DOM, the hydrator/shell/script injection layer and the QuickJS host are deleted with `third_party/quickjs`. CEF supplies a real DOM, real V8, real script injection and real navigation. |
| `security` | **done (M5)** | NULLBYTE: HMAC-SHA256 seals, persisted key. |
| `server` | **done (M5)** | TinyElysia (Mongoose) + Moonbug capture server + dashboard. Mongoose is GPL-2.0/commercial. |
| `cef_host` | **new (Session 19)** | Owns CEF: `initCef`/`shutdownCef`, subprocess dispatch (`executeCefProcess`), instance creation, `visit`, JS eval/extract via `CefV8Context`, click/fill/submit via DOM, cookie get/set/clear, offscreen `OnPaint` frames, resource handler feeding Moonbug, PNG screenshots, resource-dir + `$ORIGIN` rpath handling. |
| `controller` + `apps/lucyn` | **done, growing** | JSON stdio loop over CEF: `visit`, `html`, `eval`, `extract`, `extractAll`, `click`, `fill`, `submit`, `waitFor`, `waitIdle`, `cookies`/`setCookie`/`clearCookies`, `download`, `screenshot`, `search`, `profile`, `info`, `moonbug`/`captures`. Built without CEF (`LUCYN_WITH_CEF=OFF`) it still starts the stdio loop and answers every command with an error. |
| `search` | **done (Session 14)** | Provider registry and URL builder for ten free web search pages; extensible through `Registry::add`. SearXNG requires an instance URL. Controller `search` command and Node client `search()` use it. |
| `paint`, `layout` | **REMOVED (Session 19, step 10)** | Painter, display list, box tree and the litehtml/stb/font third-party code are gone; CEF renders. The `layout` module's old notes are in `SESSION11.md`/`SESSION12.md`. The row below was: box tree from `dom`; block stacking + inline word-wrap; **inline `style=""`**: display, width/height (content-box), margin/padding/border (px only, one border width), position relative/absolute (containing blocks, shrink-to-fit), visibility, z-index (stacking contexts), overflow clip, `background-color`/`color`/`border-color`. **No** `<style>` blocks/selectors, `%`/`em`, `margin:auto`, flexbox/grid/floats/tables. See `layout.h`'s header comment and `SESSION12.md`. |
| `lnm` (`lnminterpreter/`) | **REMOVED (Session 18)** | The whole LNM interpreter (parser, compile, App runtime) and its `lucyn-lnm` CLI were deleted with the headless pivot. `LNM_SPEC.md` and the example apps went too. |
| `ui` | **REMOVED (Session 18)** | The SDL2 window module, Ghostful Mode, and `apps/lucyn-browser` are gone. Nothing in the tree links SDL anymore. |
| `apps/lucyn` | **done** | the engine binary: JSON stdio protocol, `--profile`/`--settle`/`--console`/`--moonbug`/`--events`/`--download`/`--config`/`--version`/`--help`. `--show` and `--ghostful` are gone (unknown option). |

### Verified (in the sandbox, Ubuntu 24 / gcc, zero warnings)
- **10/10 ctest suites pass** on a fresh build: `core, search, profile, identity, security, moonbug, server, cef_host,
  controller, cli_download` — build is 0 errors / 0 warnings.
- End to end against a loopback server: `visit` loads, `waitFor` fires, `fill` puts a value into a real `<form>`,
  `submit` navigates Chromium, `extract` reads the destination page back; the captures produced on the way carry full
  Chrome/156 request headers (that is Chromium, not us).
- `lucyn --download` works from a **copied** binary in a temp dir: one line on stdout, exit 0/1, no `.part` leftovers,
  never overwrites silently (`--name`/`--overwrite` honoured).
- Chrome `sec-ch-ua` brand algorithm (decoy brand + order from the major version) matches real headers for Chrome 107..136
  **as remembered by a previous session, not from captured traffic.** It is locked in `tests/test_identity.cpp`. (The
  identity tables still exist; nothing injects them into Chromium yet.)

### NOT verified (do not claim otherwise)
- **No page has been loaded from the real internet** (sandbox network allowlist). Every `visit`/`fill`/`submit`/`extract`
  check so far ran against a loopback server. Detection pages are pending.
- **The persona is not on the wire.** Chromium sends *its own* UA/client hints; `profile::Switcher` selects a persona but
  nothing injects `Surface` into CEF yet. Until that lands, our identity work and the actual fingerprint can disagree —
  this is the top open gap.
- **`ControllerOptions::persistCookies` is currently a no-op** (declared, never read): CEF keeps its own cookie store in
  memory for the session, and the per-identity Netscape jar in `nothing-data/cookies/` is not wired to it yet.
- GPU / screen / hardware tables are hand-written from memory (Windows: UHD 630, Iris Xe, GTX 1650, RTX 3060, Radeon; Mac: M1..M2 Pro).
  Mobile tables are coarse. Firefox WebGL strings are guesses.
- Pre-`chrome101` targets send full version numbers in the real UA; we emit the modern reduced form. Safari patch versions in UA
  (`26.0.1` vs `Version/26.0`), macOS `platformVersion` mapping, Android `Linux armv81`, and frozen `Android 10; K` are from memory.
- Author's CPU: assumed `x86_64` (they once picked a `riscv64` download by mistake; `uname -m` check was suggested).

## 5. Known gaps and open questions
- **Headers: who owns what (rewritten in Session 19).** Chromium owns the whole header set and its order now; there is no
  `HttpClient` left to merge onto (the old M7 write-up is historical). What is **not** done: nothing feeds
  `Surface::userAgent`/`sec-ch-ua`/`acceptLanguage` into CEF, so the profile you select does not change a single header.
  (The M7-era write-up — merge-by-name onto libcurl's ordered set, appended unknown headers before `Connection`, never
  owning the base list — is historical; see `SESSION7.md`/`SESSION8.md`. None of that code exists any more.)
- **Cookies live in Chromium, session-only.** `cookies`/`setCookie`/`clearCookies` go through CEF's cookie manager, so a
  jar survives navigation but not a restart. `ControllerOptions::persistCookies` (the per-identity Netscape file in
  `nothing-data/cookies/`) is **declared but not wired** after the CEF move — re-implement it on top of CefCookieManager,
  or drop the flag.
- `settle()` / `visit()` do not wait for requests in flight (timers only); `waitIdle` does (it now counts in-flight requests).
- ~~Inline `on*=""` attributes are not compiled~~ **done (M8)**: `compileInlineHandlers()` runs at `parsed` stage.
- The binaries carry `-Wl,-rpath,$ORIGIN` and `libcef.so` + `icudtl.dat` + `*.pak` + `locales/` are copied next to them,
  so a built `build/bin/` folder is self-contained — but `initCef` also embeds the **absolute** distro `Resources/` path
  as a fallback. Moving the whole `third_party/cef/` distro after building means re-running CMake.
- `third_party/cef/` is ~1.5 GB unpacked and is **not** in git (it is the binary distribution); `third_party/mongoose/`
  and `third_party/README.md` are the only tracked third-party content left.
- `tor145` identity is **refused** (Tor fixes its own values); `IdentityManager` vetoes switching to it.
- Region defaults to `us-east`; it should match the proxy exit IP (timezone/language vs IP mismatch is a classic tell).
- `select()` of the already-active target is a silent no-op (no way to force a fresh handle).

## 6. Recommended next steps (in order)

0. **Put the persona on the wire (the big one):** inject `Surface` (UA, `sec-ch-ua`, `Accept-Language`) into Chromium via
   `CefRequestContext`/`OnBeforeResourceLoad`, then hit an echo endpoint from a real site and compare what it saw against
   `lucyn-profile identity <target>`. Today Chromium's own headers go out regardless of the selected profile.
1. **Re-wire `persistCookies`** on top of `CefCookieManager` (or delete the flag), and decide what happens to cookies on
   a profile switch.
2. ~~`dom` module~~ **deleted (Session 19)** — use CEF's DOM (`extract`/`extractAll`/`html`).
3. ~~`apps/lucyn` + `controller`~~ **done (M4-M7)**: `visit`, `eval`, `waitFor`, `waitIdle`, `click`, `cookies`/`setCookie`/`clearCookies`, `moonbug`/`captures`.
4. ~~`js` module (M3)~~ **superseded (Session 19)**: QuickJS is deleted and V8 inside Chromium runs page JS for real.
5. ~~`document.cookie` bridge~~ **superseded (Session 19)**: cookies go through CEF. ~~persist the jar~~ **regressed** — see §5.
6. ~~Profile identity in requests~~ **done for Accept-Language (M7)**; verify UA/`sec-ch-ua` equality against a real capture, then fix header ORDER for appended headers.
7. ~~**Rendering**~~ **rewritten (Session 19)**: `screenshot` reads CEF's `OnPaint` frame and encodes PNG in `cef_host`.
8. ~~Sessions 12–17 (inline styles, LNM, SDL window, browser app)~~ **all superseded by Session 18's headless pivot**: LNM/SDL/chrome are deleted; see `SESSION18.md`. Other open work: streams; canvas; header order; flexbox; `getBoundingClientRect`/`offsetWidth` real numbers.

### 6a. Long-term goal: terminal / LLM control (not urgent, keep designing toward it)

This is an **end-of-project aim, not a current task** — but every session should keep building in a way that doesn't close this door.

The controller's JSON-over-stdio protocol (newline-delimited, `{"id", "cmd", ...}` in / `{"id", "ok", ...}` out, unprompted events for captures) and the CLI binaries (`lucyn`, `lucyn-profile`, `lucyn-search`) are already close to ideal for an LLM to drive directly from a terminal: structured in, structured out, no screen-scraping, no CDP-style heavyweight automation surface for target sites to detect. Keep new controller commands and CLI flags in that same shape (flat, JSON-friendly, one clear reply per call) rather than anything that assumes an interactive human (prompts, confirmations, ncurses-style UI).

**Known gap to close eventually:** there's no `help`/`capabilities`/`status` controller command yet — an LLM driving this blind today has to already know the MD files' contents. Add a command that lists valid `cmd` values, their args, and current engine state, so an LLM can discover the protocol at runtime instead of needing docs in its context window.

**Deployment shape to design toward:** the expectation is a user (or an LLM acting for one) downloads/builds the engine once into a predictable folder layout, e.g.:
```
newfolder/
  engine/        # the built LucyN tree: build/bin/lucyn (+ the CEF runtime files it needs) + nothing-data/
  browsertest/   # the user's own scripts/project driving the engine over the JSON protocol
```
i.e. the engine lives in its own folder, sibling to whatever project is controlling it — not installed system-wide, not assumed to be on `PATH`. This matches the existing `nothing-data` rule (everything relative to the running binary, nothing in `~/.config`): an LLM should be able to find the engine by a known relative path from the project folder, run it, and talk JSON to it, with zero global install step. No code change needed for this today — just keep that deployment shape in mind when anything touches paths, install steps, or docs.

### Lessons from Nothing Browser's source (use as a "what not to do" list; session 4 applied the navigator ones, see `SESSION4.md`)
The author shared `IdentityGenerator.cpp`, `IdentityStore.cpp`, `FingerprintSpoofer.cpp` (not in this zip). Flaws found:
- `toDataURL` noise **stacks**: it writes noised pixels back, so two calls differ. `getChannelData` noise likely stacks too. **LucyN: noise must be deterministic per seed and idempotent.**
- Leaky globals (`window.__NB_FP_INIT__`, `__NB_NATIVE_SET__`) and `[NB]` console logs visible to pages.
- Objects rebuilt per access: `navigator.plugins === navigator.plugins` is false (also `mimeTypes`, `connection`, `userAgentData`). **Cache them.**
- `userAgentData` hardcoded (Chrome 124 / Linux), ignoring the identity. Renderer string always "Intel HD Graphics 3000 (SNB GT2) ... OpenGL 4.6" even for NVIDIA/AMD vendor.
- Random float WebGL range params (real GPUs report whole numbers); `webdriver` returns `undefined` (real Chrome: `false`); RAM up to 32 (Chrome caps `deviceMemory` at 8).
- `webglSeed`, `fontSeed`, cores, RAM, language were generated/stored but not applied in the shared files.
- Docs said seeds change per session; the code makes one **permanent** identity until the file is deleted.

## 7. Environment notes (for the AI)
- Sandbox: Ubuntu 24, `g++` present; `cmake` + a C++17 compiler are the only build requirements (no SDL2, no libcurl,
  no libgumbo — CEF is prebuilt in `third_party/cef/`). `python3 -m http.server` on loopback is the test web server.
- Network allowlist: `github.com`/`raw.githubusercontent.com`/`codeload.github.com` (release downloads and raw file fetches work),
  `api.github.com` (rate-limited, often 403/404 — raw.githubusercontent.com is more reliable for single-file fetches),
  pypi/npm/crates, `archive.ubuntu.com`/`security.ubuntu.com` (apt). **Everything else is blocked** (e.g. tls.peet.ws,
  example.com, fonts.google.com, dejavu-fonts.org), so live fingerprint/network checks must be run by the author. A local
  `python3 -m http.server` loopback works for testing the engine's own HTTP client/rendering without real internet —
  background it with `setsid ... &` in the SAME tool call that uses it, since each tool call may be a fresh shell and kill
  anything backgrounded in a previous one.
- `mkdir -p a/{b,c}` brace expansion misbehaved in this tool: use explicit `mkdir` calls.
- CMake per-byte string loops are slow; prefer one `string(REGEX REPLACE ...)` pass for anything binary-to-C-array sized
  (this advice came from the deleted `modules/paint/embed_font.cmake`: one regex pass beat a per-byte
  `string(SUBSTRING ...)` loop by miles — keep it for any binary-to-header conversion, e.g. embedding HTML/JS).
- A module's own `CMakeLists.txt` cannot see a target defined further down in whoever `add_subdirectory()`'d it (chicken
  and egg — the subdirectory is processed before the parent's own `add_library()` line runs). Fix: the parent wires the
  cross-module `target_link_libraries`/`target_compile_definitions` itself, after both targets exist — see `SESSION11.md`'s
  "CMake note" for worked examples (the rule still applies — e.g. `controller` wiring `lucyn::cef_host` itself).
- Author's style: chatty and funny replies preferred; types casually with typos (read generously); likes "explain like I'm a kid"
  analogies when confused; wants plain honesty about what is and is not tested.

## 8. Zip map
```
lucyn/
  CMakeLists.txt  README.md  HANDOFF.md  .gitignore
  SESSION2.md .. SESSION19.md   SESSION12_PLAN.md   LUCYN_ENGINE_API_SPEC.md   (LNM_SPEC.md deleted with LNM)
  modules/profile/   include/lucyn/profile/{profile.h,identity.h}  src/{profile.cpp,identity.cpp,identity_store.cpp,detail.h}
                     tools/lucyn-profile.cpp  tests/{test_profile.cpp,test_identity.cpp}  README.md
  modules/net/       include/lucyn/net/moonbug.h  src/moonbug.cpp  tests/test_moonbug.cpp   (Moonbug only — HttpClient/fetch are gone)
  modules/core/      include/lucyn/core/paths.h  src/paths.cpp  tests/test_core.cpp  README.md
  modules/cef_host/  include/lucyn/cef_host/browser_host.h  src/browser_host.cpp  tools/lucyn_cef_smoke.cpp  tests/test_cef_host.cpp   (Session 19 — owns CEF)
  modules/{security,server,controller}/   (done, see table above)
  modules/{adblock,logger}/               (filter rules / diagnostics)
  modules/search/     include/lucyn/search/search.h  src/search.cpp  tools/lucyn-search.cpp  README.md   (Session 14)
  third_party/cef/               (CEF binary distribution: headers, Release/, Resources/, libcef_dll/ — NOT in git)
  third_party/mongoose/          (vendored Mongoose, GPL-2.0/commercial)
  third_party/README.md          (what belongs in third_party/)
  client/            src/index.js  README.md   (nothing-browser JS client; screenshot/setWindow/setVisible added Session 11)
  dashboard/          (empty)
  # modules/{dom,shell,js,paint,layout}/, third_party/{quickjs,stb,fonts,litehtml}/ and lnminterpreter/ are gone
```

## 9. Suggested opening message for the next session
> "Read HANDOFF.md, SESSION19_CEF_MIGRATION.md, README.md and LUCYN_ENGINE_API_SPEC.md first. Then:
> [paste what you want next]."

> The engine config is still v0.1-alpha — extend `LUCYN_ENGINE_API_SPEC.md` with any new configs the CEF engine
> naturally requires. Same rule as the code: say what you added and why.


more context this was from previous session it had questions it wanted me to answer so here is the questio/concern
Ha, reading that generously, I think you're asking what comes next: the engine API, or something else, or are we good? Here's my take.

**We're good to stop.** Everything from this session is delivered and tested, so nothing is half-built.

**The engine API is the right next thing.** Right now LNM is a library plus a CLI, and nothing in the real engine starts it. The spec already has `engine.config.lnm.entry = "./ui/shell.lnm"`, and the plan is to make the engine read that, load it into the `App`, and show it in the window. Without that step LNM works but nothing runs it. Behind it comes mouse input in the window, because nothing delivers hover or click to LNM yet. Then `runjs`, so buttons can call JS functions.

**Two small decisions first:**
1. **The one-word `net`/adblock fix.** I can apply it if you want. It's your work in progress, so I left it alone.
2. **CSS direction.** Do you want LNM to keep styling things itself, as `LNM_SPEC.md` says, or switch to pointing at real `.css` files, as the pasted text in the README hints? The engine wiring is the same either way, but the CSS direction decides whether `layout` needs a full CSS parser later.

My suggested order is the `net` fix, then engine config → LNM, then mouse input, then `runjs`. Tell me the CSS answer and whether to go, and I'll start on the engine wiring.
and i answerd
lnm will never use .css files for now i dont want to write css perser nope and i also want you to add some small fix i want to set the default curl impersonate path in the third party folder because the engine setupa and build will look for it their and also the add block i want you to set the filter files default in the add block module name it any folder name and it will have txt files any formart
so continue
i am thinking the next thing to handle is engine api
