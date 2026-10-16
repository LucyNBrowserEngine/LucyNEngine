# nothing-browser

JS client for the LucyN headless engine. Spawns `lucyn` as a child process and
drives it over the M4 JSON command protocol.

## Install

```bash
# From the monorepo root after building LucyN:
cd client
npm link   # or just: import Lucyn from "./client/src/index.js"
```

The client looks for the `lucyn` binary in this order:
1. `LUCYN_BIN` env var
2. `../../build/bin/lucyn` (monorepo default after `cmake --build build`)
3. `lucyn` on `PATH`

## Usage

```js
import Lucyn from "nothing-browser";

const a = new Lucyn({
  profile: "chrome",          // browser identity (default: "chrome" -> chrome146)
  settleTimeoutMs: 5000,      // JS idle timeout per visit
  console: false,             // echo page console.* to stderr
  adblock: true,               // built-in request filtering (default: true)
  adblockList: "./filters.txt", // optional additional EasyList/uBO rules
  config: "./engine.json",     // optional engine config (search defaults and LNM entry)
  show: true,                   // open the SDL window visibly at startup
  ghostful: false,            // create a real (hidden) SDL window at startup; see README.md's Ghostful Mode (Session 11)
});

// Navigate
const { status, url, title } = await a.visit("https://example.com");
console.log(status, title);   // 200 "Example Domain"

// Direct navigation and free web search use the same visit pipeline.
await a.open("https://example.org");
await a.search("LucyN browser", { engine: "brave" });

// Query the settled DOM
const heading = await a.extract("h1");          // first match, text content
const links   = await a.extractAll("a");        // all matches
const html    = await a.html();                  // full settled HTML

// Switch identity
await a.profile("safari184");

// Engine info
const { profile, impersonating } = await a.info();

// Events (page errors, notes, future Moonbug captures)
a.on("pageError", (e) => console.error("page error:", e.message));
a.on("note",      (e) => console.log("note:", e.message));

// Always close when done
await a.close();
```

## Commands

| Command | Args | Returns |
|---|---|---|
| `visit(url, [opts])` | `url`, optional `opts.profile` | `{status, url, title}` |
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
| `screenshot([name], [opts])` | optional file name, `opts.width` | absolute path of the saved PNG in `nothing-data/screenshots/`; renders offscreen (layout + SoftwarePainter), no SDL/display needed (Session 11) |
| `setWindow([opts])` | `opts.visible`, `opts.width`, `opts.height` | `{visible, width, height}`; rejects with a clear message if the engine was built without SDL2 (Session 11) |
| `setVisible(visible)` | boolean | convenience for `setWindow({visible})` — matches the `engine.setVisible(false)` examples above (Session 11) |
| `profile(name)` | profile target | resolved profile name |
| `info()` | none | `{profile, url, impersonating}` |
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
