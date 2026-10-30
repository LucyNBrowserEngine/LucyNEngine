# Moonbug — the wiretap

LucyN runs Chromium inside the engine, so it sees its own decrypted traffic natively:
**no MITM certificate, no proxy, no CDP, no debug port.** Moonbug sits on that path and records
every request the engine makes — page loads, stylesheets, images, and anything a page script asks
for with `fetch()`/XHR.

```
traffic → Moonbug (capture) → NULLBYTE (sign) → TinyElysia/Mongoose (serve) → dashboard (view)
```

## What a capture holds

Every request lands as one `Capture` object (`modules/net/include/lucyn/net/moonbug.h`):

| Field group | Fields |
|---|---|
| identity | `id` (1, 2, 3 … per run), `tsMs` (unix ms when the request started) |
| request   | `method`, `url`, `finalUrl` (after redirects), `redirects`, `requestBody`, request headers in **wire order** |
| response  | `status`, `httpVersion` (`"1.1"`/`"2"`/`"3"`), `responseBody` (preview), `responseBytes`, `bodyTruncated`, response headers |
| transport | `remoteIp`, `remotePort`, `timing` (`dnsMs`, `connectMs`, `tlsMs`, `firstByteMs`, `totalMs` — cumulative, like curl) |
| outcome   | `error` (empty on success), `done` |
| seal      | `sig` — NULLBYTE's HMAC-SHA256 over the capture, when signing is on |

Limits, per `Moonbug::Options`: **1 MiB** of response body kept per capture (`maxBodyBytes`) and
the last **500** captures kept for late joiners and the dashboard (`historySize`). The body preview
is cut, but `responseBytes` always reports the true size.

**Where `responseBody` comes from.** Two complementary paths (see the README's *See
everything* section):

- **The response filter (primary).** Chromium never volunteers response bytes to the browser
  process, but CEF lets the engine install a `CefResponseFilter` on the response stream
  (`GetResourceResponseFilter` in `modules/cef_host/src/browser_host.cpp`): every text/code
  resource — main document, scripts, stylesheets, JSON, error pages — passes through to the page
  untouched while a copy is attached to the capture as a `Body` event. Cap: 4 MiB per response,
  and it carries the response `Content-Type`, which is also what decides whether the file becomes
  an artifact in `nothing-data/artifacts/` (full bytes on disk; Moonbug then trims its own preview
  to the 1 MiB above).
- **The page-side tap (fallback).** The older path wraps the page's own `fetch()` and
  `XMLHttpRequest` (`kNetTapJs`) and reports what the page read. Both paths can see the same
  response; the controller recognizes the duplicate by content hash, so a capture ends up with
  exactly one body and a file is saved once.

A body usually arrives just after the capture's `Finish`: the capture is then patched in place and
re-published (same `id`), which is what `onUpdate` and the dashboard's live refresh are for.
Binary responses (images, fonts, media) get no filter at all — headers and byte count only.

Listeners fire in four places: `onRequest` (request about to be sent — request fields only),
`onResponse` (response or failure is in), `onCapture` (the finished capture, after that) and
`onUpdate` (a stored capture gained its body — same id, new content).

## Turning it on

**CLI** — all three listeners, on the conventional ports:

```bash
./build/bin/lucyn --moonbug            # raw 2005, signed 2006, dashboard 6969 (127.0.0.1 only)
./build/bin/lucyn --dashboard 8080     # dashboard alone on your own port (0 = any free port)
./build/bin/lucyn --events             # also print one {"event":"request"} line per capture on stdout
```

**JSON protocol**

```json
{"v":1,"id":1,"cmd":"moonbug","raw":2005,"signed":2006,"dashboard":6969,"on":true}
{"v":1,"id":2,"cmd":"captures","limit":20}
```

`on:true` starts the stdout `request` events (the Node client asks for them at spawn time, so a
listener attached late still sees the very first request).

**Node**

```js
const a = new Lucyn({ moonbug: { dashboard: 6969 } });
a.moonbug.onRequest((r) => console.log(r.status, r.url, r.sig));
const { raw, signed, dashboard } = await a.moonbug.serve({ dashboard: 0 });  // 0 = any free port
```

Everything binds `127.0.0.1` only and refuses non-loopback `Host`/`Origin`. A port that is already
taken is reported (`raw stream already active`), never silently swapped.

## The three listeners

| Listener | Default port | Endpoints |
|---|---|---|
| **raw stream** | 2005 | `GET /` help text · `GET /captures?limit=N` history as a JSON array · `ws /stream` one JSON capture per frame |
| **NULLBYTE-signed stream** | 2006 | `GET /` help · `GET /captures?limit=N` history as envelopes · `POST /verify` body = an envelope → `{"ok":..,"error":..}` · `ws /stream` one envelope per frame |
| **dashboard** | 6969 | `GET /` the dashboard page (embedded in the binary, or `--dashboard-file <f>` to serve your own, re-read on every request) · `ws /stream` envelopes · `GET /api/captures?limit=N` · `POST /api/verify` · `GET /api/status` |

`{"cmd":"moonbug"}` replies with the ports actually bound (`0` = that listener is off), and an
`{"event":"moonbug", ...}` line announces them. The dashboard's "Verify seal" button posts the
envelope to `/api/verify`.

## NULLBYTE: why you can trust a capture

Each capture can be sealed with **HMAC-SHA256** over the exact bytes of that capture, so you can
answer "was this captured cleanly, and has it been touched since?".

**The envelope** (one JSON object):

```json
{"v":1,"alg":"HMAC-SHA256","sig":"<64 hex>","payload":"<the signed bytes, as a JSON string>"}
```

`payload` is a JSON *string* holding the exact signed text, so verification never re-serialises
anything: unescape, HMAC, compare. Verification is a constant-time compare and trusts nothing in
the envelope except the math.

**The key** lives at `nothing-data/security/nullbyte.key` (32 random bytes, owner-only
permissions, created on first use). A key file that exists but is unreadable or corrupt is
**reported, never silently replaced** — replacing it would orphan every earlier signature. The
same `id` + `sig` appear on the signed stream, in the dashboard and in the `request` event, so the
three always agree.

What it does **not** give you: a seal, not a sandbox. The key lives in the engine process, so this
does not protect against a compromised engine — it protects the capture after it was written.

**Verifying by hand**

```bash
# from the signed stream or the dashboard
curl -s -X POST http://127.0.0.1:2006/verify --data-binary @envelope.json
# -> {"ok":true,"parsed":{...},"error":""}
```

```js
const caps = await a.moonbug.captures(1);
console.log(caps[0].sig);        // the same sig the signed stream carries
```

## Reading captures

```json
{"v":1,"id":3,"cmd":"captures","limit":5}
```

```json
{"id":3,"ok":true,"total":42,"captures":[
  {"id":41,"method":"GET","url":"https://example.com/app.js","status":200,"ms":58.2,
   "httpVersion":"2","responseBytes":1024,"bodyTruncated":false,"timing":{"totalMs":58.2}}]}
```

The history is oldest-first, capped at 500; `total` keeps counting past that. Captures are also
what `waitIdle` watches: a request that has begun but not finished keeps the engine waiting.
