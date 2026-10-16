// controller.cpp — M4 JSON command loop implementation.
// Reads lines from stdin, dispatches, writes replies to stdout.
// Thread-safety: none needed — one command at a time.

#include <lucyn/controller/controller.h>
#include <lucyn/core/paths.h>
#include <lucyn/profile/identity.h>
#include <lucyn/profile/profile.h>
#include <lucyn/shell/hydrator.h>
#include <lucyn/shell/scripts.h>
#include <lucyn/shell/url.h>
#include <lucyn/js/quickjs_host.h>
#include <lucyn/net/http_client.h>
#include <lucyn/net/moonbug.h>
#include <lucyn/security/nullbyte.h>
#include <lucyn/server/capture_server.h>
#ifdef LUCYN_CONTROLLER_HAVE_SEARCH
#include <lucyn/search/search.h>
#endif
#include <blocker.h>
#include <filesystem>

// screenshot/window (SESSION11_PLAN.md Part D): independently optional, see
// the root CMakeLists.txt comment where LUCYN_CONTROLLER_HAVE_PAINT/_UI get
// defined. Neither define implies the other: a build can have paint+layout
// (screenshot works, no SDL needed) without ui (window replies ok:false),
// or — in principle, though no root option combination currently does this —
// the reverse.
#ifdef LUCYN_CONTROLLER_HAVE_PAINT
#include <lucyn/layout/layout.h>
#include <lucyn/layout/css.h>
#include <lucyn/paint/software_painter.h>
#ifdef LUCYN_HAVE_LITEHTML
#include <lucyn/layout/litehtml_layout.h>
#endif
#endif
#ifdef LUCYN_CONTROLLER_HAVE_LNM
#include <lucyn/lnm/app.h>
#endif
#ifdef LUCYN_CONTROLLER_HAVE_UI
#include <lucyn/ui/window.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
std::string eventLine(const std::string& type, const std::string& message) {
    return "{\"event\":\"" + jsonEsc(type) + "\",\"message\":\"" + jsonEsc(message) + "\"}";
}

// --- DOM helpers used by extract/extractAll ----------------------------------

// Walk the DOM and collect text content of nodes matching `selector`.
// We use the dom module's querySelector/querySelectorAll-style selectors
// via Node::select() which is already part of the dom module.
static std::vector<std::string> queryAll(const lucyn::dom::Document& doc, const std::string& sel) {
    std::vector<std::string> results;
    auto nodes = doc.querySelectorAll(sel);
    for (auto* n : nodes) results.push_back(n->textContent());
    return results;
}

static std::optional<std::string> queryOne(const lucyn::dom::Document& doc, const std::string& sel) {
    auto* n = doc.querySelector(sel);
    if (!n) return std::nullopt;
    return n->textContent();
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

std::string headerValue(const lucyn::net::Response& r, const std::string& lowerName) {
    for (auto& kv : r.headers) if (lowerAscii(kv.first) == lowerName) return kv.second;
    return {};
}

// scheme://host[:port] of an absolute URL ("" if it has none).
std::string originOfUrl(const std::string& url) {
    auto p = url.find("://");
    if (p == std::string::npos) return {};
    auto e = url.find_first_of("/?#", p + 3);
    return url.substr(0, e == std::string::npos ? url.size() : e);
}

// What a browser puts in Referer for a navigation from `from` to `to` (strict-origin-when-cross-origin).
std::string referrerFor(const std::string& from, const std::string& to) {
    if (from.empty()) return {};
    const std::string fo = originOfUrl(from);
    if (fo.empty()) return {};
    if (fo == originOfUrl(to)) return from.substr(0, from.find('#'));
    return fo + "/";
}

std::string stripFragment(const std::string& u) { return u.substr(0, u.find('#')); }

// <meta http-equiv="refresh" content="N; url=..."> -> a GET navigation. Only a refresh to a DIFFERENT page, due within 10 s, is
// followed: a bare "refresh this page every 30 s" must not turn into a reload loop. (<noscript> content never gets here: the
// Hydrator removes it for pages that run scripts.)
std::optional<lucyn::js::Navigation> metaRefreshOf(const lucyn::dom::Document& doc, const std::string& pageUrl) {
    for (const lucyn::dom::Node* m : doc.querySelectorAll("meta")) {
        const std::string* he = m->attr("http-equiv");
        const std::string* content = m->attr("content");
        if (!he || !content || lowerAscii(*he) != "refresh") continue;
        const std::string& c = *content;
        size_t i = 0;
        while (i < c.size() && std::isspace(static_cast<unsigned char>(c[i]))) ++i;
        size_t d0 = i;
        while (i < c.size() && (std::isdigit(static_cast<unsigned char>(c[i])) || c[i] == '.')) ++i;
        if (i == d0) continue;
        const double delay = std::atof(c.substr(d0, i - d0).c_str());
        if (delay > 10.0) continue;
        while (i < c.size() && (std::isspace(static_cast<unsigned char>(c[i])) || c[i] == ';' || c[i] == ',')) ++i;
        if (lowerAscii(c.substr(i, 3)) == "url") {
            i += 3;
            while (i < c.size() && std::isspace(static_cast<unsigned char>(c[i]))) ++i;
            if (i < c.size() && c[i] == '=') ++i;
            while (i < c.size() && std::isspace(static_cast<unsigned char>(c[i]))) ++i;
        }
        std::string target = c.substr(i);
        while (!target.empty() && std::isspace(static_cast<unsigned char>(target.back()))) target.pop_back();
        if (target.size() >= 2 && (target.front() == '\'' || target.front() == '"') && target.back() == target.front())
            target = target.substr(1, target.size() - 2);
        if (target.empty()) continue;
        std::string abs = lucyn::shell::resolveUrl(pageUrl, target);
        if (abs.rfind("http://", 0) != 0 && abs.rfind("https://", 0) != 0) continue;
        if (stripFragment(abs) == stripFragment(pageUrl)) continue;
        lucyn::js::Navigation nav;
        nav.url = abs;
        return nav;
    }
    return std::nullopt;
}

} // anon namespace

// ---- Controller::Impl -------------------------------------------------------


#ifdef LUCYN_CONTROLLER_HAVE_PAINT
// ---- blank-page guard -----------------------------------------------------------------------------------------------
// A page can lay out to nothing (JS emptied the body, content sits in <noscript>/<template>, the server sent a
// challenge page, ...). The window would then show pure white with no clue why. These helpers detect that and let
// the controller fall back to something readable.
namespace {

static size_t countPaintedContent(const lucyn::layout::Box& b) {
    size_t n = 0;
    if (b.visible && (!b.text.empty() || (b.node && b.node->name == "input"))) ++n;
    for (const auto& c : b.children) n += countPaintedContent(c);
    return n;
}

static bool documentPaintsContent(const lucyn::dom::Document& doc, float width, float height) {
    lucyn::layout::LayoutOptions o;
    o.viewportWidth = width;
    o.viewportHeight = height;
    return countPaintedContent(lucyn::layout::layoutDocument(doc, o)) > 0;
}

static std::string htmlEsc(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            default:  o += c;
        }
    }
    return o;
}

static bool isBreakTag(const std::string& t) {
    static const char* tags[] = {"div","p","li","ul","ol","br","tr","section","article","header","footer","main","nav",
                                 "h1","h2","h3","h4","h5","h6","form","table","blockquote","pre","aside","hr","dt","dd"};
    for (const char* x : tags) if (t == x) return true;
    return false;
}

// Plain text of a subtree, one block per line. Unlike Node::textContent it KEEPS <noscript> text (that is often the
// only human-readable thing on a JS challenge page) and appends link targets.
static void collectLines(const lucyn::dom::Node& n, std::string& cur, std::vector<std::string>& lines) {
    auto flush = [&] {
        std::string t;
        bool sp = false;
        for (unsigned char c : cur) {
            if (std::isspace(c)) { sp = !t.empty(); continue; }
            if (sp) { t += ' '; sp = false; }
            t += static_cast<char>(c);
        }
        if (!t.empty() && lines.size() < 400) lines.push_back(t.size() > 400 ? t.substr(0, 400) + "..." : t);
        cur.clear();
    };
    if (n.isText()) { cur += n.text; return; }
    if (n.isElement()) {
        const std::string& t = n.name;
        if (t == "script" || t == "style" || t == "head" || t == "template" || t == "svg") return;
        const bool brk = isBreakTag(t);
        if (brk) flush();
        for (const auto& c : n.children) collectLines(*c, cur, lines);
        if (t == "a") {
            const std::string* href = n.attr("href");
            if (href && href->rfind("http", 0) == 0 && cur.find(*href) == std::string::npos) cur += " (" + *href + ")";
        }
        if (brk) flush();
        return;
    }
    for (const auto& c : n.children) collectLines(*c, cur, lines);
    if (n.type == lucyn::dom::NodeType::Document) flush();
}

// A small readable page built from what the engine does know about a visit.
static std::unique_ptr<lucyn::dom::Document> buildReadableFallback(const lucyn::dom::Document* source,
                                                                   const std::string& url, long status,
                                                                   const std::string& title, size_t htmlBytes,
                                                                   const std::string& reason) {
    std::vector<std::string> lines;
    if (source) {
        std::string cur;
        collectLines(source->root(), cur, lines);
    }
    std::string h = "<!doctype html><html><head><title>" + htmlEsc(title) + "</title></head><body>";
    h += "<h2>" + htmlEsc(title.empty() ? url : title) + "</h2>";
    h += "<p>" + htmlEsc(url) + "</p>";
    h += "<p>LucyN text view (" + htmlEsc(reason) + "). HTTP " + std::to_string(status) + ", " +
         std::to_string(htmlBytes) + " bytes of HTML.</p><hr>";
    if (lines.empty())
        h += "<p>The page has no readable text. It is probably built entirely by scripts LucyN cannot run yet, "
             "or the site refused this client (HTTP " + std::to_string(status) + ").</p>";
    for (const auto& l : lines) h += "<p>" + htmlEsc(l) + "</p>";
    h += "</body></html>";
    return lucyn::dom::parseHtml(h);
}

}  // namespace
#endif

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
    net::Moonbug      moonbug;     // the wiretap; declared before `client` so it outlives it
    std::atomic<bool> eventsOn{false};
    net::Moonbug::ListenerId  eventTap = 0;
    profile::Switcher::Id     cookieListenerId_ = 0;
    profile::Switcher sw;
    net::HttpClient   client;
    profile::IdentityStore idStore;
    profile::IdentityManager idm;
    shell::ScriptRegistry scripts;
#ifdef LUCYN_CONTROLLER_HAVE_SEARCH
    lucyn::search::Registry searchEngines;
#endif

    // The hydrator is re-created per visit (stateless between visits).
    // We keep the last hydrate result alive so html/extract/eval can use it.
    // jsHost_ is kept alive across commands (keepAlive=true) so eval/waitFor/click work.
    std::optional<shell::HydrateResult> lastResult;
    std::unique_ptr<js::QuickJsHost>    jsHost_;
    shell::NativeHooks                  liveHooks_;  // persistent hooks for the live context
    std::string lastUrl;
#ifdef LUCYN_CONTROLLER_HAVE_PAINT
    // What the painter should draw for the current visit. null = the live document. Set by chooseRenderDocument()
    // when the live page would paint blank. The live document is never replaced (eval/extract keep using it).
    std::unique_ptr<lucyn::dom::Document> renderOverride_;
    std::string renderReason_;
    // Stylesheet built from the live document's <style> blocks and any
    // <link rel=stylesheet> CSS we managed to fetch. Rebuilt on each visit;
    // reused by screenshot/window so both render paths see the same styles.
    std::unique_ptr<lucyn::layout::Stylesheet> stylesheet_;

    // Build the stylesheet for the current page. Walks <style> blocks for their
    // text, and <link rel="stylesheet" href="..."> for external CSS (fetched
    // synchronously via the HTTP client — same path as <script src>). The
    // sheet is stored in stylesheet_ and used by every render path.
    void buildStylesheetFor(const lucyn::dom::Document& doc, const std::string& pageUrl) {
        stylesheet_ = std::make_unique<lucyn::layout::Stylesheet>();
        for (auto* node : doc.getElementsByTagName("style")) {
            std::string text;
            for (auto& c : node->children) if (c->isText()) text += c->text;
            if (!text.empty()) stylesheet_->add(text);
        }
        for (auto* link : doc.getElementsByTagName("link")) {
            const std::string* rel = link->attr("rel");
            if (!rel) continue;
            std::string relLower;
            for (char c : *rel) relLower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (relLower.find("stylesheet") == std::string::npos) continue;
            const std::string* href = link->attr("href");
            if (!href || href->empty()) continue;
            std::string abs = shell::resolveUrl(pageUrl, *href);
            // Fetch via the HTTP client. We use the same `dest: style` path
            // fetch/XHR uses, so adblock and impersonation apply.
            net::RequestOptions o;
            o.method = "GET";
            o.pageUrl = pageUrl;
            o.fetchMode = "no-cors";
            o.fetchDest = "style";
            o.requestType = adblock::RequestType::Stylesheet;
            auto resp = client.request(abs, o);
            if (!resp.ok() || resp.status < 200 || resp.status >= 300) continue;
            stylesheet_->add(resp.body);
        }
    }
#endif

    // Set when the user closes the window; ends the command loop so the process exits.
    std::atomic<bool> quitRequested_{false};

#ifdef LUCYN_CONTROLLER_HAVE_UI
    // Ghostful Mode: created lazily by the first "window" command (or at
    // startup if ControllerOptions ever grows a --show flag; not this
    // session, see SESSION11.md), hidden by default. One window for the
    // controller's whole lifetime, matching "one browser window" in practice.
    std::unique_ptr<lucyn::ui::Window> window_;
#endif
#ifdef LUCYN_CONTROLLER_HAVE_LNM
    std::unique_ptr<lucyn::lnm::App> lnmApp_;
    lucyn::dom::Node* focusedLnmInput_ = nullptr;
#endif

    std::istream& in;
    std::ostream& out;
    std::mutex outMu;              // fetch() runs on a worker thread, so captures can race with replies
    // Declared LAST: destroyed first, so its listeners stop before anything they use goes away.
    std::unique_ptr<server::CaptureServer> srv;

    explicit Impl(ControllerOptions o)
        : opts(std::move(o))
        , nullbyte(makeNullbyte(opts, &keyNote))
        , sw(opts.initialProfile)
        , client(sw)
        , idStore(profile::IdentityStore::defaultDir())
        , idm(sw, idStore)
        , in(opts.in ? *opts.in : std::cin)
        , out(opts.out ? *opts.out : std::cout)
    {
        // No user scripts in M4 — the controller manages all commands directly.
        // User scripts (nothing-data/scripts/) can be added later.
        client.setMoonbug(&moonbug);
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
        if (opts.adblockEnabled) {
            client.setRequestFilter([this](const std::string& url, const std::string& source, adblock::RequestType type) {
                return adblocker.check(adblock::Request::build(url, source, type)).matched_rule;
            });
        }
        client.setHeaderProvider([this](bool impersonating) { return personaHeaders(impersonating); });
        eventsOn = opts.captureEvents;
        eventTap = moonbug.onCapture([this](const net::Capture& c) { if (eventsOn) emit(requestEvent(c)); });
        // Cookie persistence: load the jar for the initial profile, then track every switch.
        if (opts.persistCookies) {
            loadCookies(sw.current().target);
            cookieListenerId_ = sw.subscribe([this](const profile::Profile& old, const profile::Profile& now) {
                saveCookies(old.target);
                loadCookies(now.target);
            });
        }
    }

    ~Impl() {
        if (cookieListenerId_) sw.unsubscribe(cookieListenerId_);
        srv.reset();                          // stop listeners first
        moonbug.off(eventTap);
        client.setHeaderProvider(nullptr);
        client.setRequestFilter(nullptr);
        client.setMoonbug(nullptr);
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

    js::HostOptions makeJsHostOpts() {
        js::HostOptions hopts;
        hopts.scriptTimeoutMs = opts.settleTimeoutMs > 0 ? opts.settleTimeoutMs : 5000;
        hopts.persona = [this]() -> std::optional<js::Persona> {
            auto id = idm.identity();
            auto su = idm.surface();
            if (!id || !su) return std::nullopt;
            return js::Persona{*su, id->device};
        };
        // ONE path for everything the page requests (fetch, XHR, <script src>): the full request with its page
        // context goes to the client, which sends browser-correct subresource headers and the shared cookie jar.
        hopts.httpFetch = [this](const js::HttpRequest& rq) -> js::HttpGetResult {
            net::RequestOptions o;
            o.method    = rq.method;
            o.body      = rq.body;
            o.hasBody   = rq.hasBody;
            o.headers   = rq.headers;
            o.pageUrl   = rq.pageUrl;
            o.fetchMode = rq.mode;
            o.fetchDest = rq.dest;
            if (rq.dest == "script") o.requestType = adblock::RequestType::Script;
            else if (rq.dest == "style") o.requestType = adblock::RequestType::Stylesheet;
            else if (rq.dest == "image") o.requestType = adblock::RequestType::Image;
            else if (rq.dest == "font") o.requestType = adblock::RequestType::Font;
            else if (rq.dest == "audio" || rq.dest == "video") o.requestType = adblock::RequestType::Media;
            else if (rq.dest == "document" || rq.dest == "iframe") o.requestType = adblock::RequestType::Subdocument;
            else o.requestType = adblock::RequestType::XmlHttpRequest;
            return toJs(client.request(rq.url, o));
        };
        // document.cookie reads and writes the SAME jar the HTTP requests use (HttpOnly cookies stay invisible to scripts).
        hopts.cookieGet = [this](const std::string& pageUrl) { return client.cookieStringFor(pageUrl); };
        hopts.cookieSet = [this](const std::string& pageUrl, const std::string& cookie) { client.setCookieFromScript(pageUrl, cookie); };
        if (opts.onConsole) hopts.onConsole = opts.onConsole;
        return hopts;
    }

    static js::HttpGetResult toJs(net::Response r) {
        js::HttpGetResult res;
        res.status       = r.status;
        res.body         = std::move(r.body);
        res.headers      = std::move(r.headers);
        res.effectiveUrl = std::move(r.effectiveUrl);
        res.error        = std::move(r.error);
        res.redirects    = r.redirects;
        res.statusText   = std::move(r.statusText);
        res.httpVersion  = std::move(r.httpVersion);
        return res;
    }

    // Headers the active identity puts on EVERY request. Called from worker threads (IdentityManager is
    // mutex-guarded). With libcurl-impersonate the library already sends the whole browser set in the right
    // order, so only what the library cannot know is added: Accept-Language, from the same Surface that feeds
    // navigator.languages (so the header and the page can never disagree). User-Agent and Sec-CH-UA are NOT
    // overridden there: a second opinion could only break the match with the TLS fingerprint. On stock libcurl
    // nothing is sent by default (not even a browser User-Agent), so the persona's headers fill that in.
    net::HeaderList personaHeaders(bool impersonating) {
        net::HeaderList h;
        auto su = idm.surface();
        if (!su) return h;
        if (!su->acceptLanguage.empty()) h.emplace_back("Accept-Language", su->acceptLanguage);
        if (!impersonating) {
            if (!su->userAgent.empty()) h.emplace_back("User-Agent", su->userAgent);
            if (!su->secChUa.empty()) {
                h.emplace_back("Sec-CH-UA", su->secChUa);
                h.emplace_back("Sec-CH-UA-Mobile", su->secChUaMobile);
                h.emplace_back("Sec-CH-UA-Platform", "\"" + su->secChUaPlatform + "\"");
            }
        }
        return h;
    }

    // ---- Cookie persistence helpers -----------------------------------------------------------------

    // Path for the cookie file for the given profile name.
    static std::string cookieFilePath(const std::string& profile) {
        return core::dataDir("cookies") + "/" + profile + ".txt";
    }

    // Save the current jar to disk (only when persistCookies is on).
    void saveCookies(const std::string& profile) {
        if (!opts.persistCookies || profile.empty()) return;
        std::string text = client.exportCookieFile();
        std::string path = cookieFilePath(profile);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (f) f << text;
    }

    // Load cookies from disk into the current jar (only when persistCookies is on).
    void loadCookies(const std::string& profile) {
        if (!opts.persistCookies || profile.empty()) return;
        std::string path = cookieFilePath(profile);
        std::ifstream f(path, std::ios::binary);
        if (!f) return;
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        client.importCookieFile(text);
    }

    static std::string cookieJson(const net::Cookie& c) {
        return "{\"name\":\"" + jsonEsc(c.name) + "\",\"value\":\"" + jsonEsc(c.value) + "\",\"domain\":\"" + jsonEsc(c.domain) +
               "\",\"path\":\"" + jsonEsc(c.path) + "\",\"hostOnly\":" + (c.hostOnly ? "true" : "false") +
               ",\"secure\":" + (c.secure ? "true" : "false") + ",\"httpOnly\":" + (c.httpOnly ? "true" : "false") +
               ",\"expires\":" + std::to_string(c.expires) + "}";
    }

    // cookies {url?}: with a url, exactly what a request to it would carry (HttpOnly included); without, the whole jar.
    std::string cmdCookies(long long id, const std::string& url) {
        std::vector<net::Cookie> list = url.empty() ? client.cookies() : client.cookiesFor(url, true);
        std::string s = "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"cookies\":[";
        for (size_t i = 0; i < list.size(); ++i) { if (i) s += ','; s += cookieJson(list[i]); }
        return s + "]}";
    }

    // setCookie {name, value, domain | url, path?, secure?, httpOnly?, expires?}: import a session. An `expires`
    // in the past deletes the cookie.
    std::string cmdSetCookie(long long id, const std::string& line) {
        net::Cookie c;
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
        if (!client.setCookie(c)) return replyErr(id, "cookie rejected (bad characters or no domain)");
        return replyOk(id);
    }

    // ---- command handlers ---------------------------------------------------

    // The most pages one visit()/click()/eval() will load by following what each page asks for (location.href, reload, a
    // submitted form, a meta refresh). A JS challenge needs one or two; the cap stops a page that redirects to itself forever.
    static constexpr int kMaxNavHops = 8;

    // Loads `first` (GET, or POST for a submitted form) and then keeps following whatever the page asks for until it settles on a
    // page that stays. Cookies live in the one shared jar, so a challenge page that sets a cookie and reloads gets the real page on
    // the second hop. Returns "" on success (lastResult/jsHost_ describe the final page) or an error message.
    // `referrer` is the URL of the page that started this navigation ("" for the address bar).
    std::string loadPageFollowing(js::Navigation next, std::string referrer) {
        for (int hop = 0;; ++hop) {
            // Tear down any existing live JS context (host first: it points into the document).
            jsHost_.reset();
            lastResult.reset();
#ifdef LUCYN_CONTROLLER_HAVE_PAINT
            renderOverride_.reset();
#endif
            const std::string hopUrl = next.url;
            shell::Fetcher fetcher = [this, next, referrer](const std::string& u) {
                shell::FetchResult r;
                net::RequestOptions o;
                if (u == next.url && next.method == "POST") {
                    o.method  = "POST";
                    o.body    = next.body;
                    o.hasBody = true;
                    if (!next.contentType.empty()) o.headers.emplace_back("Content-Type", next.contentType);
                    const std::string origin = originOfUrl(referrer);
                    if (!origin.empty()) o.headers.emplace_back("Origin", origin);
                }
                const std::string ref = referrerFor(referrer, u);
                if (!ref.empty()) o.headers.emplace_back("Referer", ref);
                auto resp = client.request(u, o);
                r.ok       = resp.ok();
                r.status   = resp.status;
                r.body     = std::move(resp.body);
                r.finalUrl = resp.effectiveUrl;
                r.error    = resp.error;
                return r;
            };

            // Build JS host as a persistent member so it survives across commands.
            jsHost_ = std::make_unique<js::QuickJsHost>(makeJsHostOpts());

            shell::Hydrator hy(fetcher, scripts, *jsHost_);
            hy.options.saveSnapshots    = false;
            hy.options.settleTimeoutMs  = opts.settleTimeoutMs > 0 ? opts.settleTimeoutMs : 5000;
            hy.options.keepAlive        = true;   // do NOT detach after settle(); eval/waitFor need the live context

            auto result = hy.hydrate(hopUrl);
            if (!result.ok) {
                jsHost_.reset();
                return result.error.empty() ? "navigation failed: " + hopUrl : result.error;
            }

            // Emit notes and page errors as events.
            for (auto& n : jsHost_->notes())
                emit(eventLine("note", n));
            for (auto& e : jsHost_->pageErrors())
                emit(eventLine("pageError", e));

            lastUrl    = result.url;
            lastResult = std::move(result);

            // Did the page ask to go somewhere? (script first, then a <meta refresh>)
            std::optional<js::Navigation> want = jsHost_->takeNavigation();
            if (!want && lastResult->document) want = metaRefreshOf(*lastResult->document, lastResult->url);
            if (!want) break;
            if (hop + 1 >= kMaxNavHops) {
                emit(eventLine("note", "stopped after " + std::to_string(kMaxNavHops) + " page loads; the page still wants " +
                                           want->method + " " + want->url));
                break;
            }
            emit(eventLine("note", "page navigated: " + want->method + " " + want->url));
            referrer = lastResult->url;
            next     = *want;
        }

#ifdef LUCYN_CONTROLLER_HAVE_PAINT
        // Build the stylesheet before chooseRenderDocument() so the blank-page
        // guard sees the same styles as the eventual screenshot. The sheet is
        // rebuilt on every visit (styles can change between pages).
        if (lastResult && lastResult->document) buildStylesheetFor(*lastResult->document, lastResult->url);
        chooseRenderDocument();
#endif
#ifdef LUCYN_CONTROLLER_HAVE_UI
        presentCurrentView();
#endif

#ifdef LUCYN_CONTROLLER_HAVE_UI
        // A freshly-created jsHost_ starts the new page at the prelude's own
        // default (__lucynHidden = true); sync it to the real window's actual
        // current state immediately, so a page loaded while the window is
        // already visible does not briefly/incorrectly report itself hidden.
        pushVisibilityToPage();
#endif

        // Persist cookies acquired during the page load (Set-Cookie headers).
        saveCookies(sw.current().target);
        return {};
    }

    // A click, eval or wait can make the live page ask to navigate (a link, a submit button, location.href = ...).
    // Follows it and returns "" (nothing to follow, or it loaded) or an error message.
    std::string followPageNavigation() {
        if (!jsHost_) return {};
        auto nav = jsHost_->takeNavigation();
        if (!nav) return {};
        emit(eventLine("note", "page navigated: " + nav->method + " " + nav->url));
        return loadPageFollowing(*nav, lastResult ? lastResult->url : std::string());
    }

    std::string cmdVisit(long long id, const std::string& url, const std::string& profile) {
        // Switch profile if requested.
        if (!profile.empty()) {
            std::string err;
            if (!sw.select(profile, &err))
                return replyErr(id, "profile switch failed: " + err);
        }

        js::Navigation first;
        first.url = url;
        std::string err = loadPageFollowing(first, {});
        if (!err.empty()) return replyErr(id, err);

        // Reply with status + title.
        std::string r = "{\"id\":" + std::to_string(id) + ",\"ok\":true"
                        ",\"status\":" + std::to_string(lastResult->status) +
                        ",\"url\":\"" + jsonEsc(lastResult->url) + "\""
                        ",\"title\":\"" + jsonEsc(lastResult->title) + "\"}";
        return r;
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
        return cmdVisit(id, *target, {});
    }
#endif

    std::string cmdHtml(long long id) {
        if (!lastResult) return replyErr(id, "no page loaded");
        return replyValue(id, lastResult->afterHtml);
    }

    std::string cmdExtract(long long id, const std::string& selector) {
        if (!lastResult || !lastResult->document)
            return replyErr(id, "no page loaded");
        auto v = queryOne(*lastResult->document, selector);
        if (!v) return replyErr(id, "selector not found: " + selector);
        return replyValue(id, *v);
    }

    std::string cmdExtractAll(long long id, const std::string& selector) {
        if (!lastResult || !lastResult->document)
            return replyErr(id, "no page loaded");
        auto values = queryAll(*lastResult->document, selector);
        return replyValues(id, values);
    }

    std::string cmdEval(long long id, const std::string& js) {
        if (!jsHost_) return replyErr(id, "no page loaded; call visit() first");
        std::string result, error;
        if (!jsHost_->eval(js, &result, &error))
            return replyErr(id, error);
        std::string navErr = followPageNavigation();      // eval("location.reload()") and friends really navigate now
        if (!navErr.empty()) return replyErr(id, navErr);
        return replyValue(id, result);
    }

    std::string cmdWaitFor(long long id, const std::string& selector, int timeoutMs) {
        if (!lastResult || !lastResult->document)
            return replyErr(id, "no page loaded");
        if (!jsHost_)
            return replyErr(id, "JS context not available");
        if (timeoutMs <= 0) timeoutMs = opts.settleTimeoutMs > 0 ? opts.settleTimeoutMs : 5000;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            // pump timers + jobs so dynamic content can render
            jsHost_->settle(50);
            if (lastResult->document->querySelector(selector))
                return replyValue(id, selector);
            if (std::chrono::steady_clock::now() >= deadline)
                return replyErr(id, "timeout waiting for selector: " + selector);
        }
    }

    std::string cmdWaitIdle(long long id, int quietMs, int timeoutMs) {
        if (quietMs <= 0)   quietMs   = 500;
        if (timeoutMs <= 0) timeoutMs = opts.settleTimeoutMs > 0 ? opts.settleTimeoutMs : 5000;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        size_t prevTotal = moonbug.total();
        auto quiteStart = std::chrono::steady_clock::now();
        for (;;) {
            if (jsHost_) jsHost_->settle(50);
            else std::this_thread::sleep_for(std::chrono::milliseconds(50));
            size_t nowTotal = moonbug.total();
            // A request that is still running is not "quiet": without this a slow response looked idle.
            if (nowTotal != prevTotal || moonbug.inFlight() > 0) { prevTotal = nowTotal; quiteStart = std::chrono::steady_clock::now(); }
            auto quiet = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - quiteStart).count();
            if (quiet >= quietMs) return replyOk(id);
            if (std::chrono::steady_clock::now() >= deadline)
                return replyErr(id, "timeout waiting for network idle");
        }
    }

    std::string cmdClick(long long id, const std::string& selector) {
        if (!jsHost_) return replyErr(id, "no page loaded");
        // Use eval to call .click() on the element, then settle briefly.
        std::string code =
            "(function(){"
            "  var el = document.querySelector(" + jsonStringLiteral(selector) + ");"
            "  if (!el) throw new Error('selector not found: ' + " + jsonStringLiteral(selector) + ");"
            "  el.click();"
            "  return el.tagName || 'OK';"
            "})()";
        std::string result, error;
        if (!jsHost_->eval(code, &result, &error))
            return replyErr(id, error);
        jsHost_->settle(200);
        std::string navErr = followPageNavigation();      // a clicked link / submit button loads its page
        if (!navErr.empty()) return replyErr(id, navErr);
        return replyValue(id, result);
    }

    // Helper: wrap a C++ string as a JS string literal (safe for inline eval).
    static std::string jsonStringLiteral(const std::string& s) {
        return "\"" + jsonEsc(s) + "\"";
    }

    std::string cmdProfile(long long id, const std::string& name) {
        std::string err;
        if (!sw.select(name, &err))
            return replyErr(id, "profile switch failed: " + err);
        return replyValue(id, sw.current().target);
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

        static std::atomic<unsigned> counter{0};
        const fs::path tmp = fs::u8path(dir) / (".download-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(counter++) + ".part");

        struct Sink { std::ofstream f; unsigned long long n = 0; bool tooBig = false, diskErr = false; } sk;
        net::RequestOptions ro;
        ro.requestType = adblock::RequestType::Other;
        ro.timeoutSec = 0;                       // big files take longer than the 30 s page default
        if (!referer.empty()) ro.headers.emplace_back("Referer", referer);
        ro.bodySink = [&](const char* d, size_t len) {
            if (maxBytes > 0 && sk.n + len > (unsigned long long)maxBytes) { sk.tooBig = true; return false; }
            if (!sk.f.is_open()) {
                sk.f.open(tmp, std::ios::binary | std::ios::trunc);
                if (!sk.f) { sk.diskErr = true; return false; }
            }
            sk.f.write(d, (std::streamsize)len);
            if (!sk.f) { sk.diskErr = true; return false; }
            sk.n += len;
            return true;
        };

        net::Response r = client.request(url, ro);
        if (sk.f.is_open()) { sk.f.close(); if (sk.f.fail()) sk.diskErr = true; }
        auto fail = [&](const std::string& msg) { fs::remove(tmp, ec); return replyErr(id, msg); };

        if (sk.tooBig)  return fail("download is larger than maxBytes (" + std::to_string(maxBytes) + ")");
        if (sk.diskErr) return fail("cannot write to the downloads folder");
        if (!r.ok())    return fail(r.error);
        if (r.status < 200 || r.status >= 300)
            return fail("HTTP " + std::to_string(r.status) + (r.statusText.empty() ? "" : " " + r.statusText));

        if (!fs::exists(tmp, ec)) {              // a 2xx with an empty body: the sink never ran
            std::ofstream z(tmp, std::ios::binary | std::ios::trunc);
            if (!z) return fail("cannot write to the downloads folder");
        }

        if (chosen.empty()) chosen = sanitizeFileName(contentDispositionName(headerValue(r, "content-disposition")));
        if (chosen.empty()) chosen = sanitizeFileName(urlLastSegment(r.effectiveUrl.empty() ? url : r.effectiveUrl));
        if (chosen.empty()) chosen = "download.bin";

        fs::path finalPath = uniqueDownloadPath(dir, chosen, overwrite);
        fs::rename(tmp, finalPath, ec);
        if (ec) return fail("cannot move the finished download into place: " + ec.message());

        const std::string ct = headerValue(r, "content-type");
        const std::string type = lowerAscii(trimWs(ct.substr(0, ct.find(';'))));
        return "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"path\":\"" + jsonEsc(finalPath.generic_u8string()) +
               "\",\"name\":\"" + jsonEsc(finalPath.filename().generic_u8string()) +
               "\",\"bytes\":" + std::to_string(r.bodyBytes) +
               ",\"status\":" + std::to_string(r.status) +
               ",\"type\":\"" + jsonEsc(type) +
               "\",\"url\":\"" + jsonEsc(r.effectiveUrl.empty() ? url : r.effectiveUrl) + "\"}";
    }

#ifdef LUCYN_CONTROLLER_HAVE_PAINT
    // ---- screenshot: render the current page through layout + SoftwarePainter, offscreen -------------------
    // No SDL and no display needed (SESSION11_PLAN.md: "This must work with no SDL and no display"), so this
    // works in a sandbox with no GPU, same as every other command.
    // The document the painter should draw: the live page unless it would paint blank (see chooseRenderDocument).
    const lucyn::dom::Document& renderDocument() const {
        return renderOverride_ ? *renderOverride_ : *lastResult->document;
    }

    // Blank-page guard. Order: live page -> the HTML the server sent -> a readable text view. Always leaves the
    // window showing *something* and says why in a "note" event, so a white window is never a mystery again.
    void chooseRenderDocument(float w = 0, float h = 0) {
        renderOverride_.reset();
        renderReason_.clear();
        if (!lastResult || !lastResult->document) return;
#ifdef LUCYN_CONTROLLER_HAVE_UI
        if (w <= 0 && window_) { w = static_cast<float>(window_->width()); h = static_cast<float>(window_->height()); }
#endif
        if (w <= 0) { w = 800.0f; h = 600.0f; }
        if (documentPaintsContent(*lastResult->document, w, h)) return;

        auto before = lucyn::dom::parseHtml(lastResult->beforeHtml);
        const size_t liveText = lastResult->document->body() ? lastResult->document->body()->textContent().size() : 0;
        const size_t bodyBytes = lastResult->beforeHtml.size();
        if (documentPaintsContent(*before, w, h)) {
            renderReason_ = "page painted blank after scripts ran; showing the HTML the server sent";
            renderOverride_ = std::move(before);
        } else {
            renderReason_ = "page has nothing LucyN can paint";
            renderOverride_ = buildReadableFallback(before.get(), lastResult->url, lastResult->status,
                                                    lastResult->title, bodyBytes, renderReason_);
        }
        emit(eventLine("note", "render guard: " + renderReason_ + " (live body text " + std::to_string(liveText) +
                       " chars, server HTML " + std::to_string(bodyBytes) + " bytes, HTTP " +
                       std::to_string(lastResult->status) + ")"));
    }

    std::string cmdScreenshot(long long id, const std::string& line) {
        if (!lastResult || !lastResult->document) return replyErr(id, "no page loaded");

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

        const long long reqWidth = jsonGetInt(line, "width");
        lucyn::layout::LayoutOptions opts;
        opts.viewportWidth = reqWidth > 0 ? static_cast<float>(reqWidth) : 800.0f;

        if (!renderOverride_ && !documentPaintsContent(*lastResult->document, opts.viewportWidth, 600.0f))
            chooseRenderDocument(opts.viewportWidth, 600.0f);

#ifdef LUCYN_HAVE_LITEHTML
        lucyn::layout::LitehtmlRenderer lhRenderer(static_cast<int>(opts.viewportWidth), 600);
        lhRenderer.loadHtml(renderDocument().root().outerHTML(), "");
        lhRenderer.render();
        const lucyn::paint::DisplayList& displayList = lhRenderer.displayList();
        int height = static_cast<int>(lhRenderer.documentHeight()) > 0
                     ? static_cast<int>(lhRenderer.documentHeight()) + 20 : 600;
        lucyn::paint::Framebuffer fb;
        fb.init(static_cast<int>(opts.viewportWidth), height, lucyn::paint::packRGBA(255, 255, 255, 255));
        lucyn::paint::SoftwarePainter painter(fb, &lhRenderer.imageTable());
        painter.draw(displayList);
#else
        lucyn::layout::Box root = lucyn::layout::layoutDocument(renderDocument(), opts, stylesheet_.get());
        lucyn::paint::DisplayList displayList = lucyn::layout::paint(root);
        lucyn::paint::Framebuffer fb;
        int height = static_cast<int>(root.height) > 0 ? static_cast<int>(root.height) + 20 : 600;
        fb.init(static_cast<int>(opts.viewportWidth), height, lucyn::paint::packRGBA(255, 255, 255, 255));
        lucyn::paint::SoftwarePainter painter(fb);
        painter.draw(displayList);
#endif

        fs::path finalPath = uniqueDownloadPath(dir, chosen, /*overwrite=*/false);
        if (!fb.savePNG(finalPath.u8string()))
            return replyErr(id, "failed to write the screenshot file");

#ifdef LUCYN_CONTROLLER_HAVE_UI
        // If a window exists (even hidden — Ghostful Mode), also present this
        // render into it, so "window" and "screenshot" never disagree about
        // what the page looks like.
        if (window_) window_->present(fb);
#endif

        return "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"path\":\"" +
               jsonEsc(finalPath.generic_u8string()) + "\"}";
    }
#endif

#ifdef LUCYN_CONTROLLER_HAVE_UI
#if defined(LUCYN_CONTROLLER_HAVE_PAINT)
    void presentDocument(const lucyn::dom::Document& document) {
        if (!window_) return;
        lucyn::paint::Framebuffer fb;
        fb.init(window_->width(), window_->height(), lucyn::paint::packRGBA(255, 255, 255, 255));
#ifdef LUCYN_HAVE_LITEHTML
        lucyn::layout::LitehtmlRenderer lhRenderer(window_->width(), window_->height());
        lhRenderer.loadHtml(document.root().outerHTML(), "");
        lhRenderer.render();
        lucyn::paint::SoftwarePainter painter(fb, &lhRenderer.imageTable());
        painter.draw(lhRenderer.displayList());
#else
        lucyn::layout::LayoutOptions options;
        options.viewportWidth = static_cast<float>(window_->width());
        options.viewportHeight = static_cast<float>(window_->height());
        lucyn::layout::Box root = lucyn::layout::layoutDocument(document, options, stylesheet_.get());
        lucyn::paint::DisplayList list = lucyn::layout::paint(root);
        lucyn::paint::SoftwarePainter painter(fb);
        painter.draw(list);
#endif
        window_->present(fb);
    }
#endif

    void presentCurrentView() {
#if defined(LUCYN_CONTROLLER_HAVE_PAINT)
        if (!window_) return;
        if (lastResult && lastResult->document) {
            presentDocument(renderDocument());
            return;
        }
#ifdef LUCYN_CONTROLLER_HAVE_LNM
        if (lnmApp_ && lnmApp_->document()) presentDocument(*lnmApp_->document());
#endif
#endif
    }

#if defined(LUCYN_CONTROLLER_HAVE_PAINT) && defined(LUCYN_CONTROLLER_HAVE_LNM)
    static const lucyn::dom::Node* inputAt(const lucyn::layout::Box& box, float x, float y) {
        for (auto it = box.children.rbegin(); it != box.children.rend(); ++it)
            if (const auto* found = inputAt(*it, x, y)) return found;
        if (box.node && box.node->name == "input" && x >= box.x && y >= box.y &&
            x < box.x + box.width && y < box.y + box.height)
            return box.node;
        return nullptr;
    }
#endif

    static const lucyn::dom::Node* firstInput(const lucyn::layout::Box& box) {
        if (box.node && box.node->name == "input") return box.node;
        for (const auto& child : box.children)
            if (const auto* found = firstInput(child)) return found;
        return nullptr;
    }

    // Typing with nothing focused goes to the first input, so the search box works without a prior click
    // (a click can be eaten by window focus on some desktops). Returns true if an input got focus.
    bool focusFirstLnmInput() {
        if (!lnmApp_ || lastResult || !lnmApp_->document() || !window_) return false;
        lucyn::layout::LayoutOptions opts;
        opts.viewportWidth = static_cast<float>(window_->width());
        opts.viewportHeight = static_cast<float>(window_->height());
        auto root = lucyn::layout::layoutDocument(*lnmApp_->document(), opts);
        focusedLnmInput_ = const_cast<lucyn::dom::Node*>(firstInput(root));
        return focusedLnmInput_ != nullptr;
    }

    // Esc on a loaded page: drop the page and show the LNM home view again.
    void goHome() {
        if (!lnmApp_ || !lastResult) return;
        lastResult.reset();      // before jsHost_: the page's document belongs to that run
        jsHost_.reset();
        lastUrl.clear();
        focusedLnmInput_ = nullptr;
        pushVisibilityToPage();
        emit(eventLine("note", "returned to the LNM home view"));
    }

    void submitLnmInput() {
#if defined(LUCYN_CONTROLLER_HAVE_LNM) && defined(LUCYN_CONTROLLER_HAVE_PAINT)
        if (!lnmApp_ || lastResult || !focusedLnmInput_) return;
        const std::string* type = focusedLnmInput_->attr("type");
        const std::string* value = focusedLnmInput_->attr("value");
        if (!type || !value || value->empty()) return;
        const std::string inputType = *type;
        const std::string query = *value;
        if (inputType == "search") {
#ifdef LUCYN_CONTROLLER_HAVE_SEARCH
            std::string command = "{\"cmd\":\"search\",\"query\":\"" + jsonEsc(query) + "\"";
            if (const std::string* engine = focusedLnmInput_->attr("data-engine"))
                command += ",\"engine\":\"" + jsonEsc(*engine) + "\"";
            command += "}";
            std::string result = cmdSearch(0, command);
            if (result.find("\"ok\":false") != std::string::npos)
                emit(eventLine("note", "LNM search failed: " + jsonGetStr(result, "error")));
            else
                emit(eventLine("note", "LNM search loaded " + jsonGetStr(result, "url")));
            focusedLnmInput_ = nullptr;
#else
            emit(eventLine("note", "LNM search needs the search module in this build"));
#endif
        } else if (inputType == "url") {
            std::string url = query;
            if (url.find("://") == std::string::npos) url = "https://" + url;
            std::string result = cmdVisit(0, url, {});
            if (result.find("\"ok\":false") != std::string::npos)
                emit(eventLine("note", "LNM navigation failed: " + jsonGetStr(result, "error")));
            else
                emit(eventLine("note", "LNM opened " + jsonGetStr(result, "url")));
            focusedLnmInput_ = nullptr;
        }
#endif
    }

    void pumpWindowEvents() {
#ifdef LUCYN_CONTROLLER_HAVE_UI
        if (!window_) return;
        bool sawQuit = false;
        bool repaint = false;
        for (const auto& ev : window_->pollEvents()) {
            if (ev.type == lucyn::ui::EventType::Quit) {
                sawQuit = true;
            } else if (ev.type == lucyn::ui::EventType::Resize) {
#ifdef LUCYN_CONTROLLER_HAVE_LNM
                if (lnmApp_) lnmApp_->setViewport({static_cast<float>(ev.width), static_cast<float>(ev.height)});
#endif
                repaint = true;
            }
#if defined(LUCYN_CONTROLLER_HAVE_LNM) && defined(LUCYN_CONTROLLER_HAVE_PAINT)
            else if (ev.type == lucyn::ui::EventType::MouseClick && lnmApp_ && !lastResult && lnmApp_->document()) {
                lucyn::layout::LayoutOptions opts;
                opts.viewportWidth = static_cast<float>(window_->width());
                opts.viewportHeight = static_cast<float>(window_->height());
                auto root = lucyn::layout::layoutDocument(*lnmApp_->document(), opts);
                focusedLnmInput_ = const_cast<lucyn::dom::Node*>(inputAt(root, static_cast<float>(ev.clickX), static_cast<float>(ev.clickY)));
                repaint = true;
            } else if (ev.type == lucyn::ui::EventType::TextInput && !lastResult && lnmApp_ &&
                       (focusedLnmInput_ || focusFirstLnmInput())) {
                const std::string* old = focusedLnmInput_->attr("value");
                std::string value = old ? *old : "";
                value += ev.text;
                focusedLnmInput_->setAttr("value", value);
                repaint = true;
            } else if (ev.type == lucyn::ui::EventType::KeyDown && ev.key == 27 && lnmApp_ && lastResult) {
                goHome();
                repaint = true;
            } else if (ev.type == lucyn::ui::EventType::KeyDown && focusedLnmInput_ && !lastResult) {
                const std::string* old = focusedLnmInput_->attr("value");
                std::string value = old ? *old : "";
                if (ev.ctrl && (ev.key == 'a' || ev.key == 'A')) {
                    value.clear();
                    focusedLnmInput_->setAttr("value", value);
                    repaint = true;
                } else if (ev.key == 8 && !value.empty()) {
                    size_t start = value.size() - 1;
                    while (start > 0 && (static_cast<unsigned char>(value[start]) & 0xC0) == 0x80) --start;
                    value.erase(start);
                    focusedLnmInput_->setAttr("value", value);
                    repaint = true;
                } else if (ev.key == 13 || ev.key == 1073741912 /* SDLK_KP_ENTER */) {
                    submitLnmInput();
                    repaint = true;
                } else if (ev.key == 27) {
                    focusedLnmInput_ = nullptr;
                    repaint = true;
                }
            }
#endif
        }
        if (sawQuit) {
            // The user closed the window: end the whole engine, not just the window. (Before, this only hid the
            // window and the stdin command loop kept running with nothing to show for it.)
            quitRequested_ = true;
            window_->setVisible(false);
            pushVisibilityToPage();
            emit(eventLine("note", "window closed; shutting down"));
        }
        if (repaint) presentCurrentView();
#endif
    }

    bool readLineWithWindowEvents(std::string& line) {
#ifdef LUCYN_CONTROLLER_HAVE_UI
        if (quitRequested_) return false;
        if (window_ && &in == &std::cin) {
            // The reader thread blocks in getline() and cannot be interrupted, so it must never be joined while the
            // window can still close (that is what kept the terminal alive). State is shared and the thread detached:
            // if the window closes first we simply stop waiting and the process exits under it.
            struct Shared {
                std::mutex m;
                std::condition_variable cv;
                bool finished = false;
                bool ok = false;
                std::string value;
            };
            auto st = std::make_shared<Shared>();
            std::istream* src = &in;
            std::thread([st, src] {
                std::string v;
                bool ok = static_cast<bool>(std::getline(*src, v));
                {
                    std::lock_guard<std::mutex> lock(st->m);
                    st->value = std::move(v);
                    st->ok = ok;
                    st->finished = true;
                }
                st->cv.notify_one();
            }).detach();
            for (;;) {
                pumpWindowEvents();
                if (quitRequested_) return false;
                std::unique_lock<std::mutex> lock(st->m);
                if (st->cv.wait_for(lock, std::chrono::milliseconds(8), [&] { return st->finished; })) break;
            }
            line = std::move(st->value);
            pumpWindowEvents();
            return st->ok;
        }
#endif
        return static_cast<bool>(std::getline(in, line));
    }

    // "Consistent lies while hidden" (README, SESSION11_PLAN.md Part D): push the real window's
    // visible/hidden state into the live page, if one exists, via the same eval() path the
    // "eval" command uses. Best-effort: a page script that clobbered window.__lucynSetHidden
    // should not break window/visit commands over it.
    void pushVisibilityToPage() {
        if (!jsHost_) return;
        bool hidden = !window_ || !window_->visible();
        std::string js = std::string("window.__lucynSetHidden(") + (hidden ? "true" : "false") + ")";
        std::string result, error;
        jsHost_->eval(js, &result, &error);
    }

    // ---- window: show/hide/resize the (always-real) Ghostful Mode window -------------------------------------
    std::string cmdWindow(long long id, const std::string& line) {
        const long long w = jsonGetInt(line, "width");
        const long long h = jsonGetInt(line, "height");
        const int visibleFlag = jsonGetBool(line, "visible");  // -1 = not given

        if (!window_) {
            window_ = std::make_unique<lucyn::ui::Window>();
            int cw = w > 0 ? static_cast<int>(w) : 1024;
            int ch = h > 0 ? static_cast<int>(h) : 768;
            bool startVisible = visibleFlag == 1;
            if (!window_->create(cw, ch, "LucyN", startVisible)) {
                std::string err = "failed to create the window: " + window_->lastError();
                window_.reset();
                return replyErr(id, err);
            }
        } else {
            if (w > 0 || h > 0) {
                int cw = w > 0 ? static_cast<int>(w) : window_->width();
                int ch = h > 0 ? static_cast<int>(h) : window_->height();
                bool wasVisible = window_->visible();
                if (!window_->create(cw, ch, "LucyN", wasVisible)) {
                    std::string err = "failed to resize the window: " + window_->lastError();
                    return replyErr(id, err);
                }
            }
            if (visibleFlag == 0) window_->setVisible(false);
            if (visibleFlag == 1) window_->setVisible(true);
        }

        pushVisibilityToPage();
        presentCurrentView();

        return "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"visible\":" +
               (window_->visible() ? "true" : "false") +
               ",\"width\":" + std::to_string(window_->width()) +
               ",\"height\":" + std::to_string(window_->height()) + "}";
    }
#else
    // Built without SDL2 (LUCYN_MODULE_UI off, or SDL2 not found): "window" is
    // still a recognized command — never "unknown command: window" — it just
    // honestly can't do anything (SESSION11_PLAN.md: "replies ok:false with a
    // clear message if built without it").
    std::string cmdWindow(long long id, const std::string& /*line*/) {
        return replyErr(id, "built without SDL2 (ui module not available) — install libsdl2-dev and rebuild");
    }
#endif

    std::string cmdInfo(long long id) {
        const auto p = sw.current();
        std::string r = "{\"id\":" + std::to_string(id) + ",\"ok\":true"
                        ",\"profile\":\"" + jsonEsc(p.target) + "\""
                        ",\"url\":\"" + jsonEsc(lastUrl) + "\""
                        ",\"impersonating\":" + (client.impersonating() ? "true" : "false") +
                        ",\"captures\":" + std::to_string(moonbug.total()) + "}";
        return r;
    }

    // ---- run loop -----------------------------------------------------------

    int run() {
        if (!keyNote.empty()) emit(eventLine("note", keyNote));
        if (!adblockNote.empty()) emit(eventLine("note", adblockNote));
        std::string startErr = startServers(opts.moonbugRawPort, opts.nullbytePort, opts.dashboardPort, opts.dashboardFile);
        if (!startErr.empty()) emit(eventLine("note", "Moonbug: " + startErr));

#ifdef LUCYN_CONTROLLER_HAVE_LNM
        if (!opts.lnmEntry.empty()) {
            const fs::path entry = fs::u8path(opts.lnmEntry);
            const fs::path base = entry.parent_path();
            lnmApp_ = std::make_unique<lucyn::lnm::App>(lucyn::lnm::diskReader(base.u8string()));
            std::ifstream file(entry, std::ios::binary);
            std::ostringstream source;
            if (!file) {
                emit(eventLine("note", "LNM entry could not be read: " + entry.u8string()));
                lnmApp_.reset();
            } else {
                source << file.rdbuf();
                if (!lnmApp_->load(source.str())) {
                    for (const auto& d : lnmApp_->diagnostics())
                        emit(eventLine("note", "LNM: " + d.message));
                    lnmApp_.reset();
                }
            }
        }
#elif !defined(LUCYN_CONTROLLER_HAVE_LNM)
        if (!opts.lnmEntry.empty()) emit(eventLine("note", "LNM entry configured but this build has no LNM module"));
#endif

        if (opts.showWindow || opts.ghostfulWindow
#ifdef LUCYN_CONTROLLER_HAVE_LNM
            || lnmApp_
#endif
        ) {
#ifdef LUCYN_CONTROLLER_HAVE_UI
            window_ = std::make_unique<lucyn::ui::Window>();
            bool startVisible = opts.showWindow;  // ghostfulWindow alone: created, but hidden
            if (!window_->create(1024, 768, "LucyN", startVisible)) {
                emit(eventLine("note", "window creation at startup failed: " + window_->lastError()));
                window_.reset();
            } else presentCurrentView();
#else
            emit(eventLine("note", "showWindow/ghostfulWindow was requested but this build has no ui module (SDL2 not found) — staying headless"));
#endif
        }
        std::string line;
        while (readLineWithWindowEvents(line)) {
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
                std::string url     = jsonGetStr(line, "url");
                std::string profile = jsonGetStr(line, "profile");
                if (url.empty()) { reply(replyErr(id, "'url' required")); continue; }
                reply(cmdVisit(id, url, profile));
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
            } else if (cmd == "profile") {
                std::string name = jsonGetStr(line, "name");
                if (name.empty()) { reply(replyErr(id, "'name' required")); continue; }
                reply(cmdProfile(id, name));
            } else if (cmd == "cookies") {
                reply(cmdCookies(id, jsonGetStr(line, "url")));
            } else if (cmd == "setCookie") {
                reply(cmdSetCookie(id, line));
            } else if (cmd == "clearCookies") {
                client.clearCookies();
                reply(replyOk(id));
            } else if (cmd == "download") {
                reply(cmdDownload(id, line));
#ifdef LUCYN_CONTROLLER_HAVE_PAINT
            } else if (cmd == "screenshot") {
                reply(cmdScreenshot(id, line));
#endif
            } else if (cmd == "window") {
                reply(cmdWindow(id, line));
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
