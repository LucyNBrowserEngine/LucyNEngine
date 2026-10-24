# LucyN: session 2 notes (7 Oct 2026)

Read `HANDOFF.md` first (session 1), then this. This file only lists what changed.

**One-line state:** identity + network + **DOM parsing** done. Still no JS, no rendering, no app.

## What was added

### 1. `modules/dom/` (M1, new)
HTML text -> a tree you can search. Gumbo (HTML5 tree builder) parses; we copy into our own `Node` tree so no Gumbo types leak.

- `include/lucyn/dom/dom.h`: `parseHtml()`, `Document` (`title()`, `getElementById`, `getElementsByTagName`, `querySelector(All)`), `Node` (`attr`, `hasClass`, `textContent`, `outerHTML`, `innerHTML`), `dumpTree()`.
- Selectors: `tag`, `#id`, `.class`, `[a]`, `[a=v]`, compounds, descendant, `>`, comma lists, `*`. Invalid selector returns empty, never crashes.
- `tools/lucyn-dom`: `[--title|--tree|--text|--html] [-q selector] [-a attr] <file|->` and `--url <url> [-i target]` (only when built with `net`).
- `tests/test_dom.cpp`: tag soup, selectors, escaping, round-trip, garbage input, 5000-deep nesting.
- Needs `sudo apt install libgumbo-dev`. Builds standalone too.
- Root `CMakeLists.txt`: added `LUCYN_MODULE_DOM` option + `add_subdirectory(modules/dom)` (after `net`).

### 2. Bug fixed in `net`
`curl_easy_impersonate()` sends the browser's `Accept-Encoding` header but leaves decoding **off**, so servers sent gzip/br and we parsed compressed bytes (empty `<title>`). Fix: `curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "")` right after a successful impersonate call, in `modules/net/src/http_client.cpp`.
Checked on a local server: header per persona is unchanged (chrome/firefox `gzip, deflate, br, zstd`, safari `gzip, deflate, br`) and the body is decoded.
Confirmed on the author's machine: `lucyn-dom --url https://example.com --title` prints `Example Domain`.

## Verified this session
- Author's live capture of `tls.peet.ws` for chrome146 / safari184 / firefox147: UA, `sec-ch-ua`, TLS and HTTP/2 shapes look like the real browsers. Our computed `sec-ch-ua` for chrome146 equals the one actually sent.
- 37/37 profiles accepted; 4/4 ctest suites pass (profile, identity, net, dom).

## Still NOT verified
- JA4 / HTTP/2 hashes were not compared against captures of real browsers, only checked for plausible shape.
- No automated test for the decode fix (needs a local gzip server; a manual check was done).
- Deep-nesting copy is recursive: 100k-deep pages could overflow the stack.

## Facts about the author's setup
- curl-impersonate lives at `/home/Moonbug/libs/curl-impersonate/` (outside the repo), found via `LUCYN_CURL_IMPERSONATE_ROOT`. Binaries have an absolute RPATH to it, so moving it needs a rebuild. `third_party/curl-impersonate` does not exist.
- Machine is x86_64 Linux. Terminal output pasted as plain text.

## Moonbug (not built)
README: capture layer between libcurl and the engine (method, URL, headers, body, status, timing), passed through untouched. Planned home: a per-request hook inside `net::HttpClient`, with ports (2005) and dashboard later. It needs POST + cookies in `net` first.

## Next steps (pick one)
1. `lucyn-fetch --verify`: fetch an echo endpoint, diff what the server saw against `surfaceFor()`.
2. `net`: POST, cookies, then Moonbug capture hook.
3. `apps/lucyn` + `controller`: headless stdio JSON loop (`visit`, `html`, `profile`).
4. `js` (M3): QuickJS + DOM bridge, `navigator`/`screen` as native getters reading `Surface`.

## Suggested opening message
> "Read HANDOFF.md and SESSION2.md in the zip. Next I want to build <verify / net POST+cookies+Moonbug / apps/lucyn / js>."
