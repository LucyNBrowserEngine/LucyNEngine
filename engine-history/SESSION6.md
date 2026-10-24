# Session 6: M5 - Moonbug + NULLBYTE + TinyElysia + capture dashboard

## What was built

**M5 = "nothing that touches the network escapes Moonbug."** Pipeline:
`libcurl -> Moonbug (capture) -> NULLBYTE (sign) -> TinyElysia (serve) -> dashboard`.

### New / changed files
```
modules/security/                       NEW  NULLBYTE: SHA-256, HMAC-SHA256, key file, signed envelope
  include/lucyn/security/nullbyte.h  src/nullbyte.cpp  tests/test_security.cpp  README.md
modules/net/
  include/lucyn/net/moonbug.h           NEW  Capture struct + Moonbug (listeners, history ring, toJson)
  src/moonbug.cpp                       NEW
  src/http_client.cpp  http_client.h    get() captures through Moonbug; setMoonbug(); impersonating() fix (see below)
  tests/test_moonbug.cpp                NEW  includes a real loopback HTTP server (POSIX)
modules/server/                         NEW  TinyElysia (HTTP+WebSocket on Mongoose) + CaptureServer + CLI
  include/lucyn/server/{tinyelysia,capture_server}.h  src/*.cpp  tools/lucyn-moonbug.cpp
  embed_dashboard.cmake  tests/test_server.cpp  README.md
dashboard/home_dashboard.html           NEW  the capture dashboard (embedded into the binary at build time)
third_party/mongoose/                   NEW  Mongoose 7.23, vendored, + VENDORED.txt (license warning!)
modules/controller/                     `moonbug` + `captures` commands, `request`/`moonbug` events, stdout mutex
apps/lucyn/main.cpp                     --moonbug --raw --signed --dashboard --dashboard-file --events; version 0.5.0-m5
client/src/index.js                     a.moonbug.onRequest / serve / captures; version 0.5.0
CMakeLists.txt                          LUCYN_MODULE_SECURITY, LUCYN_MODULE_SERVER; controller now requires both
```

### How to use it
```bash
build/bin/lucyn-moonbug https://example.com        # starts 2005/2006/6969, fetches, prints one line per capture, then reads urls from stdin
build/bin/lucyn --moonbug                          # the engine with all listeners on; open http://127.0.0.1:6969/
```
```js
const a = new Lucyn({ moonbug: { dashboard: 6969 } });
a.moonbug.onRequest(r => console.log(r.status, r.url, r.sig));
await a.visit("https://example.com");
```

Protocol additions (all in `controller.h`): commands `moonbug` (`on`, `raw`, `signed`, `dashboard`, `file`), `captures` (`limit`);
events `request` (`id method url status ms bytes error sig`) and `moonbug` (`raw signed dashboard`); `info` gained `captures`.

## Design decisions (keep these)
1. **Moonbug listeners run outside every lock.** `HttpClient::get` reads state under its mutex, fires `onRequest` with no lock, performs, then fires `onResponse`/`onCapture` with no lock. A listener may call back into the client. (Tested.)
2. **Request headers come from libcurl's debug channel** (`CURLINFO_HEADER_OUT`): the real decrypted bytes, in wire order. Verbose is turned on only while a Moonbug is attached and switched off again when detached (the handle is reused). It never prints to stderr.
3. **A failed request is still traffic**: connection refused / timeout are captured with `error` set.
4. **The seal is over the exact JSON bytes.** The envelope carries the signed text as a JSON *string*, so verifying never re-serialises. The same capture has the same `sig` on stdout events, the signed stream and the dashboard.
5. **Local ports are a wiretap, so they are guarded.** WebSockets ignore CORS: without a check, any web page open in your normal browser could read `ws://127.0.0.1:2005` (cookies, auth headers). TinyElysia refuses a non-loopback `Host` (DNS rebinding) or `Origin`. No CORS headers, ever. The dashboard builds nodes with `textContent` only + a strict CSP; tested against a hostile page.
6. **stdout is the protocol channel.** Mongoose's logger writes to stdout, so it is compiled out (`MG_ENABLE_LOG=0`, PUBLIC on the target so struct layouts match). Listener addresses are announced as a `moonbug` event and on stderr.
7. **Bodies**: capture keeps up to 1 MiB (`bodyBytes` is the true size); JSON previews are 16 KiB, cut on a UTF-8 boundary, scrubbed, and sent as base64 when binary.
8. **`CaptureServer` shares state with the Moonbug tap through a `shared_ptr` + `closed` flag.** Moonbug copies listeners out before calling them, so a capture already in flight can reach the tap after the server is destroyed. (Tested: destroy mid-fetch.)
9. The controller serializes stdout writes with a mutex, because the JS `fetch()` bridge calls `httpGet` from a worker thread and capture events race with replies.

## Bugs found and fixed while building (all have regression tests)
* TinyElysia self-deadlock: `mg_ws_upgrade()` fires `MG_EV_WS_OPEN` synchronously and the handler re-locked `routesMu`.
* TinyElysia keep-alive hang: Mongoose sets `is_resp` per request and parses nothing more on that connection until it is cleared (only `mg_http_reply()` clears it). Replies are written by hand here, so TinyElysia clears it. `test_keepalive` fails without the fix (verified by removing it).
* Pre-existing: `HttpClient::impersonating()` was `true` for an empty-target profile ("wearing" nothing). Now `false`.
* Pre-existing, zip only: `test_controller.cpp` used `rc` without declaring it (your machine compiled, so you had patched it locally). Fixed the same way.

## Verified (Ubuntu 24, gcc 13, stock libcurl 8.5, zero warnings)
* ctest: 12/12 (core, profile, identity, net, **moonbug**, **security**, **server**, dom, dom_mutate, shell, js, controller) in Debug and Release. New tests use `CHECK`, not `assert`, so they still bite in Release.
* ThreadSanitizer: 0 reports on security / moonbug / server (incl. 4-thread broadcast storm, destroy-server-mid-fetch).
* ASan + UBSan + LeakSanitizer: 0 reports on security / moonbug / server.
* NULLBYTE against FIPS 180-4 SHA-256 vectors and RFC 4231 HMAC vectors (incl. keys > block size); HMAC cross-checked against Python's `hmac`.
* End to end with the real `lucyn` binary + Node 22 client against a local web server: JS ran on the page, `request` event received, captures have wire-order headers, signed history verifies, a forged capture is rejected, event `sig` == envelope `sig`, a foreign `Origin` gets 403.
* The dashboard's own JavaScript, run in jsdom against the live engine: history load, live WebSocket rows, detail panel, verify, tamper-and-verify, filter, verify-all; hostile body/header/URL render as inert text (`window.pwned === undefined`).

## NOT verified (do not claim otherwise)
* **Anything with real HTTPS / curl-impersonate.** The sandbox has stock libcurl, so captures show `chrome146 (no)` (requested, not worn). Run `lucyn-moonbug https://example.com` on your machine with `third_party/curl-impersonate` in place.
* **HTTP/2 request headers.** `CURLINFO_HEADER_OUT` text for h2/h3 was not seen: pseudo-header order (`:method`, `:path`...) may be missing or reconstructed. `requestHeaders` is empty rather than invented when libcurl reports nothing. Check against a real h2 site.
* **A real browser rendering the dashboard** (only jsdom). Layout is untested; behavior is.
* **Windows**: TinyElysia/Moonbug are portable C++17 (Mongoose on ws2_32, key via BCrypt) but untested; the socket tests are POSIX-only and skip on Windows.
* Slow-WebSocket-client dropping (8 MiB cap) is implemented, not tested.

## Licensing: needs your decision
**Mongoose is GPL-2.0-only OR commercial.** The README chose it, so it is vendored, but a LucyN binary that links it is GPL-2.0
unless you buy Cesanta's commercial license. Fine for personal use / GPL release. If `nothing-browser` or LucyN must stay MIT/proprietary,
swap TinyElysia's backend before shipping: the `TinyElysia` header deliberately exposes no Mongoose types, so only `src/tinyelysia.cpp` changes.

## Known gaps / ideas
* No auth token on the local ports (loopback + Host/Origin guards only). A per-run token in the dashboard URL would also stop other *local processes* reading the wiretap.
* NULLBYTE seals each capture independently, so it proves "not altered" but not "none deleted". A hash chain (`prev` sig in each payload) would add deletion evidence.
* `HttpClient` is still GET-only: `method`, `requestBody` exist in `Capture` for POST/XHR later. Redirect hops are collapsed into one capture (final-hop headers + `redirects` count).
* Moonbug ad-blocking hook (M8) fits at `HttpClient::get` before perform; nothing built.
* `eval`, `waitFor`, `waitIdle` are still not implemented (the JS context is torn down after settle).
* `visit` events: capture ids are per-Moonbug (per process), not per visit.

## Suggested next steps
1. Author: on your machine run `lucyn-moonbug https://tls.peet.ws/api/all` with curl-impersonate and look at the dashboard: compare `requestHeaders` order with real Chrome.
2. Decide the Mongoose license question (above).
3. M6/M7 (SDL window, layout) or finish M3/M4 gaps: POST/XHR (Capture already has the fields), `waitFor`/`waitIdle`, keeping the JS context alive for `eval`.

//this were done on my machine additional context is here Yap, this is a very good result. It's one site and one sample, but the part curl-impersonate controls matches real Chrome.

**Identical to real Chrome**

| Check | LucyN (terminal) | Real Chrome (browser) |
|---|---|---|
| **JA4** | `t13d1516h2_8daaf6152771_d8a2da3f94cd` | same |
| **JA4_r** (ciphers, sorted extensions, sig algs) | same | same |
| **HTTP/2 Akamai fingerprint** | `1:65536;2:0;4:6291456;6:262144\|15663105\|0\|m,a,s,p` | same, same hash |
| **Peetprint hash** | `1d4ffe9b…` | same |
| **Header order** | `sec-ch-ua` trio → `upgrade-insecure-requests` → UA → accept → `sec-fetch-*` → encoding → language → `priority` | same |

The `ja3_hash` differs, but that's expected. Chrome shuffles its TLS extension order on every connection, so JA3 changes every time (JA4 sorts the extensions, so it doesn't). Run the fetch two or three times: if your `ja3_hash` changes between runs, the shuffling is working too.

The remaining differences are all intended or just a different build of Chrome:
- **Version and brands:** Chrome 146 vs 147, and your browser is a UOS build, so it lacks the "Google Chrome" brand.
- **Platform:** LucyN claims macOS, your browser is on Linux. That's the persona doing its job.
- **`sec-fetch-site`:** `none` vs `cross-site`. That depends on how you reached the page (typed URL vs a link), not on the engine.

**One real catch: the OS fingerprint**
Look at the `tcp_syn` / `p0f` section. It's identical in both because both ran on your Linux kernel. LucyN says "I'm a Mac" at the HTTP and TLS layers, but the TCP handshake says "Linux". A macOS machine has a different TCP signature (different window and options order). Some anti-bot systems check that, passively and without being able to be fooled by headers. That's exactly the "Windows UA with Linux fonts" mismatch your README warns about, just one layer lower. The options are:
- Use a persona that matches the machine it runs on, such as a Linux Chrome target if curl-impersonate has one.
- Run it on a real Mac.
- Accept the risk for sites that don't check TCP.

I wouldn't try to fake TCP options from user space. It's a kernel-level thing and easy to get wrong.

**A fix for me**
Moonbug showed `Host` as the first header, but the real wire sends `:method`, `:authority`, `:scheme`, `:path` first and has no `Host`. The regular headers' order matches, as your comparison confirms. But the dashboard shows curl's HTTP/1-style rebuild, not the real frames, so it isn't literally the wire. I can make Moonbug render the h2 pseudo-headers properly if you want.
Yep, nothing is on your PATH. The binaries live in `build/bin/`, so run them from the repo root with the path in front:

```bash
cd ~/dev/lucyn
build/bin/lucyn-fetch -i chrome146 --body https://tls.peet.ws/api/all
```

I'd use `lucyn-fetch` for this check rather than `lucyn-moonbug`. `lucyn-moonbug` only prints a one-line summary per capture, and `lucyn-fetch --body` prints the JSON that tls.peet.ws returns. That JSON is the useful part: your JA3/JA4, the HTTP/2 fingerprint, the headers the server saw and the `user_agent`.

Paste me that output and I'll help you compare it with real Chrome. The fields to look at are:
- `ja4` (and `ja3_hash`)
- `http_version` (it should say `h2`)
- the HTTP/2 `akamai_fingerprint` and the order of its pseudo-headers
- the `sent_frames` / SETTINGS values

You can also compare the whole fingerprint against real Chrome by opening https://tls.peet.ws/api/all in an actual Chrome 146 and copying its JSON. Real Chrome randomizes TLS extension order, so JA3 differs between runs. JA4 sorts the extensions, so it stays stable, and that's the one to compare.

If you'd rather watch it in the dashboard while you fetch, this also works:

```bash
build/bin/lucyn-moonbug https://tls.peet.ws/api/all
```

Then open `http://127.0.0.1:6969`. Stop the earlier `lucyn-moonbug` first with an empty line or Ctrl+C. It's probably still holding ports 2005, 2006 and 6969, and the new one would complain they're in use.
