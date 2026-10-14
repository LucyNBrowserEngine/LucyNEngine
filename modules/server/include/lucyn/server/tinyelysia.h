// lucyn/server/tinyelysia.h - TinyElysia: LucyN's tiny internal HTTP + WebSocket server (on Mongoose).
//
// API in the spirit of Elysia.js, but pure C++ and no Bun/Node/npm:
//
//   TinyElysia app;
//   app.get("/live", [](const auto& req, auto& res) { res.json("{\"ok\":true}"); });
//   app.ws("/stream", { .onOpen = [](const auto& c) { ... } });
//   app.listen(6969);                          // binds 127.0.0.1 ONLY
//   app.broadcast("/stream", "{\"hello\":1}"); // any thread
//
// Rules:
//  * Binds to 127.0.0.1 only. There is no way to bind elsewhere.
//  * Request handlers and WebSocket callbacks run on the server thread: keep them short.
//  * broadcast()/send() are thread-safe and asynchronous (queued, delivered by the server thread).
//  * Register routes before listen().
//  * Guards (on by default): a request is refused with 403 when its Host header is not a loopback name
//    (DNS rebinding) or its Origin header is present and not a loopback origin. Browsers do NOT apply CORS
//    to WebSockets, so without this ANY web page open in the user's normal browser could connect to
//    ws://127.0.0.1:<port> and read the wiretap. No CORS headers are ever sent.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lucyn::server {

struct Request {
    std::string method;                 // "GET", "POST", ...
    std::string path;                   // "/api/captures" (no query string, not percent-decoded)
    std::string query;                  // "limit=10"
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    // Case-insensitive header lookup; "" when absent.
    std::string header(const std::string& name) const;
    // Value of ?name=... in the query string; "" when absent. (Not percent-decoded.)
    std::string param(const std::string& name) const;
};

struct Reply {
    int status = 200;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    Reply& code(int s)                                         { status = s; return *this; }
    Reply& header(const std::string& k, const std::string& v)  { headers.emplace_back(k, v); return *this; }
    Reply& json(std::string b) { body = std::move(b); return header("Content-Type", "application/json; charset=utf-8"); }
    Reply& html(std::string b) { body = std::move(b); return header("Content-Type", "text/html; charset=utf-8"); }
    Reply& text(std::string b) { body = std::move(b); return header("Content-Type", "text/plain; charset=utf-8"); }
};

using Handler = std::function<void(const Request&, Reply&)>;

struct WsClient {
    std::uint64_t id = 0;   // pass to send()
    std::string   path;     // the ws route it connected to
};

struct WsHandlers {
    std::function<void(const WsClient&)> onOpen;
    std::function<void(const WsClient&, const std::string& message, bool binary)> onMessage;
    std::function<void(const WsClient&)> onClose;
};

class TinyElysia {
public:
    TinyElysia();
    ~TinyElysia();
    TinyElysia(const TinyElysia&) = delete;
    TinyElysia& operator=(const TinyElysia&) = delete;

    // `path` is exact, or ends in '*' for a prefix match ("/static/*"). A later registration for the same
    // method+path replaces the earlier one.
    TinyElysia& route(const std::string& method, const std::string& path, Handler h);
    TinyElysia& get(const std::string& path, Handler h)  { return route("GET", path, std::move(h)); }
    TinyElysia& post(const std::string& path, Handler h) { return route("POST", path, std::move(h)); }
    TinyElysia& ws(const std::string& path, WsHandlers h);

    // Starts serving on 127.0.0.1:port (0 = pick a free port; read it back with port()).
    // Returns false and fills *err when the port is taken. One listener per instance.
    bool listen(std::uint16_t port, std::string* err = nullptr);
    void stop();                                   // idempotent; also done by the destructor
    bool running() const;
    std::uint16_t port() const;                    // 0 until listen() succeeded

    // Thread-safe, asynchronous. Text frames.
    void   broadcast(const std::string& wsPath, const std::string& text);
    bool   send(std::uint64_t clientId, const std::string& text);
    std::size_t clientCount(const std::string& wsPath = "") const;   // "" = all paths
    // Messages dropped because a client could not keep up (its send buffer was over 8 MiB) or the queue overflowed.
    std::uint64_t droppedMessages() const;

    // Turns the Host/Origin guards off. Tests only; there is no good reason in production.
    void setGuards(bool on);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace lucyn::server
