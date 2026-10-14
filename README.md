Here is the consolidated README, merging the new content (LNM, Google Sign-In, Logger, and the "Why it makes LucyN better" section) into the existing structure while maintaining the tone and technical depth.

***

# LucyN Engine

**Built for Nothing Browser. Powered by the Nothing philosophy: we don't care about beauty, we care about stealth, capture, and truth.**

> Status: **pre-alpha, design phase.** Everything below describes where LucyN is going. Sections are marked with what is built and what is planned. See [Roadmap](#roadmap).

---

## What is LucyN

LucyN is a small browser engine built from the ground up with one goal: **be invisible.**

No CDP. No remote debugging ports. No automation flags. No difference between headless and headful. The engine always presents itself as a real, human-operated browser, because as far as it is concerned, it always is.

This is not an engine for building pretty browsers. If you want pretty, use Chromium. LucyN is for people who need to see what the web is actually doing, and do it quietly.

**The LucyN rendering promise:**

| Question | Answer |
|---|---|
| Did the site fully render? | ✅ Yes |
| Does the site look like sh*t? | 💩 Oh hell yes |
| Can I scrape it? | 👻 Absolutely |

**LucyN is a scraping engine that happens to be able to build apps.**

---

## Core Philosophy

1. **One mode.** The engine does not know what headless means.
2. **Stealth first.** Every decision is made through the lens of detectability.
3. **Capture everything.** Nothing that touches the network escapes Moonbug.
4. **CSS is duct tape.** Sites render. JS runs. Beauty is not the mission.
5. **Tiny on purpose.** No multi-gigabyte toolchains. Vendored single-purpose libraries, small builds, small repo.
6. **Google can hate us.** Brave Search and DuckDuckGo ship by default.
7. **Cloudflare is an engine-level problem**, not an app-level one. (We will claim victory only after testing, not before.)

---

## Why LNM

LucyN ships with **LNM (Lucy Native Markup)** — a declarative UI language for building the engine's own interface.

The alternative was C++ UI code or QPainter. That is not happening. Writing native C++ UI is painful, verbose, and something only the engine internals should ever touch. QPainter and every Qt-based UI solution carries the same problem — it is a C++ developer's tool, and most people building on LucyN are not thinking in C++.

Everyone already knows HTML. Everyone already knows CSS. LNM takes that knowledge and adds one small layer on top: a handful of APIs for wiring events, switching views, and calling JS functions. That is the entire learning curve.

```lnm
grab(./shell.html)
grab.js(./api.js)

#search-btn:onclick {
    runjs: handleSearch()
}

.nav-item:onhover {
    scale: 1.05
    textcolor: #ffffff
    transition: 150ms
}
```

No C++. No QPainter. No framework. Just HTML for structure, LNM for wiring, and JS for logic — compiled into the binary by `lnmc` before the app ships.

**LNM is the glue.** It is the layer that connects HTML, CSS-style declarations, and JS functions into one coherent UI without any of them having to know about each other. You do not need a framework. You do not need React. LNM is that thing — and it compiles away completely so there is zero runtime overhead.

This also makes LucyN genuinely useful beyond scraping. **Single-tab apps, internal tools, desktop utilities** — anything that needs a UI and wants to ship as one small binary. LNM handles the entire UI layer. The engine handles the rest.

---

## Why Google Sign-In

Because clicking the Cloudflare checkbox is annoying and it should not exist for a legitimate engine making legitimate requests.

The checkbox exists because Cloudflare cannot tell if the request is from a human or a bot. A valid Google identity token attached to a session is proof that Google already verified this is a real person. Cloudflare trusts Google's verification. The checkbox goes away — or at minimum the trust score jumps high enough that the engine's own fingerprint spoofing takes it the rest of the way.

LucyN implements Google Sign-In using raw OAuth2 + OpenID Connect. No Google SDK. No Play Services. Four HTTP calls, a local redirect server, and a token stored encrypted on disk. The user signs in once through their real browser. After that LucyN handles token refresh silently.

**The trust model:**
```
Google auth     →  proves humanity to Cloudflare  →  trust score: ~5
Engine spoofing →  looks like a real browser      →  trust score: 8-9
```

Two separate systems. Two separate jobs.

---

## Why the Logger

Terminal output from an engine should not look like a wall of grey text. Ernest-logger is an Ernest Tech House npm library — the logging style, colors, and emoji conventions from it are the reference for LucyN's C++ engine logger in `modules/logger/`. Dev builds show everything. Prod builds show warnings and more severe messages. The planned `LNM.log` API will be injected into LNM contexts once the LNM runtime exists; page `console` is currently separate.

It explains itself the moment you run the engine.

---

## Everything Added Makes LucyN Better

LucyN started as a scraping engine. That is still the core and always will be. But every addition since then makes the platform stronger without compromising that core:

| Addition | Why it makes LucyN better |
|---|---|
| **LNM** | UI without C++. Ship internal tools and single-tab apps as one binary |
| **Google Sign-In** | Cloudflare trust score jumps. Less friction. Human proof built in |
| **Logger** | Dev experience. Know exactly what the engine is doing at every step |
| **LNM.http** | JS inside LucyN can make real HTTP requests through the C++ networking layer |
| **lnmc build pipeline** | One command. HTML, JS, LNM — all compiled and embedded. One binary ships |

None of these replace the scraping capability. They sit on top of it. The engine underneath — Moonbug, NULLBYTE, curl-impersonate, QuickJS, the painter — is untouched.

---

## Stack

| Layer | Library | Notes |
|---|---|---|
| HTML parsing | **Gumbo** | Tiny, pure C, vendored |
| JS engine | **QuickJS** | A handful of C files, vendored |
| Networking | **libcurl** (via **curl-impersonate**) | Chrome-like TLS and HTTP/2 fingerprint |
| Network capture | **Moonbug** | Sits between libcurl and the engine |
| Security protocol | **NULLBYTE** | HMAC-SHA256 signing of captured traffic |
| Windowing and input | **SDL2** | Real window, can be hidden (Ghostful Mode) |
| Rendering | **Software painter** behind a display-list interface | Skia can be swapped in later |
| Text and images | **stb_truetype**, **stb_image** | Single-header, vendored |
| Internal server | **TinyElysia** (on **Mongoose**) | HTTP + WebSocket, pure C/C++ |
| UI language | **LNM** | Lucy Native Markup — declarative UI wiring, compiled by lnmc |
| JS minifier | **uglify-js** | Runs inside lnmc's embedded QuickJS — no Node required |
| Ad blocking | Built-in | Filter list applied at the request layer |
| Optional UI chrome | **Dear ImGui** | URL bar and buttons, if you want them |

**No Qt. No QtWebEngine. No Bun, no Node, no npm inside the engine.** (Node/Bun only appear on the client side, see [JS Client Library](#js-client-library).)

Total footprint: SDL2 and libcurl as system dependencies, everything else lives in `third_party/` as plain source.

---

## Architecture

```
            ┌──────────────────────────────────────────┐
            │              Your code                   │
            │  (JS client lib, or C++ embedding API)   │
            └───────────────┬──────────────────────────┘
                            │  JSON commands over stdio / local socket
                            ▼
┌────────────────────────────────────────────────────────────┐
│                      LucyN Engine                          │
│                                                            │
│  libcurl ──▶ Moonbug ──▶ NULLBYTE ──▶ TinyElysia ──▶ Dash  │
│     │         (capture)    (sign)      (serve)              │
│     ▼                                                      │
│  HTML ──▶ Gumbo ──▶ DOM ◀──▶ QuickJS (DOM bridge)          │
│                      │                                     │
│                      ▼                                     │
│                   Layout ──▶ Display list                  │
│                                   │                        │
│                        ┌──────────┴───────────┐            │
│                        ▼                      ▼            │
│                 SoftwarePainter         SkiaPainter        │
│                  (SDL window)           (someday, maybe)   │
│                                                            │
│  LNM ──▶ lnmc ──▶ .h structs ──▶ embedded in binary        │
│   (UI wiring, events, view switching, JS bridge)           │
└────────────────────────────────────────────────────────────┘
```

The key design rule: **layout never draws.** It produces a display list (a dumb list of "draw this here" commands), and exactly one backend file turns that list into pixels. That is what keeps the engine independent of any windowing or graphics library.

---

## Ghostful Mode

LucyN has no true headless mode. Instead it has **Ghostful Mode**.

In Ghostful Mode, LucyN creates a real SDL window, renders properly, and runs JS like a normal browser, but the window is hidden. A ghost.

Headless checks for a real display, real screen dimensions, and a real window all pass, because the window *is* real. It just has the curtains drawn.

```cpp
engine.setVisible(true);    // normal mode
engine.setVisible(false);   // ghostful mode: still renders, still real
```

**Honest note:** a hidden window changes some browser behavior (`document.visibilityState`, `requestAnimationFrame` throttling, focus and blur events). LucyN fakes these consistently in the JS layer so the curtains don't become the giveaway. This is tested against public detection pages (see [Testing](#testing)), not assumed.

---

## Moonbug Protocol

Moonbug sits between libcurl and the engine. Every request libcurl makes passes through Moonbug first. It captures method, URL, headers, body, response, status, and timing, logs it, then passes it to the engine untouched.

Because LucyN *is* the HTTP client, it sees decrypted traffic natively. No MITM certificates, no proxy, no CDP.

```
libcurl makes request
        ↓
Moonbug captures silently
        ↓
LucyN receives response
        ↓
Renders, runs JS, continues
```

Moonbug is not CDP. Nothing about it is reachable from inside a page's JavaScript. It is accessed only through the engine API or the control channel.

```cpp
engine.moonbug.activate(2005);                   // raw capture stream on 127.0.0.1:2005
engine.moonbug.alsouse.nullbyte(2006);           // NULLBYTE-signed stream on 127.0.0.1:2006
engine.moonbug.expose("home_dashboard.html");    // serve the capture dashboard
```

| Port | Stream |
|---|---|
| 2005 | Raw Moonbug capture stream |
| 2006 | Same stream, every packet NULLBYTE signed and verified |

**All Moonbug and TinyElysia listeners bind to `127.0.0.1` only.** They are local tooling ports, not exposed to the network, and they are never reachable from page JavaScript.

**As built (M5).** Start them with `lucyn --moonbug` (2005 raw, 2006 signed, 6969 dashboard), per port with `--raw N --signed N --dashboard N`, or from the client with `new Lucyn({ moonbug: true })`. In C++ the dot-notation sketch above is `server::CaptureServer` (`activate`, `alsoUseNullbyte`, `serveDashboard`, `expose`) fed by `net::Moonbug` (`onRequest` / `onResponse` / `onCapture`); see `modules/server/README.md`. Because browsers do not apply CORS to WebSockets, every local listener also refuses any request whose `Host` or `Origin` is not loopback: otherwise any web page open in your normal browser could read the wiretap.

---

## NULLBYTE Protocol

NULLBYTE is LucyN's internal integrity protocol. It sits on top of Moonbug and answers one question: *was this traffic captured cleanly, and has it been touched since?*

```
Intercept decrypted HTTP traffic
        ↓
Stamp it with a signature only LucyN can generate
        ↓
Store it. Verify it. Prove it was untampered.
```

NULLBYTE uses **HMAC-SHA256** underneath, the same way HTTPS uses AES underneath but is still its own thing.

```cpp
auto sig = engine.nullbyte().sign(data);
bool ok  = engine.nullbyte().verify(data, sig);
```

**What NULLBYTE does and does not prove:** it proves captured data has not been altered *after* capture by anyone without the key. It does not protect against a compromised engine process, since the key lives in that process. It is an integrity seal, not a sandbox.

---

## TinyElysia

TinyElysia is LucyN's internal server framework, built on Mongoose (HTTP + WebSocket) and running entirely in C/C++. The API design is inspired by Elysia.js, with zero Bun, zero Node, and zero npm.

```cpp
engine.tinyelysia().route("/live", handler);
engine.tinyelysia().ws("/stream", handler);
engine.tinyelysia().listen(6969);     // 127.0.0.1:6969
```

It serves the Moonbug capture dashboard (`home_dashboard.html`), which receives a real-time WebSocket stream of every captured request as pretty-printed JSON. Raw HTML and a WebSocket. No framework.

```
Website traffic → Moonbug (captures) → NULLBYTE (signs) → TinyElysia (serves) → Dashboard
```

---

## Identity

LucyN generates a convincing browser identity and holds it for the whole session. Every session looks like a real Chrome user on a real machine. Identities are saved as **profiles**, so the same "person" can come back tomorrow.

Generated consistently, never randomly per request:

- `navigator.userAgent`
- `navigator.webdriver` (always `false`)
- Screen dimensions and color depth
- GPU vendor and renderer strings
- Font list
- Canvas fingerprint
- Language, timezone, and platform values that agree with each other
- TLS and HTTP/2 fingerprint (via curl-impersonate)
- HTTP header order matching Chrome

**The rule is consistency.** A believable human is one whose details all agree. A mismatch (Windows UA with Linux fonts) is worse than a boring default.

---

## Built-in Ad Blocker

The engine applies the `modules/adblock/` network filter to top-level navigation, page fetch/XHR and script, image, style, font, media, and download requests. A small tracker rule set is enabled by default; pass a local EasyList/uBO-format file to add rules. Moonbug records blocked attempts with their matching rule, and blocked requests are not sent to libcurl.

```sh
lucyn --adblock-list ./filters.txt   # add rules for this run
lucyn --no-adblock                  # disable filtering
```

The C++ API exposes the same controls through `ControllerOptions::adblockEnabled` and `adblockFilterListPath`. Filter parsing support and current limitations are documented in [modules/adblock/DOCS.md](modules/adblock/DOCS.md).

---

## Developer API (C++)

LucyN is designed to be embedded. The API uses dot notation throughout: readable by humans, readable by LLMs, readable by anyone.

### Core

```cpp
#include <lucyn/lucyn.h>

LucyN::App app;
LucyN::Engine engine;

engine.createwindow(1280, 720);
engine.setVisible(true);
engine.load("https://example.com");
engine.reload();
engine.stop();
```

### FunAPI: Rendering Controls

```cpp
engine.funapi.noloadcss();        // strip all CSS, raw HTML only
engine.funapi.noloadimages();     // skip all images
engine.funapi.noloadfonts();      // skip custom fonts
engine.funapi.noloadvideos();     // skip video embeds
// Adblock is enabled by default; use `lucyn --no-adblock` to disable it,
// or `lucyn --adblock-list ./filters.txt` to load more rules.
```

### FunAPI: Mode

```cpp
engine.switch.mode(LucyN::Mode::PC);      // desktop viewport and UA
engine.switch.mode(LucyN::Mode::Mobile);  // mobile viewport and UA
```

### FunAPI: Window and Controls

```cpp
engine.createwindow(1280, 720);
engine.setVisible(false);                          // ghostful mode
engine.allowrender("https://example.com");         // whitelist a URL
engine.providecustomcontrols(true);                // tabs, back, basic UI
engine.hometab.uselocalhtml("mybrowser.html");     // custom new tab page
engine.nointernet.uselocalhtml("offline.html");    // custom offline page
```

### Moonbug and NULLBYTE

```cpp
engine.moonbug.activate(2005);
engine.moonbug.alsouse.nullbyte(2006);
engine.moonbug.expose("home_dashboard.html");

engine.moonbug().onRequest([](auto req)  { /* ... */ });
engine.moonbug().onResponse([](auto res) { /* ... */ });
engine.moonbug().onCapture([](auto cap)  { /* every request lands here */ });

engine.nullbyte().sign(data);
engine.nullbyte().verify(data, signature);
```

### CMake Integration

```cmake
find_package(LucyN REQUIRED)
target_link_libraries(mybrowser LucyN::Engine)
```

---

## JS Client Library

Most people should never touch C++. LucyN ships with a JS client (`nothing-browser`) that drives the engine the way Puppeteer or Playwright drive Chromium, except the thing underneath was *built* to be driven.

```js
import Lucyn from "nothing-browser";

const a = new Lucyn({
  profile: "default",                  // persistent identity
  headers: { "Accept-Language": "en-US,en;q=0.9" },
  cookies: true,                       // persist cookies between runs
  ghostful: true                       // window hidden, still real
});

await a.visit("https://example.com");

const title  = await a.extract("title");        // CSS selector
const prices = await a.extractAll(".price");    // array of results
const html   = await a.html();

a.moonbug.onRequest(req => console.log(req.url));   // the wiretap, from JS

await a.waitFor(".results");
await a.waitIdle();                                  // no network activity for N ms
const value = await a.eval("document.title");        // escape hatch

await a.close();
```

Design rules for the client:

- **Everything is async**, because every call crosses a process boundary.
- **Config goes in the constructor**, because identity must be fixed before the first request.
- **Flat methods**, no hidden "am I inside a visit?" state. The current page is implicit.
- **Sessions are isolated.** Two `new Lucyn()` instances get separate cookies and identities.
- **Crash safe.** If the engine process dies, pending promises reject with a clear error instead of hanging.

### Control channel

The client talks to the engine with **newline-delimited JSON over stdio** (or a local Unix socket / named pipe). There is no TCP port involved, and nothing the page's JavaScript can see.

```json
{"v": 1, "id": 1, "cmd": "visit", "url": "https://example.com"}
{"id": 1, "ok": true, "status": 200}

{"id": 2, "cmd": "extract", "selector": "title"}
{"id": 2, "ok": true, "value": "Example Domain"}

{"event": "request", "url": "...", "method": "GET", "sig": "a91f..."}
```

The protocol is versioned (`"v": 1`) from day one. Events (Moonbug captures) stream back unprompted; replies are matched to calls by `id`.

### Downloads  *(built, Session 10 addendum; a human-requested upgrade)*

One command saves a URL straight into `nothing-data/downloads/`, using the same identity, cookie jar and Moonbug capture as everything else:

```bash
./bin/lucyn --download https://example.com/file.pdf          # prints the saved path, exit code 0 or 1
./bin/lucyn --download https://example.com/file.pdf --name my.pdf
```
```json
{"v": 1, "id": 7, "cmd": "download", "url": "https://example.com/file.pdf"}
{"id": 7, "ok": true, "path": ".../nothing-data/downloads/file.pdf", "bytes": 10340250, "status": 200, "type": "application/pdf"}
```
```js
const r = await a.download("https://example.com/file.pdf");   // r.path, r.bytes
```
Streams to disk (a big file never sits in RAM), writes a hidden `.part` file and renames it only after a complete 2xx transfer, never overwrites unless `--overwrite`, and always keeps the file name inside `downloads/`. Details and limits: `SESSION10.md` (addendum).

---

## The Shell, Script Injection and Snapshots  *(built; the JS side is the `js` module)*

Every site opens inside a **shell page**, the way a Vite or Next app boots from an almost empty `index.html`:

```html
<script src="./inject.js"></script>      <!-- the user's scripts: 20 on one page is fine -->
<script src="./hydrator.js"></script>    <!-- loads the site into this page -->
```

LucyN owns the shell, so injecting JS is trivial. Each user script says when it runs:

```js
// ==LucyScript==
// @name   hide-banner
// @run-at start        // start = before the site's own scripts, end = after the page settled
// @match  https://*.example.com/*
// ==/LucyScript==
```

**Before / after.** Every run saves the raw page as the server sent it (`before.html`) and the live page once the scripts ran (`after.html`), plus a diff of which tags are new or gone.
A script can take extra snapshots at any moment. `lucyn-dom --diff before.html after.html` compares any two pages.

**Where data lives.** By default and always in `nothing-data/` next to the running binary: identities, snapshots, scripts, the shell files, downloads, screenshots (planned). Nothing in `~/.config`. Delete the folder and the browser forgets everything.

Details: `modules/shell/README.md`, `modules/core/README.md`.

---

## Rendering Pipeline

```
HTML string (from libcurl via Moonbug)
        ↓
Gumbo parses it → DOM tree
        ↓
CSS parser → attaches styles (layout-relevant properties only)
        ↓
Layout engine → box positions
        ↓
Display list (draw rect / draw text / draw image)
        ↓
Painter backend draws it to the window (or an offscreen buffer)
        ↓
QuickJS runs → JS manipulates the DOM
        ↓
JS changes something → repeat from style/layout
```

### The Painter interface

```cpp
struct DrawRect  { float x, y, w, h; };
struct DrawText  { float x, y; std::string text; float size; };
struct DrawImage { float x, y, w, h; int imageId; };

using DisplayList = std::vector<std::variant<DrawRect, DrawText, DrawImage>>;

struct Painter {
    virtual void draw(const DisplayList&) = 0;
    virtual ~Painter() = default;
};
```

`SoftwarePainter` (pixel buffer + stb_truetype, shown through an SDL window) ships first. A `SkiaPainter` can implement the same interface later without anything else in the engine noticing.

### CSS: what LucyN parses

LucyN only parses CSS that affects **functionality**, not beauty.

**Parsed:** `display` (block / inline / none / flex), `width`, `height`, `position` (relative / absolute), `visibility`, `z-index`, `overflow`, margin, padding, border widths.

**Ignored:** `border-radius`, `box-shadow`, gradients, transitions, animations, custom fonts, colors (beyond what scraping needs).

### Layout

Every HTML element is a box with four layers: margin, border, padding, content. The layout engine answers one question for every box: *where does it go and how big is it?*

```cpp
struct Box {
    float x, y;
    float width, height;
    float margin, padding, border;
    std::vector<Box*> children;
    Box* parent;
    void layout();
};
```

- **Block layout:** boxes stack vertically.
- **Inline layout:** boxes sit horizontally and wrap.
- **Flexbox (basic):** planned early, because most modern pages depend on it.
- **Grid, floats, tables:** stretch goals.
- **Geometry APIs** (`getBoundingClientRect`, `offsetWidth`, etc.) return real values where layout exists and plausible, consistent values elsewhere.

---

## QuickJS and the DOM Bridge

Modern sites send almost no HTML. They send a single `<div>` and build the entire page in JS. That means **QuickJS and the DOM bridge matter more than layout** for real-world scraping, and the bridge is expected to be the largest part of the project.

Every time page JS touches the browser (`document`, `window`, `navigator`, `canvas`, `localStorage`, `fetch`, timers), it calls an API LucyN implements in C++ and exposes to QuickJS.

**Priority order for the bridge:**

1. `document`, `querySelector`, `querySelectorAll`, node traversal, `textContent`, attributes
2. `window`, `navigator`, `location`, `history`
3. Timers (`setTimeout`, `setInterval`), `Promise`, microtasks
4. `fetch`, `Headers`, `Request`/`Response`, `AbortController`, `FormData`, `XMLHttpRequest` (all routed through the one network layer: any method and body, shared cookie jar, browser-correct subresource headers; no streams/Blob/CORS enforcement)
5. `localStorage`, `sessionStorage` (basic per-origin storage exists), `document.cookie` (shared in-memory jar, HttpOnly respected; not persisted yet)
6. Events (`addEventListener`, dispatch, `isTrusted` handling)
7. Canvas, WebGL, AudioContext, and similar fingerprinting surfaces (stubbed consistently)

**QuickJS is not V8.** Error messages, stack traces, and `Function.prototype.toString()` output differ in small ways that anti-bot scripts can notice. The bridge wraps errors and natives so they look Chrome-shaped. This is a known, ongoing chore.

---

## Project Layout

LucyN is a set of **modules**. Each lives in `modules/<name>/`, has its own `CMakeLists.txt`, and builds and tests on its own. The root `CMakeLists.txt` only wires them together.

```
lucyn/
├── CMakeLists.txt          # root: one option + one add_subdirectory per module
├── modules/
│   ├── core/               # where data lives: <binary>/nothing-data (no dependencies)   [built]
│   ├── profile/            # identity + browser switching                                [built]
│   ├── net/                # libcurl wrapper, follows the profile module; Moonbug capture [built]
│   ├── dom/                # Gumbo -> mutable DOM tree, selectors, before/after diff      [built]
│   ├── shell/              # shell page, script injection (@run-at), snapshots, hooks    [built, JS seam ready]
│   ├── js/                 # QuickJS setup + DOM bridge: implements shell's ScriptHost    [built, v0: see modules/js/README.md]
│   ├── security/           # NULLBYTE (HMAC-SHA256)                                      [built]
│   ├── layout/             # box tree, block/inline (flex/grid/floats/tables: Session 12+) [built, Session 11]
│   ├── paint/              # display list, Painter interface, SoftwarePainter            [built, Session 11]
│   ├── server/             # TinyElysia + Moonbug capture server + dashboard             [built]
│   ├── ui/                 # always reports "I am providing a UI"; SDL2 window            [built, Session 11; optional: needs SDL2]
│   └── controller/         # JSON command loop (M4); today, not yet the UI switch         [built]
├── third_party/            # quickjs, gumbo, mongoose, stb (Session 11), fonts (Session 11), (imgui)
├── client/                 # nothing-browser JS library
└── dashboard/              # home_dashboard.html
```

Built tools all land in `build/bin/` (so they share one `build/bin/nothing-data`).

Every module has the same shape: `include/lucyn/<name>/`, `src/`, `tools/` (a small CLI to poke it), `tests/`.

System dependencies: **libcurl** (curl-impersonate build), **Gumbo**, and optional **SDL2** (see `modules/ui/README.md`). Other dependencies are vendored source or separately supplied bundles.

## Licensing

LucyN-authored code and documentation are MIT-licensed; clone the repository, modify it, and build your own applications subject to the included license. Third-party components keep their own terms. In particular, the default application links Mongoose, which is GPL-2.0-only or commercially licensed. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) before redistributing a build.

---

## Roadmap

| Milestone | Goal | Status |
|---|---|---|
| **M1** | libcurl fetch → Moonbug capture → signed JSON log → Gumbo title print. Console only, no window. | Planned |
| **M2** | Swap in curl-impersonate; verify TLS/HTTP2 fingerprint against public checkers | Planned |
| **M3** | QuickJS + minimal DOM (`document`, `window`, `fetch`, timers). Scrape JS-rendered pages with no pixels. | DOM, shell/injection and QuickJS bridge built; real fetch/XHR/cookies (M7), classic `<script src>`, per-origin local/session storage, CacheStorage subset, limited Web Crypto and IntersectionObserver exist. streams/Blob, service workers and layout remain |
| **M4** | JSON command loop + `nothing-browser` JS client (`visit`, `extract`, `html`, `eval`) | `modules/controller/` + `apps/lucyn/` + `client/` built and tested. `eval` is a stub (JS context torn down after settle); `waitFor` deferred to M5+. |
| **M5** | Moonbug capture hooks + NULLBYTE + TinyElysia + capture dashboard | `net` (Moonbug), `security` (NULLBYTE), `server` (TinyElysia, dashboard) built and tested; wired into `lucyn` and the JS client. See `SESSION6.md` |
| **M6** | SDL window, SoftwarePainter, Ghostful Mode | Built and tested, Session 11 (`modules/paint`, `modules/ui`; see `SESSION11.md`). `screenshot`/`window` controller commands, `document.hidden`/`visibilityState`/focus/blur all consistent with real window state. |
| **M7** | CSS parsing, block/inline layout, basic flexbox | Block/inline box tree built, Session 11 (`modules/layout`; UA default rules by tag, no CSS parsing yet — see `SESSION11.md`'s ladder). Inline `style=""`, `<style>` blocks, flexbox: **NEXT: Session 12**, see `SESSION12_PLAN.md` |
| **M8** | Identity profiles, ad blocker, cookie persistence | Planned |
| **M9** | Fingerprint hardening, expanded API coverage, process isolation | Planned |

A useful v0.4 exists **before a single pixel is drawn.**

> **Numbering note.** The session notes use their own M-labels: `SESSION5` = M4 (controller), `SESSION6` = M5 (Moonbug),
> `SESSION7` = M6 (eval/waitFor), `SESSION8` = M7 (cookies, fetch/XHR), `SESSION9` = M8 (WebSocket), `SESSION10` = M9 (Blob/File),
> `SESSION11` = M10 (painter, layout, UI — see `SESSION11.md`).
> The table above is the original plan, so its M6/M7 rows (rendering) are what **Session 11** built. Next: CSS (`style=""`,
> `<style>` blocks), flexbox, and whatever `SESSION12_PLAN.md` decides about the LNM language — see that file.

---

## Testing

Tests drive the feature list, not the other way around. LucyN is checked against public detection and fingerprint pages, including:

- TLS/JA3/JA4 checkers (e.g. tls.peet.ws)
- bot.sannysoft.com
- CreepJS
- BrowserLeaks

A failing check becomes a ticket. "Stealth" is a measured result, not a claim.

---

## Known Limitations

Being honest up front:

- **No JIT.** QuickJS is an interpreter. Heavy single-page apps run slower than in Chrome.
- **Small engines are easy to single out.** Chrome hides users in a huge crowd; LucyN is a crowd of one until its fingerprint matches Chrome closely enough. Consistency is the defense.
- **Partial web platform.** Only the APIs needed to run and scrape most sites are implemented. Missing APIs are added when real sites need them.
- **Single process.** Page JS runs in the engine process. Process isolation is on the roadmap before LucyN is used near sensitive credentials.
- **Text is basic.** One embedded font, ASCII only this session; complex scripts, bidirectional text, and advanced shaping come later (HarfBuzz).
- **Animations, transitions, and pretty effects will not render.** By design.
- **No CSS yet.** `modules/layout` (Session 11) uses UA default rules by tag only — no `style=""`, no `<style>` blocks, no flexbox/grid/floats/tables. Pages lay out block-and-inline, the way a 1998 browser with no stylesheet would.
- **No `requestAnimationFrame`.** Not implemented at all yet, so nothing to leak through Ghostful Mode either — added when something needs it.

---

## Warning

Do not use LucyN if you want a normal browser engine. It presents a constructed identity to websites. It captures everything. It has no interest in web standards compliance beyond what is needed to run JS and render pages well enough to scrape them. It will not pass the Acid test. It does not care.

**Use it responsibly.** Scraping and automation legality, and website terms of service, vary by site and jurisdiction. Research, price monitoring, archiving, and working with your own data are common legitimate uses. You are responsible for how you use it.

If that is what you need, welcome.

---

*Named after LucyN. She knows why.*
