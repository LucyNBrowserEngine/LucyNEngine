# nothing-browser — the Node.js client

`client/` holds the npm package **`nothing-browser`**: it spawns the `lucyn` binary as a child
process and drives it over the JSON command protocol (see [commands.md](commands.md)). Node 18+,
ESM, zero dependencies.

```bash
# from the repo root, after building
node -e 'import("./client/src/index.js").then(m => console.log(typeof m.default))'   # -> function
# or, as a package:
cd client && npm link        # then: import Lucyn from "nothing-browser"
```

**Finding the binary.** In order: `opts.binary` → `LUCYN_BIN` env var → `../../build/bin/lucyn`
(the monorepo default after `cmake --build build`) → `lucyn` on `PATH`.

## Quick start

```js
import Lucyn from "nothing-browser";

const a = new Lucyn();

const { status, url, title } = await a.visit("https://example.com");
console.log(status, title);                 // 200 "Example Domain"

await a.waitFor("#ready", { timeout: 10000 });
await a.fill("input[name=q]", "Ada");
await a.submit("form#search");
await a.waitIdle({ quiet: 300 });

const heading = await a.extract("h1");
const links   = await a.extractAll("a");
const html    = await a.html();

await a.close();                            // always close: it ends the engine process
```

## Constructor

```js
const a = new Lucyn({
  settleTimeoutMs: 5000,   // JS/network settle budget per visit
  console: false,          // echo page console.* to stderr
  adblock: true,           // built-in request filtering
  adblockList: "",         // extra EasyList/uBO rules file
  config: "",              // engine config JSON (search defaults)
  binary: "",              // explicit path to the lucyn binary
  moonbug: null,           // {raw, signed, dashboard} — start capture listeners with the engine
});
```

Every session is its own process with its own cookie jar.

## Commands

| Method | Args | Returns | Notes |
|---|---|---|---|
| `visit(url)` | `url` | `{status, url, title}` | waits for the page to settle |
| `open(url)` | `url` | `{status, url, title}` | alias of `visit` |
| `search(query, [opts])` | `opts.engine` | `{status, url, title}` | opens a provider's real results page (Brave by default; also SearXNG, Google, DuckDuckGo, Bing, Ecosia, Startpage, Mojeek, Qwant, Swisscows) |
| `html()` | — | `string` | fully settled HTML |
| `extract(selector)` | CSS selector | `string` | first match; **rejects** when there is none |
| `extractAll(selector)` | CSS selector | `string[]` | every match, document order |
| `eval(js)` | JS expression | result | evaluated in the live page (V8) |
| `waitFor(selector, [opts])` | `opts.timeout` (ms) | `string` (the selector) | polls the live DOM every 100 ms |
| `waitIdle([opts])` | `opts.quiet` (500), `opts.timeout` | `void` | waits for network silence **and** in-flight requests |
| `click(selector)` | CSS selector | `string` (tagName) | a real `.click()` in the page |
| `fill(selector, value)` | selector, value | `string` (tagName) | sets `value`, then fires `input` + `change` |
| `submit(selector)` | selector of a form (or a button in it) | `string` (tagName) | navigates and settles |
| `cookies([url])` | optional URL | `Promise<Cookie[]>` | the jar, or what a request to `url` would carry |
| `setCookie(cookie)` | `{name, value, domain\|url, path?, secure?, httpOnly?, hostOnly?, expires?}` | `void` | past `expires` deletes |
| `clearCookies()` | — | `void` | empties the jar |
| `download(url, [opts])` | `opts.name`, `opts.overwrite`, `opts.maxBytes`, `opts.referer` | `{path, name, bytes, status, type, url}` | saved into `nothing-data/downloads/`; **rejects** on non-2xx and leaves no file |
| `screenshot([name], [opts])` | `opts.width` | `string` (absolute PNG path) | Chromium's offscreen frame, `nothing-data/screenshots/` |
| `info()` | — | `{url, captures}` | last URL + Moonbug's capture count |
| `close()` | — | `void` | graceful `quit`, then ends the process |

Cookie shape returned by `cookies()`:
`{name, value, domain, path, hostOnly, secure, httpOnly, expires}` — `expires` in unix seconds,
`0` = session.

```js
const jar = await a.cookies("https://example.com");
await a.setCookie({ name: "sid", value: "abc", domain: "example.com", secure: true });

const { path, bytes } = await a.download("https://example.com/files/data.bin", {
  name: "data.bin", maxBytes: 5_000_000,
});

const png = await a.screenshot("page.png", { width: 1280 });
```

The client rejects with the engine's own error text (`selector not found: h1`,
`timeout waiting for selector: #ready`, `HTTP 404`, ...), so `try/catch` is enough — there is no
separate error type to inspect.

## Moonbug from JS

```js
const a = new Lucyn({ moonbug: { dashboard: 6969 } });   // or true = raw 2005 + signed 2006 + dashboard 6969
a.moonbug.onRequest((r) => console.log(r.status, r.url, r.sig));
await a.visit("https://example.com");
const ports = await a.moonbug.serve({ raw: 0, signed: 0, dashboard: 0 });   // 0 = any free port
const last  = await a.moonbug.captures(10);                                  // headers, timing, body preview
```

| Method | Returns |
|---|---|
| `moonbug.onRequest(fn)` | unsubscribe function |
| `moonbug.onResponse(fn)` | alias of `onRequest` |
| `moonbug.serve({raw, signed, dashboard, file})` | `{raw, signed, dashboard}` — ports actually bound |
| `moonbug.captures(limit=20)` | array of capture objects (1..500) |

`req` = `{id, method, url, status, ms, bytes, error, sig}`. `sig` is NULLBYTE's HMAC-SHA256 seal
of the full capture — see [moonbug.md](moonbug.md).

## Events

```js
a.on("pageError", (e) => console.error("page error:", e.message));
a.on("note",      (e) => console.log("note:", e.message));
a.on("request",   (r) => console.log(r.method, r.status, r.url));
a.on("exit",      (code, signal) => console.log("engine exited", code, signal));
```

| Event | Payload |
|---|---|
| `pageError` | `{event:"pageError", message}` |
| `note` | `{event:"note", message}` |
| `request` | `{event:"request", id, method, url, status, ms, bytes, error, sig}` |
| `moonbug` | `{event:"moonbug", raw, signed, dashboard}` — listeners are up (`0` = off) |
| `event` | catch-all for any unprompted event line |
| `raw` | an unparseable stdout line (debugging) |
| `exit` | `(code, signal)` — the engine exited on its own |
| `error` | `Error` — spawn/IO failure |

`on(type, fn)` / `off(type, fn)` come from the built-in `EventEmitter`.

## Protocol notes

Requests go out as `{"v":1,"id":N,"cmd":"...", ...}`; replies come back with the same `id` and
are matched internally, so several commands can be in flight and the promises resolve
independently. Nothing about the transport is visible to the page — no port, no CDP, no
automation flag.
