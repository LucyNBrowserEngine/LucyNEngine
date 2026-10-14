# LucyN: handoff for the next session

**Latest: `SESSION11.md` (M10: `paint` + `layout` + `ui` — the painter, block/inline layout, SDL window + Ghostful Mode,
`screenshot`/`window` controller commands; read it first).** Then `SESSION10.md` (M9: `Blob`/`File`, real `FormData` file
parts, `Response`/`Request.blob()`, XHR `responseType: 'blob'`, plus the `download` command addendum), `SESSION9.md` (M8:
inline `on*` attribute compilation + full WebSocket), `SESSION8.md` (M7: cookies, fetch/XHR rebuilt, identity +
browser-context headers, the POST hang fix), `SESSION7.md` (M6: eval/waitFor/waitIdle/click), `SESSION6.md` (M5: Moonbug +
NULLBYTE + TinyElysia, incl. licensing note), `SESSION5.md` (M4 controller/client), `SESSION4.md` and `modules/js/README.md`
(JS design rules), then `README.md` (spec/vision). Written at the end of session 1 (6 Oct 2026); updated session 11 (13 Oct 2026).

**One-line state:** identity, network (any HTTP method, **cookie jar, persisted per identity**, identity + browser-context
headers, `download`), a **mutable DOM**, the **shell + script injection + snapshot/diff** layer, a **real JS engine (QuickJS +
DOM bridge)** with real `fetch`/`Headers`/`Request`/`Response`/`AbortController`/`FormData`/`XMLHttpRequest`/`document.cookie`/
`WebSocket`/`Blob`/`File`, inline `on*` attribute compilation, `eval()`/`waitFor()`/`waitIdle()`/`click()`/`cookies`/`download`
controller commands, **and now (Session 11) a real rendering pipeline**: `paint` (display list + SoftwarePainter, PNG out, no
SDL needed), `layout` (block/inline box tree from the DOM, UA default rules by tag, word wrap), `ui` (SDL2 window, Ghostful
Mode — the window always exists for real, only its visibility is faked), and `screenshot`/`window` controller commands, with
`document.hidden`/`visibilityState`/focus/blur all consistently tracking the real window state. 20/20 test suites pass on
stock libcurl (not re-verified against real libcurl-impersonate this session — no network access in the sandbox). No CSS
parsing yet (`style=""`/`<style>` blocks), no flexbox, no LNM language yet — see `SESSION12_PLAN.md`.

---

## 1. What LucyN is (30 seconds)

A from-scratch browser engine in C++17, **headless-first** ("a useful v0.4 exists before a single pixel is drawn"),
built for the author's own project, *Nothing Browser*. The author is **not** copying Nothing Browser's code (it is a
Qt/Chromium app that patches a real browser with injected JS); LucyN owns its HTTP stack, DOM and JS engine, so it can do
things natively instead of patching them. `README.md` has the full vision, roadmap (M1..M9) and API ideas.

## 2. Build and test

```bash
sudo apt install build-essential cmake libgumbo-dev libcurl4-openssl-dev libsdl2-dev   # SDL2 optional: ui module skips itself without it
cmake -S . -B build
cmake --build build -j
SDL_VIDEODRIVER=dummy ctest --test-dir build --output-on-failure
# 20 suites: core, profile, identity, security, net, moonbug, net_request, server, dom, dom_mutate,
#            shell, js, js_net, paint, layout, ui, controller, controller_net, controller_download, cli_download
```

Each module also builds alone: `cmake -S modules/profile -B b`, `cmake -S modules/net -B b`, `cmake -S modules/paint -B b`,
`cmake -S modules/layout -B b` (pulls in `dom`+`paint`), `cmake -S modules/ui -B b` (pulls in `paint`; needs SDL2).
Root options: `LUCYN_MODULE_CORE|PROFILE|NET|DOM|SHELL|JS|PAINT|LAYOUT|UI`, `LUCYN_BUILD_TESTS`, `LUCYN_BUILD_TOOLS`.
DOM needs `libgumbo-dev`; `ui` needs `libsdl2-dev` (optional — everything else builds and tests without it).

**curl-impersonate** (real Chrome/Safari/Firefox TLS + HTTP/2 handshakes) is a prebuilt dependency. The author is moving it to
`third_party/curl-impersonate/`; CMake then finds it automatically (see `third_party/README.md` for the exact download).
Pinned **v2.2.3**, repo `github.com/lexiforest/curl-impersonate`. Release tarball layout is *flat*: `libcurl-impersonate.so`
+ `include/curl/` + LICENSE files (the 62 MB `libcurl-impersonate.a` is unused and can be deleted; verified).
Without it, everything builds against stock libcurl and reports `[NOT impersonating]` honestly.

CLI tools to poke things:
```
build/bin/lucyn-profile  list | info | resolve | switch | identity ...
build/bin/lucyn-fetch    --probe | -i <target[,target...]> [--body] [--headers] <url>
build/bin/lucyn-dom      --tree | -q <selector> | --diff before.html after.html
build/bin/lucyn-shell    <url> | --file page.html | --init | --list-scripts
build/bin/lucyn-paint    --demo out.png | --file page.html out.png
build/bin/lucyn          [--show | --ghostful] [--download <url> ...] [--profile ...] ...   # the real engine binary
```

## 3. Architecture decisions (keep these)

1. **Modules.** `modules/<name>/` = `include/lucyn/<name>/`, `src/`, `tools/` (CLI), `tests/`, own `CMakeLists.txt`
   that works standalone *and* via `add_subdirectory`. Target alias `lucyn::<name>`. Root CMake only wires modules.
2. **One shared identity.** A single `profile::Switcher` holds the active persona. Modules *follow* it: `net` adds a validator
   (asks libcurl "do you accept this target?") and a listener (rebuilds its curl handle). `ui`, `controller`, `js` will do the same.
   Nobody talks to anybody else about identity.
3. **Persona vs device.**
   - *Persona* = which browser (a `Profile` such as `safari184`). Everything the browser reports (`Surface`: UA, platform, vendor,
     `sec-ch-ua`, `userAgentData`, WebGL strings, which JS APIs exist) is **derived**, never stored, so TLS and JS cannot disagree.
   - *Device* = the machine (cores, memory, screen, GPU, timezone, languages, noise seeds). Generated once, **stored per
     (profile name, OS class)**, so "the same person comes back tomorrow"; Chrome-on-Mac and Safari-on-Mac share one Mac.
4. **Persona OS comes from curl-impersonate's own table**, not the host. `chrome146` claims macOS Tahoe, so the default
   `"chrome"` persona is a Mac even on a Linux host. The device follows the persona.
5. **Switching builds a fresh curl handle** (no connection-pool or TLS-session leak between identities).
6. A failed `select()` leaves the old profile active. A corrupt identity file is **reported, never silently replaced**.
7. Lenient by default: unknown target names pass through (the library has the final say). `setStrict(true)` limits to the table.
8. `engine.switch.mode(...)` from the README **cannot compile** (`switch` is a C++ keyword). Rename (`engine.swap`, `engine.profile.set`).
9. UI/controller split (author's design): the `ui` module *always* reports "I am providing a UI"; the `controller`
   module decides whether it is shown. That is where "always headless" (Ghostful Mode) comes from. **Built, Session 11**:
   `ui::providingUI()` is a hardcoded `true`; `ui::Window` always creates a real (possibly `SDL_WINDOW_HIDDEN`) SDL window;
   the controller's `window` command is the only thing that ever calls `setVisible()`. See `SESSION11.md`.
10. **Layout never draws** (README's rule, now built): `modules/layout` only ever produces a `paint::DisplayList`; exactly
    one backend (`paint::SoftwarePainter`) turns that into pixels. No module skips this — `screenshot` goes through both,
    `window` just also calls `Window::present()` with the same `Framebuffer`.

## 4. Status

| Module | State | Notes |
|---|---|---|
| `profile` | **done** | Registry (37 browsers), Switcher, JSON, identity generator, Surface, consistency checker, IdentityStore, IdentityManager. Depends only on `core`. |
| `net` | **done (M7)** | `HttpClient::request()` (any method/body/headers, merged by name onto the browser's own set), libcurl cookie engine + jar API, page-context headers (`Sec-Fetch-*`/`Referer`/`Origin`), identity header provider, `Response.statusText/httpVersion/redirects`. `get()`/`post()` are thin wrappers. Jar is in-memory (not persisted). |
| `core` | **done** | `nothing-data` paths. Everything LucyN stores goes there (rule from the author). |
| `dom` | **done** | Mutable tree, selectors, innerHTML, mutation hook, tag diff. ASan/UBSan clean. |
| `shell` | **done** | Shell page, mounting, `@run-at` scripts, snapshots, hooks, `ScriptHost` seam, `NullHost`. |
| `js` | **done (v0, growing compatibility layer)** | `QuickJsHost`: DOM bridge, events, MutationObserver, timers, location/history/navigator/screen/URL, real fetch/Headers/Request/Response/AbortController/FormData, full XHR, `document.cookie`, TextEncoder/Decoder (M7), `WebSocket`/`CloseEvent`/`MessageEvent` (M8), inline `on*` attribute compilation (M8), `Blob`/`File` incl. real multipart file parts and `responseType: 'blob'` (M9), classic `<script src>`, storage, CacheStorage subset, limited Web Crypto and IntersectionObserver. QuickJS-ng 0.17.0 vendored. No canvas, no streams. See `modules/js/README.md`. |
| `security` | **done (M5)** | NULLBYTE: HMAC-SHA256 seals, persisted key. |
| `server` | **done (M5)** | TinyElysia (Mongoose) + Moonbug capture server + dashboard. Mongoose is GPL-2.0/commercial. |
| `controller` + `apps/lucyn` | **done, growing** | JSON stdio loop; `visit`, `eval`, `waitFor`, `waitIdle`, `click`, `cookies`/`setCookie`/`clearCookies`, `moonbug`/`captures`, `download` (M9 addendum), **`screenshot`/`window` (M10/Session 11)**. |
| `paint` | **done (M10/Session 11)** | Display list, `Painter` interface, `SoftwarePainter` (CPU-only, no SDL). `stb_truetype`/`stb_image`/`stb_image_write` vendored; one embedded font (PT Sans, OFL). ASan/UBSan clean. `lucyn-paint --demo`/`--file`. See `modules/paint/README.md`. |
| `layout` | **done, "Must" rung only (M10/Session 11)** | Box tree from `dom`; UA default display by tag; block stacking + inline word-wrap; `display:none` honoured; line-height from real font metrics; produces a `DisplayList`. **No CSS parsing** (`style=""`, `<style>` blocks — ladder's "Should"/"Could" rungs), no flexbox/grid/floats/tables. See `modules/layout/include/lucyn/layout/layout.h`'s header comment and `SESSION11.md`. |
| `ui` | **done, optional (M10/Session 11)** | SDL2 window wrapper, Ghostful Mode (`providingUI()` always `true`; `setVisible()` is the only real/hidden switch). Builds only if SDL2 is found — everything else is unaffected if it's absent. Tests run under `SDL_VIDEODRIVER=dummy`; a real on-screen check is still the author's to do. See `modules/ui/README.md`. |
| `apps/lucyn` | **done** | the engine binary: JSON stdio protocol, `--profile`/`--settle`/`--console`/`--moonbug`/`--events`/`--download`/`--show`/`--ghostful`/`--version`/`--help`. |

### Verified (in the sandbox, Ubuntu 24 / gcc, zero warnings)
- All 3 ctest suites pass, with stock libcurl **and** with real libcurl-impersonate v2.2.3 (x86_64).
- Against the real library: `curl_easy_impersonate` found, `lucyn-fetch --probe` => **37 / 37** table profiles accepted,
  `select("safari184")` accepted and a fake name rejected.
- The author ran the stock build on their own Linux machine: fetch of example.com returned 200.
- Chrome `sec-ch-ua` brand algorithm (decoy brand + order from the major version) matches real headers for Chrome 107..136
  **as remembered by the previous session, not from captured traffic.** It is locked in `tests/test_identity.cpp`.

### NOT verified (do not claim otherwise)
- **The JS module has never run a real website** (sandbox network). Detection pages and a side-by-side `Function.prototype.toString`/`navigator` comparison with real Chrome are pending. Details: `SESSION4.md`.
- **No live TLS fingerprint has ever been compared** (sandbox blocks tls.peet.ws: `host_not_allowed`). The author's first
  real check is still pending.
- Whether our computed UA / `sec-ch-ua` for `chrome146` equals what curl-impersonate actually sends (compare with the header in `curl_chrome146`).
- GPU / screen / hardware tables are hand-written from memory (Windows: UHD 630, Iris Xe, GTX 1650, RTX 3060, Radeon; Mac: M1..M2 Pro).
  Mobile tables are coarse. Firefox WebGL strings are guesses.
- Pre-`chrome101` targets send full version numbers in the real UA; we emit the modern reduced form. Safari patch versions in UA
  (`26.0.1` vs `Version/26.0`), macOS `platformVersion` mapping, Android `Linux armv81`, and frozen `Android 10; K` are from memory.
- Author's CPU: assumed `x86_64` (they once picked a `riscv64` download by mistake; `uname -m` check was suggested).

## 5. Known gaps and open questions
- **Headers: who owns what (decided in M7).** With libcurl-impersonate the library owns the browser header set and its order;
  `HttpClient` only *merges by name* (a same-named header keeps its slot, `"Name:"` removes one; verified on the wire). Added: `Accept-Language`
  from `Surface::acceptLanguage` (same source as `navigator.languages`) on every request; on stock libcurl also `User-Agent` + `Sec-CH-UA*`.
  `User-Agent`/`Sec-CH-UA` are deliberately NOT overridden in impersonating mode (still unverified that `Surface` equals what the library sends).
  For page-initiated requests the navigation headers are corrected (`Accept: */*`, `Sec-Fetch-Mode/Dest/Site`, no `Upgrade-Insecure-Requests` /
  `Sec-Fetch-User`) and `Referer`/`Origin` added. **Known deviation:** headers that did not exist in the base list are *appended* (before
  Connection), not placed in Chrome's exact slot, and Firefox keeps its navigation `Priority: u=0, i` / `Te`. Fixing order means owning the base
  list (`CURLOPT_HTTPBASEHEADER`); not attempted without a reference capture from a real browser.
- ~~Cookies are in-memory and die with the curl handle~~ **done**: `ControllerOptions::persistCookies` saves/loads the jar as a Netscape
  cookie file per identity (`nothing-data/cookies/<profile>.txt`) on every profile switch and at shutdown (`net::HttpClient::export/importCookieFile`,
  wired in `controller.cpp`). Off by default; the in-memory-only behaviour described above is what you get without the flag.
- `settle()` / `visit()` do not wait for requests in flight (timers only); `waitIdle` does (it now counts in-flight requests).
- ~~Inline `on*=""` attributes are not compiled~~ **done (M8)**: `compileInlineHandlers()` runs at `parsed` stage.
- Build RPATH is **absolute** to wherever `third_party/curl-impersonate` was at build time. Moving the folder after building
  needs a rebuild or `LD_LIBRARY_PATH`. Idea: copy the `.so` next to the binaries / use `$ORIGIN`.
- `third_party/curl-impersonate` is ~31 MB (after deleting the `.a`): decide commit vs `.gitignore` + setup script.
- `tor145` identity is **refused** (Tor fixes its own values); `IdentityManager` vetoes switching to it.
- Region defaults to `us-east`; it should match the proxy exit IP (timezone/language vs IP mismatch is a classic tell).
- `select()` of the already-active target is a silent no-op (no way to force a fresh handle).

## 6. Recommended next steps (in order)

0. **Author, on their machine:** unpack curl-impersonate into `third_party/`, rebuild, run `--probe`, then
   `lucyn-fetch -i chrome146,safari184,firefox147 --body https://tls.peet.ws/api/all` and paste the output. Compare JA4, HTTP/2
   fingerprint, `user_agent`, `sec-ch-ua` against `lucyn-profile identity <target>`.
1. **`lucyn-fetch --verify`**: fetch an echo endpoint, diff what the server saw against `surfaceFor()`. First place profile and network can disagree out loud.
2. ~~`dom` module~~ **done (session 3)**, now mutable.
3. ~~`apps/lucyn` + `controller`~~ **done (M4-M7)**: `visit`, `eval`, `waitFor`, `waitIdle`, `click`, `cookies`/`setCookie`/`clearCookies`, `moonbug`/`captures`.
4. ~~`js` module (M3)~~ **growing (M9)**. fetch/XHR/cookies/WebSocket/inline `on*`/`Blob`/`File` are real. Next: streams, then canvas.
5. ~~`document.cookie` bridge~~ **done (M7)**. ~~persist the jar~~ **done**: `persistCookies`, per identity, in `nothing-data/cookies/`.
6. ~~Profile identity in requests~~ **done for Accept-Language (M7)**; verify UA/`sec-ch-ua` equality against a real capture, then fix header ORDER for appended headers.
7. ~~**Rendering**~~ **done, Session 11** (M10): `paint`, `layout` (block/inline only), `ui`. See `SESSION11.md`.
8. **Next (Session 12, see `SESSION12_PLAN.md`):** the author's call, same as every session boundary. Candidates on the
   table: CSS (`style=""` then `<style>` blocks), flexbox, the LNM language (`LNM_SPEC.md` — needs `layout`/`paint`/`ui`,
   which now exist, so this is unblocked but still not started), `getBoundingClientRect`/`offsetWidth` real numbers,
   streams, canvas, header order.

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
- Sandbox: Ubuntu 24, `g++` present; run `apt-get update` before `apt-get install cmake libcurl4-openssl-dev libgumbo-dev libsdl2-dev`.
- Network allowlist: `github.com`/`raw.githubusercontent.com`/`codeload.github.com` (release downloads and raw file fetches work),
  `api.github.com` (rate-limited, often 403/404 — raw.githubusercontent.com is more reliable for single-file fetches),
  pypi/npm/crates, `archive.ubuntu.com`/`security.ubuntu.com` (apt). **Everything else is blocked** (e.g. tls.peet.ws,
  example.com, fonts.google.com, dejavu-fonts.org), so live fingerprint/network checks must be run by the author. A local
  `python3 -m http.server` loopback works for testing the engine's own HTTP client/rendering without real internet —
  background it with `setsid ... &` in the SAME tool call that uses it, since each tool call may be a fresh shell and kill
  anything backgrounded in a previous one.
- `mkdir -p a/{b,c}` brace expansion misbehaved in this tool: use explicit `mkdir` calls.
- CMake per-byte string loops are slow; prefer one `string(REGEX REPLACE ...)` pass for anything binary-to-C-array sized
  (see `modules/paint/embed_font.cmake` — ~320ms for a 442KB font via one regex pass vs. an unmeasured but clearly much
  slower per-byte `string(SUBSTRING ...)` loop).
- A module's own `CMakeLists.txt` cannot see a target defined further down in whoever `add_subdirectory()`'d it (chicken
  and egg — the subdirectory is processed before the parent's own `add_library()` line runs). Fix: the parent wires the
  cross-module `target_link_libraries`/`target_compile_definitions` itself, after both targets exist — see `SESSION11.md`'s
  "CMake note" for two real examples (`js`→`lucyn-shell`, pre-existing; `layout`→`lucyn-paint`'s `--file`, Session 11).
- Author's style: chatty and funny replies preferred; types casually with typos (read generously); likes "explain like I'm a kid"
  analogies when confused; wants plain honesty about what is and is not tested.

## 8. Zip map
```
lucyn/
  CMakeLists.txt  README.md  HANDOFF.md  .gitignore
  SESSION2.md .. SESSION11.md   SESSION12_PLAN.md   LNM_SPEC.md   LUCYN_ENGINE_API_SPEC.md
  modules/profile/   include/lucyn/profile/{profile.h,identity.h}  src/{profile.cpp,identity.cpp,identity_store.cpp,detail.h}
                     tools/lucyn-profile.cpp  tests/{test_profile.cpp,test_identity.cpp}  README.md
  modules/net/       include/lucyn/net/http_client.h  src/http_client.cpp  tools/lucyn-fetch.cpp  tests/{test_net,test_net_request,test_moonbug}.cpp  tests/mini_server.h
  modules/core/      include/lucyn/core/paths.h  src/paths.cpp  tests/test_core.cpp  README.md
  modules/dom/       include/lucyn/dom/{dom.h,diff.h}  src/{dom,parse,mutate,diff}.cpp  tools/lucyn-dom.cpp  tests/{test_dom,test_dom_mutate}.cpp  README.md
  modules/shell/     include/lucyn/shell/{scripts,url,snapshot,host,hydrator,assets}.h  src/*.cpp  tools/lucyn-shell.cpp  tests/test_shell.cpp  README.md
  modules/js/        include/lucyn/js/quickjs_host.h  src/{host,dom_bridge,window,prelude,fetch_bridge,storage_bridge,websocket_bridge}.cpp  src/prelude/p{1,2,3,4,5,9}_*.js  embed_prelude.cmake  tests/{test_js,test_js_net}.cpp  README.md
  modules/paint/     include/lucyn/paint/{displaylist,painter,framebuffer,software_painter}.h  src/{software_painter,font_data}.cpp
                     tools/lucyn-paint.cpp  tests/test_paint.cpp  embed_font.cmake  README.md   (Session 11)
  modules/layout/    include/lucyn/layout/layout.h  src/layout.cpp  tests/test_layout.cpp                        (Session 11)
  modules/ui/        include/lucyn/ui/window.h  src/window.cpp  tests/test_ui.cpp  README.md                     (Session 11, optional: needs SDL2)
  modules/{security,server,controller}/   (done, see table above)
  third_party/quickjs/           (vendored QuickJS-ng v0.17.0, see VENDORED.txt)
  third_party/stb/               (vendored stb_truetype/stb_image/stb_image_write, see VENDORED.txt — Session 11)
  third_party/fonts/             (vendored PTSans-Regular.ttf, OFL-1.1, see VENDORED.txt — Session 11)
  third_party/README.md          (curl-impersonate goes in third_party/curl-impersonate/)
  client/            src/index.js  README.md   (nothing-browser JS client; screenshot/setWindow/setVisible added Session 11)
  dashboard/          (empty)
  lnminterpreter/     (new folder, empty/placeholder — reserved for the LNM language work; see SESSION12_PLAN.md)
```

## 9. Suggested opening message for the next session
> "Read HANDOFF.md, SESSION11.md, SESSION12_PLAN.md, README.md, LNM_SPEC.md, and LUCYN_ENGINE_API_SPEC.md first. Then:
> [paste what you want next]."

> The engine config and LNM spec are still v0.1-alpha — extend `LUCYN_ENGINE_API_SPEC.md` with any new configs that LNM,
> the painter, UI module, or layout work naturally requires. Same rule as the code: say what you added and why. The
> `lnminterpreter/` folder is reserved for LNM parsing/interpreter work specifically — keep it separate from `modules/layout`
> and `modules/paint` (which LNM will eventually drive, not replace).