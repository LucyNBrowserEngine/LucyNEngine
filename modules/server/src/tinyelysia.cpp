#include <lucyn/server/tinyelysia.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

extern "C" {
#include "mongoose.h"
}

namespace lucyn::server {

namespace {

constexpr std::size_t kMaxSendBacklog = 8u * 1024 * 1024;   // per client
constexpr std::size_t kMaxQueued      = 10000;              // queued outbound messages (all clients)

std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower((unsigned char)c)); return s; }
std::string toStr(const mg_str& s) { return std::string(s.buf, s.len); }

const char* reason(int code) {
    switch (code) {
        case 200: return "OK";          case 201: return "Created";      case 204: return "No Content";
        case 301: return "Moved Permanently"; case 302: return "Found";  case 304: return "Not Modified";
        case 400: return "Bad Request"; case 403: return "Forbidden";    case 404: return "Not Found";
        case 405: return "Method Not Allowed"; case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default:  return "OK";
    }
}

// "127.0.0.1:6969" / "localhost" / "[::1]:80" -> is the host part a loopback name?
bool loopbackHost(std::string h) {
    h = lower(std::move(h));
    std::string host;
    if (!h.empty() && h[0] == '[') { auto e = h.find(']'); host = e == std::string::npos ? h : h.substr(0, e + 1); }
    else host = h.substr(0, h.find(':'));
    return host == "127.0.0.1" || host == "localhost" || host == "[::1]";
}

// Origin: "http://127.0.0.1:6969" -> loopback? "null" and anything else are refused.
bool loopbackOrigin(const std::string& o) {
    std::string s = lower(o);
    auto p = s.find("://");
    if (p == std::string::npos) return false;
    std::string scheme = s.substr(0, p);
    if (scheme != "http" && scheme != "https") return false;
    std::string rest = s.substr(p + 3);
    rest = rest.substr(0, rest.find('/'));
    return loopbackHost(rest);
}

struct Route { std::string method, path; Handler fn; };
struct Outgoing { std::string wsPath; std::uint64_t clientId = 0; std::string text; };

}  // namespace

std::string Request::header(const std::string& name) const {
    std::string n = lower(name);
    for (auto& kv : headers) if (lower(kv.first) == n) return kv.second;
    return {};
}

std::string Request::param(const std::string& name) const {
    std::size_t i = 0;
    while (i <= query.size()) {
        std::size_t e = query.find('&', i);
        std::string pair = query.substr(i, e == std::string::npos ? std::string::npos : e - i);
        auto eq = pair.find('=');
        if (pair.substr(0, eq) == name) return eq == std::string::npos ? std::string() : pair.substr(eq + 1);
        if (e == std::string::npos) break;
        i = e + 1;
    }
    return {};
}

// ================================================================== Impl

struct TinyElysia::Impl {
    mg_mgr mgr;
    bool   mgrInit = false;
    mg_connection* listener = nullptr;
    unsigned long  listenerId = 0;
    std::uint16_t  port = 0;
    std::thread    th;
    std::atomic<bool> running{false};
    std::atomic<bool> guards{true};
    std::atomic<std::uint64_t> dropped{0};

    mutable std::mutex routesMu;
    std::vector<Route> routes;
    std::map<std::string, WsHandlers> wsRoutes;

    std::mutex outMu;
    std::deque<Outgoing> outbox;

    // server thread only
    std::unordered_map<unsigned long, std::string> pendingWs;           // upgrade accepted, WS_OPEN not seen yet
    std::unordered_map<unsigned long, WsClient>    clients;
    mutable std::mutex countMu;
    std::map<std::string, std::size_t> counts;                           // for clientCount() from other threads

    Impl() {}

    // lifeMu makes wake() (any thread) and mg_mgr_free() (stop) mutually exclusive: no wakeup on a freed manager.
    std::mutex lifeMu;
    void wake() { std::lock_guard<std::mutex> lk(lifeMu); if (mgrInit && listenerId) mg_wakeup(&mgr, listenerId, "x", 1); }
    void freeMgr() { std::lock_guard<std::mutex> lk(lifeMu); if (mgrInit) { mg_mgr_free(&mgr); mgrInit = false; listenerId = 0; } }

    // ---- server thread -------------------------------------------------------------------------

    const Route* match(const std::string& method, const std::string& path, bool* pathKnown) {
        const Route* best = nullptr;
        for (auto& r : routes) {
            bool hit = r.path == path ||
                       (!r.path.empty() && r.path.back() == '*' && path.compare(0, r.path.size() - 1, r.path, 0, r.path.size() - 1) == 0);
            if (!hit) continue;
            *pathKnown = true;
            if (r.method == method) best = &r;
        }
        return best;
    }

    static void sendReply(mg_connection* c, int status, const std::vector<std::pair<std::string, std::string>>& hdrs, const std::string& body) {
        std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\n";
        bool haveType = false;
        for (auto& kv : hdrs) {
            if (lower(kv.first) == "content-type") haveType = true;
            head += kv.first + ": " + kv.second + "\r\n";
        }
        if (!haveType) head += "Content-Type: text/plain; charset=utf-8\r\n";
        head += "X-Content-Type-Options: nosniff\r\n";
        head += "Cache-Control: no-store\r\n";
        head += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        mg_send(c, head.data(), head.size());
        if (!body.empty()) mg_send(c, body.data(), body.size());
        // Mongoose sets is_resp when it dispatches a request and parses no further request on this connection
        // until it is cleared. mg_http_reply() clears it; we write replies ourselves, so we must. Without this a
        // keep-alive client (libcurl, every browser) hangs on its second request.
        c->is_resp = 0;
    }

    void onHttp(mg_connection* c, mg_http_message* hm) {
        Request req;
        req.method = toStr(hm->method);
        req.path   = toStr(hm->uri);
        req.query  = toStr(hm->query);
        req.body   = toStr(hm->body);
        for (int i = 0; i < MG_MAX_HTTP_HEADERS && hm->headers[i].name.len > 0; ++i)
            req.headers.emplace_back(toStr(hm->headers[i].name), toStr(hm->headers[i].value));

        if (guards.load()) {
            std::string host = req.header("Host");
            std::string origin = req.header("Origin");
            if ((!host.empty() && !loopbackHost(host)) || (!origin.empty() && !loopbackOrigin(origin))) {
                sendReply(c, 403, {{"Content-Type", "text/plain; charset=utf-8"}}, "forbidden: LucyN's local ports only talk to loopback origins\n");
                c->is_draining = 1;
                return;
            }
        }

        // WebSocket upgrade?
        std::string upgrade = lower(req.header("Upgrade"));
        if (upgrade == "websocket") {
            bool known;
            { std::lock_guard<std::mutex> lk(routesMu); known = wsRoutes.count(req.path) > 0; }
            if (known) {
                pendingWs[c->id] = req.path;
                // NOT under routesMu: mg_ws_upgrade() fires MG_EV_WS_OPEN synchronously, and that handler locks it.
                mg_ws_upgrade(c, hm, nullptr);
                return;
            }
            sendReply(c, 404, {}, "no such websocket\n");
            return;
        }

        Handler fn;
        bool pathKnown = false;
        {
            std::lock_guard<std::mutex> lk(routesMu);
            if (const Route* r = match(req.method, req.path, &pathKnown)) fn = r->fn;
        }
        if (!fn) {
            if (pathKnown) sendReply(c, 405, {}, "method not allowed\n");
            else           sendReply(c, 404, {}, "not found\n");
            return;
        }
        Reply rep;
        try { fn(req, rep); }
        catch (const std::exception& e) { rep = Reply{}; rep.status = 500; rep.text(std::string("handler error: ") + e.what() + "\n"); }
        catch (...)                     { rep = Reply{}; rep.status = 500; rep.text("handler error\n"); }
        sendReply(c, rep.status, rep.headers, rep.body);
    }

    void sendFrame(mg_connection* c, const std::string& text) {
        if (c->send.len > kMaxSendBacklog) { dropped.fetch_add(1); return; }   // slow client: drop, never balloon memory
        mg_ws_send(c, text.data(), text.size(), WEBSOCKET_OP_TEXT);
    }

    void drain() {
        std::deque<Outgoing> todo;
        { std::lock_guard<std::mutex> lk(outMu); todo.swap(outbox); }
        for (auto& m : todo) {
            if (m.clientId) {
                for (mg_connection* c = mgr.conns; c; c = c->next)
                    if (c->id == m.clientId && c->is_websocket) { sendFrame(c, m.text); break; }
            } else {
                for (mg_connection* c = mgr.conns; c; c = c->next) {
                    if (!c->is_websocket) continue;
                    auto it = clients.find(c->id);
                    if (it != clients.end() && it->second.path == m.wsPath) sendFrame(c, m.text);
                }
            }
        }
    }

    void bump(const std::string& path, int delta) {
        std::lock_guard<std::mutex> lk(countMu);
        auto& n = counts[path];
        if (delta < 0 && n > 0) --n; else if (delta > 0) ++n;
    }

    void onEvent(mg_connection* c, int ev, void* evData) {
        switch (ev) {
            case MG_EV_HTTP_MSG:
                onHttp(c, static_cast<mg_http_message*>(evData));
                break;
            case MG_EV_WS_OPEN: {
                auto it = pendingWs.find(c->id);
                if (it == pendingWs.end()) break;
                WsClient cl{c->id, it->second};
                pendingWs.erase(it);
                clients[c->id] = cl;
                bump(cl.path, +1);
                WsHandlers h; { std::lock_guard<std::mutex> lk(routesMu); auto r = wsRoutes.find(cl.path); if (r != wsRoutes.end()) h = r->second; }
                if (h.onOpen) { try { h.onOpen(cl); } catch (...) {} }
                break;
            }
            case MG_EV_WS_MSG: {
                auto it = clients.find(c->id);
                if (it == clients.end()) break;
                auto* wm = static_cast<mg_ws_message*>(evData);
                WsHandlers h; { std::lock_guard<std::mutex> lk(routesMu); auto r = wsRoutes.find(it->second.path); if (r != wsRoutes.end()) h = r->second; }
                if (h.onMessage) {
                    try { h.onMessage(it->second, std::string(wm->data.buf, wm->data.len), (wm->flags & 15) == WEBSOCKET_OP_BINARY); } catch (...) {}
                }
                break;
            }
            case MG_EV_CLOSE: {
                pendingWs.erase(c->id);
                auto it = clients.find(c->id);
                if (it != clients.end()) {
                    WsClient cl = it->second;
                    clients.erase(it);
                    bump(cl.path, -1);
                    WsHandlers h; { std::lock_guard<std::mutex> lk(routesMu); auto r = wsRoutes.find(cl.path); if (r != wsRoutes.end()) h = r->second; }
                    if (h.onClose) { try { h.onClose(cl); } catch (...) {} }
                }
                break;
            }
            case MG_EV_WAKEUP:
                drain();
                break;
            case MG_EV_POLL:
                if (c->is_listening) drain();   // fallback: even if a wakeup is lost, delivery is at most one poll tick late
                break;
            default: break;
        }
    }

    static void trampoline(mg_connection* c, int ev, void* evData) {
        static_cast<Impl*>(c->fn_data)->onEvent(c, ev, evData);
    }
};

// ================================================================== TinyElysia

TinyElysia::TinyElysia() : p_(new Impl) {}
TinyElysia::~TinyElysia() { stop(); }

TinyElysia& TinyElysia::route(const std::string& method, const std::string& path, Handler h) {
    std::lock_guard<std::mutex> lk(p_->routesMu);
    for (auto& r : p_->routes) if (r.method == method && r.path == path) { r.fn = std::move(h); return *this; }
    p_->routes.push_back({method, path, std::move(h)});
    return *this;
}

TinyElysia& TinyElysia::ws(const std::string& path, WsHandlers h) {
    std::lock_guard<std::mutex> lk(p_->routesMu);
    p_->wsRoutes[path] = std::move(h);
    return *this;
}

bool TinyElysia::listen(std::uint16_t port, std::string* err) {
    if (p_->running) { if (err) *err = "already listening"; return false; }
    mg_mgr_init(&p_->mgr);
    p_->mgrInit = true;
    std::string url = "http://127.0.0.1:" + std::to_string(port);
    mg_connection* l = mg_http_listen(&p_->mgr, url.c_str(), &Impl::trampoline, p_.get());
    if (!l) {
        if (err) *err = "cannot listen on " + url.substr(7) + " (port in use?)";
        p_->freeMgr();
        return false;
    }
    p_->listener = l;
    p_->listenerId = l->id;
    p_->port = mg_ntohs(l->loc.port);
    if (!mg_wakeup_init(&p_->mgr)) { /* not fatal: MG_EV_POLL drains the queue every tick anyway */ }
    p_->running = true;
    p_->th = std::thread([this] {
        while (p_->running) mg_mgr_poll(&p_->mgr, 100);
    });
    return true;
}

void TinyElysia::stop() {
    if (!p_->running.exchange(false)) { p_->freeMgr(); return; }
    p_->wake();
    if (p_->th.joinable()) p_->th.join();
    p_->freeMgr();
    p_->listener = nullptr;
    p_->port = 0;
    {
        std::lock_guard<std::mutex> lk(p_->countMu);
        p_->counts.clear();
    }
}

bool TinyElysia::running() const { return p_->running; }
std::uint16_t TinyElysia::port() const { return p_->port; }

void TinyElysia::broadcast(const std::string& wsPath, const std::string& text) {
    if (!p_->running) return;
    {
        std::lock_guard<std::mutex> lk(p_->outMu);
        if (p_->outbox.size() >= kMaxQueued) { p_->outbox.pop_front(); p_->dropped.fetch_add(1); }
        p_->outbox.push_back({wsPath, 0, text});
    }
    p_->wake();
}

bool TinyElysia::send(std::uint64_t clientId, const std::string& text) {
    if (!p_->running || clientId == 0) return false;
    {
        std::lock_guard<std::mutex> lk(p_->outMu);
        if (p_->outbox.size() >= kMaxQueued) { p_->outbox.pop_front(); p_->dropped.fetch_add(1); }
        p_->outbox.push_back({std::string(), clientId, text});
    }
    p_->wake();
    return true;
}

std::size_t TinyElysia::clientCount(const std::string& wsPath) const {
    std::lock_guard<std::mutex> lk(p_->countMu);
    if (!wsPath.empty()) { auto it = p_->counts.find(wsPath); return it == p_->counts.end() ? 0 : it->second; }
    std::size_t n = 0;
    for (auto& kv : p_->counts) n += kv.second;
    return n;
}

std::uint64_t TinyElysia::droppedMessages() const { return p_->dropped.load(); }
void TinyElysia::setGuards(bool on) { p_->guards = on; }

}  // namespace lucyn::server
