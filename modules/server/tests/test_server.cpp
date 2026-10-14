// test_server.cpp - TinyElysia + CaptureServer, driven over real loopback sockets (a hand-rolled HTTP and
// WebSocket client below: the actual wire protocol a browser speaks). No external network.
// Uses CHECK (not assert) so it still checks in Release builds. POSIX only (skipped on Windows).
#include <cstdio>

#ifdef _WIN32
int main() { std::puts("SKIP: server tests use POSIX sockets"); return 0; }
#else

#include <lucyn/net/http_client.h>
#include <lucyn/net/moonbug.h>
#include <lucyn/security/nullbyte.h>
#include <lucyn/server/capture_server.h>
#include <lucyn/server/tinyelysia.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

using namespace lucyn;
using namespace std::chrono_literals;

// ------------------------------------------------------------------ tiny clients

static int dial(int port, int timeoutMs = 3000) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(uint16_t(port));
    if (connect(fd, (sockaddr*)&a, sizeof a) != 0) { close(fd); return -1; }
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

static bool sendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) { ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL); if (n <= 0) return false; off += size_t(n); }
    return true;
}

struct HttpResp { int status = 0; std::string head, body; std::string header(const std::string& n) const {
    std::string lowHead = head; for (auto& c : lowHead) c = char(tolower((unsigned char)c));
    std::string key = "\r\n" + n + ":"; for (auto& c : key) c = char(tolower((unsigned char)c));
    auto p = lowHead.find(key); if (p == std::string::npos) return "";
    p += key.size(); while (p < head.size() && head[p] == ' ') ++p;
    return head.substr(p, head.find("\r\n", p) - p);
} };

// One request, one response (Connection: close semantics: we read until the body is complete).
static HttpResp http(int port, const std::string& method, const std::string& path, const std::string& body = "",
                     const std::vector<std::string>& extra = {}, const std::string& host = "") {
    HttpResp r;
    int fd = dial(port);
    if (fd < 0) return r;
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + (host.empty() ? "127.0.0.1:" + std::to_string(port) : host) + "\r\n";
    for (auto& h : extra) req += h + "\r\n";
    if (!body.empty() || method == "POST") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    sendAll(fd, req);
    std::string buf; char tmp[8192];
    size_t need = std::string::npos, headEnd = std::string::npos;
    for (;;) {
        ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) break;
        buf.append(tmp, size_t(n));
        if (headEnd == std::string::npos && (headEnd = buf.find("\r\n\r\n")) != std::string::npos) {
            r.head = buf.substr(0, headEnd);
            auto cl = r.header("Content-Length");
            need = headEnd + 4 + (cl.empty() ? 0 : size_t(std::stoul(cl)));
        }
        if (need != std::string::npos && buf.size() >= need) break;
    }
    close(fd);
    if (headEnd == std::string::npos) return r;
    r.status = std::atoi(buf.c_str() + 9);
    r.body = buf.substr(headEnd + 4);
    return r;
}

struct WsConn {
    int fd = -1;
    int status = 0;                 // handshake status (101 = upgraded)
    ~WsConn() { if (fd >= 0) close(fd); }

    bool open(int port, const std::string& path, const std::vector<std::string>& extra = {}) {
        fd = dial(port);
        if (fd < 0) return false;
        std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";
        for (auto& h : extra) req += h + "\r\n";
        req += "\r\n";
        sendAll(fd, req);
        std::string buf; char c;
        while (buf.find("\r\n\r\n") == std::string::npos) { if (recv(fd, &c, 1, 0) != 1) return false; buf += c; }
        status = std::atoi(buf.c_str() + 9);
        return status == 101;
    }

    bool readn(char* p, size_t n) { while (n) { ssize_t k = recv(fd, p, n, 0); if (k <= 0) return false; p += k; n -= size_t(k); } return true; }

    // Next text frame, or "" on timeout / close.
    std::string readText() {
        for (;;) {
            unsigned char h[2];
            if (!readn((char*)h, 2)) return "";
            int op = h[0] & 15; size_t len = h[1] & 127;
            if (len == 126) { unsigned char e[2]; if (!readn((char*)e, 2)) return ""; len = size_t(e[0]) << 8 | e[1]; }
            else if (len == 127) { unsigned char e[8]; if (!readn((char*)e, 8)) return ""; len = 0; for (int i = 0; i < 8; ++i) len = len << 8 | e[i]; }
            std::string p(len, '\0');
            if (len && !readn(&p[0], len)) return "";
            if (op == 1) return p;
            if (op == 8) return "";
            // ping/pong/other: skip
        }
    }

    void sendText(const std::string& s) {   // clients MUST mask
        std::string f; f += char(0x81);
        unsigned char mask[4] = {1, 2, 3, 4};
        if (s.size() < 126) f += char(0x80 | s.size());
        else { f += char(0x80 | 126); f += char(s.size() >> 8); f += char(s.size() & 255); }
        f.append((char*)mask, 4);
        for (size_t i = 0; i < s.size(); ++i) f += char(s[i] ^ mask[i & 3]);
        sendAll(fd, f);
    }
};

static bool waitFor(const std::function<bool()>& pred, int ms = 3000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) { if (pred()) return true; std::this_thread::sleep_for(5ms); }
    return pred();
}

// ------------------------------------------------------------------ TinyElysia

static void test_http_basics() {
    server::TinyElysia app;
    app.get("/hi", [](const server::Request& q, server::Reply& r) {
        r.json("{\"m\":\"" + q.method + "\",\"limit\":\"" + q.param("limit") + "\",\"x\":\"" + q.header("x-test") + "\"}");
    });
    app.post("/echo", [](const server::Request& q, server::Reply& r) { r.text(q.body).code(201).header("X-Seen", q.path); });
    app.get("/static/*", [](const server::Request& q, server::Reply& r) { r.text("static:" + q.path); });
    app.get("/boom", [](const server::Request&, server::Reply&) { throw std::runtime_error("kaboom"); });
    app.get("/dup", [](const server::Request&, server::Reply& r) { r.text("first"); });
    app.get("/dup", [](const server::Request&, server::Reply& r) { r.text("second"); });   // replaces

    std::string err;
    CHECK(app.listen(0, &err));
    int port = app.port();
    CHECK(port > 0 && app.running());

    auto r = http(port, "GET", "/hi?limit=42&a=b", "", {"X-Test: yes"});
    CHECK(r.status == 200);
    CHECK(r.body == "{\"m\":\"GET\",\"limit\":\"42\",\"x\":\"yes\"}");
    CHECK(r.header("Content-Type").find("application/json") == 0);
    CHECK(r.header("X-Content-Type-Options") == "nosniff");
    CHECK(r.header("Cache-Control") == "no-store");
    CHECK(r.header("Access-Control-Allow-Origin").empty());                       // never any CORS headers

    auto p = http(port, "POST", "/echo", std::string("bin\0ary body", 12));
    CHECK(p.status == 201 && p.body == std::string("bin\0ary body", 12) && p.header("X-Seen") == "/echo");

    CHECK(http(port, "GET", "/static/a/b.js").body == "static:/static/a/b.js");
    CHECK(http(port, "GET", "/nope").status == 404);
    CHECK(http(port, "POST", "/hi").status == 405);                                // path exists, method does not
    auto b = http(port, "GET", "/boom");
    CHECK(b.status == 500 && b.body.find("kaboom") != std::string::npos);
    CHECK(http(port, "GET", "/hi").status == 200);                                // server survived the throw
    CHECK(http(port, "GET", "/dup").body == "second");

    // port in use is reported, not crashed
    server::TinyElysia other;
    std::string e2;
    CHECK(!other.listen(uint16_t(port), &e2) && e2.find("cannot listen") != std::string::npos);
    CHECK(!app.listen(0, &e2));                                                   // second listen on the same instance
    app.stop(); app.stop();                                                       // idempotent
    CHECK(!app.running() && app.port() == 0);
    CHECK(dial(port) < 0);                                                        // really closed
    CHECK(app.listen(0, &err) && http(app.port(), "GET", "/hi").status == 200);   // can be restarted
    std::puts("PASS: http_basics");
}

// Regression: Mongoose parses no further request on a connection while is_resp is set, and only
// mg_http_reply() clears it. Replies written by hand must clear it too, or keep-alive clients hang.
static void test_keepalive() {
    server::TinyElysia app;
    app.get("/n", [](const server::Request& q, server::Reply& r) { r.text("reply-" + q.param("i")); });
    CHECK(app.listen(0));
    int fd = dial(app.port(), 2000);
    CHECK(fd >= 0);
    bool allOk = true;
    for (int i = 0; i < 5 && allOk; ++i) {                 // five requests down ONE connection
        sendAll(fd, "GET /n?i=" + std::to_string(i) + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
        std::string buf; char c;
        while (buf.find("\r\n\r\n") == std::string::npos) { if (recv(fd, &c, 1, 0) != 1) { allOk = false; break; } buf += c; }
        if (!allOk) break;
        std::string want = "reply-" + std::to_string(i), body;
        while (body.size() < want.size()) { if (recv(fd, &c, 1, 0) != 1) { allOk = false; break; } body += c; }
        if (body != want) allOk = false;
    }
    CHECK(allOk);
    close(fd);
    std::puts("PASS: keepalive");
}

static void test_guards() {
    server::TinyElysia app;
    app.get("/secret", [](const server::Request&, server::Reply& r) { r.text("captured cookies live here"); });
    app.ws("/stream", {});
    CHECK(app.listen(0));
    int port = app.port();

    CHECK(http(port, "GET", "/secret").status == 200);                                           // plain client: fine
    CHECK(http(port, "GET", "/secret", "", {"Origin: http://127.0.0.1:6969"}).status == 200);    // our own dashboard origin
    CHECK(http(port, "GET", "/secret", "", {"Origin: http://localhost:3000"}).status == 200);
    CHECK(http(port, "GET", "/secret", "", {"Origin: https://evil.example"}).status == 403);     // a random web page
    CHECK(http(port, "GET", "/secret", "", {"Origin: null"}).status == 403);                     // sandboxed iframe / file://
    CHECK(http(port, "GET", "/secret", "", {"Origin: http://127.0.0.1.evil.example"}).status == 403);   // look-alike host
    CHECK(http(port, "GET", "/secret", "", {}, "evil.example").status == 403);                   // DNS rebinding: Host is not loopback
    CHECK(http(port, "GET", "/secret", "", {}, "localhost:" + std::to_string(port)).status == 200);
    CHECK(http(port, "GET", "/secret").body == "captured cookies live here");

    WsConn bad;
    CHECK(!bad.open(port, "/stream", {"Origin: https://evil.example"}) && bad.status == 403);    // WebSocket ignores CORS: guard is the defence
    WsConn good;
    CHECK(good.open(port, "/stream", {"Origin: http://127.0.0.1:" + std::to_string(port)}) && good.status == 101);
    WsConn none;
    CHECK(none.open(port, "/nonexistent") == false && none.status == 404);

    app.setGuards(false);
    CHECK(http(port, "GET", "/secret", "", {"Origin: https://evil.example"}).status == 200);     // explicit opt-out works
    std::puts("PASS: guards");
}

static void test_websocket() {
    server::TinyElysia app;
    std::mutex mu;
    std::vector<std::string> events;
    std::vector<uint64_t> ids;
    server::WsHandlers h;
    h.onOpen = [&](const server::WsClient& c) { std::lock_guard<std::mutex> l(mu); events.push_back("open:" + c.path); ids.push_back(c.id); };
    h.onMessage = [&](const server::WsClient& c, const std::string& m, bool bin) { std::lock_guard<std::mutex> l(mu); events.push_back("msg:" + m + (bin ? "(bin)" : "")); (void)c; };
    h.onClose = [&](const server::WsClient& c) { std::lock_guard<std::mutex> l(mu); events.push_back("close:" + c.path); };
    app.ws("/stream", h);
    app.ws("/other", {});
    CHECK(app.listen(0));
    int port = app.port();

    WsConn a, b, o;
    CHECK(a.open(port, "/stream") && b.open(port, "/stream") && o.open(port, "/other"));
    CHECK(waitFor([&] { return app.clientCount("/stream") == 2 && app.clientCount("/other") == 1 && app.clientCount() == 3; }));

    app.broadcast("/stream", "hello-all");
    CHECK(a.readText() == "hello-all");
    CHECK(b.readText() == "hello-all");

    app.broadcast("/other", "only-other");                                       // path isolation
    CHECK(o.readText() == "only-other");

    // big frame (needs the 16-bit length form) and unicode survive intact
    std::string big(70000, 'x'), uni = "caf\xc3\xa9 \xf0\x9f\x91\xbb";
    app.broadcast("/stream", big);
    CHECK(a.readText() == big);
    CHECK(b.readText() == big);
    app.broadcast("/stream", uni);
    CHECK(a.readText() == uni);
    b.readText();

    // targeted send
    uint64_t firstId; { std::lock_guard<std::mutex> l(mu); firstId = ids.at(0); }
    CHECK(app.send(firstId, "just-you"));
    CHECK(a.readText() == "just-you");
    CHECK(!app.send(0, "nobody"));

    // client -> server frame (masked), incl. a message that needs extended length
    a.sendText("ping-from-client");
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(mu); for (auto& e : events) if (e == "msg:ping-from-client") return true; return false; }));

    // ordering: 200 broadcasts arrive complete and in order
    for (int i = 0; i < 200; ++i) app.broadcast("/stream", "n" + std::to_string(i));
    bool ordered = true;
    for (int i = 0; i < 200; ++i) if (b.readText() != "n" + std::to_string(i)) { ordered = false; break; }
    CHECK(ordered);

    // close bookkeeping
    { WsConn tmp; CHECK(tmp.open(port, "/stream")); CHECK(waitFor([&] { return app.clientCount("/stream") == 3; })); }
    CHECK(waitFor([&] { return app.clientCount("/stream") == 2; }));
    {   std::lock_guard<std::mutex> l(mu);
        int opens = 0, closes = 0;
        for (auto& e : events) { if (e == "open:/stream") ++opens; if (e == "close:/stream") ++closes; }
        CHECK(opens == 3 && closes == 1);
    }

    // many threads broadcasting while clients come and go, then stop() while busy: no crash, no hang
    std::atomic<bool> go{true};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) ts.emplace_back([&] { while (go) app.broadcast("/stream", "storm"); });
    for (int i = 0; i < 5; ++i) { WsConn c; c.open(port, "/stream"); c.readText(); }
    go = false;
    for (auto& t : ts) t.join();
    app.stop();
    app.broadcast("/stream", "after-stop");                                      // harmless no-op
    CHECK(!app.running());
    std::puts("PASS: websocket");
}

// ------------------------------------------------------------------ CaptureServer end to end

static std::string field(const std::string& json, const std::string& key) {   // crude: "key":123 or "key":"str"
    auto p = json.find("\"" + key + "\":");
    if (p == std::string::npos) return "";
    p += key.size() + 3;
    if (json[p] == '"') { auto e = json.find('"', p + 1); return json.substr(p + 1, e - p - 1); }
    auto e = json.find_first_of(",}", p);
    return json.substr(p, e - p);
}

static void test_capture_server() {
    // The "internet": another TinyElysia that we fetch from, so the whole path is exercised offline.
    server::TinyElysia origin;
    origin.get("/page", [](const server::Request&, server::Reply& r) { r.html("<title>Origin</title><h1>caf\xc3\xa9</h1>"); });
    CHECK(origin.listen(0));
    const std::string pageUrl = "http://127.0.0.1:" + std::to_string(origin.port()) + "/page";

    profile::Switcher sw("");
    net::HttpClient client(sw);
    net::Moonbug mb;
    client.setMoonbug(&mb);
    security::Nullbyte nb(std::string(32, 'K'));
    std::atomic<bool> go{true};
    std::thread bg;           // keeps fetching while the CaptureServer is destroyed

    {
        server::CaptureServer cs(mb, nb);
        std::string err;
        cs.expose("");
        CHECK(cs.activate(0, &err) && cs.alsoUseNullbyte(0, &err) && cs.serveDashboard(0, &err));
        int rawP = cs.rawPort(), sigP = cs.signedPort(), dashP = cs.dashboardPort();
        CHECK(rawP > 0 && sigP > 0 && dashP > 0 && rawP != sigP && sigP != dashP);
        CHECK(!cs.activate(0, &err) && err.find("already") != std::string::npos);

        // an early capture BEFORE any client joins: must show up in history for late joiners
        client.get(pageUrl);
        CHECK(mb.total() == 1);

        WsConn raw, signedC, dash;
        CHECK(raw.open(rawP, "/stream") && signedC.open(sigP, "/stream") && dash.open(dashP, "/stream"));
        std::this_thread::sleep_for(50ms);   // let the three clients register

        client.get(pageUrl);                                                      // live capture #2

        std::string rawMsg = raw.readText(), sigMsg = signedC.readText(), dashMsg = dash.readText();
        CHECK(!rawMsg.empty() && !sigMsg.empty() && !dashMsg.empty());
        CHECK(field(rawMsg, "id") == "2" && field(rawMsg, "status") == "200" && field(rawMsg, "url") == pageUrl);
        CHECK(rawMsg.find("caf\xc3\xa9") != std::string::npos);                    // unicode body intact on the wire
        CHECK(rawMsg.find("\"bodyEncoding\":\"utf8\"") != std::string::npos);

        // the signed stream carries the SAME capture, sealed; and it verifies, and it IS the raw JSON
        auto opened = nb.open(sigMsg);
        CHECK(opened.ok && opened.payload == rawMsg);
        CHECK(nb.open(dashMsg).ok);
        CHECK(!security::Nullbyte(std::string(32, 'X')).open(sigMsg).ok);          // a different key cannot

        // verify endpoints: valid, tampered, garbage
        auto v1 = http(sigP, "POST", "/verify", sigMsg);
        CHECK(v1.status == 200 && v1.body.find("\"ok\":true") != std::string::npos);
        std::string tampered = sigMsg;
        auto at = tampered.find("200");
        CHECK(at != std::string::npos);
        tampered.replace(at, 3, "404");
        auto v2 = http(dashP, "POST", "/api/verify", tampered);
        CHECK(v2.status == 200 && v2.body.find("\"ok\":false") != std::string::npos && v2.body.find("\"parsed\":true") != std::string::npos);
        auto v3 = http(sigP, "POST", "/verify", "not json at all");
        CHECK(v3.status == 200 && v3.body.find("\"ok\":false") != std::string::npos && v3.body.find("\"parsed\":false") != std::string::npos);

        // history (late joiner): both captures, oldest first, raw and signed
        auto hr = http(rawP, "GET", "/captures");
        CHECK(hr.status == 200 && hr.body.front() == '[' && hr.body.back() == ']');
        CHECK(hr.body.find("\"id\":1") != std::string::npos && hr.body.find("\"id\":2") != std::string::npos);
        CHECK(hr.body.find("\"id\":1") < hr.body.find("\"id\":2"));
        auto h1 = http(rawP, "GET", "/captures?limit=1");
        CHECK(h1.body.find("\"id\":1,") == std::string::npos && h1.body.find("\"id\":2,") != std::string::npos);   // limit keeps the newest
        auto hs = http(dashP, "GET", "/api/captures?limit=5");
        CHECK(hs.status == 200 && hs.body.find("\"sig\":\"") != std::string::npos);
        CHECK(http(sigP, "GET", "/captures").body.find("\"alg\":\"HMAC-SHA256\"") != std::string::npos);

        // dashboard page + security headers + status
        auto page = http(dashP, "GET", "/");
        CHECK(page.status == 200 && page.body.find("Moonbug") != std::string::npos && page.body.find("<script>") != std::string::npos);
        CHECK(page.header("Content-Type").find("text/html") == 0);
        CHECK(page.header("Content-Security-Policy").find("default-src 'none'") == 0);
        CHECK(page.header("Content-Security-Policy").find("frame-ancestors 'none'") != std::string::npos);
        CHECK(page.header("X-Lucyn-Dashboard") == "embedded");
        CHECK(page.body == server::CaptureServer::embeddedDashboard());
        CHECK(page.body.find("innerHTML") == std::string::npos);                    // captured data never goes through innerHTML
        auto st = http(dashP, "GET", "/api/status");
        CHECK(field(st.body, "captures") == "2" && field(st.body, "dashboard") == std::to_string(dashP) && field(st.body, "raw") == std::to_string(rawP));
        CHECK(http(rawP, "GET", "/").body.find("raw capture stream") != std::string::npos);

        // expose(): serve a file instead, live-reloaded; unreadable falls back honestly
        std::filesystem::path f = std::filesystem::temp_directory_path() / "lucyn_dash_test.html";
        { std::ofstream o(f); o << "<h1>custom v1</h1>"; }
        cs.expose(f.string());
        auto c1 = http(dashP, "GET", "/home_dashboard.html");
        CHECK(c1.body == "<h1>custom v1</h1>" && c1.header("X-Lucyn-Dashboard") == "file");
        { std::ofstream o(f); o << "<h1>custom v2</h1>"; }
        CHECK(http(dashP, "GET", "/").body == "<h1>custom v2</h1>");
        std::filesystem::remove(f);
        auto c3 = http(dashP, "GET", "/");
        CHECK(c3.body == server::CaptureServer::embeddedDashboard() && c3.header("X-Lucyn-Dashboard").find("unreadable") != std::string::npos);

        // non-loopback origins are refused on the capture ports too (this is the wiretap!)
        CHECK(http(rawP, "GET", "/captures", "", {"Origin: https://evil.example"}).status == 403);
        WsConn evil; CHECK(!evil.open(rawP, "/stream", {"Origin: https://evil.example"}) && evil.status == 403);

        // captures keep flowing with a client that never reads (slow consumer) and traffic from other threads
        std::vector<std::thread> ts;
        for (int t = 0; t < 3; ++t) ts.emplace_back([&] { for (int i = 0; i < 10; ++i) client.get(pageUrl); });
        for (auto& t : ts) t.join();
        CHECK(mb.total() == 32);
        for (int i = 0; i < 30; ++i) CHECK(!raw.readText().empty());              // 30 more live captures arrive (ids 3..32)

        // destroy the server while a fetch is in flight on another thread: must not crash (Moonbug fires listeners unlocked)
        bg = std::thread([&] { while (go) client.get(pageUrl); });
        std::this_thread::sleep_for(30ms);
    }   // ~CaptureServer here, while bg keeps fetching
    std::this_thread::sleep_for(50ms);
    go = false;
    bg.join();
    uint64_t before = mb.total();
    client.get(pageUrl);
    CHECK(mb.total() == before + 1);                                              // Moonbug itself still works after the server is gone
    std::puts("PASS: capture_server");
}

int main() {
    test_http_basics();
    test_keepalive();
    test_guards();
    test_websocket();
    test_capture_server();
    if (g_fail) { std::fprintf(stderr, "%d check(s) FAILED\n", g_fail); return 1; }
    std::puts("server: all passed");
    return 0;
}
#endif
