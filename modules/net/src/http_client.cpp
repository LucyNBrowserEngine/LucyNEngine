#include <lucyn/net/http_client.h>
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <limits>
#include <mutex>

namespace lucyn::net {

static void globalInit() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

static size_t onBody(char* p, size_t sz, size_t n, void* ud) {
    if (n && sz > std::numeric_limits<size_t>::max() / n) return 0;
    const size_t bytes = sz * n;
    try {
        static_cast<std::string*>(ud)->append(p, bytes);
        return bytes;
    } catch (...) {
        // Exceptions must never cross libcurl's C callback boundary.
        return 0;
    }
}

// Streaming body (RequestOptions::bodySink). Decides ONCE, at the first chunk, whether the response is 2xx;
// non-2xx bodies are counted and dropped. Redirect bodies never arrive here (libcurl ignores them while following).
struct StreamCtx {
    const std::function<bool(const char*, size_t)>* sink = nullptr;
    CURL*              h = nullptr;
    unsigned long long seen = 0;      // every body byte libcurl delivered
    bool               decided = false;
    bool               pass = false;  // 2xx: forward to the sink
};

static size_t onStreamBody(char* p, size_t sz, size_t n, void* ud) {
    if (n && sz > std::numeric_limits<size_t>::max() / n) return 0;
    const size_t bytes = sz * n;
    auto* c = static_cast<StreamCtx*>(ud);
    try {
        if (!c->decided) {
            long code = 0;
            curl_easy_getinfo(c->h, CURLINFO_RESPONSE_CODE, &code);
            c->pass = code >= 200 && code < 300;
            c->decided = true;
        }
        c->seen += bytes;
        if (c->pass && c->sink && *c->sink && !(*c->sink)(p, bytes)) return 0;   // 0 = abort the transfer
        return bytes;
    } catch (...) {
        return 0;   // exceptions must never cross libcurl's C callback boundary
    }
}

// Where onHeader puts what it parses: the header fields of the FINAL hop, plus that hop's status line.
struct HeaderSink {
    HeaderList* list;
    std::string* statusLine;
};

static size_t onHeader(char* p, size_t sz, size_t n, void* ud) {
    if (n && sz > std::numeric_limits<size_t>::max() / n) return 0;
    const size_t bytes = sz * n;
    auto* sink = static_cast<HeaderSink*>(ud);
    try {
        std::string line(p, bytes);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.rfind("HTTP/", 0) == 0) {   // new hop: keep the final one only
            sink->list->clear();
            *sink->statusLine = line;
            return bytes;
        }
        auto colon = line.find(':');
        if (colon == std::string::npos) return bytes;
        size_t v = colon + 1;
        while (v < line.size() && line[v] == ' ') ++v;
        sink->list->emplace_back(line.substr(0, colon), line.substr(v));
        return bytes;
    } catch (...) {
        // Exceptions must never cross libcurl's C callback boundary.
        return 0;
    }
}

// Moonbug: libcurl's debug channel hands us the request headers exactly as they were sent (decrypted, wire
// order). Only CURLINFO_HEADER_OUT is kept; everything else (TLS blobs, body bytes) is ignored. Verbose
// output goes to this callback, never to stderr.
static int onDebug(CURL*, curl_infotype type, char* data, size_t size, void* ud) {
    if (type == CURLINFO_HEADER_OUT) {
        try { static_cast<std::string*>(ud)->append(data, size); } catch (...) {}
    }
    return 0;
}

// Splits the accumulated sent-header text into hops (blank line between them) and returns the LAST hop's
// headers; the request line ("GET /x HTTP/1.1") is skipped.
static HeaderList parseSentHeaders(const std::string& raw) {
    HeaderList out;
    std::vector<std::string> hops;
    std::string cur;
    size_t i = 0;
    while (i < raw.size()) {
        size_t e = raw.find('\n', i);
        std::string line = raw.substr(i, e == std::string::npos ? std::string::npos : e - i);
        i = (e == std::string::npos) ? raw.size() : e + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) { if (!cur.empty()) { hops.push_back(cur); cur.clear(); } }
        else { cur += line; cur += '\n'; }
    }
    if (!cur.empty()) hops.push_back(cur);
    if (hops.empty()) return out;
    const std::string& last = hops.back();
    size_t pos = 0;
    bool first = true;
    while (pos < last.size()) {
        size_t e = last.find('\n', pos);
        std::string line = last.substr(pos, e - pos);
        pos = e + 1;
        if (first) {   // request line = "METHOD target HTTP/x": a space comes before any colon
            first = false;
            size_t sp = line.find(' '), co = line.find(':');
            if (sp != std::string::npos && (co == std::string::npos || sp < co) && line.find(" HTTP/") != std::string::npos) continue;
        }
        auto colon = line.find(':', line.empty() || line[0] != ':' ? 0 : 1);   // keep HTTP/2 pseudo-headers (":method") intact
        if (colon == std::string::npos) continue;
        size_t v = colon + 1;
        while (v < line.size() && line[v] == ' ') ++v;
        out.emplace_back(line.substr(0, colon), line.substr(v));
    }
    return out;
}

// libcurl's default read callback is fread(stdin). Our request bodies are always handed over via
// COPYPOSTFIELDS, so nothing should ever be read - but if a future change gets the body options wrong
// (as post() once did), the failure must be "empty body", never "block on / steal the process's stdin".
static size_t onRead(char*, size_t, size_t, void*) { return 0; }

// Builds a ready-to-use handle. `target` empty = plain libcurl, no impersonation.
static CURL* makeHandle(const std::string& target, std::string* err) {
    globalInit();
    CURL* h = curl_easy_init();
    if (!h) { if (err) *err = "curl_easy_init failed"; return nullptr; }

    // Generic options go FIRST. curl_easy_impersonate() then applies the browser's
    // TLS / HTTP/2 / header settings on top; anything set after it would override
    // them and could break the fingerprint.
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_READFUNCTION, onRead);
    curl_easy_setopt(h, CURLOPT_READDATA, nullptr);
    // In-memory cookie engine (empty file name = no file). libcurl stores Set-Cookie from every hop
    // (redirects included) and sends matching cookies back. The jar dies with the handle, so switching
    // profile starts a new "person" with no cookies. Persisting it is a later milestone.
    curl_easy_setopt(h, CURLOPT_COOKIEFILE, "");

#ifdef LUCYN_HAS_IMPERSONATE
    if (!target.empty()) {
        // HTTP/3 targets: to use h3, set CURLOPT_HTTP_VERSION BEFORE this call.
        CURLcode rc = curl_easy_impersonate(h, target.c_str(), 1 /* browser default headers */);
        if (rc != CURLE_OK) {
            if (err) *err = "libcurl-impersonate rejected '" + target + "': " + curl_easy_strerror(rc);
            curl_easy_cleanup(h);
            return nullptr;
        }
        curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");   // library sends the Accept-Encoding header but leaves decoding off
    }
#else
    if (!target.empty()) {
        if (err) *err = "built against stock libcurl, which cannot impersonate (set LUCYN_CURL_IMPERSONATE_ROOT)";
        curl_easy_cleanup(h);
        return nullptr;
    }
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");   // stock build: let curl decode gzip for us
#endif
    return h;
}

bool HttpClient::impersonationBuiltIn() {
#ifdef LUCYN_HAS_IMPERSONATE
    return true;
#else
    return false;
#endif
}

bool HttpClient::probe(const profile::Profile& p, std::string* err) {
    CURL* h = makeHandle(p.target, err);
    if (!h) return false;
    curl_easy_cleanup(h);
    return true;
}

HttpClient::HttpClient(profile::Switcher& sw) : sw_(sw) {
    rebuild(sw_.current());

    validator_ = sw_.addValidator([](const profile::Profile& next, std::string* err) {
        return HttpClient::probe(next, err);
    });
    listener_ = sw_.subscribe([this](const profile::Profile&, const profile::Profile& now) {
        rebuild(now);
    });
}

HttpClient::~HttpClient() {
    sw_.unsubscribe(listener_);
    sw_.removeValidator(validator_);
    std::lock_guard<std::mutex> lk(mu_);
    if (easy_) curl_easy_cleanup(static_cast<CURL*>(easy_));
    easy_ = nullptr;
}

// Swapping the whole handle (instead of calling impersonate again on the old one)
// throws away the connection pool and TLS session cache, so the old fingerprint
// can never be resumed or reused under the new name.
void HttpClient::rebuild(const profile::Profile& p) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string err;
    CURL* fresh = makeHandle(p.target, &err);
    bool wearing = fresh != nullptr;
    if (!fresh) {                       // fall back to a plain handle so get() still works
        lastError_ = err;
        fresh = makeHandle("", nullptr);
    } else {
        lastError_.clear();
    }
    if (easy_) curl_easy_cleanup(static_cast<CURL*>(easy_));
    easy_ = fresh;
    impersonating_ = wearing && !p.target.empty();   // an empty target asks for nothing, so it is "wearing" nothing
}

static std::string httpVersionName(long v) {
    switch (v) {
        case CURL_HTTP_VERSION_1_0: return "1.0";
        case CURL_HTTP_VERSION_1_1: return "1.1";
        case CURL_HTTP_VERSION_2_0: return "2";
#ifdef CURL_HTTP_VERSION_3
        case CURL_HTTP_VERSION_3:   return "3";
#endif
        default: return "";
    }
}


// ================================================================ URLs and header planning

namespace {

std::string lowerStr(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
std::string upperStr(std::string s) { for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return s; }
bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
std::string trimStr(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

struct Url {
    bool        valid = false;
    std::string scheme, host;
    std::string path = "/";
    std::string pathQuery = "/";   // path + query, no fragment
    int         port = 0;
};

Url parseUrl(const std::string& u) {
    Url r;
    size_t se = u.find("://");
    if (se == std::string::npos) return r;
    r.scheme = lowerStr(u.substr(0, se));
    if (r.scheme != "http" && r.scheme != "https") return r;
    size_t a = se + 3;
    size_t pe = u.find_first_of("/?#", a);
    std::string auth = u.substr(a, pe == std::string::npos ? std::string::npos : pe - a);
    std::string rest = pe == std::string::npos ? "" : u.substr(pe);
    size_t at = auth.rfind('@');
    if (at != std::string::npos) auth = auth.substr(at + 1);
    std::string host = auth;
    int port = 0;
    if (!host.empty() && host[0] == '[') {
        size_t rb = host.find(']');
        if (rb == std::string::npos) return r;
        std::string after = host.substr(rb + 1);
        host = host.substr(0, rb + 1);
        if (!after.empty() && after[0] == ':') port = std::atoi(after.c_str() + 1);
    } else {
        size_t c = host.rfind(':');
        if (c != std::string::npos) { port = std::atoi(host.c_str() + c + 1); host = host.substr(0, c); }
    }
    if (host.empty()) return r;
    r.host = lowerStr(host);
    r.port = port ? port : (r.scheme == "https" ? 443 : 80);
    size_t frag = rest.find('#');
    std::string pq = frag == std::string::npos ? rest : rest.substr(0, frag);
    size_t qm = pq.find('?');
    std::string path = qm == std::string::npos ? pq : pq.substr(0, qm);
    r.path = path.empty() ? "/" : path;
    r.pathQuery = (pq.empty() || pq[0] == '?') ? "/" + pq : pq;
    r.valid = true;
    return r;
}

std::string originOf(const Url& u) {
    std::string o = u.scheme + "://" + u.host;
    const int def = u.scheme == "https" ? 443 : 80;
    if (u.port != def) o += ":" + std::to_string(u.port);
    return o;
}

bool isIpHost(const std::string& h) {
    if (h.find(':') != std::string::npos) return true;   // IPv6 literal
    bool digitsAndDots = !h.empty();
    for (char c : h) if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.')) { digitsAndDots = false; break; }
    return digitsAndDots;
}

bool isLocalHost(const std::string& h) { return h == "localhost" || h == "127.0.0.1" || h == "[::1]"; }

// Approximation of "registrable domain" (eTLD+1) without shipping the public-suffix list: last two
// labels, or three when the TLD is a 2-letter ccTLD under a common second-level (co.uk, com.au, ...).
std::string registrableDomain(const std::string& host) {
    if (isIpHost(host)) return host;
    std::vector<std::string> labels;
    size_t i = 0;
    while (i <= host.size()) {
        size_t d = host.find('.', i);
        if (d == std::string::npos) d = host.size();
        labels.push_back(host.substr(i, d - i));
        i = d + 1;
    }
    const size_t n = labels.size();
    if (n <= 2) return host;
    const std::string& last = labels[n - 1];
    const std::string& second = labels[n - 2];
    size_t take = 2;
    if (last.size() == 2 && (second == "co" || second == "com" || second == "org" || second == "net" ||
                             second == "gov" || second == "edu" || second == "ac"))
        take = 3;
    std::string out;
    for (size_t k = n - take; k < n; ++k) { if (!out.empty()) out += '.'; out += labels[k]; }
    return out;
}

}  // namespace

HeaderPlan planHeaders(const std::string& url, const RequestOptions& o, const HeaderList& persona) {
    HeaderPlan plan;
    auto drop = [&](const std::string& name) {
        plan.remove.erase(std::remove_if(plan.remove.begin(), plan.remove.end(), [&](const std::string& r) { return ieq(r, name); }), plan.remove.end());
    };
    auto put = [&](const std::string& name, const std::string& value) {
        drop(name);                                         // an explicit value beats an earlier removal
        for (auto& kv : plan.set) if (ieq(kv.first, name)) { kv.second = value; return; }
        plan.set.emplace_back(name, value);
    };
    auto remove = [&](const std::string& name) {
        plan.set.erase(std::remove_if(plan.set.begin(), plan.set.end(), [&](const std::pair<std::string, std::string>& kv) { return ieq(kv.first, name); }), plan.set.end());
        for (auto& r : plan.remove) if (ieq(r, name)) return;
        plan.remove.push_back(name);
    };

    for (auto& kv : persona) put(kv.first, kv.second);

    if (!o.pageUrl.empty()) {
        Url page = parseUrl(o.pageUrl), tgt = parseUrl(url);
        if (page.valid && tgt.valid) {
            const std::string method = upperStr(o.method.empty() ? "GET" : o.method);
            const bool sameOrigin = page.scheme == tgt.scheme && page.host == tgt.host && page.port == tgt.port;
            const bool sameSite = page.scheme == tgt.scheme && registrableDomain(page.host) == registrableDomain(tgt.host);

            // A subresource request is not a navigation.
            remove("Upgrade-Insecure-Requests");
            remove("Sec-Fetch-User");
            put("Accept", "*/*");
            put("Sec-Fetch-Site", sameOrigin ? "same-origin" : (sameSite ? "same-site" : "cross-site"));
            put("Sec-Fetch-Mode", o.fetchMode.empty() ? "cors" : o.fetchMode);
            put("Sec-Fetch-Dest", o.fetchDest.empty() ? "empty" : o.fetchDest);

            // Default referrer policy: strict-origin-when-cross-origin.
            if (sameOrigin) {
                put("Referer", originOf(page) + page.pathQuery);
            } else if (!(page.scheme == "https" && tgt.scheme == "http")) {
                put("Referer", originOf(page) + "/");
            }
            const bool unsafe = method != "GET" && method != "HEAD";
            if (unsafe || (!sameOrigin && o.fetchMode != "no-cors")) put("Origin", originOf(page));
        }
    }

    for (auto& kv : o.headers) put(kv.first, kv.second);
    for (auto& r : o.removeHeaders) remove(r);
    return plan;
}

// ================================================================ cookie jar

namespace {

bool parseJarLine(std::string line, Cookie& c) {
    const std::string pfx = "#HttpOnly_";
    if (line.rfind(pfx, 0) == 0) { c.httpOnly = true; line = line.substr(pfx.size()); }
    else if (line.empty() || line[0] == '#') return false;
    std::vector<std::string> f;
    size_t i = 0;
    for (int k = 0; k < 6; ++k) {
        size_t t = line.find('\t', i);
        if (t == std::string::npos) return false;
        f.push_back(line.substr(i, t - i));
        i = t + 1;
    }
    f.push_back(line.substr(i));
    std::string dom = f[0];
    if (!dom.empty() && dom[0] == '.') dom.erase(0, 1);
    c.domain = lowerStr(dom);
    c.hostOnly = f[1] == "FALSE";
    c.path = f[2].empty() ? "/" : f[2];
    c.secure = f[3] == "TRUE";
    c.expires = std::atoll(f[4].c_str());
    c.name = f[5];
    c.value = f[6];
    return true;
}

std::string jarLine(const Cookie& c) {
    std::string s;
    if (c.httpOnly) s += "#HttpOnly_";
    s += c.hostOnly ? c.domain : "." + c.domain;
    s += '\t'; s += c.hostOnly ? "FALSE" : "TRUE";
    s += '\t'; s += c.path.empty() ? "/" : c.path;
    s += '\t'; s += c.secure ? "TRUE" : "FALSE";
    s += '\t'; s += std::to_string(c.expires);
    s += '\t'; s += c.name;
    s += '\t'; s += c.value;
    return s;
}

std::vector<Cookie> readJar(CURL* h) {
    std::vector<Cookie> out;
    curl_slist* list = nullptr;
    if (curl_easy_getinfo(h, CURLINFO_COOKIELIST, &list) != CURLE_OK) return out;
    for (curl_slist* n = list; n; n = n->next) {
        Cookie c;
        if (n->data && parseJarLine(n->data, c)) out.push_back(std::move(c));
    }
    curl_slist_free_all(list);
    return out;
}

void writeJar(CURL* h, const std::vector<Cookie>& cookies) {
    curl_easy_setopt(h, CURLOPT_COOKIELIST, "ALL");
    for (auto& c : cookies) curl_easy_setopt(h, CURLOPT_COOKIELIST, jarLine(c).c_str());
}

bool cookieDomainMatches(const Cookie& c, const std::string& host) {
    if (host == c.domain) return true;
    if (c.hostOnly || isIpHost(host)) return false;
    return host.size() > c.domain.size() &&
           host.compare(host.size() - c.domain.size(), std::string::npos, c.domain) == 0 &&
           host[host.size() - c.domain.size() - 1] == '.';
}

bool cookiePathMatches(const std::string& cp, const std::string& reqPath) {
    if (reqPath == cp) return true;
    if (reqPath.compare(0, cp.size(), cp) != 0) return false;
    return cp.back() == '/' || reqPath[cp.size()] == '/';
}

bool badCookieChars(const std::string& s) {
    for (unsigned char ch : s) if (ch == '\t' || ch == '\r' || ch == '\n' || ch == 0) return true;
    return false;
}

}  // namespace

std::vector<Cookie> HttpClient::cookies() {
    std::lock_guard<std::mutex> lk(mu_);
    return easy_ ? readJar(static_cast<CURL*>(easy_)) : std::vector<Cookie>{};
}

std::vector<Cookie> HttpClient::cookiesFor(const std::string& url, bool includeHttpOnly) {
    std::vector<Cookie> hit;
    Url u = parseUrl(url);
    if (!u.valid) return hit;
    const long long now = static_cast<long long>(std::time(nullptr));
    const bool secureCtx = u.scheme == "https" || isLocalHost(u.host);
    for (auto& c : cookies()) {
        if (c.expires != 0 && c.expires <= now) continue;
        if (c.secure && !secureCtx) continue;
        if (c.httpOnly && !includeHttpOnly) continue;
        if (!cookieDomainMatches(c, u.host) || !cookiePathMatches(c.path, u.path)) continue;
        hit.push_back(std::move(c));
    }
    std::stable_sort(hit.begin(), hit.end(), [](const Cookie& a, const Cookie& b) { return a.path.size() > b.path.size(); });
    return hit;
}

std::string HttpClient::cookieStringFor(const std::string& url, bool includeHttpOnly) {
    std::string out;
    for (auto& c : cookiesFor(url, includeHttpOnly)) {
        if (!out.empty()) out += "; ";
        out += c.name.empty() ? c.value : c.name + "=" + c.value;
    }
    return out;
}

bool HttpClient::setCookie(const Cookie& c) {
    if (c.domain.empty() || badCookieChars(c.name) || badCookieChars(c.value) || badCookieChars(c.path) || badCookieChars(c.domain)) return false;
    std::lock_guard<std::mutex> lk(mu_);
    CURL* h = static_cast<CURL*>(easy_);
    if (!h) return false;
    auto jar = readJar(h);
    jar.erase(std::remove_if(jar.begin(), jar.end(), [&](const Cookie& e) {
        return e.name == c.name && e.domain == lowerStr(c.domain) && e.path == (c.path.empty() ? "/" : c.path);
    }), jar.end());
    const long long now = static_cast<long long>(std::time(nullptr));
    if (!(c.expires != 0 && c.expires <= now)) {
        Cookie n = c;
        n.domain = lowerStr(n.domain);
        if (!n.domain.empty() && n.domain[0] == '.') n.domain.erase(0, 1);
        jar.push_back(std::move(n));
    }
    writeJar(h, jar);
    return true;
}

void HttpClient::clearCookies() {
    std::lock_guard<std::mutex> lk(mu_);
    if (easy_) curl_easy_setopt(static_cast<CURL*>(easy_), CURLOPT_COOKIELIST, "ALL");
}

std::string HttpClient::exportCookieFile() {
    auto jar = cookies();   // acquires mu_ internally
    std::string out;
    out += "# Netscape HTTP Cookie File\n";
    for (auto& c : jar) out += jarLine(c) + "\n";
    return out;
}

void HttpClient::importCookieFile(const std::string& text) {
    std::lock_guard<std::mutex> lk(mu_);
    CURL* h = static_cast<CURL*>(easy_);
    if (!h) return;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        std::string line = text.substr(i, e == std::string::npos ? std::string::npos : e - i);
        i = (e == std::string::npos) ? text.size() : e + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        // Feed the raw Netscape line directly to libcurl's cookie engine.
        curl_easy_setopt(h, CURLOPT_COOKIELIST, line.c_str());
    }
}

bool HttpClient::setCookieFromScript(const std::string& url, const std::string& cookieString) {
    Url u = parseUrl(url);
    if (!u.valid || badCookieChars(cookieString)) return false;

    // name=value is everything before the first ';'
    std::vector<std::string> parts;
    {
        size_t i = 0;
        while (i <= cookieString.size()) {
            size_t sc = cookieString.find(';', i);
            if (sc == std::string::npos) sc = cookieString.size();
            parts.push_back(cookieString.substr(i, sc - i));
            i = sc + 1;
        }
    }
    std::string pair = trimStr(parts[0]), name, value;
    size_t eq = pair.find('=');
    if (eq == std::string::npos) value = pair;
    else { name = trimStr(pair.substr(0, eq)); value = trimStr(pair.substr(eq + 1)); }
    if (name.empty() && value.empty()) return false;
    if (name.size() + value.size() > 4096) return false;

    bool secureAttr = false, hasDomain = false, hasPath = false, hasMaxAge = false, hasExpires = false;
    std::string domainAttr, pathAttr;
    long long maxAge = 0, expiresAt = 0;
    for (size_t k = 1; k < parts.size(); ++k) {
        std::string a = trimStr(parts[k]);
        if (a.empty()) continue;
        size_t e2 = a.find('=');
        std::string key = lowerStr(trimStr(e2 == std::string::npos ? a : a.substr(0, e2)));
        std::string val = e2 == std::string::npos ? "" : trimStr(a.substr(e2 + 1));
        if (key == "httponly") return false;               // script may not create HttpOnly cookies
        else if (key == "secure") secureAttr = true;
        else if (key == "domain") { domainAttr = lowerStr(val); if (!domainAttr.empty() && domainAttr[0] == '.') domainAttr.erase(0, 1); hasDomain = !domainAttr.empty(); }
        else if (key == "path") { if (!val.empty() && val[0] == '/') { pathAttr = val; hasPath = true; } }
        else if (key == "max-age") {
            char* end = nullptr;
            long long v = std::strtoll(val.c_str(), &end, 10);
            if (end != val.c_str() && *end == '\0') { maxAge = v; hasMaxAge = true; }
        } else if (key == "expires") {
            time_t t = curl_getdate(val.c_str(), nullptr);
            if (t != static_cast<time_t>(-1)) { expiresAt = static_cast<long long>(t); hasExpires = true; }
        }
    }

    const bool secureCtx = u.scheme == "https" || isLocalHost(u.host);
    if (secureAttr && !secureCtx) return false;
    if (name.rfind("__Secure-", 0) == 0 && !(secureAttr && secureCtx)) return false;
    if (name.rfind("__Host-", 0) == 0 && !(secureAttr && secureCtx && !hasDomain && hasPath && pathAttr == "/")) return false;

    Cookie c;
    c.name = name; c.value = value; c.secure = secureAttr;
    if (hasDomain) {
        const bool exact = domainAttr == u.host;
        const bool suffix = u.host.size() > domainAttr.size() && u.host.compare(u.host.size() - domainAttr.size(), std::string::npos, domainAttr) == 0 && u.host[u.host.size() - domainAttr.size() - 1] == '.';
        if (!exact && !suffix) return false;
        if (isIpHost(u.host) && !exact) return false;
        if (domainAttr.find('.') == std::string::npos && !exact) return false;   // never a bare TLD like "com"
        c.domain = domainAttr;
        c.hostOnly = isIpHost(u.host) || u.host.find('.') == std::string::npos;
    } else {
        c.domain = u.host;
        c.hostOnly = true;
    }
    if (hasPath) c.path = pathAttr;
    else {
        size_t sl = u.path.rfind('/');
        c.path = (u.path.empty() || u.path[0] != '/' || sl == 0 || sl == std::string::npos) ? "/" : u.path.substr(0, sl);
    }

    const long long now = static_cast<long long>(std::time(nullptr));
    bool del = false;
    if (hasMaxAge) { if (maxAge <= 0) del = true; else c.expires = now + maxAge; }
    else if (hasExpires) { if (expiresAt <= now) del = true; else c.expires = expiresAt; }

    std::lock_guard<std::mutex> lk(mu_);
    CURL* h = static_cast<CURL*>(easy_);
    if (!h) return false;
    auto jar = readJar(h);
    for (auto it = jar.begin(); it != jar.end(); ++it) {
        if (it->name == c.name && it->domain == c.domain && it->path == c.path) {
            if (it->httpOnly) return false;                // script may not overwrite an HttpOnly cookie
            jar.erase(it);
            break;
        }
    }
    if (!del) jar.push_back(c);
    writeJar(h, jar);
    return true;
}

void HttpClient::setHeaderProvider(HeaderProvider p) {
    std::lock_guard<std::mutex> lk(mu_);
    headerProvider_ = std::move(p);
}

void HttpClient::setRequestFilter(RequestFilter filter) {
    std::lock_guard<std::mutex> lk(mu_);
    requestFilter_ = std::move(filter);
}

// ================================================================ requests

Response HttpClient::get(const std::string& url) {
    return request(url, RequestOptions{});
}

Response HttpClient::post(const std::string& url, const std::string& body, const PostOptions& opts) {
    RequestOptions o;
    o.method = "POST";
    o.body = body;
    o.hasBody = true;
    o.headers.emplace_back("Content-Type", opts.contentType);
    for (auto& kv : opts.extraHeaders) o.headers.push_back(kv);
    return request(url, o);
}

Response HttpClient::request(const std::string& url, const RequestOptions& o) {
    Moonbug* mb = moonbug_.load();
    const std::string method = upperStr(o.method.empty() ? "GET" : o.method);

    // Snapshot what we need from the shared state, then release the lock: the identity provider and
    // Moonbug's listeners run WITHOUT it, so they may call back into this client or the switcher.
    HeaderProvider provider;
    RequestFilter requestFilter;
    std::string target;
    bool wearing;
    { std::lock_guard<std::mutex> lk(mu_); provider = headerProvider_; requestFilter = requestFilter_; target = sw_.current().target; wearing = impersonating_; }
    HeaderList persona = provider ? provider(wearing) : HeaderList{};
    HeaderPlan plan = planHeaders(url, o, persona);

    Capture cap;
    if (mb) {
        cap = mb->begin(method, url, target, wearing);
        if (o.hasBody) cap.requestBody = o.body;
    }

    if (requestFilter) {
        const std::string rule = requestFilter(url, o.pageUrl, o.requestType);
        if (!rule.empty()) {
            Response blocked;
            blocked.blocked = true;
            blocked.error = "blocked by adblock: " + rule;
            if (mb) {
                cap.error = blocked.error;
                mb->finish(std::move(cap));
            }
            return blocked;
        }
    }

    curl_slist* hdrs = nullptr;
    for (auto& kv : plan.set)
        hdrs = curl_slist_append(hdrs, (kv.second.empty() ? kv.first + ";" : kv.first + ": " + kv.second).c_str());
    for (auto& name : plan.remove) hdrs = curl_slist_append(hdrs, (name + ":").c_str());

    Response r;
    {
        std::lock_guard<std::mutex> lk(mu_);
        CURL* h = static_cast<CURL*>(easy_);
        if (!h) {
            r.error = "no curl handle";
        } else {
            callbackBody_.clear();
            callbackHeaders_.clear();
            callbackReqHeaders_.clear();
            std::string statusLine;
            HeaderSink sink{&callbackHeaders_, &statusLine};
            StreamCtx stream;
            stream.sink = &o.bodySink;
            stream.h    = h;
            const bool streaming = static_cast<bool>(o.bodySink);

            auto setopt = [&](CURLoption option, auto value) {
                CURLcode code = curl_easy_setopt(h, option, value);
                if (code != CURLE_OK) {
                    r.error = std::string("curl_easy_setopt failed: ") + curl_easy_strerror(code);
                    return false;
                }
                return true;
            };

            // Start from a clean method state: the handle is reused, so nothing from the previous
            // request may leak into this one.
            curl_easy_setopt(h, CURLOPT_NOBODY, 0L);
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, nullptr);
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, nullptr);

            // POST/PUT/PATCH always carry a body (possibly empty, which sends Content-Length: 0).
            const bool withBody = method != "GET" && method != "HEAD" &&
                                  (o.hasBody || method == "POST" || method == "PUT" || method == "PATCH");

            bool armed = setopt(CURLOPT_URL, url.c_str());
            if (armed) {
                if (withBody) {
                    // ORDER MATTERS: the size must be set BEFORE COPYPOSTFIELDS. Setting a larger size
                    // after the copy makes libcurl drop the copied body, leaving "POST with no data",
                    // which then blocks reading the body from stdin.
                    armed = setopt(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(o.body.size())) &&
                            setopt(CURLOPT_COPYPOSTFIELDS, o.body.c_str());
                    if (armed && method != "POST") armed = setopt(CURLOPT_CUSTOMREQUEST, method.c_str());
                } else {
                    armed = setopt(CURLOPT_HTTPGET, 1L);
                    if (armed && method == "HEAD") armed = setopt(CURLOPT_NOBODY, 1L);
                    else if (armed && method != "GET") armed = setopt(CURLOPT_CUSTOMREQUEST, method.c_str());
                }
            }
            armed = armed &&
                setopt(CURLOPT_HTTPHEADER, hdrs) &&
                setopt(CURLOPT_WRITEFUNCTION, streaming ? onStreamBody : onBody) &&
                setopt(CURLOPT_WRITEDATA, streaming ? static_cast<void*>(&stream) : static_cast<void*>(&callbackBody_)) &&
                setopt(CURLOPT_HEADERFUNCTION, onHeader) &&
                setopt(CURLOPT_HEADERDATA, &sink) &&
                // Verbose is switched on ONLY for Moonbug, and its output only ever goes to onDebug.
                // (The handle is reused, so it is explicitly switched back off when nobody is listening.)
                setopt(CURLOPT_DEBUGFUNCTION, onDebug) &&
                setopt(CURLOPT_DEBUGDATA, &callbackReqHeaders_) &&
                setopt(CURLOPT_VERBOSE, mb ? 1L : 0L);

            if (armed && o.timeoutSec >= 0) {
                curl_easy_setopt(h, CURLOPT_TIMEOUT, o.timeoutSec);
                if (o.timeoutSec == 0) {   // no overall limit, but a dead connection must still end
                    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
                    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
                }
            }

            if (armed) {
                CURLcode rc = curl_easy_perform(h);
                r.body = std::move(callbackBody_);
                r.bodyBytes = streaming ? stream.seen : static_cast<unsigned long long>(r.body.size());
                r.headers = std::move(callbackHeaders_);

                char* eff = nullptr;
                long redirects = 0;
                curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &r.status);
                curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &eff);
                curl_easy_getinfo(h, CURLINFO_TOTAL_TIME, &r.totalTime);
                curl_easy_getinfo(h, CURLINFO_REDIRECT_COUNT, &redirects);
                long httpVer = 0;
                curl_easy_getinfo(h, CURLINFO_HTTP_VERSION, &httpVer);
                if (eff) r.effectiveUrl = eff;
                r.redirects = static_cast<int>(redirects);
                r.httpVersion = httpVersionName(httpVer);
                {   // "HTTP/1.1 404 Not Found" -> "Not Found"; HTTP/2+ status lines carry no phrase
                    size_t a = statusLine.find(' ');
                    size_t b = a == std::string::npos ? a : statusLine.find(' ', a + 1);
                    if (b != std::string::npos) r.statusText = trimStr(statusLine.substr(b + 1));
                }
                if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);

                if (mb) {
                    cap.finalUrl = r.effectiveUrl;
                    cap.status = r.status;
                    cap.responseHeaders = r.headers;
                    cap.responseBody = r.body;                 // Moonbug trims to maxBodyBytes
                    cap.responseBytes = static_cast<size_t>(r.bodyBytes);
                    cap.requestHeaders = parseSentHeaders(callbackReqHeaders_);
                    cap.impersonating = impersonating_;
                    cap.profile = sw_.current().target;
                    double t = 0; long l = 0; char* ip = nullptr;
                    if (curl_easy_getinfo(h, CURLINFO_NAMELOOKUP_TIME, &t) == CURLE_OK)    cap.timing.dnsMs = t * 1000;
                    if (curl_easy_getinfo(h, CURLINFO_CONNECT_TIME, &t) == CURLE_OK)       cap.timing.connectMs = t * 1000;
                    if (curl_easy_getinfo(h, CURLINFO_APPCONNECT_TIME, &t) == CURLE_OK)    cap.timing.tlsMs = t * 1000;
                    if (curl_easy_getinfo(h, CURLINFO_STARTTRANSFER_TIME, &t) == CURLE_OK) cap.timing.firstByteMs = t * 1000;
                    cap.timing.totalMs = r.totalTime * 1000;
                    if (curl_easy_getinfo(h, CURLINFO_HTTP_VERSION, &l) == CURLE_OK)       cap.httpVersion = httpVersionName(l);
                    if (curl_easy_getinfo(h, CURLINFO_PRIMARY_IP, &ip) == CURLE_OK && ip)  cap.remoteIp = ip;
                    if (curl_easy_getinfo(h, CURLINFO_PRIMARY_PORT, &l) == CURLE_OK)       cap.remotePort = static_cast<int>(l);
                    cap.redirects = r.redirects;
                }
            }
            // Leave the reused handle clean. The header list must be detached BEFORE it is freed below.
            curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);   // the default set in makeHandle(); a download may have changed it
            curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 0L);
            curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 0L);
            curl_easy_setopt(h, CURLOPT_HTTPHEADER, nullptr);
            curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
            curl_easy_setopt(h, CURLOPT_NOBODY, 0L);
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, nullptr);
        }
    }   // mu_ released here

    if (hdrs) curl_slist_free_all(hdrs);

    if (mb) {
        cap.error = r.error;
        if (cap.finalUrl.empty()) cap.finalUrl = url;
        mb->finish(std::move(cap));
    }
    return r;
}

}  // namespace lucyn::net
