#include <lucyn/server/capture_server.h>
#include <lucyn/server/tinyelysia.h>
#include <lucyn/net/moonbug.h>
#include <lucyn/security/nullbyte.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>

namespace lucyn::server {

namespace {

const char kDashboard[] =
#include "dashboard_gen.inc"
;

std::size_t limitParam(const Request& req, std::size_t def) {
    std::string v = req.param("limit");
    if (v.empty() || v.size() > 6) return def;
    std::size_t n = 0;
    for (char c : v) { if (c < '0' || c > '9') return def; n = n * 10 + std::size_t(c - '0'); }
    return std::clamp<std::size_t>(n, 1, 1000);
}

// The dashboard renders text captured from arbitrary websites. It builds every node with textContent
// (never innerHTML), and this policy is the second lock: no external scripts, no framing, no eval, and
// it may only talk back to loopback.
const char* kCsp =
    "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
    "connect-src 'self' ws://127.0.0.1:* ws://localhost:*; img-src 'self' data:; "
    "base-uri 'none'; form-action 'none'; frame-ancestors 'none'";

}  // namespace

std::string CaptureServer::embeddedDashboard() { return std::string(kDashboard); }

// State shared with the Moonbug tap. Moonbug calls listeners WITHOUT a lock, so a capture that is already in
// flight on a fetch thread can reach the tap after the CaptureServer was destroyed. The tap therefore holds this
// (shared_ptr) and checks `closed` under the mutex; it never touches the CaptureServer itself.
struct Shared {
    std::mutex mu;                       // guards everything below
    bool closed = false;
    const security::Nullbyte* nb = nullptr;
    std::unique_ptr<TinyElysia> raw, signedS, dash;

    void onCapture(const net::Capture& cap) {
        std::lock_guard<std::mutex> lk(mu);
        if (closed) return;
        const bool wantRaw    = raw     && raw->clientCount("/stream") > 0;
        const bool wantSigned = signedS && signedS->clientCount("/stream") > 0;
        const bool wantDash   = dash    && dash->clientCount("/stream") > 0;
        if (!wantRaw && !wantSigned && !wantDash) return;         // nobody is watching: skip the work
        std::string json = net::Moonbug::toJson(cap);
        if (wantRaw) raw->broadcast("/stream", json);
        if (wantSigned || wantDash) {
            std::string env = nb->envelope(json);
            if (wantSigned) signedS->broadcast("/stream", env);
            if (wantDash)   dash->broadcast("/stream", env);
        }
    }
};

struct CaptureServer::Impl {
    net::Moonbug& mb;
    const security::Nullbyte& nb;
    net::Moonbug::ListenerId tap = 0;
    net::Moonbug::ListenerId updTap = 0;   // captures that gain their body after finishing
    std::shared_ptr<Shared> sh = std::make_shared<Shared>();
    std::atomic<std::uint16_t> pRaw{0}, pSigned{0}, pDash{0};   // read by request handlers on server threads

    std::mutex fileMu;
    std::string exposed;

    Impl(net::Moonbug& m, const security::Nullbyte& n) : mb(m), nb(n) { sh->nb = &n; }

    // The Moonbug tap is installed once, by the first listener that starts.
    void ensureTap() {
        if (tap) return;
        std::shared_ptr<Shared> keep = sh;
        tap = mb.onCapture([keep](const net::Capture& cap) { keep->onCapture(cap); });
        // A CEF capture finishes from its headers and gets its body a moment later:
        // the update is re-broadcast as the SAME id with new JSON, so a live client
        // can replace the row instead of counting a second request.
        updTap = mb.onUpdate([keep](const net::Capture& cap) { keep->onCapture(cap); });
    }

    std::string historyRaw(std::size_t n) {
        std::string out = "[";
        bool first = true;
        for (auto& c : mb.recent(n)) { if (!first) out += ','; first = false; out += net::Moonbug::toJson(c); }
        return out + "]";
    }
    std::string historySigned(std::size_t n) {
        std::string out = "[";
        bool first = true;
        for (auto& c : mb.recent(n)) { if (!first) out += ','; first = false; out += nb.envelope(net::Moonbug::toJson(c)); }
        return out + "]";
    }

    std::string verifyJson(const std::string& body) {
        auto o = nb.open(body);
        std::string r = std::string("{\"ok\":") + (o.ok ? "true" : "false") + ",\"parsed\":" + (o.parsed ? "true" : "false") + ",\"error\":";
        net::Moonbug::appendJsonString(r, o.error);
        r += ",\"alg\":\"";
        r += security::Nullbyte::kAlgorithm;
        r += "\"}";
        return r;
    }

    static std::string help(const char* what, const char* lines) {
        return std::string("LucyN Moonbug: ") + what + "\n\n" + lines + "\nLoopback only. See README.md (Moonbug Protocol).\n";
    }

    void dashboardPage(Reply& res) {
        std::string path;
        { std::lock_guard<std::mutex> lk(fileMu); path = exposed; }
        std::string html;
        bool fromFile = false;
        if (!path.empty()) {
            std::ifstream in(path, std::ios::binary);
            if (in) {
                html.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                fromFile = in.good() || in.eof();
                if (html.size() > (4u << 20)) { html.clear(); fromFile = false; }
            }
        }
        if (!fromFile || html.empty()) {
            html = kDashboard;
            res.header("X-Lucyn-Dashboard", path.empty() ? "embedded" : "embedded (exposed file unreadable)");
            fromFile = false;
        } else {
            res.header("X-Lucyn-Dashboard", "file");
        }
        res.html(std::move(html));
        res.header("Content-Security-Policy", kCsp);
        res.header("X-Frame-Options", "DENY");
        res.header("Referrer-Policy", "no-referrer");
    }
};

CaptureServer::CaptureServer(net::Moonbug& mb, const security::Nullbyte& nb) : p_(new Impl(mb, nb)) {}

CaptureServer::~CaptureServer() {
    stop();
    std::lock_guard<std::mutex> lk(p_->sh->mu);
    p_->sh->closed = true;   // for good: a straggling in-flight capture must never see a dead server as "open"
}

void CaptureServer::stop() {
    // 1. Detach from Moonbug, 2. mark closed (a late in-flight capture becomes a no-op), 3. stop the listeners.
    if (p_->tap) { p_->mb.off(p_->tap); p_->tap = 0; }
    if (p_->updTap) { p_->mb.off(p_->updTap); p_->updTap = 0; }
    std::unique_ptr<TinyElysia> a, b, c;
    {
        std::lock_guard<std::mutex> lk(p_->sh->mu);
        p_->sh->closed = true;
        a = std::move(p_->sh->raw); b = std::move(p_->sh->signedS); c = std::move(p_->sh->dash);
    }
    p_->pRaw = p_->pSigned = p_->pDash = 0;
    if (a) a->stop();       // joins the server threads outside the lock
    if (b) b->stop();
    if (c) c->stop();
    {   // the object may be reused after stop(): reopen the gate
        std::lock_guard<std::mutex> lk(p_->sh->mu);
        p_->sh->closed = false;
    }
}

bool CaptureServer::activate(std::uint16_t port, std::string* err) {
    { std::lock_guard<std::mutex> lk(p_->sh->mu); if (p_->sh->raw) { if (err) *err = "raw stream already active"; return false; } }
    auto s = std::make_unique<TinyElysia>();
    Impl* im = p_.get();
    s->get("/", [](const Request&, Reply& r) {
        r.text(Impl::help("raw capture stream", "ws  /stream            one JSON capture per frame\nGET /captures?limit=N  history (JSON array)\n"));
    });
    s->get("/captures", [im](const Request& q, Reply& r) { r.json(im->historyRaw(limitParam(q, 100))); });
    s->ws("/stream", {});
    if (!s->listen(port, err)) return false;
    p_->pRaw = s->port();
    { std::lock_guard<std::mutex> lk(p_->sh->mu); p_->sh->raw = std::move(s); }
    p_->ensureTap();
    return true;
}

bool CaptureServer::alsoUseNullbyte(std::uint16_t port, std::string* err) {
    { std::lock_guard<std::mutex> lk(p_->sh->mu); if (p_->sh->signedS) { if (err) *err = "signed stream already active"; return false; } }
    auto s = std::make_unique<TinyElysia>();
    Impl* im = p_.get();
    s->get("/", [](const Request&, Reply& r) {
        r.text(Impl::help("NULLBYTE-signed capture stream (HMAC-SHA256)",
                          "ws   /stream            one envelope per frame: {v, alg, sig, payload}\n"
                          "GET  /captures?limit=N  history (array of envelopes)\n"
                          "POST /verify            body = an envelope -> {ok, parsed, error}\n"));
    });
    s->get("/captures", [im](const Request& q, Reply& r) { r.json(im->historySigned(limitParam(q, 100))); });
    s->post("/verify", [im](const Request& q, Reply& r) { r.json(im->verifyJson(q.body)); });
    s->ws("/stream", {});
    if (!s->listen(port, err)) return false;
    p_->pSigned = s->port();
    { std::lock_guard<std::mutex> lk(p_->sh->mu); p_->sh->signedS = std::move(s); }
    p_->ensureTap();
    return true;
}

bool CaptureServer::serveDashboard(std::uint16_t port, std::string* err) {
    { std::lock_guard<std::mutex> lk(p_->sh->mu); if (p_->sh->dash) { if (err) *err = "dashboard already running"; return false; } }
    auto s = std::make_unique<TinyElysia>();
    Impl* im = p_.get();
    auto page = [im](const Request&, Reply& r) { im->dashboardPage(r); };
    s->get("/", page);
    s->get("/home_dashboard.html", page);
    s->get("/api/captures", [im](const Request& q, Reply& r) { r.json(im->historySigned(limitParam(q, 100))); });
    s->post("/api/verify", [im](const Request& q, Reply& r) { r.json(im->verifyJson(q.body)); });
    s->get("/api/status", [im](const Request&, Reply& r) {
        std::string j = "{\"version\":\"";
        j += kVersion;
        j += "\",\"captures\":" + std::to_string(im->mb.total()) +
             ",\"raw\":" + std::to_string(im->pRaw.load()) +
             ",\"signed\":" + std::to_string(im->pSigned.load()) +
             ",\"dashboard\":" + std::to_string(im->pDash.load()) + "}";
        r.json(std::move(j));
    });
    s->ws("/stream", {});
    if (!s->listen(port, err)) return false;
    p_->pDash = s->port();
    { std::lock_guard<std::mutex> lk(p_->sh->mu); p_->sh->dash = std::move(s); }
    p_->ensureTap();
    return true;
}

void CaptureServer::expose(const std::string& htmlPath) {
    std::lock_guard<std::mutex> lk(p_->fileMu);
    p_->exposed = htmlPath;
}

std::uint16_t CaptureServer::rawPort() const       { return p_->pRaw; }
std::uint16_t CaptureServer::signedPort() const    { return p_->pSigned; }
std::uint16_t CaptureServer::dashboardPort() const { return p_->pDash; }

}  // namespace lucyn::server
