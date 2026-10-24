# Session 8: M7 (cookies, real fetch/XHR, identity + page-context headers)

Goal: finish the "Recommended next steps (M7)" list from `SESSION7.md`. Version is now `lucyn 0.7.0-m7`, client `0.7.0`.

**Result: 15/15 test suites pass on stock libcurl AND against the real libcurl-impersonate v2.2.3 (x86_64), and the new
tests are clean under AddressSanitizer + UBSan + LeakSanitizer with debug asserts on.** The session started by testing the M6
features properly, and that found bugs worse than the M7 items. Read "Bugs found" first.

## What was verified, and how (so you know how much to trust it)

- Baseline reproduced first: builds, 12/12 pass. But **no test touched XHR, POST or cookies**, so "12/12" said nothing about them.
- Against the **real** libcurl-impersonate (downloaded to a scratch folder, not in this zip) with a raw-socket server dumping
  the bytes on the wire. Every header claim below comes from that, not from reading code.
- Full-stack test (`test_controller_net`): JSON protocol -> controller -> JS -> HttpClient -> loopback server, asserting on what the
  *server* received.
- Mutation check: putting the old (buggy) POST option order back makes `test_net_request` crash, so the regression test bites.
- **Not verified:** anything against a real website (the sandbox has no route to one). Item 0 of HANDOFF section 6 is still yours.

## Bugs found in M6 (all fixed, all with tests)

1. **`HttpClient::post()` hung forever and could eat your command stream.** It called `CURLOPT_COPYPOSTFIELDS` and *then*
   `CURLOPT_POSTFIELDSIZE`. libcurl then discards the copied body (documented: the size must come first), leaves the method as
   POST with no data, and falls back to its default read callback, which does `fread(stdin)`. In LucyN, stdin is the JSON command
   channel. Reproduced on stock libcurl and on impersonate. A stale size from a previous request also made a later small POST
   over-read memory (found by the mutation check under ASan). Fix: size first, and a read callback that returns 0 so it can never
   touch stdin again.
2. **XHR event handlers never fired.** `xhrSetCallback` was defined but never attached, so `xhr.onload = fn` was a plain JS expando the C++
   never saw; `readystatechange` listeners were not called either; there was no `getResponseHeader`/`getAllResponseHeaders`; `responseURL`
   echoed the request URL; sync XHR was async; `abort()` fired nothing; each XHR kept a reference to itself (leak). XHR is rewritten (below).
3. **Page `fetch()`/XHR went out as page *navigations*:** `Sec-Fetch-Mode: navigate`, `Sec-Fetch-Dest: document`,
   `Upgrade-Insecure-Requests: 1`, `Accept: text/html...`, and no `Referer`/`Origin`. A strong bot tell.
4. **PUT/PATCH/DELETE/HEAD/OPTIONS were silently turned into GETs** by the controller (only `POST` was dispatched), and the Content-Type match
   was case-sensitive.
5. **`click` broke on any selector containing `'`** (e.g. `[data-x='y']`): the selector was spliced into a single-quoted JS string.
6. **`waitIdle` ignored requests in flight** (it only saw *completed* captures), so a slow request looked idle. Moonbug now counts them.
7. **`test_controller` asserted nothing in Release builds**: it uses `assert()`, and Release defines `NDEBUG`. (A plain `cmake -B build` with no
   build type keeps asserts, which is why it went unnoticed.) The file now `#undef NDEBUG`s. With asserts live it passes.
8. `new Headers()`, `new AbortController()`, `FormData`, `TextEncoder` did not exist (ReferenceError), and `fetch()` results were not real
   `Response` instances.

Corrections to `SESSION7.md`: its example JSON used `"method"`/`"params"`; the real protocol is `"cmd"` (the client was always right). And
"XHR works" was true only for `addEventListener('load')` on a plain GET.

## What changed

### `modules/net`
- `HttpClient::request(url, RequestOptions)`: any method, body (bytes), headers. `get()`/`post()` are wrappers.
- Headers are **merged by name** onto the browser's own set. Verified: a same-named header keeps its slot, `"Name:"` removes one, resetting
  restores the set (so the old `HTTPHEADER = nullptr` cleanup was fine). `planHeaders()` is public and unit-tested without a network.
- **Page-context headers** when `pageUrl` is set: `Accept: */*`; `Sec-Fetch-Site` (same-origin / same-site / cross-site; registrable domain is a
  small heuristic, not the public-suffix list), `-Mode`, `-Dest`; no `Upgrade-Insecure-Requests` / `Sec-Fetch-User`; `Referer` under
  strict-origin-when-cross-origin (none on https->http); `Origin` for non-GET or cross-origin cors.
- **Identity headers:** `setHeaderProvider()`, called per request outside the client lock.
- **Cookie jar** (libcurl's engine, in memory): sent on every request and every redirect hop. API: `cookies()`, `cookiesFor(url)`,
  `cookieStringFor(url)` (document.cookie view, no HttpOnly), `setCookieFromScript(url, str)` (browser rules: rejects HttpOnly, foreign
  Domain, bare TLD, `Secure` from non-secure origin, `__Secure-`/`__Host-` violations, overwriting an HttpOnly cookie; Max-Age/Expires delete),
  `setCookie(Cookie)`, `clearCookies()`. Loopback counts as a secure context (as in Chrome). The jar lives in the curl handle, so a profile
  switch starts empty. Not persisted.
- `Response` gained `redirects`, `statusText` (the server's reason phrase, empty on HTTP/2+), `httpVersion`. `Moonbug::inFlight()`.

### `modules/js`
- `fetch_bridge.cpp` is now a small native primitive (`httpSend/httpSendSync/httpCancel`, `utf8Encode/Decode`, `cookieGet/Set`). The old
  C++-built `Response` objects and `xhr_bridge.cpp` are gone.
- New `src/prelude/p4_net.js` (real classes): `Headers`, `FormData` (strings only), `Request`, `Response`, `fetch`, `AbortController`/`AbortSignal`,
  `ProgressEvent`, `XMLHttpRequest` + `upload`, `TextEncoder`/`TextDecoder`. The tail of `p3_window.js` moved to `p9_publish.js`.
  Bodies are bytes (binary survives, BOM stripped by `text()`), `data:` URLs work, errors are `TypeError: Failed to fetch` like Chrome.
- `document.cookie`: through the host's jar when `cookieGet`/`cookieSet` are wired, else a small in-memory per-page jar.
- `HostOptions` gained `httpFetch` (full request + page context; wins over `httpRequest` > `httpGet`, which still work), `cookieGet`, `cookieSet`.
  `<script src>` uses it too (`mode: no-cors`, `dest: script`). Worker threads still never touch the JS context; results are pumped on the JS
  thread with a microtask checkpoint after each.

### `modules/controller`, `client`
- One `httpFetch` path for everything the page requests; `document.cookie` and HTTP share the same jar.
- Persona headers: `Accept-Language` (from `Surface`, the same source as `navigator.languages`). On stock libcurl, which sends *no* browser headers at all,
  also `User-Agent` and `Sec-CH-UA*`. In impersonating mode UA/`sec-ch-ua` are deliberately **not** overridden (see decisions).
- New commands `cookies {url?}`, `setCookie {...}`, `clearCookies`; client `cookies()`, `setCookie()`, `clearCookies()`. Client README brought up to date.
- Fixes: `click` quoting, `waitIdle` in-flight.

## Decisions worth knowing

- **Who owns headers:** the library owns the browser set and its order; we only override by name. Not overriding UA/sec-ch-ua when impersonating
  avoids a second opinion that could break the match with the TLS fingerprint. It is still unverified that `Surface` equals what the library sends.
- `settle()` stays timer-based, so `visit()` returns before a fetch the page started on load. `waitIdle`/`waitFor` are the network-aware waits
  (and the client README now says so). Making `visit` wait would add up to `settleTimeoutMs` to any page with a hanging beacon.
- `credentials: 'omit'`, `redirect: 'manual'`, CORS and streams are not implemented (a scraper wants permissive cross-origin reads).

## Known limitations (honest list)

- **Header order for appended headers.** On the wire (real library, Chrome 146): `Referer`, `Origin`, `Content-Type`, `Content-Length` land after
  `Accept-Language`; real Chrome interleaves them elsewhere. Firefox keeps its navigation `Priority: u=0, i` / `Te: trailers`. Needs a reference
  capture from a real browser, then owning the base list.
- Cookies are not persisted. `Blob`/`File`, streams, `WebSocket`, `responseType` `blob`/`document`, `responseXML` are not implemented.
- Inline `on*=""` attributes are not compiled (pre-existing; found when a test page used `onclick=`).
- `Set-Cookie` parsing/expiry/SameSite semantics are libcurl's. Script-side parsing is ours and covers the common attributes; `SameSite` is accepted
  and ignored.
- Everything above was verified on loopback only.

## Tests

| Suite | What it covers |
|---|---|
| `net_request` (new) | POST body regression (+300 KB, embedded NUL), all verbs, handle reuse, header merge, cookie jar (HttpOnly, paths, redirects, script rules, import), `planHeaders` matrix, wire headers, in-flight counter |
| `js_net` (new) | Headers, fetch (bodies of every kind, verbs, abort, redirects, h2 statusText, binary), XHR (event order, headers, abort, timeout, sync, upload, state errors), cookies (both jars), TextEncoder/Decoder, legacy callbacks, `<script src>`, teardown with requests in flight |
| `controller_net` (new) | the whole stack against a loopback server (see above) |
| the 12 existing | unchanged; `controller` now actually asserts in Release |

Run with the real library: `cmake -S . -B build -DLUCYN_CURL_IMPERSONATE_ROOT=third_party/curl-impersonate && cmake --build build && ctest --test-dir build`.

## Recommended next steps (M8)

0. **You, on your machine:** HANDOFF section 6 item 0 (real-site probe), and compare a page-initiated request in DevTools (header order!) with a
   Moonbug capture of the same request from LucyN. That reference is what the header-order fix needs.
1. Persist the cookie jar (per identity), and add the `cookies: true` client option the README mentions.
2. Own the base header list (`CURLOPT_HTTPBASEHEADER`) to fix header order for page-initiated requests; verify `Surface` UA/`sec-ch-ua` against the library.
3. `Blob`/`File` + multipart file parts; streams for `response.body`.
4. Inline `on*` attributes. `WebSocket`. Canvas.

## Opening message for Session 9

> "Read HANDOFF.md and SESSION8.md in the zip. Here is a real-site capture from LucyN and the same request from Chrome DevTools: [paste]. Next I want: [cookie persistence / header order / Blob+FormData files / canvas]."
