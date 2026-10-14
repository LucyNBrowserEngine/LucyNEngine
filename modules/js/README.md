# js - QuickJS and the DOM bridge

The real `shell::ScriptHost`. Give it to the `Hydrator` and injected scripts run, the site's own `<script>`s run, timers fire, and
`before != after`.

```cpp
js::HostOptions o;
o.persona = [&] { /* IdentityManager: surface() + identity()->device */ };   // what navigator / screen report
js::QuickJsHost host(o);
shell::Hydrator hy(fetcher, scripts, host);
auto r = hy.hydrate(url);
host.pageErrors();   // uncaught errors from timers, listeners, page scripts
host.notes();        // things LucyN could not do and says so
```

Build: `cmake -S modules/js -B b && cmake --build b && ctest --test-dir b` (needs `libgumbo-dev`; QuickJS-ng is vendored).
`lucyn-shell` links it automatically (root CMake), `--console` prints the page's console to stderr.

## The rules (do not break these)

1. **`lucyn` is a closure parameter, never a global.** Every user script is wrapped as `(function(lucyn){<source>\n})`, the opening on
   the script's own first line so error line numbers stay the author's. The object is frozen and non-enumerable. Consequence: a
   script's top-level `var`/`function` stay private; share with the page via `window.x = ...`.
2. **`detach()` never touches the Document or the hooks.** Either may already be gone (see `Live` in `impl.h`). The DOM mutation
   listener the host leaves on a Document that outlives it checks a shared token and becomes a no-op.
3. **Hooks are called by name at call time**, never cached: the Hydrator removes them after the run.
4. **No JS runs inside a DOM callback.** MutationObserver records are queued and delivered as a microtask; dynamically inserted
   `<script>`s are queued and run as a microtask. Otherwise a handler could mutate the tree in the middle of the DOM's own operation.
5. **A script cannot take the engine down.** Per-script / per-timer / per-drain watchdog (`scriptTimeoutMs`, default 10 s, not
   catchable by `try/catch`), memory cap (256 MB), stack cap. A failing script is reported and the next one runs.
6. **Everything the prelude defines looks native** to `Function.prototype.toString` (`function add() { [native code] }`).
   Define new prelude functions through `method()` / `accessor()` / `markClass()`, not by assigning.
7. **Never invent an identity.** No persona callback means `navigator`/`screen` exist but are blank. Objects that a page can compare
   (`navigator`, `navigator.languages`, `navigator.userAgentData`, `.brands`) are created once and cached.

## Layout

| File | Job |
|---|---|
| `src/host.cpp` | attach/detach, `runUserScript`, `runPageScripts`, `settle`, watchdog, timer loop, error text, the `lucyn` object |
| `src/dom_bridge.cpp` | native `Node/Element/Text/Comment/Document` over `lucyn::dom`; `Node*` -> wrapper cache (`el === el`) |
| `src/window.cpp` | timers, `queueMicrotask`, `console`, the private `__n` object, persona -> JSON |
| `src/prelude/p*.js` | the JS half (events, `classList`, `style`, `dataset`, per-tag classes, `MutationObserver`, `location`, `history`, `navigator`, `screen`, `URL`, ...). Embedded at build time by `embed_prelude.cmake` (ASCII only) |

`__n` (the private native object) is a parameter of the prelude function: it is not a property of any global.

## Lifecycle

`attach` (readyState `loading`) -> start scripts -> site mounted -> `runPageScripts` (inline scripts in document order, then
`readystatechange`/`DOMContentLoaded`, readyState `interactive`) -> `settle` (readyState `complete`, `readystatechange`, `load`, then
the event loop) -> end scripts -> `detach`.

`settle(ms)` returns true when the page is idle. One-shot `setTimeout`s keep it busy; `setInterval` and `requestAnimationFrame` run
when due but never hold it open. A one-shot due more than 250 ms after the deadline does not count (a 60 s keep-alive), but a tight
chain (`setTimeout(tick, 5)`) does, so a page that never stops working returns false.

## Implemented

DOM: tree, attributes, `innerHTML`/`outerHTML`, `insertAdjacent*`, `append/prepend/before/after/replaceWith/remove`, `querySelector(All)`,
`matches`, `closest`, `isEqualNode`, `cloneNode`, `importNode`, `classList`, inline `style`, `dataset`, per-tag classes with reflected properties
(`a.href` resolved, `img.src`, `input.value`, ...), `document.title/readyState/currentScript/URL/cookie`.
Events: capture/bubble/target, `once`, `stopPropagation`, `stopImmediatePropagation`, `on<type>` handlers, `Event/CustomEvent/UIEvent/
MouseEvent/KeyboardEvent/FocusEvent/InputEvent` (`isTrusted` is false), `el.click()`. `MutationObserver` (childList, attributes + filter,
characterData, subtree). Timers, `queueMicrotask`, Promises. `location`, `history` (`pushState`/`replaceState` update `location`),
`URL`, `URLSearchParams`, `atob`/`btoa`, `DOMException`, `matchMedia` (never matches), `getComputedStyle` (inline only), `performance.now`.
`navigator` (UA, platform, vendor, languages, cores, `deviceMemory` capped at 8, `webdriver === false`, `userAgentData` incl.
`getHighEntropyValues`) and `screen` come from the persona. `crypto.getRandomValues` supports integer typed arrays up to 65536 bytes;
`crypto.randomUUID` is also available. `IntersectionObserver` asynchronously reports observed elements as intersecting because no layout exists yet.

**Networking (M7).** `fetch`, `Headers`, `Request`, `Response`, `AbortController`/`AbortSignal`, `FormData`, `XMLHttpRequest`
(+ `upload`, `ProgressEvent`), `TextEncoder`/`TextDecoder` and `document.cookie` are real implementations. The classes are JavaScript in
`src/prelude/p4_net.js`; they reach the network through a handful of natives in `src/fetch_bridge.cpp` (`n.httpSend`, `n.httpSendSync`,
`n.httpCancel`, `n.utf8Encode/Decode`, `n.cookieGet/Set`). Any method and body (string, `URLSearchParams`, `FormData`, `Blob`, `ArrayBuffer`/typed
array) is sent; response bodies are kept as bytes, so binary data survives and `text()` strips a BOM. XHR has the full event sequence
(`readystatechange` 1..4, `loadstart`, `progress`, `load`/`error`/`abort`/`timeout`, `loadend`), `on*` properties and listeners,
`getResponseHeader`/`getAllResponseHeaders` (`Set-Cookie` is hidden from pages, as in a browser), `responseURL`, `responseType`
`''|text|json|arraybuffer|blob`, `timeout`, `abort()` and synchronous mode. `statusText` is the server's reason phrase, empty on HTTP/2+ (as in Chrome).

**`Blob`/`File` (M9).** In-memory only (no streams): a `Blob` is parts (`string`/`ArrayBuffer`/typed array/`Blob`) concatenated once at
construction time into one buffer. `size`, `type` (lower-cased, ASCII-only — anything else collapses to `""`, matching browsers),
`slice()`, `arrayBuffer()`/`text()`/`bytes()` (all resolve immediately, nothing streams). `File extends Blob` adds `name`/`lastModified`/
`webkitRelativePath` (`""`); the constructor requires a name. Wired everywhere a body or a response can carry one: `fetch`/`XHR` request
bodies, `FormData.append/set(name, blobOrFile, filename?)` (a bare `Blob` becomes a `File` entry named `"blob"` by default) with real
multipart file parts (`serializeForm` now builds raw bytes, not a string, so binary parts survive), `Response.blob()`/`Request.blob()`,
and XHR `responseType: 'blob'`. File `<input>` elements still do not produce `File`s (no file picker exists); `collectForm()` still skips
them, same as before.

**`WebSocket` + inline `on*` (M8).** `WebSocket`/`CloseEvent`/`MessageEvent` are real (Mongoose-based client, binary frames as
`ArrayBuffer`; no `binaryType: 'blob'`, no reconnection/compression). Inline `on*=""` attributes (`<button onclick="...">`) are compiled
at the `parsed` stage, before any page `<script>` runs, same scoping as a browser (`new Function('event', code)`, element as `this`,
window-delegated for `<body>` events).

The host decides what the network is. `HostOptions::httpFetch` receives the whole request **with page context** (`pageUrl`, `mode`, `dest`),
which is what lets the host send a browser's subresource headers (`Sec-Fetch-*`, `Referer`, `Origin`) and attach cookies; it takes
precedence over the older `httpRequest` (method/body/headers) and `httpGet` (GET only), which still work. `cookieGet`/`cookieSet` back
`document.cookie`; without them it is a small per-page in-memory jar. `<script src>` is fetched through the same callback (`mode: no-cors`,
`dest: script`) and executed for classic scripts. `settle()` waits for timers, **not** for requests in flight (that is `waitIdle`'s job
in the controller). In-memory `CacheStorage` supports named caches and the usual `open`/`match`/`put`/`delete`/`keys` operations; cache data lasts only
for the attached page and does not implement ServiceWorker/offline behavior.


## NOT implemented (and what happens instead)

- `indexedDB`, canvas, WebGL, audio, workers: **undefined**. A page that uses them throws a `ReferenceError`, which is reported in `pageErrors()`.
- Fetch/XHR gaps: no streams (`response.body` is `null`; `Blob`/`File` have no `.stream()`), XHR `responseType: 'document'` gives `null`,
  no CORS enforcement or preflight (a cross-origin read just works), `redirect: 'manual'|'error'` and `credentials: 'omit'` are ignored
  (redirects are always followed, cookies always sent), `responseXML` is `null`. `Request`/`Response` and CacheStorage are compatibility
  subsets, not full browser implementations. File `<input>` elements never populate `FormData` with a `File` (no file picker exists).
- CacheStorage is in-memory only. `IntersectionObserver` reports intersections without real geometry; it is a visibility-hook compatibility stub.
- `<script src>` classic scripts are fetched, but `<script type=module>` is skipped (noted). `document.write` is absent.
- The shell's own `inject.js` / `hydrator.js` are recognized as shell-owned and not executed or fetched as page scripts.
- No `DocumentFragment`, `<template>`, Shadow DOM, `customElements`. `childNodes`/`children`/`querySelectorAll` return plain Arrays, not live lists.
- Selectors are whatever `lucyn::dom` supports (no `:not`, `:first-child`, `[a^=b]`). An unsupported selector matches nothing, silently.
- No layout: `offsetWidth`, `getBoundingClientRect()` etc. are honestly zero; `getComputedStyle` has no cascade.
- `navigator.plugins/mimeTypes/connection/getBattery/mediaDevices`, `window.chrome`, `Notification`, `Intl` quirks: not done.
- Navigation (`location.href = other`, `assign`, `reload`) is **ignored and noted**; only hash changes apply.
- `window.innerHeight` is `availHeight - 85`: an invented constant for browser chrome. Replace it when real UI exists.
- Stack-trace/error text is QuickJS's, not Chrome-shaped.
- After a runaway microtask loop the queue is cut off per drain but not emptied (QuickJS has no API for that); the next drain cuts it again.
