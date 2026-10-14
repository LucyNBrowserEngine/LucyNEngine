# server - TinyElysia + the Moonbug capture server

`lucyn::server`. Everything here binds **127.0.0.1 only**. Built on vendored Mongoose 7.23
(**GPL-2.0 or commercial**: read `third_party/mongoose/VENDORED.txt` before shipping binaries).

## TinyElysia (`tinyelysia.h`)

```cpp
server::TinyElysia app;
app.get("/live",  [](const auto& req, auto& res) { res.json("{\"ok\":true}"); });
app.post("/echo", [](const auto& req, auto& res) { res.text(req.body); });
app.ws("/stream", { .onOpen = [](const server::WsClient& c) {} });
app.listen(6969);                       // 0 = any free port, read back with app.port()
app.broadcast("/stream", "text");       // any thread; queued, delivered by the server thread
```

* Handlers and WebSocket callbacks run on the server thread: keep them short. Register routes before `listen()`.
* **Guards (on by default).** A request is refused with 403 if its `Host` is not a loopback name (DNS rebinding) or its
  `Origin` is present and not a loopback origin. Browsers do **not** apply CORS to WebSockets, so without this any web page
  open in your normal browser could connect to `ws://127.0.0.1:2005` and read decrypted traffic, cookies included. No CORS
  headers are ever sent.
* Slow WebSocket clients are dropped from (never buffered without bound): >8 MiB pending => frames for that client are skipped
  and counted in `droppedMessages()`.
* Mongoose's logger is compiled out (`MG_ENABLE_LOG=0`): its default writes to **stdout**, which in `lucyn` is the JSON protocol.
* Replies are written by hand, so TinyElysia clears Mongoose's `is_resp` flag itself. Forgetting that makes every keep-alive
  client (libcurl, browsers) hang on its second request; `test_keepalive` guards it.

## CaptureServer (`capture_server.h`)

`Website traffic -> Moonbug (captures) -> NULLBYTE (signs) -> TinyElysia (serves) -> Dashboard`

| call | default port | what |
|---|---|---|
| `activate(p)` | 2005 | raw: `ws /stream` one JSON capture per frame; `GET /captures?limit=N` |
| `alsoUseNullbyte(p)` | 2006 | signed: `ws /stream` one envelope per frame; `GET /captures`; `POST /verify` |
| `serveDashboard(p)` | 6969 | `GET /` dashboard; `ws /stream` (envelopes); `GET /api/captures`, `POST /api/verify`, `GET /api/status` |
| `expose(file)` | | serve this HTML instead of the embedded dashboard (re-read per request) |

The dashboard (`dashboard/home_dashboard.html`) is embedded in the binary at build time. It renders text captured from arbitrary
websites, so it only ever uses `textContent` (never `innerHTML`) and is served with a strict CSP.

CLI: `lucyn-moonbug [--once] [url ...]` starts the three listeners, fetches urls (type more on stdin) and prints one line per capture.

Tests: `ctest -R server` (real loopback sockets with a hand-written HTTP/WebSocket client).
