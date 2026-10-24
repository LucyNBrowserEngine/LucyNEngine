# Session 9: M8 — inline on* attributes + WebSocket

Goal: two items from SESSION8.md's "Recommended next steps (M8)" list — inline `on*` attributes and `WebSocket`.

**Result: both implemented and tested; 16/16 test suites pass (15 from M7 + 1 new).**

## What changed

### `modules/js/src/prelude/p9_publish.js`
- Added `compileInlineHandlers()` — runs at `stage === 'parsed'` (before any page `<script>` fires).
  - Iterates all elements, finds `on*` attributes, compiles them with `new Function('event', code)` — same scoping as browsers.
  - `<body>`/`<frameset>` events in `WINDOW_BODY_EVENTS` (load, resize, hashchange, etc.) delegate to `window`; all others bind to the element as `this`.
  - Syntax errors are silently swallowed (matches browser behaviour).
- Published `WebSocket`, `CloseEvent`, `MessageEvent` in the global class map.

### `modules/js/src/prelude/p5_websocket.js` (NEW)
- Full `WebSocket` class extending `EventTarget`.
  - Constructor: `n.wsConnect(url, protocolStr)` → id; then `n.wsSetCallbacks(id, onopen, onmessage, onclose, onerror)`.
  - If `n.wsAvailable` is false or id = 0: fires a synthetic error + close via `setTimeout(0)`.
  - `send(data)`: checks `readyState`, calls `n.wsSend(id, data, binary)` where binary = `instanceof ArrayBuffer` or `ArrayBufferView`.
  - `close(code, reason)`: sets `CLOSING`, calls `n.wsClose`.
  - Static constants: `CONNECTING=0`, `OPEN=1`, `CLOSING=2`, `CLOSED=3`.
  - `onopen`/`onmessage`/`onclose`/`onerror` as settable properties forwarding to `addEventListener`.
- `CloseEvent extends Event` — adds `wasClean`, `code`, `reason`.
- `MessageEvent extends Event` — adds `data`, `origin`, `lastEventId`, `source`, `ports`.

### `modules/js/src/websocket_bridge.cpp` (NEW)
- Mongoose-based WebSocket client; `lucyn::mongoose` added as a PRIVATE dep to `lucyn_js`.
- **Threading**: a single background thread owns the `mg_mgr` and calls `mg_mgr_poll` at 50 ms intervals. The JS context is never touched from the thread. Commands (CONNECT, SEND, CLOSE) flow JS→thread through a mutex-protected command queue; events (Open, Message, Close, Error) flow thread→JS through a matching event queue, drained by `pumpWs` on the JS thread.
- **CONNECT encoding**: negative id in the command queue distinguishes a new connection from a SEND/CLOSE, avoiding a separate queue.
- **Mongoose event handler** (`wsHandler`): handles `MG_EV_WS_OPEN`, `MG_EV_WS_MSG`, `MG_EV_WS_CTL` (close frame), `MG_EV_ERROR`, `MG_EV_CLOSE`.
- **Natives on `n`**: `wsConnect`, `wsSend`, `wsClose`, `wsSetCallbacks`, `wsForget`, `wsAvailable`.
- Public API: `installWebSocket(State&, JSValueConst n_obj)`, `pumpWs(State&)`, `teardownWs(JSContext*)`.

### `modules/js/src/impl.h`
- Declared `installWebSocket`, `pumpWs`, `teardownWs`.

### `modules/js/src/host.cpp`
- `drainJobs`: added `pumpWs(*this)` after `pumpFetch`.
- `releaseAll`: added `teardownWs(ctx)` after `teardownStorage`.

### `modules/js/src/window.cpp`
- Called `installWebSocket(s, n)` after `installFetch`.

### `modules/js/CMakeLists.txt`
- Added `third_party/mongoose` target (guarded `if(NOT TARGET lucyn::mongoose)`).
- Added `src/websocket_bridge.cpp` to sources.
- Added `lucyn::mongoose` to PRIVATE link libs.

### `modules/controller/tests/test_controller.cpp`
- `test_inline_on_attributes()`: page with `<button onclick="document.body.dataset.clicked='1'">`, asserts the attribute compiled and fires.
- `test_websocket()`: TinyElysia echo server at `/echo` (`srv.ws`), page connects via `new WebSocket(url)`, stores `data-open` and `data-msg` on `<body>`; asserts onopen fired and echo round-trip completes.

## Decisions

- **50 ms command latency** is acceptable (no wakeup pipe needed; avoids an extra dummy mongoose connection).
- **Mongoose as PRIVATE dep**: embedded directly in `websocket_bridge.cpp`, not wired through `HostOptions`, keeps the WebSocket implementation self-contained.
- **Binary send**: `ArrayBuffer` / `ArrayBufferView` goes as `WEBSOCKET_OP_BINARY`; strings as `WEBSOCKET_OP_TEXT`.

## Known limitations

- Only client-side WebSocket (no `WebSocketServer` for pages — not needed for a browser engine).
- `binaryType` (`'blob'` / `'arraybuffer'`) is not implemented; incoming binary frames always arrive as `ArrayBuffer`.
- Reconnection and per-message compression are not implemented.
- Not tested against a remote WebSocket server (sandbox network; loopback only).

## Tests

| Suite | What it covers |
|---|---|
| `controller` (extended) | `test_inline_on_attributes`, `test_websocket` added; all 16 pass |

## Recommended next steps (M9)

0. **Author:** real-site probe (HANDOFF section 6 item 0), Moonbug capture vs Chrome DevTools header comparison.
1. Persist the cookie jar per identity (`cookies: true` client option).
2. Own the base header list (`CURLOPT_HTTPBASEHEADER`) to fix header order for page-initiated requests.
3. `Blob`/`File` + multipart file parts; streams for `response.body`.
4. `binaryType: 'arraybuffer'` already works; add `binaryType: 'blob'` (needs `Blob` first).
5. Canvas stub (`<canvas>` + 2D context, no real rendering — just enough that `canvas.getContext('2d')` doesn't throw).

## Opening message for Session 10

> "Read HANDOFF.md and SESSION9.md in the zip. Here is a real-site capture: [paste]. Next I want: [cookie persistence / header order / Blob+File / canvas]."
