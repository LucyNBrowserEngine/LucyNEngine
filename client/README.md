# @lucyn/browser

JS client for the LucyN headless engine. Spawns `lucyn` as a child process and
drives it over the M4 JSON command protocol.

## Install

```bash
npx jsr add @lucyn/browser
```

**This package lives on JSR, not npm.** Its home is [jsr.io/@lucyn/browser](https://jsr.io/@lucyn/browser).
`npm i @lucyn/browser` does **not** work — npm cannot fetch it directly. `npx jsr add @lucyn/browser`
is the correct install command: it downloads from JSR and handles the npm-registry compatibility for
you automatically, so your `import` resolves like any other package.

**Why JSR and not npm?** You might wonder why this isn't just `npm i`. Honest answer: publishing to
npm wanted about three tokens, and we only had one — figuring out how to get the other two was a bit
of a chore. JSR was the next and nearest easy one, so that's where the package lives. So: don't use
`npm i`, use `npx jsr add`.

```bash
npx jsr add @lucyn/browser    # correct
npm i @lucyn/browser          # does not work — the package is not on npm
```

Working inside this repo instead?

```bash
# from the monorepo root after building LucyN — a repo-local dev convenience, not the install path
cd client && npm link    # or just: import Lucyn from "./client/src/index.ts"
```

The client looks for the `lucyn` binary in this order:
1. `LUCYN_BIN` env var
2. `../../build/bin/lucyn` (monorepo default after `cmake --build build`)
3. `lucyn` on `PATH`

## Usage

```js
import Lucyn from "@lucyn/browser";

const a = new Lucyn({
  settleTimeoutMs: 5000,      // JS idle timeout per visit
  console: false,             // echo page console.* to stderr
  adblock: true,              // built-in request filtering (default: true)
  headless: false,            // false (default) opens a window that follows the script
  // adblockList: "./filters.txt",   // optional extra EasyList/uBO rules (must exist)
  // config: "./engine.json",        // optional engine config (must exist)
});

// Navigate
const { status, url, title } = await a.visit("https://example.com");
console.log(status, title);   // 200 "Example Domain"

// Direct navigation and free web search use the same visit pipeline.
await a.open("https://example.org");
await a.search("LucyN browser", { engine: "brave" });

// Query the settled DOM
const first = await a.extract("p");           // first match, text content
const links  = await a.extractAll("a");       // all matches
const html   = await a.html();                // full settled HTML

// Engine info (rename `url` — the destructured name above is already taken)
const { url: currentUrl, captures } = await a.info();

// Events (page errors, notes, future Moonbug captures)
a.on("pageError", (e) => console.error("page error:", e.message));
a.on("note",      (e) => console.log("note:", e.message));

// Always close when done
await a.close();
```

Your own `package.json` needs `"type": "module"` (this package is ESM) — otherwise Node prints
`[MODULE_TYPELESS_PACKAGE_JSON]` and re-parses your file as a module on every run.

## Commands

| Command | Args | Returns |
|---|---|---|
| `visit(url)` | `url` | `{status, url, title}` |
| `open(url, [opts])` | same as `visit` | direct website navigation |
| `search(query, [opts])` | optional `opts.engine` (defaults to `search.default`) | loads provider results page; built-ins: Brave, SearXNG, Google, DuckDuckGo, Bing, Ecosia, Startpage, Mojeek, Qwant, Swisscows |
| `html()` | none | settled HTML string |
| `extract(selector)` | CSS selector | text content of first match |
| `extractAll(selector)` | CSS selector | `string[]` of text contents |
| `eval(js)` | JS expression | the result, evaluated in the live page context (M6) |
| `waitFor(selector, [opts])` | CSS selector, `opts.timeout` | resolves when the element exists (M6) |
| `waitIdle([opts])` | `opts.quiet`, `opts.timeout` | resolves when no request has been running or finishing for `quiet` ms (M6; M7 counts requests still in flight) |
| `click(selector)` | CSS selector (quotes are fine) | the clicked element's tag name (M6) |
| `cookies([url])` | optional URL | the jar, or what a request to `url` would carry; HttpOnly flagged (M7) |
| `setCookie(cookie)` | `{name, value, domain \| url, path?, secure?, httpOnly?, expires?}` | restore a session (M7) |
| `clearCookies()` | none | empties the jar (M7) |
| `download(url, [opts])` | `url` (http/https), `opts.name`, `opts.overwrite`, `opts.maxBytes`, `opts.referer` | `{path, name, bytes, status, type, url}`: saved into `nothing-data/downloads/`; rejects on a non-2xx / error and leaves no file (M9 addendum) |
| `screenshot([name], [opts])` | optional file name, `opts.width` | absolute path of the saved PNG in `nothing-data/screenshots/`; rendered offscreen by Chromium — it works with or without the window |
| `info()` | none | `{url, captures}` |
| `close()` | none | none |

`visit()` returns once the page's own scripts and timers have run; a `fetch()`/XHR the page started on load may
still be in flight. Call `waitIdle()` (or `waitFor()`) before reading the DOM of a page that loads data that way.
Adblock can be disabled with `adblock: false`; an optional local rule file is passed with `adblockList`.

## Moonbug (M5): the wiretap, from JS

```js
const a = new Lucyn({ moonbug: { dashboard: 6969 } });   // or true = raw 2005 + signed 2006 + dashboard 6969
a.moonbug.onRequest((r) => console.log(r.status, r.url, r.sig));   // every request the ENGINE makes, incl. page scripts' fetch()
await a.visit("https://example.com");
const ports = await a.moonbug.serve({ dashboard: 0 });             // start more listeners later; 0 = any free port
const last  = await a.moonbug.captures(10);                        // headers (wire order), timing, body preview
```

| Method | Returns |
|---|---|
| `moonbug.onRequest(fn)` / `onResponse(fn)` | unsubscribe function |
| `moonbug.serve({raw, signed, dashboard, file})` | `{raw, signed, dashboard}` ports actually bound |
| `moonbug.captures(limit=20)` | array of capture objects |

All listeners bind 127.0.0.1 only and refuse non-loopback `Host`/`Origin`. `sig` is NULLBYTE's HMAC-SHA256 seal of the full
capture; the same `id` + `sig` appear on the signed stream and in the dashboard, where "Verify seal" checks it.

## Events

| Event | Payload |
|---|---|
| `pageError` | `{event:"pageError", message}` |
| `note` | `{event:"note", message}` |
| `request` | `{event:"request", id, method, url, status, ms, bytes, error, sig}` (M5, one per completed request) |
| `moonbug` | `{event:"moonbug", raw, signed, dashboard}` listeners are up (M5; `0` = that listener is off) |
| `event` | any unprompted JSON event (catch-all) |
| `raw` | unparseable stdout line (debugging) |
| `exit` | `(code, signal)` — unexpected process exit |
| `error` | `Error` — process spawn/IO error |

## Protocol

The client speaks **newline-delimited JSON over stdio** to the `lucyn` process.
No TCP ports, no CDP, nothing visible to the page's JS.

```
-> {"v":1,"id":1,"cmd":"visit","url":"https://example.com"}
<- {"id":1,"ok":true,"status":200,"url":"https://example.com","title":"Example Domain"}

-> {"v":1,"id":2,"cmd":"extract","selector":"h1"}
<- {"id":2,"ok":true,"value":"Example Domain"}

<- {"event":"note","message":"<script src> not loaded: ..."}   ← unprompted
```
