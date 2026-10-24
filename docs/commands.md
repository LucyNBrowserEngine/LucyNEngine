# LucyN command protocol

LucyN speaks **newline-delimited JSON over stdio**. You write one JSON object per line to the
engine's stdin; it writes one JSON object per line back to stdout. No TCP, no CDP, no debug port —
nothing the page can see.

```bash
./build/bin/lucyn              # JSON command loop on stdin/stdout
./build/bin/lucyn --help       # every flag
```

## Framing

**Request**

```json
{"v":1, "id":7, "cmd":"visit", "url":"https://example.com"}
```

| Field | Meaning |
|---|---|
| `v` | protocol version, currently `1` |
| `id` | any integer you choose; it comes back on the reply so you can match them (use `-1`/omit to fire and forget) |
| `cmd` | the command name |
| ...   | command-specific arguments (below) |

**Replies** (every reply echoes `id`)

```json
{"id":7, "ok":true}                          // generic success
{"id":7, "ok":true, "value":"Example"}       // single string result
{"id":7, "ok":true, "values":["a","b"]}      // array result (extractAll)
{"id":7, "ok":false, "error":"..."}          // failure
```

**Events** are unprompted lines on stdout (`{"event":...}`); they never carry an `id`. See
[Events](#events) at the bottom.

Unknown `cmd` → `{"ok":false,"error":"unknown command: <name>"}`. Unknown extra fields are ignored.

---

## Commands

### visit

Navigate and wait for the page to settle (its own scripts and timers have run).

| Arg | Type | Required | Notes |
|---|---|---|---|
| `url` | string | yes | `http://` or `https://` |

```json
→ {"v":1,"id":1,"cmd":"visit","url":"https://example.com"}
← {"id":1,"ok":true,"status":200,"url":"https://example.com","title":"Example Domain"}
```

Errors: `'url' required`, `timeout loading page` (30 s), `load failed` (with Chromium's reason).

### search

Open a search provider's normal results page, then behave exactly like `visit`.

| Arg | Type | Required | Notes |
|---|---|---|---|
| `query` | string | yes | |
| `engine` | string | no | provider id; defaults to the configured `search.default` (Brave) |

```json
→ {"v":1,"id":2,"cmd":"search","query":"LucyN browser","engine":"brave"}
← {"id":2,"ok":true,"status":200,"url":"https://search.brave.com/search?q=LucyN+browser","title":"..."}
```

### html

The fully settled HTML of the current page.

```json
→ {"v":1,"id":3,"cmd":"html"}
← {"id":3,"ok":true,"value":"<html lang=\"en\">...</html>"}
```

Errors: `no page loaded` (nothing visited yet), `built without CEF`.

### extract

Text content of the **first** element matching a CSS selector.

| Arg | Type | Required |
|---|---|---|
| `selector` | string | yes |

```json
→ {"v":1,"id":4,"cmd":"extract","selector":"h1"}
← {"id":4,"ok":true,"value":"Example Domain"}
```

Errors: `'selector' required`, `selector not found: <sel>`, `timeout extracting`.

### extractAll

Text content of **every** match, in document order.

```json
→ {"v":1,"id":5,"cmd":"extractAll","selector":"a"}
← {"id":5,"ok":true,"values":["One","Two","Three"]}
```

### eval

Evaluate JavaScript in the live page (Chromium's V8) and return the result as a string.

| Arg | Type | Required |
|---|---|---|
| `js` | string | yes (may be empty) |

```json
→ {"v":1,"id":6,"cmd":"eval","js":"document.querySelectorAll('li').length"}
← {"id":6,"ok":true,"value":"12"}
```

Errors: `eval failed` (syntax/runtime error carries the message), `timeout evaluating`.

### waitFor

Poll the live page until the selector exists.

| Arg | Type | Required | Notes |
|---|---|---|---|
| `selector` | string | yes | |
| `timeout` | number | no | ms; default = the settle timeout (`--settle`, 5000). Polls every 100 ms |

```json
→ {"v":1,"id":7,"cmd":"waitFor","selector":"#ready","timeout":10000}
← {"id":7,"ok":true,"value":"#ready"}
```

Error: `timeout waiting for selector: <sel>`.

### waitIdle

Wait until the network has been quiet.

| Arg | Type | Required | Notes |
|---|---|---|---|
| `quiet` | number | no | ms of silence required, default 500 |
| `timeout` | number | no | total ms, default = settle timeout (5000) |

The quiet window alone is not enough: a page that fires its next request right after the previous
one finished keeps the engine waiting, and requests still in flight count.

```json
→ {"v":1,"id":8,"cmd":"waitIdle","quiet":300,"timeout":5000}
← {"id":8,"ok":true}
```

Error: `timeout waiting for network idle`.

### click

Real `.click()` inside the page's JS context, then settle briefly.

```json
→ {"v":1,"id":9,"cmd":"click","selector":"button#go"}
← {"id":9,"ok":true,"value":"button"}
```

Errors: `'selector' required`, `click failed`.

### fill

Set a form field's `value` in the DOM, then dispatch bubbling `input` and `change` (in that order,
like a browser), so React/Vue-style listeners pick it up.

```json
→ {"v":1,"id":10,"cmd":"fill","selector":"input[name=q]","value":"Ada"}
← {"id":10,"ok":true,"tag":"input"}
```

Errors: `'selector' required`, `fill failed`, `timeout filling element`.

### submit

Submit a form. Pass the form itself (uses `form.submit()`, which navigates without firing a
submit event — browser semantics) or an input/button inside it (a real click, so the page's submit
handler runs and may cancel). Waits for the navigation and settle like `visit`.

```json
→ {"v":1,"id":11,"cmd":"submit","selector":"form#login"}
← {"id":11,"ok":true,"tag":"form"}
```

### cookies

Read the jar. With `url`: exactly what a request to it would carry (HttpOnly cookies included and
flagged). Without: the whole jar.

```json
→ {"v":1,"id":12,"cmd":"cookies","url":"https://example.com"}
← {"id":12,"ok":true,"cookies":[
      {"name":"sid","value":"abc","domain":"example.com","path":"/",
       "hostOnly":true,"secure":true,"httpOnly":true,"expires":0}]}
```

`expires` is unix seconds; `0` = session cookie. The jar is Chromium's own store: page scripts see
it through `document.cookie` (minus HttpOnly), and it survives navigation but not a restart.

### setCookie

Put a cookie in the jar (e.g. restore a logged-in session). Give `domain`, or a `url` to take the
domain from. An `expires` in the past deletes the cookie.

| Arg | Type | Required |
|---|---|---|
| `name` | string | yes |
| `value` | string | yes |
| `domain` or `url` | string | one of the two |
| `path`, `secure`, `httpOnly`, `hostOnly`, `expires` | | no |

```json
→ {"v":1,"id":13,"cmd":"setCookie","name":"sid","value":"abc","domain":"example.com","secure":true}
← {"id":13,"ok":true}
```

### clearCookies

Empty the jar.

```json
→ {"v":1,"id":14,"cmd":"clearCookies"}
← {"id":14,"ok":true}
```

### download

Stream a URL into `nothing-data/downloads/`. Chromium fetches it with the page's own cookie jar;
the file is written only after a complete 2xx transfer (it streams to a hidden `.part` file and
renames on success — nothing half-written is ever left).

| Arg | Type | Required | Notes |
|---|---|---|---|
| `url` | string | yes | `http(s)` only — `file://` and friends are refused |
| `name` | string | no | file name; any path part is dropped |
| `overwrite` | bool | no | default false; an existing name becomes `name (1).ext` |
| `maxBytes` | number | no | abort and delete past this (0 = no limit) |
| `referer` | string | no | |

```json
→ {"v":1,"id":15,"cmd":"download","url":"https://example.com/files/data.bin"}
← {"id":15,"ok":true,"path":"/.../nothing-data/downloads/data.bin","name":"data.bin",
   "bytes":1234,"status":200,"type":"application/octet-stream","url":"https://example.com/files/data.bin"}
```

Errors: `HTTP <code>` for a non-2xx, `only http(s) URLs are supported`, `over max bytes`, and on
any failure **no file is left behind**. There is also a one-shot CLI form:
`lucyn --download <url>` prints the saved path and exits 0/1.

### screenshot

Capture Chromium's current offscreen frame as a PNG in `nothing-data/screenshots/`. No window ever
opens; this is Chromium's own renderer.

| Arg | Type | Required | Notes |
|---|---|---|---|
| `name` | string | no | default `screenshot.png`; unusable names are rejected |
| `width` | number | no | viewport width used for the shot |

```json
→ {"v":1,"id":16,"cmd":"screenshot","name":"page.png","width":1280}
← {"id":16,"ok":true,"path":"/.../nothing-data/screenshots/page.png"}
```

Errors: `timeout capturing screenshot` (it waits ~1.2 s for a fresh frame), `screenshot failed`.

### info

```json
→ {"v":1,"id":17,"cmd":"info"}
← {"id":17,"ok":true,"url":"https://example.com","captures":42}
```

### moonbug

Start/adjust the capture listeners and/or turn on capture events.

| Arg | Type | Notes |
|---|---|---|
| `raw` | number | raw JSON capture stream port (0 = any free port, absent = leave as is) |
| `signed` | number | NULLBYTE-signed stream port |
| `dashboard` | number | dashboard port |
| `file` | string | serve this HTML instead of the embedded dashboard |
| `on` | bool | stream a `request` event line per capture on stdout |

All listeners bind `127.0.0.1` only and refuse non-loopback `Host`/`Origin`.

```json
→ {"v":1,"id":18,"cmd":"moonbug","raw":2005,"signed":2006,"dashboard":6969,"on":true}
← {"id":18,"ok":true,"events":true,"raw":2005,"signed":2006,"dashboard":6969}
```

Error: `port out of range (0..65535)`, or `raw stream already active` etc. if that port is taken.
The CLI flag `--moonbug` starts all three on those ports before the loop begins.

### captures

Recent captures, oldest first — with headers in wire order, timing and a body preview (unlike the
`request` event, which is a summary line).

| Arg | Type | Notes |
|---|---|---|
| `limit` | number | 1..500, default 20 |

```json
→ {"v":1,"id":19,"cmd":"captures","limit":5}
← {"id":19,"ok":true,"total":42,"captures":[{"id":1,"method":"GET","url":"http://...",...}]}
```

### quit

```json
→ {"v":1,"id":20,"cmd":"quit"}
← {"id":20,"ok":true}
```

Clean shutdown; the process exits 0. Closing stdin does the same thing.

---

## Errors

| Shape | Meaning |
|---|---|
| `{"ok":false,"error":"'url' required"}` | a required argument was missing |
| `{"ok":false,"error":"selector not found: h1"}` | the page has no such element |
| `{"ok":false,"error":"timeout loading page"}` | the command's deadline passed (visit 30 s, others 15 s) |
| `{"ok":false,"error":"no browser instance (CEF not initialised)"}` | no Chromium instance yet |
| `{"ok":false,"error":"built without CEF"}` | built with `-DLUCYN_WITH_CEF=OFF`; the loop still runs, every engine command fails like this |

## Events

Unprompted lines on stdout, no `id`:

```json
{"event":"moonbug","raw":2005,"signed":2006,"dashboard":6969}   // listeners are up (0 = off)
{"event":"request","id":3,"method":"GET","url":"...","status":200,"ms":123.4,
 "bytes":5678,"error":"","sig":"<NULLBYTE HMAC-SHA256>"}         // only after {"cmd":"moonbug","on":true}
{"event":"pageError","message":"Uncaught TypeError: ..."}         // an uncaught page error
{"event":"note","message":"..."}                                  // engine notes
```

Use `--events` on the CLI to get the capture events from the very first request (the Node client
does this), or turn them on later with `{"cmd":"moonbug","on":true}`.
