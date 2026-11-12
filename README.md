# LucyN

**A Linux browser for reverse-engineering websites.**

LucyN has shrunk its serve options on purpose. It started as a scraping engine — a full
automation surface with fill/submit/click and a command for everything — and it kept growing away
from what actually matters: **seeing what a website does.** The mission is now narrow and sharp:
you browse normally in a real Chromium window (or drive it with JSON/JS when a script is
quicker), and LucyN records the wire underneath — every request, every response, every body,
every file the site loads — then saves and decodes it for you to study. Controlling the browser is
a means; understanding the site is the point.

There is no CDP, no remote debugging port and no automation flag — nothing on the wire for a page
to detect. Chromium does the browsing (HTTP, TLS, JavaScript, cookies, rendering); LucyN does the
watching.

```json
{"v":1,"id":1,"cmd":"visit","url":"https://example.com"}
{"id":1,"ok":true,"status":200,"url":"https://example.com","title":"Example Domain"}
```

- **See everything.** Every capture carries the request headers in wire order, the response
  headers, the timing — **and the response body**: documents, scripts, stylesheets, JSON, error
  pages, not just what `fetch()` happened to read.
- **Keep everything.** Every `.js`/`.html`/`.txt`/… file the site serves is saved verbatim into
  `nothing-data/artifacts/` while you browse.
- **Decode the scripts.** Every saved script automatically gets a best-effort **deobfuscation**
  pass — two views of each file, raw and decoded.
- **Chromium underneath.** Real navigation, real cookies, real headers — no reimplementation to
  leak through, nothing to fake, nothing to detect.

## See everything

Moonbug — the wiretap built into the engine — now sees the **body** of every text/code response,
not only the headers. Chromium does not hand response bytes to the browser process on its own, so
LucyN installs a `CefResponseFilter` on the response stream: the bytes pass through to the page
untouched while a copy lands in the capture (capped at 4 MiB per response).

- **Every resource**: main document, scripts, stylesheets, JSON/XHR, text files — everything a
  header-only tool would never show you.
- The page's own `fetch()`/`XHR` reads are cross-checked by a renderer-side tap, so an API
  response cannot slip through on either path.
- **Binary bodies** (images, fonts, media) are deliberately not buffered: they appear with
  headers, status and byte count; use `html`/`screenshot` for documents and `download` to save a
  file on demand.
- The dashboard (<http://127.0.0.1:6969/>) and both live streams carry the body; the detail panel
  renders it with its size.

## Everything the site loaded, on disk

As you browse, every text/code asset the site serves is written to
**`nothing-data/artifacts/`** next to the binary, named `host__file.ext` — collisions get `~1`,
`~2`, so nothing is ever overwritten:

```text
nothing-data/artifacts/
  example.com__index.html          # raw view: exactly as the server sent it
  example.com__app.js              # raw view
  example.com__app.deobf.js        # second view: deobfuscated
  example.com__style.css
  example.com__data.json           # extensionless API responses get an extension from Content-Type
```

Every **script** gets the second view automatically: a deobfuscation pass writes `*.deobf.js`
next to the raw file. It re-indents the code and attempts, in order:

1. decoding `\xNN` / `\uNNNN` / octal escapes to literal characters,
2. finding `var _0x1234 = ['..','..']` string arrays, following the classic
   `push(shift())` rotation IIFE, and replacing constant index reads (and their `f(a,b)` wrapper
   functions) with the actual strings,
3. folding `"a"+"b"` concatenations and inlining constant `atob(...)` / `unescape(...)` /
   `decodeURIComponent(...)` calls.

It **never executes the script** — text in, text out. It is a heuristic: anything it cannot
decode with confidence (a mutated array, a non-constant index, a rotation shape it does not know)
is left exactly as written, because a wrong string is worse than no string. A plain script comes
back essentially unchanged — just re-indented. The unit tests assert both directions.

Each save is announced in the log:

```text
[INFO] ℹ️ 💾 saved artifacts/example.com__app.js (321 bytes) from https://example.com/app.js
[INFO] ℹ️ 🔍 deobfuscated artifacts/example.com__app.js → artifacts/example.com__app.deobf.js (2 strings, 2 concats, 17 escapes)
```

Images and fonts are not written to disk (they stay headers-only in Moonbug); user-triggered
downloads still land in `nothing-data/downloads/`.

## Live Overrides (`nothing-data/overrides/`)

LucyN does not just let you study saved files; it lets you **run modified code live in the browser**.
Place any replacement file in **`nothing-data/overrides/`**:

```text
nothing-data/overrides/
  example.com__app.js              # flat artifact naming
  example.com/static/js/app.js     # or hierarchical directory mirror
```

When Chromium loads resources:
- If a matching file exists in `nothing-data/overrides/`, LucyN's C++ handler intercepts the request before it hits the network and serves your local replacement with permissive CORS headers.
- **`--override-deobf`**: Passing this flag tells LucyN to automatically serve the deobfuscated sibling (`artifacts/*__*.deobf.js`) for any script that was deobfuscated.
- **`--no-overrides`**: Disable all local file overrides.
- **JSON commands**: Tools and scripts can also manage overrides dynamically over stdin/stdout:
  - `{"cmd":"override", "url":"https://example.com/app.js", "content":"console.log(1);"}`
  - `{"cmd":"clearOverride", "url":"https://example.com/app.js"}`
  - `{"cmd":"listOverrides"}`
  - `{"cmd":"setOverrides", "enabled":true, "autoDeobf":true}`

## No new windows, ever

LucyN has exactly one window and exactly one browser. **Nothing a page does can open a second
one** — not `window.open()`, not `<a target="_blank">`, not `target="_new"`, not a middle-click,
not a script that tries all of them in a loop. Every one of those requests goes through Chromium's
popup gate (`CefLifeSpanHandler::OnBeforePopup`), which is refused before a browser or window
comes into existence; if one ever slipped through another path, the stray browser is closed on
sight. Blocked attempts are reported in the log:

```text
[INFO] ℹ️ 🚫 blocked new window: https://ads.example/popup
```

This is deliberate. It kills the most common ad and redirect-popup trick, and it also means a
genuine site's "open in a new window" link simply does nothing — LucyN is a wiretap, not a tab
zoo, and a second window would be a second surface nobody is watching. Navigate in the one window
instead: `visit`, `click`, or the listen-mode URL bar.

## WebSocket frames

Moonbug records the **handshake and the frames** of every WebSocket the page opens (`ws://` and
`wss://`) — WhatsApp Web, chat widgets, live dashboards, socket.io apps. Chromium never hands
WebSocket frames to the browser process (they are a stream, not an HTTP resource), so the engine
taps them in the renderer: the page's `WebSocket` constructor and `send()` are wrapped
passively, and every frame the page sends or receives is reported up to Moonbug. The wrapper
returns the real socket with the real prototype, so the page cannot tell the difference and no
frame is ever delayed, reordered or rewritten.

- **in** and **out** frames, text verbatim and binary as a short hex preview plus the byte count,
  capped at `--ws-max-bytes` (1 MiB default) per frame.
- The dashboard's **WebSocket** tab shows them live; `GET /api/ws?limit=N` serves the recent ones
  and the raw stream (`ws://127.0.0.1:2005/stream`) carries them as
  `{"event":"wsFrame",...}` lines next to the HTTP captures.
- `--ws-intercept` (on by default), `--ws-max-bytes <n>`, and `--ws-dir <path>` to also append
  every frame as JSONL to `<path>/ws-frames.jsonl`.

Frames are not sealed with NULLBYTE: a seal attests an HTTP request/response pair, and a frame
has neither. They are observational data like the rest of the wiretap.

One honest limit: the tap runs in the page context of every frame, so a socket a page opens in a
**Web Worker** or **Service Worker** is outside what CEF exposes to the engine and its frames are
not captured.

## Install

### Releases are source only — you build it yourself

> **Using LucyN needs you to build it on your machine.** We have no servers that can build LucyN
> for you, let alone host the builds. The reason is CEF: the Chromium Embedded Framework
> distribution is about **1.5 GB per platform** and **3+ GB unpacked**, which is too big for
> GitHub Actions and far too big for us to publish for every OS and CPU architecture.

So a **`V*.*.*`** tag (capital V) publishes exactly two files on the Releases page, and nothing is
compiled by CI:

```text
LucyN-<version>-source.zip   # the source tree at that tag — no CEF, no build output
release.json                 # version, the CEF build it expects, and the download URL
```

Capital V is deliberate: lowercase `v*.*.*` tags publish the `@lucyn/browser` package to JSR,
which accepts each version exactly once, while `V*.*.*` only cuts a source release — so a broken
release can be re-pushed as often as it needs without the registry refusing the version.

Each release states the CEF build it expects. Builds are verified against one CEF build and the
runtime next to the binary must be **that same version** (see [CEF versions](#cef-versions)).

### What version am I looking at?

`release.json` at the repo root is the machine-readable answer, and it is refreshed on `main` by
every source release. Ping it and you get the current version, the CEF build it needs, and where
to download the source:

```bash
curl -s https://raw.githubusercontent.com/LucyNBrowserEngine/LucyNEngine/main/release.json
```

```json
{
  "name": "LucyN",
  "version": "1.0.1",
  "cef_version": "156.0.7+g47dbc1d+chromium-156.0.8078.17",
  "cef_build": "3652",
  "chromium_version": "156.0.8078.17",
  "requires_local_build": true,
  "notice": "USING LUCYN REQUIRES YOU TO BUILD IT ON YOUR OWN MACHINE. …",
  "download_url": "https://github.com/LucyNBrowserEngine/LucyNEngine/releases/download/V1.0.1/LucyN-1.0.1-source.zip",
  "source_archive_url": "https://github.com/LucyNBrowserEngine/LucyNEngine/archive/refs/tags/V1.0.1.zip",
  "release_url": "https://github.com/LucyNBrowserEngine/LucyNEngine/releases/tag/V1.0.1",
  "docs_url": "https://github.com/LucyNBrowserEngine/LucyNEngine/blob/main/docs/building.md"
}
```

`download_url` is the release attachment; `source_archive_url` is GitHub's own archive of the tag
and always works, even for tags cut before source-only releases existed.

### Build it

You need a C++17 compiler, CMake ≥ 3.16, python3, and the CEF (Chromium) binary distribution —
about 1.5 GB, downloaded for you.

```bash
git clone <this repo> && cd Lucynengine

./scripts/install.sh      # detects your OS/arch, asks you to confirm, unpacks CEF into third_party/cef/

cmake -S . -B build
cmake --build build -j
```

### CEF versions

LucyN compiles against one specific CEF build, and the Chromium runtime next to the binary must
be **that same version**:

| | |
|---|---|
| **CEF** | `156.0.7+g47dbc1d+chromium-156.0.8078.17` (build number 3652) |
| **Chromium** | `156.0.8078.17` |
| platforms in the scripts | `linux64`, `macosx64`, `macosarm64`, `windows64` |

- `./scripts/install.sh` fetches that distribution from
  [cef-builds.spotifycdn.com](https://cef-builds.spotifycdn.com/), detects your OS/arch and asks
  before downloading (~1.5 GB). CI/non-interactive: `./scripts/install.sh --yes --platform linux64`.
- `./scripts/update_cef.sh --check` compares your installed CEF with the newest **stable** build
  in the CDN's index and prints both; `./scripts/update_cef.sh` upgrades in place (rebuild after).
- `./build/bin/lucyn --version` prints the engine version **and** the CEF version it was built
  against, so you can compare with `third_party/cef/include/cef_version.h`.

The binary verifies the loaded `libcef` against its build-time headers **before Chromium starts**.
A mismatch is refused with an explanatory error instead of the `stack smashing detected` crash a
half-updated runtime used to produce. The same crash appears when only `lucyn` was copied
somewhere without its `cef/` subfolder — always copy `build/bin/cef/` **together with** the
binary (or set `LUCYN_BIN` and leave the binary where it is).

## Platform support

| Platform | Status |
|----------|--------|
| Linux x64 | ✅ Supported |
| Linux arm64 | Not supported now but in the next 3rd version it is expected to be supported|
| Windows x64 | 🚧 Builds are currently erroring. We are investigating the cause; Windows is not supported for now. |
| macOS | ⏳ Planned |

The install script can fetch CEF for additional platforms, but that does not mean LucyN itself is
supported on them. To check for a newer CEF later: `./scripts/update_cef.sh --check`.

Verify the build:

```bash
ctest --test-dir build --output-on-failure     # 9 suites
```

Everything lands in `build/bin/` — the binaries and, in their own **`build/bin/cef/` subfolder**,
the whole Chromium runtime (`libcef.so`, `icudtl.dat`, `*.pak`, `locales/`). The binary's own
folder stays clean: just `lucyn`, `nothing-data/` and `adblock-filters/`.
More detail — including every CMake option — is in [docs/building.md](docs/building.md).

## The JSON command protocol

One JSON object per line in, one JSON object per line out. Every reply echoes the `id` you sent;
errors come back as `{"id":N,"ok":false,"error":"..."}`.

| Command | Arguments | Reply |
|---|---|---|
| `visit` | `url` | `{status, url, title}` |
| `search` | `query`, `engine?` | `{status, url, title}` — opens a real results page |
| `html` | — | `{value}` — fully settled HTML |
| `extract` | `selector` | `{value}` — text of the first match |
| `extractAll` | `selector` | `{values:[...]}` — every match |
| `eval` | `js` | `{value}` — evaluated in the live page |
| `waitFor` | `selector`, `timeout?` | `{value}` — polls the live DOM |
| `waitIdle` | `quiet?`, `timeout?` | `{ok}` — network quiet **and** in-flight requests done |
| `click` | `selector` | `{value}` — a real `.click()` in the page |
| `fill` | `selector`, `value` | `{tag}` — sets the value, fires `input` + `change` |
| `submit` | `selector` | `{tag}` — submits the form and follows the navigation |
| `screenshot` | `name?`, `width?` | `{path}` — Chromium's offscreen frame as PNG |
| `download` | `url`, `name?`, `overwrite?`, `maxBytes?`, `referer?` | `{path, name, bytes, status, type, url}` |
| `cookies` | `url?` | `{cookies:[...]}` — the jar, or what a request to `url` would carry |
| `setCookie` | `name`, `value`, `domain`\|`url`, ... | `{ok}` |
| `clearCookies` | — | `{ok}` |
| `info` | — | `{url, captures}` |
| `moonbug` | `raw?`, `signed?`, `dashboard?`, `on?` | `{events, raw, signed, dashboard}` |
| `captures` | `limit?` | `{total, captures:[...]}` |
| `quit` | — | `{ok}` — clean shutdown |

Run it straight from a shell:

```bash
printf '%s\n' '{"v":1,"id":1,"cmd":"visit","url":"https://example.com"}' \
              '{"v":1,"id":2,"cmd":"extract","selector":"p"}' \
              '{"v":1,"id":3,"cmd":"quit"}' | ./build/bin/lucyn --headless
```

**Headful by default.** Launch `./build/bin/lucyn` and a real Chromium window opens on
`https://nothing-browser-docs.pages.dev/` (`--open <url>` changes that) and follows every command
you send — `visit` navigates that window. It prints where it is and how to drive it:

```
[SUCCESS] ✅ engine is up and running — window opens on https://nothing-browser-docs.pages.dev/ and follows every command you send
[INFO] ℹ️ write a JS/TS file to control the browser:  npx jsr add @lucyn/browser
```

`--headless` (or `headless: true` in the JS client) skips the window for servers, CI and scripts.
Either way the page is rendered off-screen, so `screenshot` works in both modes.

## Listen mode

`lucyn --listen` is the browser you drive **with your hands** while the engine listens:

```bash
./build/bin/lucyn --listen
```

- **an empty tab with a URL bar.** The viewer window has no chrome of its own, so the engine
  injects a small dark bar into every page: back button + URL field. Type a URL, press Enter
  (bare words become a DuckDuckGo search), click anything — mouse, keyboard, wheel and focus
  are forwarded straight into Chromium.
- **Moonbug is already on**: raw stream `ws://127.0.0.1:2005/stream`, signed stream `:2006`,
  dashboard at <http://127.0.0.1:6969/>.
- **every request is logged** to your terminal through the logger module, one line each:
  `[INFO] ℹ️ captured: GET https://example.com/ -> 200 (87.4 ms)` — plus notes and page errors.
- raw JSON event lines stay **off** the terminal until a script attaches; the attached script
  then receives them on its own connection.
- the engine advertises itself (next section), so a script started afterwards **hooks into this
  window** instead of opening a second browser.

The terminal is still a protocol channel: type JSON commands straight into it and the replies
come back on stdout.

## One engine, every script hooks in

Every engine except the one-shot `--download` writes

```json
{"pid":12345,"port":43123,"host":"127.0.0.1"}
```

to `nothing-data/engine.json` next to the binary and serves the same newline-delimited JSON on
that loopback port. The JS client reads that file **before spawning anything**: an engine that is
already alive gets attached to; only when nothing is running does the client start its own.

This is the fix for the `broken pipe` / `zygote` errors: running `./lucyn` in one terminal and a
script in another used to launch two Chromiums against the same cache directory, and the loser
died mid-startup. Now:

- `./lucyn` or `./lucyn --listen` in a terminal, then any number of scripts → they all drive
  that one browser;
- a second raw `lucyn` refuses to start: `an engine is already running (pid …, port …)`;
- an attached `close()` only disconnects — the engine and its window keep running;
- opt out per session with `new Lucyn({ attach: false })` or `LUCYN_ATTACH=0` (spawn a private
  engine); a `--new-instance` engine never publishes a descriptor;
- the engine's own stdin/stdout keep working — commands from the terminal and from attached
  scripts are merged into one loop, and every reply goes back to whoever asked.

Full argument lists, reply shapes, error strings and the unprompted event lines:
**[docs/commands.md](docs/commands.md)**.

One-shot download without the JSON loop:

```bash
./build/bin/lucyn --download https://example.com/file.zip     # prints the saved path, exits 0/1
```

## Node.js client

The `client/` package (**[`@lucyn/browser`](https://jsr.io/@lucyn/browser)**, Node 18+, ESM, no
dependencies) spawns the engine and turns every command into a promise.

```bash
npx jsr add @lucyn/browser
```

> **This package lives on JSR, not npm.** Its registry entry is
> [jsr.io/@lucyn/browser](https://jsr.io/@lucyn/browser). `npm i @lucyn/browser` does **not** work
> — npm cannot fetch it directly. `npx jsr add @lucyn/browser` is the correct install command: it
> downloads from JSR and wires up the npm-compatible import for you (JSR handles the registry
> translation automatically).

### Why JSR and not npm?

You might wonder why this isn't just `npm i`. Honest answer: publishing to npm wanted about three
tokens, and we only had one — figuring out how to get the other two was a bit of a chore. JSR was
the next and nearest easy one, so that's where the package lives. Everything else works the same:

```bash
npx jsr add @lucyn/browser    # correct
npm i @lucyn/browser          # does not work — the package is not on npm
```

So: don't use `npm i`, use `npx jsr add`. It does the npm-registry compatibility for you, so your
`import` statement looks exactly like a normal package afterwards.

```js
import Lucyn from "@lucyn/browser";

const a = new Lucyn();            // opens a window that follows the script
// const a = new Lucyn({ headless: true });   // no window: servers and CI

const { status, title } = await a.visit("https://example.com");
console.log(status, title);       // 200 "Example Domain"

const first = await a.extract("p");       // example.com's first paragraph
const links  = await a.extractAll("a");   // every match
const html   = await a.html();

await a.search("LucyN browser", { engine: "brave" });
await a.waitIdle({ quiet: 300 });

const png = await a.screenshot("results.png", { width: 1280 });

await a.close();                  // always close when done
```

Your own `package.json` needs `"type": "module"` (the package is ESM), otherwise Node warns
about `[MODULE_TYPELESS_PACKAGE_JSON]`.

It finds the binary through `opts.binary`, `LUCYN_BIN`, `build/bin/lucyn`, or `PATH`. Every method,
option and event: **[docs/node-client.md](docs/node-client.md)**.

## Example: five jobs on five websites

[test/example.js](test/example.js) drives one browser through five sites:

1. **example.com** — visit + extract (the baseline),
2. **Amazon** — scrape book titles and prices off a real results page,
3. **Hacker News** — front-page headlines and scores,
4. **Wikipedia** — article title, intro paragraph, reference count,
5. **quotes.toscrape.com** — scrape, **click "Next"**, scrape page 2 and prove it changed.

```bash
node test/example.js                 # a window follows along
LUCYN_HEADLESS=1 node test/example.js    # no window
LUCYN_BIN=/path/to/lucyn node test/example.js
```

It imports `@lucyn/browser` when you have it installed and falls back to the repo's own client
otherwise (Node ≥ 22.18 imports the `.ts` directly). Each site is its own try/catch, so one
blocked site does not hide the other four, and it finishes with `N/5 jobs passed` plus the
capture count. Headless runs close the engine cleanly; a headful run leaves the window open so
you can keep clicking — Ctrl+C quits.

## Moonbug

Moonbug is the wiretap built into the engine. Because LucyN *is* the HTTP client, it sees
decrypted traffic natively — no MITM certificate, no proxy, no debug port.

```bash
./build/bin/lucyn --moonbug      # raw stream :2005, signed stream :2006, dashboard :6969
```

- **raw stream** (`ws /stream`) — one JSON capture per frame: method, URL, headers in wire order,
  timings, body preview.
- **signed stream** — the same captures wrapped in a NULLBYTE envelope:
  `{"v":1,"alg":"HMAC-SHA256","sig":"...","payload":"..."}`, verifiable at `POST /verify`.
- **dashboard** — a local page at `http://127.0.0.1:6969/` that shows the captures live and checks
  their seals.

All three bind `127.0.0.1` only. From JS:

```js
const a = new Lucyn({ moonbug: { dashboard: 6969 } });
a.moonbug.onRequest((r) => console.log(r.method, r.status, r.url, r.sig));
const recent = await a.moonbug.captures(20);
```

Everything about what a capture holds, the endpoints and the sealing: **[docs/moonbug.md](docs/moonbug.md)**.

## Repository layout

```
apps/lucyn/        the engine binary (JSON over stdio)
modules/           core, net (Moonbug), security, server, search, logger,
                   adblock, cef_host (Chromium host), controller (command loop)
client/            the @lucyn/browser Node.js package
test/              example.js — five jobs on five websites
dashboard/         capture dashboard assets
scripts/           install.sh, update_cef.sh
third_party/       cef/ (binary distro, downloaded) + mongoose (vendored)
docs/              commands, node client, moonbug, building
engine-history/    build history: SESSION*.md, HANDOFF.md, CHANGES.md, AGENTS.md, ORG_README.md
```

## Documentation

| Doc | |
|---|---|
| [docs/commands.md](docs/commands.md) | every JSON command, its arguments, reply shape and errors |
| [docs/node-client.md](docs/node-client.md) | the `@lucyn/browser` package, method by method |
| [docs/moonbug.md](docs/moonbug.md) | captures, the dashboard, NULLBYTE signing |
| [docs/building.md](docs/building.md) | prerequisites, CMake options, tests, troubleshooting |
| [CONTRIBUTING.md](CONTRIBUTING.md) | how to propose changes |
| [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) | licences of what ships with this |
| [LUCYN_ENGINE_API_SPEC.md](LUCYN_ENGINE_API_SPEC.md) | engine configuration, such as it is |

`engine-history/` is the running build log of how LucyN got here — useful context, not user
documentation.

## Licence

LucyN-authored code is MIT (see [LICENSE](LICENSE)). Note that linking `modules/server` pulls in
Mongoose, whose terms are **GPL-2.0-only or a paid commercial licence** — read
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) before you distribute a binary.
