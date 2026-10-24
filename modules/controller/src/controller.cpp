// controller.cpp — M4 JSON command loop implementation.
// Reads lines from stdin, dispatches, writes replies to stdout.
// Thread-safety: none needed — one command at a time.

#include <lucyn/controller/controller.h>
#include <lucyn/core/paths.h>
#include <lucyn/net/moonbug.h>
#include <lucyn/security/nullbyte.h>
#include <lucyn/server/capture_server.h>
#ifdef LUCYN_CONTROLLER_HAVE_SEARCH
#include <lucyn/search/search.h>
#endif
#ifdef LUCYN_CONTROLLER_HAVE_CEF
#include <lucyn/cef_host/browser_host.h>
#endif
#include <blocker.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ---- Tiny JSON reader/writer ------------------------------------------------
// We don't vendor a JSON lib; we only need a small safe subset for the protocol.
// Real sites: the Hydrator gives us DOM, not JSON, so this is protocol-only.

namespace {

// Returns the value of a string field in a flat JSON object, or "".
// Only handles {"key":"value",...}  (no nesting needed for protocol objects).
std::string jsonGetStr(const std::string& json, const std::string& key) {
    // look for "key":"
    std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    std::string out;
    for (; pos < json.size(); ++pos) {
        char c = json[pos];
        if (c == '"') break;
        if (c == '\\' && pos + 1 < json.size()) {
            ++pos;
            switch (json[pos]) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                default:   out += json[pos]; break;
            }
        } else {
            out += c;
        }
    }
    return out;
}

// Returns a numeric field (integer) or -1 if not found / not a number.
long long jsonGetInt(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return -1;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size()) return -1;
    char* end = nullptr;
    long long val = std::strtoll(json.c_str() + pos, &end, 10);
    return (end != json.c_str() + pos) ? val : -1LL;
}

// Returns 1 / 0 for true / false, or -1 if the field is missing or not a boolean.
int jsonGetBool(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return -1;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (json.compare(pos, 4, "true") == 0) return 1;
    if (json.compare(pos, 5, "false") == 0) return 0;
    return -1;
}

// Escape a string for JSON.
std::string jsonEsc(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

// Build reply objects.
std::string replyOk(long long id) {
    char buf[64]; std::snprintf(buf, sizeof(buf), "{\"id\":%lld,\"ok\":true}", id); return buf;
}
std::string replyValue(long long id, const std::string& value) {
    return "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"value\":\"" + jsonEsc(value) + "\"}";
}
std::string replyValues(long long id, const std::vector<std::string>& values) {
    std::string s = "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"values\":[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) s += ',';
        s += '"'; s += jsonEsc(values[i]); s += '"';
    }
    s += "]}";
    return s;
}
std::string replyErr(long long id, const std::string& err) {
    return "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":\"" + jsonEsc(err) + "\"}";
}
// fill/submit reply: {"id":N,"ok":true,"tag":"input"}
std::string replyTag(long long id, const std::string& tag) {
    return "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"tag\":\"" + jsonEsc(tag) + "\"}";
}
std::string eventLine(const std::string& type, const std::string& message) {
    return "{\"event\":\"" + jsonEsc(type) + "\",\"message\":\"" + jsonEsc(message) + "\"}";
}

// --- download helpers ---------------------------------------------------------
// Everything a server tells us about a file name is hostile input. These turn it into ONE safe path segment.

namespace fs = std::filesystem;

std::string lowerAscii(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trimWs(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// %XX -> byte. Anything malformed is kept literally.
std::string pctDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hexVal(s[i + 1]) >= 0 && hexVal(s[i + 2]) >= 0) {
            out += (char)(hexVal(s[i + 1]) * 16 + hexVal(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool isHttpUrl(const std::string& u) {
    std::string l = lowerAscii(u.substr(0, 8));
    return l.rfind("http://", 0) == 0 || l.rfind("https://", 0) == 0;
}

// Raw name -> one safe file name, or "" if nothing usable is left. The result never contains a path
// separator, a control character or NUL, is never "." / "..", never starts with a dot (no hidden files, and
// the downloader's own ".download-*.part" temp files can never collide with it), and is at most 200 bytes.
std::string sanitizeFileName(const std::string& raw) {
    size_t cut = raw.find_last_of("/\\");
    std::string s = (cut == std::string::npos) ? raw : raw.substr(cut + 1);    // basename only
    std::string t;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) continue;                                   // control chars, NUL
        if (std::strchr(":*?\"<>|", c)) t += '_';                              // illegal on Windows
        else t += (char)c;
    }
    t = trimWs(t);
    while (!t.empty() && (t.back() == '.' || t.back() == ' ')) t.pop_back();   // Windows drops these anyway
    if (t.empty()) return {};
    for (size_t i = 0; i < t.size() && t[i] == '.'; ++i) t[i] = '_';           // ".bashrc" -> "_bashrc"
    {   // Windows device names (CON, NUL, COM1 ...), with or without an extension
        std::string stem = lowerAscii(t.substr(0, t.find('.')));
        static const char* bad[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6",
                                    "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6",
                                    "lpt7", "lpt8", "lpt9"};
        for (auto* b : bad) if (stem == b) { t = "_" + t; break; }
    }
    const size_t kMax = 200;
    if (t.size() > kMax) {
        size_t dot = t.rfind('.');
        std::string ext = (dot != std::string::npos && dot > 0 && t.size() - dot <= 20) ? t.substr(dot) : "";
        size_t keep = kMax - ext.size();
        while (keep > 0 && ((unsigned char)t[keep] & 0xC0) == 0x80) --keep;     // do not cut a UTF-8 character in half
        t = t.substr(0, keep) + ext;
    }
    return t;
}

// The file name in a Content-Disposition value: RFC 5987 `filename*=UTF-8''%C3%A9.pdf` first, then `filename=`.
std::string contentDispositionName(const std::string& v) {
    std::string lv = lowerAscii(v);
    size_t p = lv.find("filename*=");
    if (p != std::string::npos) {
        size_t a = p + 10, b = v.find(';', a);
        std::string val = trimWs(v.substr(a, b == std::string::npos ? std::string::npos : b - a));
        size_t q1 = val.find('\''), q2 = (q1 == std::string::npos) ? q1 : val.find('\'', q1 + 1);
        if (q2 != std::string::npos) {
            std::string charset = lowerAscii(val.substr(0, q1));
            std::string dec = pctDecode(val.substr(q2 + 1));
            if (charset == "iso-8859-1" || charset == "latin1") {              // bytes -> UTF-8
                std::string u8;
                for (unsigned char c : dec) {
                    if (c < 0x80) u8 += (char)c;
                    else { u8 += (char)(0xC0 | (c >> 6)); u8 += (char)(0x80 | (c & 0x3F)); }
                }
                dec = u8;
            }
            if (!dec.empty()) return dec;
        }
    }
    p = lv.find("filename=");
    if (p != std::string::npos) {
        size_t a = p + 9;
        while (a < v.size() && v[a] == ' ') ++a;
        if (a < v.size() && v[a] == '"') {
            std::string out;
            for (++a; a < v.size() && v[a] != '"'; ++a) {
                if (v[a] == '\\' && a + 1 < v.size()) ++a;
                out += v[a];
            }
            return out;
        }
        size_t b = v.find(';', a);
        return trimWs(v.substr(a, b == std::string::npos ? std::string::npos : b - a));
    }
    return {};
}

// Last path segment of a URL, percent-decoded ("https://h/a/b%20c.pdf?x=1#y" -> "b c.pdf").
std::string urlLastSegment(const std::string& url) {
    size_t s = url.find("://");
    if (s == std::string::npos) return {};
    size_t path = url.find('/', s + 3);
    if (path == std::string::npos) return {};
    std::string p = url.substr(path);
    p = p.substr(0, p.find_first_of("?#"));
    size_t slash = p.rfind('/');
    return pctDecode(slash == std::string::npos ? p : p.substr(slash + 1));
}

// <dir>/<name>, or "<stem> (1).<ext>", "(2)", ... if that exists and we may not overwrite.
fs::path uniqueDownloadPath(const std::string& dir, const std::string& name, bool overwrite) {
    fs::path base = fs::u8path(dir);
    fs::path p = base / fs::u8path(name);
    std::error_code ec;
    if (overwrite || !fs::exists(p, ec)) return p;
    std::string stem = name, ext;
    size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) { stem = name.substr(0, dot); ext = name.substr(dot); }
    if (ext.size() && stem.size() > 4 && lowerAscii(stem.substr(stem.size() - 4)) == ".tar") {   // archive.tar.gz
        ext = stem.substr(stem.size() - 4) + ext;
        stem = stem.substr(0, stem.size() - 4);
    }
    for (int n = 1; n < 100000; ++n) {
        fs::path c = base / fs::u8path(stem + " (" + std::to_string(n) + ")" + ext);
        if (!fs::exists(c, ec)) return c;
    }
    return p;
}

} // anon namespace

// ---- Controller::Impl -------------------------------------------------------


namespace lucyn::controller {

struct Controller::Impl {
    ControllerOptions opts;

    // NULLBYTE key: persisted in nothing-data (so seals stay verifiable tomorrow) unless the options say otherwise.
    static security::Nullbyte makeNullbyte(const ControllerOptions& o, std::string* note) {
        if (!o.ephemeralNullbyte) {
            std::string err;
            std::string key = security::Nullbyte::loadOrCreateKey(security::Nullbyte::defaultKeyPath(), &err);
            if (!key.empty()) return security::Nullbyte(std::move(key));
            if (note) *note = "NULLBYTE: " + err + " (using a throwaway key for this session)";
        }
        return security::Nullbyte(security::Nullbyte::generateKey());
    }

    // Shared engine state — lives for the life of the controller.
    std::string       keyNote;     // set when the key file was unusable; reported as a note event at run()
    std::string       adblockNote;
    adblock::Blocker  adblocker;
    security::Nullbyte nullbyte;
    net::Moonbug      moonbug;     // the wiretap
    std::atomic<bool> eventsOn{false};
    net::Moonbug::ListenerId  eventTap = 0;
#ifdef LUCYN_CONTROLLER_HAVE_SEARCH
    lucyn::search::Registry searchEngines;
#endif

    std::string lastUrl;
#ifdef LUCYN_CONTROLLER_HAVE_CEF
    // Session 19: Chromium-backed instance for the six migrated commands (visit,
    // html, eval, extract, extractAll, click). Created only when the host process
    // has already initialised CEF (apps/lucyn calls initCef). When absent — e.g.
    // unit tests that never init CEF — those commands fall back to the old path.
    std::unique_ptr<lucyn::cef_host::BrowserInstance> cef_;
    // Moonbug feed (step 8): CEF in-flight resource captures keyed by CEF's request
    // id, guarded by resMu_. onCefResource() runs on CEF's IO thread; a visit clears
    // stale entries on this thread — both take resMu_, and it is declared BEFORE
    // nothing that outlives it (destroyed after cef_ stops calling us).
    std::mutex resMu_;
    std::map<unsigned long long, net::Capture> pendingRes_;

    // Bridge one CEF resource lifecycle event into Moonbug. Called DIRECTLY on
    // CEF's IO thread from cef_host (not via pump) so captures are live even while
    // the loop blocks in a wait (waitIdle needs that). Touches only moonbug (its
    // own lock) and pendingRes_ (resMu_) → safe on that thread.
    void onCefResource(const lucyn::cef_host::BrowserInstance::ResourceEvent& ev) {
        using Ev = lucyn::cef_host::BrowserInstance::ResourceEvent;
        if (ev.phase == Ev::Start) {
            net::Capture cap = moonbug.begin(ev.method, ev.url, /*profile=*/"", /*impersonating=*/false);
            std::lock_guard<std::mutex> lk(resMu_);
            pendingRes_[ev.requestId] = std::move(cap);
            return;
        }
        // Finish: pull the capture begun at Start and complete it.
        net::Capture cap;
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(resMu_);
            auto it = pendingRes_.find(ev.requestId);
            if (it != pendingRes_.end()) { cap = std::move(it->second); pendingRes_.erase(it); have = true; }
        }
        if (!have) cap = moonbug.begin(ev.method, ev.url, "", false);  // Finish without a seen Start
        cap.requestHeaders  = ev.requestHeaders;
        cap.responseHeaders = ev.responseHeaders;
        cap.finalUrl        = ev.url;
        cap.status          = ev.status;
        cap.responseBytes   = static_cast<std::size_t>(ev.bytes < 0 ? 0 : ev.bytes);
        cap.redirects       = ev.redirects;
        cap.httpVersion     = ev.httpVersion;
        cap.error           = ev.error;
        cap.done            = true;
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (cap.tsMs > 0 && nowMs >= cap.tsMs) cap.timing.totalMs = static_cast<double>(nowMs - cap.tsMs);
        moonbug.finish(cap);   // fires onResponse + onCapture → dashboard / events
    }
#endif

    std::istream& in;
    std::ostream& out;
    std::mutex outMu;              // fetch() runs on a worker thread, so captures can race with replies
    // Declared LAST: destroyed first, so its listeners stop before anything they use goes away.
    std::unique_ptr<server::CaptureServer> srv;

    explicit Impl(ControllerOptions o)
        : opts(std::move(o))
        , nullbyte(makeNullbyte(opts, &keyNote))
        , in(opts.in ? *opts.in : std::cin)
        , out(opts.out ? *opts.out : std::cout)
    {
#ifdef LUCYN_CONTROLLER_HAVE_SEARCH
        if (!opts.searxngUrl.empty()) {
            std::string instance = opts.searxngUrl;
            while (!instance.empty() && instance.back() == '/') instance.pop_back();
            if (instance.size() >= 7 && instance.compare(instance.size() - 7, 7, "/search") == 0)
                searchEngines.add({"searxng", "SearXNG", instance + "?q={query}"});
            else
                searchEngines.add({"searxng", "SearXNG", instance + "/search?q={query}"});
        }
#endif
        // Default filter list(s): every *.txt in <exeDir>/adblock-filters/, put there at
        // build time from modules/adblock/filters/ (see that CMakeLists.txt). Falls back
        // to the old inline minimal list if the folder isn't there (e.g. a binary moved
        // away from its build output, or a module built standalone without the copy step).
        std::string defaultFilterDir = (std::filesystem::path(core::exeDir()) / "adblock-filters").string();
        std::error_code dirEc;
        bool loadedAnyDefault = false;
        if (std::filesystem::is_directory(defaultFilterDir, dirEc)) {
            for (const auto& entry : std::filesystem::directory_iterator(defaultFilterDir, dirEc)) {
                if (!entry.is_regular_file()) continue;
                if (entry.path().extension() != ".txt") continue;
                std::ifstream list(entry.path());
                if (!list) continue;
                std::ostringstream contents;
                contents << list.rdbuf();
                adblocker.load(contents.str());
                loadedAnyDefault = true;
            }
        }
        if (!loadedAnyDefault) {
            adblocker.load("||doubleclick.net^\n||googlesyndication.com^\n||google-analytics.com^\n||adservice.google.com^\n||facebook.com^$third-party,script\n||connect.facebook.net^$third-party,script\n");
        }
        if (!opts.adblockFilterListPath.empty()) {
            std::ifstream list(opts.adblockFilterListPath);
            if (list) {
                std::ostringstream contents;
                contents << list.rdbuf();
                adblocker.load(contents.str());
            } else {
                adblockNote = "adblock filter list could not be opened: " + opts.adblockFilterListPath;
            }
        }
        adblocker.finalize();
        eventsOn = opts.captureEvents;
        eventTap = moonbug.onCapture([this](const net::Capture& c) { if (eventsOn) emit(requestEvent(c)); });
#ifdef LUCYN_CONTROLLER_HAVE_CEF
        // Chromium instance — but ONLY when the host process initialised CEF. Unit
        // tests never do, so engine commands report "no page loaded" there.
        if (lucyn::cef_host::isCefInitialized()) {
            cef_ = lucyn::cef_host::createInstance();
            if (cef_)
                cef_->setResourceHandler([this](const lucyn::cef_host::BrowserInstance::ResourceEvent& ev) {
                    onCefResource(ev);
                });
        }
        // Session cookies: load nothing-data/cookies.json into Chromium's jar — this is
        // what makes a login survive a new engine run. Needs cef_ (created just above).
        loadCookiesJson();
#endif
    }

    ~Impl() {
#ifdef LUCYN_CONTROLLER_HAVE_CEF
        if (cef_) cef_->setResourceHandler(nullptr);   // stop IO-thread callbacks before members die
#endif
        saveCookiesJson();                     // clean shutdown: write Chromium's jar back to cookies.json
        srv.reset();                          // stop listeners first
        moonbug.off(eventTap);
    }

    void emit(const std::string& line) {
        std::lock_guard<std::mutex> lk(outMu);
        if (opts.onEvent) opts.onEvent(line);
        out << line << '\n';
        out.flush();
    }

    void reply(const std::string& line) {
        std::lock_guard<std::mutex> lk(outMu);
        out << line << '\n';
        out.flush();
    }

    // One capture as a protocol event. The sig is NULLBYTE's seal over the same capture JSON the signed stream and
    // dashboard carry, so an id + sig seen here can be matched with (and checked against) those.
    std::string requestEvent(const net::Capture& c) {
        char ms[32];
        std::snprintf(ms, sizeof ms, "%.1f", c.timing.totalMs);
        return "{\"event\":\"request\",\"id\":" + std::to_string(c.id) +
               ",\"method\":\"" + jsonEsc(c.method) + "\",\"url\":\"" + jsonEsc(c.url) +
               "\",\"status\":" + std::to_string(c.status) + ",\"ms\":" + ms +
               ",\"bytes\":" + std::to_string(c.responseBytes) + ",\"error\":\"" + jsonEsc(c.error) +
               "\",\"sig\":\"" + nullbyte.sign(net::Moonbug::toJson(c)) + "\"}";
    }

    // ---- Moonbug listeners ----------------------------------------------------------------------
    // port < 0 = leave alone. Returns "" on success or an error text.
    std::string startServers(int raw, int sgn, int dash, const std::string& file) {
        if (raw < 0 && sgn < 0 && dash < 0 && file.empty()) return {};
        if (!srv) srv = std::make_unique<server::CaptureServer>(moonbug, nullbyte);
        std::string err, all;
        if (!file.empty()) srv->expose(file);
        auto port = [&](int p) { return p > 65535 ? -2 : p; };
        if (port(raw) == -2 || port(sgn) == -2 || port(dash) == -2) return "port out of range (0..65535)";
        if (raw  >= 0 && srv->rawPort() == 0       && !srv->activate(std::uint16_t(raw), &err))         all += "raw stream: " + err + "; ";
        if (sgn  >= 0 && srv->signedPort() == 0    && !srv->alsoUseNullbyte(std::uint16_t(sgn), &err))  all += "NULLBYTE stream: " + err + "; ";
        if (dash >= 0 && srv->dashboardPort() == 0 && !srv->serveDashboard(std::uint16_t(dash), &err))  all += "dashboard: " + err + "; ";
        if (srv->rawPort() || srv->signedPort() || srv->dashboardPort())
            emit("{\"event\":\"moonbug\",\"raw\":" + std::to_string(srv->rawPort()) + ",\"signed\":" +
                 std::to_string(srv->signedPort()) + ",\"dashboard\":" + std::to_string(srv->dashboardPort()) + "}");
        return all;
    }

#ifdef LUCYN_CONTROLLER_HAVE_CEF
    static std::string cookieJson(const lucyn::cef_host::BrowserInstance::Cookie& c) {
        return "{\"name\":\"" + jsonEsc(c.name) + "\",\"value\":\"" + jsonEsc(c.value) + "\",\"domain\":\"" + jsonEsc(c.domain) +
               "\",\"path\":\"" + jsonEsc(c.path) + "\",\"hostOnly\":" + (c.hostOnly ? "true" : "false") +
               ",\"secure\":" + (c.secure ? "true" : "false") + ",\"httpOnly\":" + (c.httpOnly ? "true" : "false") +
               ",\"expires\":" + std::to_string(c.expires) + "}";
    }
#endif

    // Split a JSON array text into its top-level {...} element strings (string/escape aware, so a
    // '}' inside a cookie value doesn't cut the object short).
    static std::vector<std::string> jsonObjectItems(const std::string& text) {
        std::vector<std::string> items;
        size_t open = text.find('[');
        if (open == std::string::npos) return items;
        std::string cur;
        int depth = 0;
        bool inStr = false;
        for (size_t i = open + 1; i < text.size(); ++i) {
            char c = text[i];
            if (inStr) {
                cur += c;
                if (c == '\\' && i + 1 < text.size()) { cur += text[++i]; continue; }
                if (c == '"') inStr = false;
                continue;
            }
            if (c == '"')       { inStr = true; cur += c; }
            else if (c == '{')  { ++depth; cur += c; }
            else if (c == '}')  { --depth; cur += c; if (depth == 0) { items.push_back(cur); cur.clear(); } }
            else if (depth > 0) cur += c;
        }
        return items;
    }

    // nothing-data/cookies.json: the jar as the same objects `cookies` returns, so the JS client
    // can read and write it too. Loaded into Chromium's jar at startup (once cef_ exists) and
    // written back on clean shutdown.
#ifdef LUCYN_CONTROLLER_HAVE_CEF
    void loadCookiesJson() {
        if (!cef_) return;
        std::string path = core::dataRoot() + "/cookies.json";
        std::ifstream f(path, std::ios::binary);
        if (!f) return;
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        long long now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        for (const std::string& item : jsonObjectItems(text)) {
            lucyn::cef_host::BrowserInstance::Cookie c;
            c.name     = jsonGetStr(item, "name");
            c.value    = jsonGetStr(item, "value");
            c.domain   = jsonGetStr(item, "domain");
            if (c.name.empty() || c.domain.empty()) continue;
            std::string cpath = jsonGetStr(item, "path");
            if (!cpath.empty()) c.path = cpath;
            c.hostOnly = jsonGetBool(item, "hostOnly") != 0;   // like cmdSetCookie: default host-only
            c.secure   = jsonGetBool(item, "secure") == 1;
            c.httpOnly = jsonGetBool(item, "httpOnly") == 1;
            long long exp = jsonGetInt(item, "expires");
            c.expires  = exp > 0 ? exp : 0;
            if (c.expires > 0 && c.expires <= now) continue;   // expired while we were away
            // Chromium scopes a cookie by URL: synthesize one from the domain (https when secure).
            const std::string targetUrl = (c.secure ? "https://" : "http://") + c.domain + "/";
            bool done = false;
            cef_->setCookie(targetUrl, c, [&](bool, const std::string&, const std::string&) { done = true; });
            cefWait([&] { return done; }, 5000);   // best-effort: a rejected cookie is simply skipped
        }
    }
#endif

    void saveCookiesJson() {
        std::string root = core::dataRoot();
        if (root.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(root, ec);         // dataRoot() is not created until asked
#ifdef LUCYN_CONTROLLER_HAVE_CEF
        if (!cef_) return;
        bool done = false, ok = false;
        std::vector<lucyn::cef_host::BrowserInstance::Cookie> list;
        cef_->allCookies("", [&](bool o,
                                 const std::vector<lucyn::cef_host::BrowserInstance::Cookie>& cl,
                                 const std::string&) {
            done = true; ok = o; list = cl;
        });
        if (!cefWait([&] { return done; }, 15000) || !ok) return;
        std::string s = "[";
        for (size_t i = 0; i < list.size(); ++i) { if (i) s += ','; s += cookieJson(list[i]); }
        s += "]";
        std::ofstream f(root + "/cookies.json", std::ios::binary | std::ios::trunc);
        if (f) f << s;
#endif
    }

    // cookies {url?}: with a url, exactly what a request to it would carry (HttpOnly included); without, the whole jar.
    // Backed by Chromium's cookie store — the jar a CEF page actually uses.
    std::string cmdCookies(long long id, const std::string& url) {
#ifdef LUCYN_CONTROLLER_HAVE_CEF
        if (cef_) {
            bool done = false, ok = false;
            std::vector<lucyn::cef_host::BrowserInstance::Cookie> cefList;
            std::string err;
            cef_->allCookies(url, [&](bool o,
                                      const std::vector<lucyn::cef_host::BrowserInstance::Cookie>& cl,
                                      const std::string& e) {
                done = true; ok = o; cefList = cl; err = e;
            });
            if (!cefWait([&] { return done; }, 15000)) return replyErr(id, "timeout reading cookies");
            if (!ok) return replyErr(id, err.empty() ? "cannot read cookies" : err);
            std::string s = "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"cookies\":[";
            for (size_t i = 0; i < cefList.size(); ++i) {
                if (i) s += ',';
                s += cookieJson(cefList[i]);
            }
            return s + "]}";
        }
        return replyErr(id, "no browser instance (CEF not initialised)");
#else
        (void)url;
        return replyErr(id, "built without CEF");
#endif
    }

    // setCookie {name, value, domain | url, path?, secure?, httpOnly?, expires?}: import a session. An `expires`
    // in the past deletes the cookie.
    std::string cmdSetCookie(long long id, const std::string& line) {
#ifdef LUCYN_CONTROLLER_HAVE_CEF
        if (!cef_) return replyErr(id, "no browser instance (CEF not initialised)");
        lucyn::cef_host::BrowserInstance::Cookie c;
        c.name  = jsonGetStr(line, "name");
        c.value = jsonGetStr(line, "value");
        c.domain = jsonGetStr(line, "domain");
        std::string url = jsonGetStr(line, "url");
        if (c.name.empty()) return replyErr(id, "'name' required");
        if (c.domain.empty() && !url.empty()) {
            auto p = url.find("://");
            std::string host = url.substr(p == std::string::npos ? 0 : p + 3);
            host = host.substr(0, host.find_first_of("/?#:"));
            c.domain = host;
        }
        if (c.domain.empty()) return replyErr(id, "'domain' or 'url' required");
        std::string path = jsonGetStr(line, "path");
        if (!path.empty()) c.path = path;
        c.hostOnly = jsonGetBool(line, "hostOnly") != 0;           // default: host-only, like a Set-Cookie without Domain
        c.secure   = jsonGetBool(line, "secure") == 1;
        c.httpOnly = jsonGetBool(line, "httpOnly") == 1;
        long long exp = jsonGetInt(line, "expires");
        c.expires  = exp > 0 ? exp : 0;                            // missing / 0 = session cookie; a past time deletes
        // SetCookie needs a URL to scope by: use the caller's, or synthesize from the domain.
        const std::string targetUrl = url.empty() ? ((c.secure ? "https://" : "http://") + c.domain + "/") : url;
        bool done = false, ok = false;
        std::string err2;
        cef_->setCookie(targetUrl, c, [&](bool o, const std::string&, const std::string& e) {
            done = true; ok = o; err2 = e;
        });
        if (!cefWait([&] { return done; }, 15000)) return replyErr(id, "timeout setting cookie");
        if (!ok) return replyErr(id, err2.empty() ? "cookie rejected (bad characters or no domain)" : err2);
        return replyOk(id);
#else
        (void)line;
        return replyErr(id, "built without CEF");
#endif
    }

    // ---- command handlers ---------------------------------------------------

#ifdef LUCYN_CONTROLLER_HAVE_CEF
    // Bridge async CEF completions into the synchronous command loop: pump the
    // instance until `done` or timeout. Only used when cef_ is present (CEF up).
    bool cefWait(const std::function<bool()>& done, int timeoutMs) {
        if (!cef_) return false;
        const auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!done() && std::chrono::steady_clock::now() < dl) {
            cef_->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        cef_->pump();
        return done();
    }
#endif

    std::string cmdVisit(long long id, const std::string& url) {
#ifdef LUCYN_CONTROLLER_HAVE_CEF
        if (cef_) {
            bool done = false, ok = false;
            int status = 0;
            std::string title, err;
            {   // drop in-flight resource captures left over from any previous page
                std::lock_guard<std::mutex> lk(resMu_);
                pendingRes_.clear();
            }
            cef_->visit(url, [&](bool o, int s, const std::string& t, const std::string& e) {
                done = true; ok = o; status = s; title = t; err = e;
            });
            if (!cefWait([&] { return done; }, 30000)) return replyErr(id, "timeout loading page");
            if (!ok) return replyErr(id, err.empty() ? "load failed" : err);
            const std::string finalUrl = cef_->currentUrl();
            lastUrl = finalUrl;
            return "{\"id\":" + std::to_string(id) + ",\"ok\":true"
                   ",\"status\":" + std::to_string(status) +
                   ",\"url\":\"" + jsonEsc(finalUrl) + "\""
                   ",\"title\":\"" + jsonEsc(title) + "\"}";
        }
        return replyErr(id, "no browser instance (CEF not initialised)");
#else
        return replyErr(id, "built without CEF");
#endif
    }

#ifdef LUCYN_CONTROLLER_HAVE_SEARCH
    std::string cmdSearch(long long id, const std::string& line) {
        const std::string query = jsonGetStr(line, "query");
        if (query.empty()) return replyErr(id, "'query' required");
        std::string engine = jsonGetStr(line, "engine");
        if (engine.empty()) engine = opts.searchDefault.empty() ? "brave" : opts.searchDefault;
        std::string error;
        auto target = searchEngines.url(engine, query, &error);
        if (!target) return replyErr(id, error);
        return cmdVisit(id, *target);
    }
#endif

    std::string cmdHtml(long long id) {
#ifdef LUCYN_CONTROLLER_HAVE_CEF
        if (cef_) {
            bool done = false, ok = false;
            std::string val, err;
            cef_->html([&](bool o, const std::string& v, const std::string& e) { done = true; ok = o; val = v; err = e; });
            if (!cefWait([&] { return done; }, 15000)) return replyErr(id, "timeout reading html");
            if (!ok) return replyErr(id, err.empty() ? "no page loaded" : err);
            return replyValue(id, val);
        }
        return replyErr(id, "no page loaded (no browser instance; CEF not initialised)");
#else
        return replyErr(id, "built without CEF");
#endif
    }

    std::string cmdExtract(long long id, const std::string& selector) {
#ifdef LUCYN_CONTROLLER_HAVE_CEF
        if (cef_) {
            bool done = false, found = false;
            std::string val;
            cef_->extract(selector, [&](bool f, const std::string& v) { done = true; found = f; val = v; });
            if (!cefWait([&] { return done; }, 15000)) return replyErr(id, "timeout extracting");
            if (!found) return replyErr(id, "selector not found: " + selector);
            return replyValue(id, val);
        }
        return replyErr(id, "no browser instance (CEF not initialised)");
#else
        (void)selector;
        return replyErr(id, "built without CEF");
#endif
    }

    std::string cmdExtractAll(long long id, const std::string& selector) {
#ifdef LUCYN_CONTROLLER_HAVE_CEF
        if (cef_) {
            bool done = false, ok = false;
            std::vector<std::string> vals;
            std::string err;
            cef_->extractAll(selector, [&](bool o, const std::vector<std::string>& v, const std::string& e) { done = true; ok = o; vals = v; err = e; });
            if (!cefWait([&] { return done; }, 15000)) return replyErr(id, "timeout extracting");
            if (!ok) return replyErr(id, err.empty() ? "extract failed" : err);
            return replyValues(id, vals);
        }
        return replyErr(id, "no browser instance (CEF not initialised)");
#else
        (void)selector;
        return replyErr(id, "built without CEF");
#endif
    }

    std::string cmdEval(long long id, const std::string& js) {
#ifdef LUCYN_CONTROLLER_HAVE_CEF
        if (cef_) {
            bool done = false, ok = false;
            std::string val, err;
            cef_->eval(js, [&](bool o, const std::string& v, const std::string& e) { done = true; ok = o; val = v; err = e; });
            if (!cefWait([&] { return done; }, 15000)) return replyErr(id, "timeout evaluating");
            if (!ok) return replyErr(id, err.empty() ? "eval failed" : err);
            return replyValue(id, val);
        }
        return replyErr(id, "no browser instance (CEF not initialised)");
#else
        (void)js;
        return replyErr(id, "built without CEF");
#endif
    }

    std::string cmdWaitFor(long long id, const std::string& selector, int timeoutMs) {
#ifdef LUCYN_CONTROLLER_HAVE_CEF
        if (cef_) {
            // CEF path: poll the LIVE page (Chromium is the JS engine, so there is
            // no lastResult/jsHost_ to read). Each probe is an eval round-trip that
            // returns "true"/"false" for whether the selector is present yet.
            if (timeoutMs <= 0) timeoutMs = opts.settleTimeoutMs > 0 ? opts.settleTimeoutMs : 5000;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            const std::string probe = "!!document.querySelector(" + jsonStringLiteral(selector) + ")";
            for (;;) {
                bool done = false, present = false;
                cef_->eval(probe, [&](bool ok, const std::string& v, const std::string&) {
                    done = true; present = ok && v == "true";
                });
                if (!cefWait([&] { return done; }, 15000))
                    return replyErr(id, "timeout waiting for selector: " + selector);
                if (present) return replyValue(id, selector);
                if (std::chrono::steady_clock::now() >= deadline)
                    return replyErr(id, "timeout waiting for selector: " + selector);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let the page render
            }
        }
        return replyErr(id, "no browser instance (CEF not initialised)");
#else
        (void)selector; (void)timeoutMs;
        return replyErr(id, "built without CEF");
#endif
    }

    std::string cmdWaitIdle(long long id, int quietMs, int timeoutMs) {
        if (quietMs <= 0)   quietMs   = 500;
        if (timeoutMs <= 0) timeoutMs = opts.settleTimeoutMs > 0 ? opts.settleTimeoutMs : 5000;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        // The quiet window alone isn't enough: heavy XHR/fetch pages fire their next request right
        // after the initial burst settles, so the page still looks empty the moment we reply. After
        // the first quiet window passes, hold a short confirmation window too and make sure no new
        // request started in the meantime; any activity sends us back to a full fresh quiet window.
        // The caller's timeout still caps the whole wait.
        const long long confirmMs = std::min<long long>(quietMs, 300);
        size_t prevTotal = moonbug.total();
        auto quietStart = std::chrono::steady_clock::now();
        bool confirming = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));   // let CEF's IO thread feed Moonbug
            size_t nowTotal = moonbug.total();
            // A request that is still running is not "quiet": without this a slow response looked idle.
            if (nowTotal != prevTotal || moonbug.inFlight() > 0) {
                prevTotal  = nowTotal;
                quietStart = std::chrono::steady_clock::now();
                confirming = false;              // activity: restart both windows
            }
            auto quiet = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - quietStart).count();
            if (!confirming && quiet >= quietMs) { confirming = true; quietStart = std::chrono::steady_clock::now(); }
            else if (confirming && quiet >= confirmMs) return replyOk(id);
            if (std::chrono::steady_clock::now() >= deadline)
                return replyErr(id, "timeout waiting for network idle");
        }
    }

    std::string cmdClick(long long id, const std::string& selector) {
#ifdef LUCYN_CONTROLLER_HAVE_CEF
        if (cef_) {
            bool done = false, ok = false;
            std::string val, err;
            cef_->click(selector, [&](bool o, const std::string& v, const std::string& e) { done = true; ok = o; val = v; err = e; });
            if (!cefWait([&] { return done; }, 15000)) return replyErr(id, "timeout clicking");
            if (!ok) return replyErr(id, err.empty() ? "click failed" : err);
            return replyValue(id, val);
        }
        return replyErr(id, "no browser instance (CEF not initialised)");
#else
        (void)selector;
        return replyErr(id, "built without CEF");
#endif
    }

    // fill {selector, value}: set the element's value, then dispatch bubbling `input` and
    // `change` events — that order (input, then change) is what browsers do and what
    // React/Vue/Alpine listeners expect. Chromium runs the JS; the reply is the tag name.
    std::string cmdFill(long long id, const std::string& selector, const std::string& value) {
#ifdef LUCYN_CONTROLLER_HAVE_CEF
        if (!cef_) return replyErr(id, "no browser instance (CEF not initialised)");
        std::string code =
            "(function(){"
            "  var el = document.querySelector(" + jsonStringLiteral(selector) + ");"
            "  if (!el) throw new Error('selector not found: ' + " + jsonStringLiteral(selector) + ");"
            "  el.value = " + jsonStringLiteral(value) + ";"
            "  el.dispatchEvent(new InputEvent('input', { bubbles: true }));"
            "  el.dispatchEvent(new Event('change', { bubbles: true }));"
            "  return String(el.tagName || '').toLowerCase();"
            "})()";
        bool done = false, ok = false;
        std::string val, err;
        cef_->eval(code, [&](bool o, const std::string& v, const std::string& e) { done = true; ok = o; val = v; err = e; });
        if (!cefWait([&] { return done; }, 15000)) return replyErr(id, "timeout filling element");
        if (!ok) return replyErr(id, err.empty() ? "fill failed" : err);
        return replyTag(id, val);
#else
        (void)selector; (void)value;
        return replyErr(id, "built without CEF");
#endif
    }

    // submit {selector}: a form submits itself (browser-style: no submit event, just navigate);
    // an input/button inside a form gets a real .click() — its default action routes through
    // requestSubmit(), so the page's submit handler runs first and can cancel. Chromium follows
    // the navigation natively; we give it a moment to settle.
    std::string cmdSubmit(long long id, const std::string& selector) {
#ifdef LUCYN_CONTROLLER_HAVE_CEF
        if (!cef_) return replyErr(id, "no browser instance (CEF not initialised)");
        std::string code =
            "(function(){"
            "  var el = document.querySelector(" + jsonStringLiteral(selector) + ");"
            "  if (!el) throw new Error('selector not found: ' + " + jsonStringLiteral(selector) + ");"
            "  var tag = String(el.tagName || '').toLowerCase();"
            "  if (tag === 'form') { el.submit(); }"
            "  else { el.click(); }"
            "  return tag;"
            "})()";
        bool done = false, ok = false;
        std::string val, err;
        cef_->eval(code, [&](bool o, const std::string& v, const std::string& e) { done = true; ok = o; val = v; err = e; });
        if (!cefWait([&] { return done; }, 15000)) return replyErr(id, "timeout submitting form");
        if (!ok) return replyErr(id, err.empty() ? "submit failed" : err);
        // A submitted form navigates; give Chromium a moment to follow it so the next
        // command (html/extract) sees the destination page, as the old funnel did.
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        cef_->pump();
        return replyTag(id, val);
#else
        (void)selector;
        return replyErr(id, "built without CEF");
#endif
    }

    // Helper: wrap a C++ string as a JS string literal (safe for inline eval).
    static std::string jsonStringLiteral(const std::string& s) {
        return "\"" + jsonEsc(s) + "\"";
    }

    std::string cmdMoonbug(long long id, const std::string& line) {
        int on = jsonGetBool(line, "on");
        if (on >= 0) eventsOn = on == 1;
        long long raw = jsonGetInt(line, "raw"), sgn = jsonGetInt(line, "signed"), dash = jsonGetInt(line, "dashboard");
        std::string file = jsonGetStr(line, "file");
        if (raw > 65535 || sgn > 65535 || dash > 65535) return replyErr(id, "port out of range (0..65535)");
        std::string err = startServers(int(raw), int(sgn), int(dash), file);
        if (!err.empty()) return replyErr(id, err);
        return "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"events\":" + (eventsOn ? "true" : "false") +
               ",\"raw\":" + std::to_string(srv ? srv->rawPort() : 0) + ",\"signed\":" + std::to_string(srv ? srv->signedPort() : 0) +
               ",\"dashboard\":" + std::to_string(srv ? srv->dashboardPort() : 0) + "}";
    }

    std::string cmdCaptures(long long id, long long limit) {
        if (limit < 1) limit = 20;
        if (limit > 500) limit = 500;
        std::string r = "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"total\":" + std::to_string(moonbug.total()) + ",\"captures\":[";
        bool first = true;
        for (auto& c : moonbug.recent(size_t(limit))) {
            if (!first) r += ',';
            first = false;
            r += net::Moonbug::toJson(c);
        }
        return r + "]}";
    }

    // ---- download: save a URL into nothing-data/downloads/ -----------------------------------------------
    // Same client as every page request (identity, cookie jar, Moonbug capture). The body is streamed to a hidden
    // temp file and renamed into place only after a complete 2xx transfer, so a failed or killed download never
    // leaves something that looks like a finished file. Page JS is not involved.
    std::string cmdDownload(long long id, const std::string& line) {
        const std::string url = jsonGetStr(line, "url");
        if (url.empty()) return replyErr(id, "'url' required");
        if (!isHttpUrl(url)) return replyErr(id, "only http:// and https:// URLs can be downloaded");

        std::string chosen;
        const std::string wanted = jsonGetStr(line, "name");
        if (!wanted.empty()) {
            chosen = sanitizeFileName(wanted);
            if (chosen.empty()) return replyErr(id, "'name' is not a usable file name");
        }
        const bool overwrite     = jsonGetBool(line, "overwrite") == 1;
        const long long maxBytes = jsonGetInt(line, "maxBytes");      // <= 0: no limit
        const std::string referer = jsonGetStr(line, "referer");

        const std::string dir = core::dataDir("downloads");
        if (dir.empty()) return replyErr(id, "cannot resolve the downloads folder");
        std::error_code ec;
        fs::create_directories(fs::u8path(dir), ec);

#ifdef LUCYN_CONTROLLER_HAVE_CEF
        if (!cef_) return replyErr(id, "no browser instance (CEF not initialised)");
        // Chromium fetches the body (same cookie jar + identity as a page). The body is
        // buffered (capped at maxBytes) and written to a hidden temp file, then renamed
        // into place only after a complete 2xx transfer — a failed or killed download
        // never leaves something that looks like a finished file.
        static std::atomic<unsigned> counter{0};
        const fs::path tmp = fs::u8path(dir) / (".download-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(counter++) + ".part");

        bool done = false;
        lucyn::cef_host::BrowserInstance::FetchResult fr;
        cef_->fetchBody(url, maxBytes, referer,
                        [&](const lucyn::cef_host::BrowserInstance::FetchResult& r) { done = true; fr = r; });
        if (!cefWait([&] { return done; }, 120000)) { fs::remove(tmp, ec); return replyErr(id, "timeout downloading"); }
        auto fail = [&](const std::string& msg) { fs::remove(tmp, ec); return replyErr(id, msg); };

        if (fr.truncated) return fail("download is larger than maxBytes (" + std::to_string(maxBytes) + ")");
        if (!fr.ok)       return fail(fr.error.empty() ? "download failed" : fr.error);

        {   // write the fetched body (possibly empty) to the temp file
            std::ofstream z(tmp, std::ios::binary | std::ios::trunc);
            if (!z) return fail("cannot write to the downloads folder");
            if (!fr.body.empty()) z.write(fr.body.data(), (std::streamsize)fr.body.size());
            if (!z) return fail("cannot write to the downloads folder");
        }

        if (chosen.empty()) chosen = sanitizeFileName(contentDispositionName(fr.contentDisposition));
        if (chosen.empty()) chosen = sanitizeFileName(urlLastSegment(fr.finalUrl.empty() ? url : fr.finalUrl));
        if (chosen.empty()) chosen = "download.bin";

        fs::path finalPath = uniqueDownloadPath(dir, chosen, overwrite);
        fs::rename(tmp, finalPath, ec);
        if (ec) return fail("cannot move the finished download into place: " + ec.message());

        const std::string type = lowerAscii(trimWs(fr.contentType.substr(0, fr.contentType.find(';'))));
        return "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"path\":\"" + jsonEsc(finalPath.generic_u8string()) +
               "\",\"name\":\"" + jsonEsc(finalPath.filename().generic_u8string()) +
               "\",\"bytes\":" + std::to_string(fr.bytes) +
               ",\"status\":" + std::to_string(fr.status) +
               ",\"type\":\"" + jsonEsc(type) +
               "\",\"url\":\"" + jsonEsc(fr.finalUrl.empty() ? url : fr.finalUrl) + "\"}";
#else
        (void)overwrite; (void)maxBytes; (void)referer;
        return replyErr(id, "built without CEF");
#endif
    }



    // ---- screenshot: capture Chromium's offscreen frame to a PNG (Session 19 step 7) -------
    // Drives the OSR renderer; no layout/paint of our own and no window ever exists.
    std::string cmdScreenshot(long long id, const std::string& line) {
#ifdef LUCYN_CONTROLLER_HAVE_CEF
        if (!cef_) return replyErr(id, "no browser instance (CEF not initialised)");
        std::string chosen;
        const std::string wanted = jsonGetStr(line, "name");
        if (!wanted.empty()) {
            chosen = sanitizeFileName(wanted);
            if (chosen.empty()) return replyErr(id, "'name' is not a usable file name");
        }
        if (chosen.empty()) chosen = "screenshot.png";
        if (chosen.size() < 4 || chosen.compare(chosen.size() - 4, 4, ".png") != 0) chosen += ".png";

        const std::string dir = core::dataDir("screenshots");
        if (dir.empty()) return replyErr(id, "cannot resolve the screenshots folder");
        std::error_code ec;
        fs::create_directories(fs::u8path(dir), ec);
        const fs::path finalPath = uniqueDownloadPath(dir, chosen, /*overwrite=*/false);

        bool done = false, ok = false;
        std::string val, err;
        cef_->screenshot(finalPath.u8string(), [&](bool o, const std::string& v, const std::string& e) {
            done = true; ok = o; val = v; err = e;
        });
        if (!cefWait([&] { return done; }, 15000)) return replyErr(id, "timeout capturing screenshot");
        if (!ok) return replyErr(id, err.empty() ? "screenshot failed" : err);
        return "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"path\":\"" + jsonEsc(val) + "\"}";
#else
        (void)line;
        return replyErr(id, "built without CEF");
#endif
    }

    std::string cmdInfo(long long id) {
        std::string r = "{\"id\":" + std::to_string(id) + ",\"ok\":true"
                        ",\"url\":\"" + jsonEsc(lastUrl) + "\""
                        ",\"captures\":" + std::to_string(moonbug.total()) + "}";
        return r;
    }

    // ---- run loop -----------------------------------------------------------

    int run() {
        if (!keyNote.empty()) emit(eventLine("note", keyNote));
        if (!adblockNote.empty()) emit(eventLine("note", adblockNote));
        std::string startErr = startServers(opts.moonbugRawPort, opts.nullbytePort, opts.dashboardPort, opts.dashboardFile);
        if (!startErr.empty()) emit(eventLine("note", "Moonbug: " + startErr));

        std::string line;
        while (std::getline(in, line)) {
            // Strip trailing CR (Windows line endings).
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            long long id  = jsonGetInt(line, "id");
            std::string cmd = jsonGetStr(line, "cmd");

            if (cmd.empty()) {
                if (id >= 0) reply(replyErr(id, "missing or empty 'cmd' field"));
                continue;
            }

            if (cmd == "quit") {
                if (id >= 0) reply(replyOk(id));
                return 0;
            } else if (cmd == "visit") {
                std::string url = jsonGetStr(line, "url");
                if (url.empty()) { reply(replyErr(id, "'url' required")); continue; }
                reply(cmdVisit(id, url));
#ifdef LUCYN_CONTROLLER_HAVE_SEARCH
            } else if (cmd == "search") {
                reply(cmdSearch(id, line));
#endif
            } else if (cmd == "html") {
                reply(cmdHtml(id));
            } else if (cmd == "extract") {
                std::string sel = jsonGetStr(line, "selector");
                if (sel.empty()) { reply(replyErr(id, "'selector' required")); continue; }
                reply(cmdExtract(id, sel));
            } else if (cmd == "extractAll") {
                std::string sel = jsonGetStr(line, "selector");
                if (sel.empty()) { reply(replyErr(id, "'selector' required")); continue; }
                reply(cmdExtractAll(id, sel));
            } else if (cmd == "eval") {
                std::string js = jsonGetStr(line, "js");
                reply(cmdEval(id, js));
            } else if (cmd == "waitFor") {
                std::string sel = jsonGetStr(line, "selector");
                if (sel.empty()) { reply(replyErr(id, "'selector' required")); continue; }
                reply(cmdWaitFor(id, sel, int(jsonGetInt(line, "timeout"))));
            } else if (cmd == "waitIdle") {
                reply(cmdWaitIdle(id, int(jsonGetInt(line, "quiet")), int(jsonGetInt(line, "timeout"))));
            } else if (cmd == "click") {
                std::string sel = jsonGetStr(line, "selector");
                if (sel.empty()) { reply(replyErr(id, "'selector' required")); continue; }
                reply(cmdClick(id, sel));
            } else if (cmd == "fill") {
                std::string sel = jsonGetStr(line, "selector");
                if (sel.empty()) { reply(replyErr(id, "'selector' required")); continue; }
                reply(cmdFill(id, sel, jsonGetStr(line, "value")));
            } else if (cmd == "submit") {
                std::string sel = jsonGetStr(line, "selector");
                if (sel.empty()) { reply(replyErr(id, "'selector' required")); continue; }
                reply(cmdSubmit(id, sel));
            } else if (cmd == "cookies") {
                reply(cmdCookies(id, jsonGetStr(line, "url")));
            } else if (cmd == "setCookie") {
                reply(cmdSetCookie(id, line));
            } else if (cmd == "clearCookies") {
#ifdef LUCYN_CONTROLLER_HAVE_CEF
                if (cef_) {
                    // CEF path: delete every cookie in Chromium's store.
                    bool done = false, ok = false;
                    std::string err2;
                    cef_->clearCookies([&](bool o, const std::string&, const std::string& e) {
                        done = true; ok = o; err2 = e;
                    });
                    if (!cefWait([&] { return done; }, 15000)) reply(replyErr(id, "timeout clearing cookies"));
                    else if (!ok) reply(replyErr(id, err2.empty() ? "cannot clear cookies" : err2));
                    else reply(replyOk(id));
                    continue;
                }
                reply(replyErr(id, "no browser instance (CEF not initialised)"));
#else
                reply(replyErr(id, "built without CEF"));
#endif
                continue;
            } else if (cmd == "download") {
                reply(cmdDownload(id, line));
            } else if (cmd == "screenshot") {
                reply(cmdScreenshot(id, line));
            } else if (cmd == "info") {
                reply(cmdInfo(id));
            } else if (cmd == "moonbug") {
                reply(cmdMoonbug(id, line));
            } else if (cmd == "captures") {
                reply(cmdCaptures(id, jsonGetInt(line, "limit")));
            } else {
                reply(replyErr(id, "unknown command: " + cmd));
            }
        }
        return 0;  // EOF = clean exit
    }
};

// ---- Controller public API --------------------------------------------------

Controller::Controller(ControllerOptions opts)
    : p_(std::make_unique<Impl>(std::move(opts)))
{}

Controller::~Controller() = default;

int Controller::run() { return p_->run(); }

}  // namespace lucyn::controller
