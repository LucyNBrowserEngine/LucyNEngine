# Session 10: M9 — Blob/File

Goal: item 3 from `SESSION9.md`'s M9 list — `Blob`/`File`, real `FormData` file parts, and the `responseType: 'blob'` /
`response.blob()` gaps that depended on them. (Streams and canvas are still open; see below.)

**Result: implemented and tested; 15/15 suites pass (14 unchanged + `js_net` extended with a new `testBlobAndFile()`). An addendum (below) then added the `download` command: 17/17 suites.**
No cookie-persistence or header-order work was needed this session — both turned out to already be implemented and
tested (`ControllerOptions::persistCookies`, `net::HttpClient::export/importCookieFile`), just not reflected in
`HANDOFF.md`'s "recommended next steps"; that file is corrected now.

**Sandbox note:** no network access here (same restriction as every prior session), so this is the `js` module's
own synchronous/mocked test rig, not a real site. `cmake`/`libgumbo-dev`/`libcurl4-openssl-dev` were not preinstalled
this time either — installed via `apt-get` (allowlisted `archive.ubuntu.com`/`security.ubuntu.com`) before building.
Stock libcurl only, same as every previous sandbox run; curl-impersonate still untested by the AI.

## What changed

### `modules/js/src/prelude/p4_net.js`
- **New: `Blob` and `File`** (placed before `FormData`, which now depends on them).
  - `Blob` stores one concatenated `ArrayBuffer` + a normalized `type` (ASCII-only, lower-cased; anything else collapses
    to `""`, matching the real Blob type algorithm). Parts can be a string (UTF-8 encoded), `ArrayBuffer`, typed array,
    or another `Blob`.
  - `size`, `type`, `slice(start, end, contentType)`, `arrayBuffer()`, `bytes()`, `text()` — all resolve immediately;
    there is no streaming representation (matches the project's existing "no streams" stance).
  - `File extends Blob`: `name` (required — 2-arg constructor, throws `TypeError` like the DOM spec otherwise),
    `lastModified` (defaults to `Date.now()`), `webkitRelativePath` (always `""`).
  - Both published as globals in `p9_publish.js`.
- **`FormData.append`/`set`** now take an optional third `filename` argument. A `Blob` value becomes a `File` entry
  (default filename `"blob"`, matching Chrome); an appended `File` keeps its own name unless `filename` is given.
  `get`/`getAll`/`entries`/etc. needed no changes — they already returned the stored value as-is.
  `collectForm()` (building a `FormData` from a real `<form>`) still skips `<input type=file>`: there is no file
  picker in a headless engine, so there is nothing to populate it with. Unchanged from before this session.
- **`serializeForm` rewritten to build bytes, not a string.** The old version concatenated everything as a JS string,
  which cannot carry arbitrary binary file content. It now pushes `Uint8Array` chunks (UTF-8-encoded headers +
  the file's raw bytes + the text parts, each via `n.utf8Encode`) and concatenates them with a new `concatBytes()`
  helper. `extractBody()` for a `FormData` body now returns an `ArrayBuffer`, same as it already did for
  `ArrayBuffer`/typed-array bodies — no change needed on the C++ side (`fetch_bridge.cpp`'s `bytesOf()` already
  accepted `ArrayBuffer`s; it was only ever fed a string before for multipart).
- **`extractBody()`**: a `Blob` body is now recognized — its bytes go out as-is, its `type` becomes the default
  `Content-Type` (matches `fetch`/`XHR`/`Request`/`Response`, all of which share this function).
- **`Response.blob()` / `Request.blob()` / `Response.bytes()`** added, symmetric to the existing `text()`/`json()`/
  `arrayBuffer()`. Same single-read rule (`bodyUsed`) as the others.
- **XHR `responseType: 'blob'`**: `response` now returns (and caches, like the existing `arraybuffer`/`json` cases)
  a `Blob` built from the response bytes, typed from the response's own `Content-Type` header. A new `xhrContentType()`
  helper reads that header the same way `getResponseHeader` would. The no-body branch (204, a request still in
  flight that somehow reads `response`, etc.) returns an empty `Blob` instead of `null` for this responseType,
  matching the existing `arraybuffer` → `new ArrayBuffer(0)` precedent.

### `modules/js/src/prelude/p9_publish.js`
- `Blob` and `File` added to the global class map (non-enumerable, like everything else there).

### `modules/js/tests/test_js_net.cpp`
- `testBlobAndFile()`: `Blob` construction/size/type-normalization/`slice()`/`text()`; `File` name requirement;
  `FormData` with a bare `Blob` (default filename) and an explicit `File`; a real `fetch()` POST with that
  `FormData` asserting the raw multipart bytes (filename + Content-Type + body) landed on the wire; a `fetch()`
  PUT with a `Blob` body asserting the Content-Type header and the exact 3 raw bytes sent; `Response.blob()`
  round-tripping the engine's existing 256-byte binary fixture (`/bin`) byte-for-byte, and carrying the
  `Content-Type` through on a text fixture; the single-read rule on `Response.blob()`; XHR `responseType: 'blob'`
  including that repeated `.response` reads return the same cached `Blob`.
- Registered in `main()`.

### Docs
- `modules/js/README.md`: documented `Blob`/`File` under Implemented, trimmed the stale "no Blob/File" line from
  NOT-implemented, and (since this session touched the same section) also backfilled the still-missing M8 mentions
  of `WebSocket` and inline `on*` compilation that `SESSION9.md` never carried into this file.
- `HANDOFF.md`: status table, one-line state, and "recommended next steps" updated — including correcting the
  cookie-persistence and header-order lines, which the previous session's next-steps list still showed as open
  even though the code already has them (persistence; header order is still genuinely open, needs a real capture).

## Decisions

- **Multipart parts are fully buffered, not streamed.** Consistent with the rest of the engine (`response.body` is
  already `null`, XHR is not chunked) — a `Blob`'s content lives in one `ArrayBuffer` from construction, same way
  a `Response`'s body already did.
- **`FormData` entries store `File` objects directly (not a lazy wrapper).** Simpler than deferring File
  construction to access time, and matches what pages actually observe in practice; the one divergence from the
  spec is that each `get()`/`getAll()` call does not necessarily return the exact same object identity a spec-strict
  engine would for an appended `Blob` (it does for an appended `File` with no `filename` override, which is the
  common case).
- **No C++ changes.** `fetch_bridge.cpp`'s `bytesOf()` already handled `ArrayBuffer` bodies (added back in M7 for
  typed-array fetch bodies); routing `FormData`'s binary serialization through the same path needed nothing new
  on the native side. This was the main reason to pick this item over header-order (needs a real browser capture,
  which the AI cannot get in this sandbox) or canvas (the author's own "biggest time sink, least value" note).

## Known limitations

- Still no streams: `response.body`/`request.body` are `null`, and `Blob`/`File` have no `.stream()`.
- File `<input>` elements remain inert — `FormData` built from a real `<form>` never includes a `File`, since there
  is no file-picker concept in a headless engine. (Scripted `FormData.append('f', someFile)` works fine.)
- `XHR.responseType = 'document'` is still unimplemented (`null`), unrelated to this session's work.
- Not tested against a real site or real libcurl-impersonate (same sandbox limitation as every session so far).

## Real-site check and a human-added upgrade (added after the session, 13 Oct 2026)

The author ran the built binary on their own machine (real network). `visit` on example.com worked (200, "Example Domain"), and a page-JS
`fetch()` of a 10,340,250-byte binary followed by `r.blob()` gave **exactly the same byte count and content type as `curl`**. So the Blob work
holds up against a real server, not just the mocked rig. (Their one-liner pipes NDJSON into `./bin/lucyn`; `eval` returns `"[object Promise]"`
for a Promise, so they stash the result in `window.R`, `waitIdle`, then read it back.)

That test also showed the gap: **JS can fetch bytes but nothing can save them.** The author asked for a one-command download straight into the
data folder. **Built in the addendum below.**

## Addendum: the `download` command (human-requested, built after the Blob work)

**The one command:** `./bin/lucyn --download <url> [--name f] [--overwrite] [--max-bytes N] [--referer R]` saves into
`nothing-data/downloads/`, prints exactly one line on stdout (the saved path), progress on stderr, exit 0 / 1. Also the controller
command `{"cmd":"download","url":...}` and the client method `await a.download(url, {name, overwrite, maxBytes, referer})`.
Reply: `{"id":N,"ok":true,"path","name","bytes","status","type","url"}` (`url` = final URL after redirects).

### What changed
- **`net` (`http_client.h/.cpp`)**: `RequestOptions::bodySink` (stream a 2xx body chunk by chunk; `Response::body` stays empty; return false = abort),
  `RequestOptions::timeoutSec` (`-1` default 30 s, `0` = no overall limit but a stalled connection still dies: under 1 B/s for 60 s; restored after the
  request because the curl handle is reused), `Response::bodyBytes`. **Non-2xx bodies are discarded, never handed to the sink.** Moonbug still captures
  the request, with the true byte count and no body. The 30 s timeout was a real bug for downloads: any file that took longer than 30 s would have died.
- **`controller`**: `cmdDownload` + helpers. Streams to `.download-<n>.part` (hidden), renames to the final name only after a complete 2xx
  transfer, deletes the `.part` on every failure path. Name = `name` arg, else `Content-Disposition` (RFC 5987 `filename*=` then `filename=`), else the
  URL's last segment (percent-decoded), else `download.bin`. **Names are treated as hostile**: basename only (so `../../x`, `..\x`, `/etc/x` become `x`),
  control chars/NUL dropped, `: * ? " < > |` become `_`, leading dots become `_`, Windows device names (`CON`, `NUL`, `COM1`...) get a `_` prefix,
  capped at 200 bytes without cutting a UTF-8 character in half. An existing name becomes `name (1).ext` (and `a.tar.gz` -> `a (1).tar.gz`) unless `overwrite`.
  http/https only. Shares the one identity, cookie jar and Moonbug capture with page requests.
- **`apps/lucyn/main.cpp`**: `--download` and its extras; it drives a normal `Controller` with one `download` + `quit`, so the flag and the NDJSON command can never disagree.
- **`client/src/index.js`**: `download()`. **Docs**: `controller.h` command list, `client/README.md`, `modules/core` (downloads/ folder), root `README.md`.

### Tests (17/17 suites; 2 new)
| Suite | What it covers |
|---|---|
| `controller_download` (new) | bytes on disk identical (binary 0..255, 100 KB multi-chunk); `name (1)`, `overwrite`; name from `Content-Disposition` (UTF-8 and quoted) and URL; traversal (`../../`, `..\\`, absolute path, `..`) never escapes `downloads/`; 404 saves nothing; empty body; redirect (final URL + name); `maxBytes` (abort before AND after the file is open, no `.part` left); `file:`/`ftp:` refused; shared cookie jar; Moonbug capture; `Referer`; Windows-illegal/reserved/hidden names; 300-char name capped. Plus the `net` streaming contract tested directly (error page never reaches the sink, abort works, buffered requests still work on the reused handle) |
| `cli_download` (new) | runs the REAL `lucyn` binary (copied to a temp folder): exactly one stdout line = the path, exit 0; collision; `--name`/`--overwrite`; failure = exit 1 + empty stdout + reason on stderr; `--max-bytes`; refused scheme; download-only flags without `--download` are an error; no `.part` left |
| everything else | unchanged, still passing |

**Proven the tests can fail:** three deliberate breakages (remove the basename cut; stream error pages into the file; stop deleting `.part` on failure) were each caught.
The first run of that exercise showed two weak spots in my own tests, now fixed: one test wrote to a real system path when the code was broken (now confined to the temp folder),
and the `.part`-cleanup check never exercised a mid-file abort (now it does).

### Verified (AI, in the sandbox, 13 Oct 2026)
- Real **HTTPS** through the built `lucyn --download`: `raw.githubusercontent.com` (199,192 B) and a **cross-host redirect** `github.com` -> `codeload.github.com` (1,726,110 B zip): **sha256 / size identical to `curl -L`**, zip passes `unzip -t`. 404 and an unresolvable host exit 1 with the reason on stderr and no file left.
- The NDJSON pipe (second identical download became `stb_image (1).h`) and the Node client `download()` (including a `../../sneaky.c` name staying inside `downloads/`) both work.

### NOT verified
- Not run against **libcurl-impersonate** (stock libcurl only, like every sandbox session). The streaming callback is the same code path, but impersonation changes the TLS/HTTP2 stack underneath.
- Not run on the author's machine or against the author's 10 MB test file; this sandbox's network is the GitHub allowlist only.
- Download requests are navigation-style GETs. Whether a given site serves a file to that (cookies, Referer, anti-bot) is a per-site question.
- `maxBytes` counts DECODED bytes (libcurl un-gzips). There is no progress reporting and no resume (`Range`).
- **Real `blob:` URLs** (`URL.createObjectURL`, `<a download href="blob:...">`) are NOT implemented: `download` fetches http(s) URLs. That is phase 2 in `SESSION11_PLAN.md`.

## Tests

| Suite | What it covers |
|---|---|
| `js_net` (extended) | `testBlobAndFile()` added; 15/15 suites still pass (before the addendum) |
| `controller_download`, `cli_download` | see the addendum above; 17/17 suites now |

## Next: Session 11 = M10 in session labels, the painter and the UI (author's decision)

Everything below is in **`SESSION11_PLAN.md`**; read that, not this list.

0. ~~Part A: `download` command~~ **done in the addendum above.**
1. **Part B:** `paint` module: display list, `Painter`, `SoftwarePainter`, PNG output.
2. **Part C:** `layout` module: block/inline only, enough to feed the painter.
3. **Part D:** `ui` module: SDL2 window, Ghostful Mode, `screenshot` and `window` commands.

Moved to **after** Session 11 (still valid, just no longer next): streams (`ReadableStream`, `response.body`, `Blob.stream()`), owning the base
header list for header order (still needs a real Chrome capture; the standing author item 0 in `HANDOFF.md` section 6), `WebSocket`
`binaryType: 'blob'` (unblocked since `Blob` exists), canvas stub.

## Opening message for Session 11

> "Read HANDOFF.md, SESSION10.md and SESSION11_PLAN.md in the zip. The download command is already built (SESSION10 addendum). Do the painter,
> layout and UI (parts B, C, D). Keep all 17 existing suites green."
