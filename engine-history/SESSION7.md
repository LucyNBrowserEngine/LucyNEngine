# Session 7 — M6: eval / waitFor / waitIdle / click / XHR / HTTP POST

**Date:** 2026-10-10  
**Starting version:** `lucyn 0.5.0-m5` (12 tests passing)  
**Ending version:** `lucyn 0.6.0-m6` (12 tests passing, 0 warnings in new code)

---

## What was built

### 1. Full XMLHttpRequest implementation (`modules/js/src/xhr_bridge.cpp`)

A real XHR implementation was written from scratch in ~430 lines:

- `open(method, url, async)` — stores method, URL, async flag
- `setRequestHeader(name, value)` — accumulates request headers
- `send(body?)` — for async: spins a `std::thread` (same model as `fetch_bridge.cpp`) that calls `client.get()` or `client.post()`, writes into a shared atomic `XhrState`, and signals `done`; `pumpXhr()` in `drainJobs()` delivers the result back to JS
- `abort()` — sets `shared->aborted = true`; the worker ignores the result
- `addEventListener(type, fn)` / `removeEventListener(type, fn)` — proper event listener registry
- Properties: `readyState`, `status`, `statusText`, `responseText`, `response`, `responseURL` — all read-only getters via `JS_DefinePropertyGetSet`
- Callbacks: `onreadystatechange`, `onload`, `onerror`, `onloadend`
- `teardownXhr(ctx)` — sets `aborted` on all in-flight requests and releases JS values

Threading model: identical to `fetch_bridge.cpp`. Worker threads write into `XhrState` (atomic `done`); `pumpXhr(State&)` is called from `drainJobs()` every JS job drain and delivers completed results into JS.

**Wired into the engine:**
- `modules/js/src/window.cpp`: `installXhr(s, g)` called after `installFetch`
- `modules/js/src/host.cpp` `drainJobs()`: `pumpXhr(*this)` after `pumpFetch`
- `modules/js/src/host.cpp` `releaseAll()`: `teardownXhr(ctx)` after `teardownFetch`
- `modules/js/CMakeLists.txt`: `src/xhr_bridge.cpp` added to `add_library`

### 2. HTTP POST support in `net::HttpClient`

`modules/net/include/lucyn/net/http_client.h` and `modules/net/src/http_client.cpp`:

```cpp
// Now in lucyn::net namespace (outside HttpClient — GCC 13 requirement)
struct PostOptions {
    std::string contentType = "application/x-www-form-urlencoded";
    std::vector<std::pair<std::string,std::string>> extraHeaders;
};

// In HttpClient:
Response post(const std::string& url, const std::string& body,
              const PostOptions& opts = PostOptions{});
```

**GCC 13 caveat:** A nested struct with default member initializers cannot be used as a `= {}` default argument inside the same enclosing class (GCC bug / C++14 limitation). `PostOptions` was moved to the `lucyn::net` namespace level and any `net::HttpClient::PostOptions` references updated to `net::PostOptions`.

### 3. `eval()`, `waitFor()`, `waitIdle()`, `click()` in the controller

`modules/controller/src/controller.cpp` was refactored to support interactive post-visit commands:

#### keepAlive architecture
`QuickJsHost` is now a **persistent member** of `Controller::Impl` (`std::unique_ptr<js::QuickJsHost> jsHost_`). When `cmdVisit()` runs, it:
1. Resets and recreates `jsHost_`
2. Sets `hy.options.keepAlive = true` — this prevents `host_.detach()` at hydration end, keeping the JS context alive for subsequent commands

#### `eval(js)` command
```json
{ "id": 1, "method": "eval", "params": { "js": "document.title" } }
```
Calls `jsHost_->eval(js, &result, &error)` and returns the string result.

#### `waitFor(selector, timeoutMs?)` command
```json
{ "id": 2, "method": "waitFor", "params": { "selector": "#content", "timeout": 5000 } }
```
Polls `doc->querySelector(selector)` + `jsHost_->settle(50)` in a loop with a deadline. Returns the selector on success, error on timeout.

#### `waitIdle(timeoutMs?)` command
```json
{ "id": 3, "method": "waitIdle", "params": { "timeout": 10000 } }
```
Polls `moonbug.total()` for network silence (no in-flight requests for 300 ms). Uses the existing Moonbug capture layer.

#### `click(selector)` command
```json
{ "id": 4, "method": "click", "params": { "selector": "#submit" } }
```
Uses `jsHost_->eval()` to call `.click()` on the matched element:
```js
var el = document.querySelector(<selector>); if (!el) throw new Error('not found'); el.click();
```

#### `httpRequest` callback wired for POST
The `HostOptions::httpRequest` lambda now dispatches to `client.post()` when `method == "POST"` and `client.get()` otherwise. Content-type is extracted from request headers and passed via `PostOptions`.

### 4. Version bump
`apps/lucyn/main.cpp`: `"lucyn 0.6.0-m6"`

### 5. JS client updated
`client/src/index.js`: `waitFor(selector, timeout)`, `waitIdle(timeout)`, `click(selector)` methods added; `eval()` JSDoc updated; protocol comment bumped to M6.

---

## Bugs fixed during this session

| Error | Fix |
|---|---|
| `JS_NewClassID(&xhrClassId)` wrong signature | QuickJS-ng v0.17 requires `JS_NewClassID(JSRuntime*, JSClassID*)` |
| `JS_StrictEq` not declared | QuickJS-ng API name is `JS_IsStrictEqual` |
| Lambdas passed to `JS_NewCFunction` | C++ lambdas are not plain function pointers; replaced with named static C thunks (`xhrPropReadyState`, etc.) |
| `State` has no member `pendingXhrs` | Added `std::vector<JSValue> pendingXhrs;` to `State` in `impl.h` |
| `teardownXhr` declared but not defined | Implemented in `xhr_bridge.cpp` |
| `PostOptions` nested struct GCC error | Moved `PostOptions` outside `HttpClient` to `lucyn::net` namespace |
| `net::HttpClient::Response` / `net::HttpClient::PostOptions` references | `Response` was already at namespace level; `PostOptions` moved there — controller updated |

---

## Build and test results

```
cmake --build build -j$(nproc)   # zero errors, minor unused-variable warnings only
ctest --test-dir build            # 12/12 passed
```

All 12 test suites pass: core, profile, identity, security, net, moonbug, server, dom, dom_mutate, shell, js, controller.

---

## What M6 enables end-to-end

```
visit("https://example.com")
waitIdle()                        # wait for all async network to settle
waitFor("#main-content")          # wait for a specific element to appear
eval("document.title")            # inspect the live page state
click("#load-more")               # trigger a click (fires JS event handlers)
eval("document.querySelectorAll('.item').length")   # inspect post-click DOM
```

XHR is available to page scripts (both GET and POST), so sites that use `new XMLHttpRequest()` for data loading will work.

---

## What is NOT in M6 (next sessions)

- **Canvas / WebGL**: still unimplemented (no rendering module yet)
- **Cookies**: libcurl handles them internally but LucyN has no JS `document.cookie` bridge
- **Redirect policy / CORS headers**: not yet enforced in XHR
- **Sync XHR**: `open(method, url, false)` — the `async = false` path is a stub
- **FormData / Blob XHR body**: only plain string body supported
- **Screenshot / PDF export**: rendering is M8+
- **Profile identity in request headers**: `Accept-Language`, `Sec-CH-UA` etc. from the active persona not yet injected
- **Real TLS fingerprint verification**: still requires author's machine with curl-impersonate + tls.peet.ws

---

## Recommended next steps (M7)

1. **Author verification run:** on their own machine, `visit` a real site that uses XHR, capture with Moonbug, compare headers with real Chrome DevTools
2. **`document.cookie` bridge** — most authentication flows need it
3. **`fetch()` POST body forwarding** — currently fetch only does GET (needs same httpRequest dispatch as XHR); confirm fetch POST reaches the controller callback
4. **Redirect following in XHR** — libcurl follows redirects by default; expose `responseURL` correctly (already stubbed in `xhrGetResponseURL`)
5. **Headers in XHR response** — `getResponseHeader()` / `getAllResponseHeaders()` stub out the response headers from libcurl
6. **Profile identity headers** — inject `Accept-Language`, `User-Agent` overrides from the active `Surface` into every request

---

## Opening message for Session 8

> "Read HANDOFF.md and SESSION7.md. Here is what I got from visiting a real site with XHR: [paste lucyn output + Moonbug capture]. Next I want: [cookies / fetch POST / response headers / profile headers in requests]."
