#include <lucyn/shell/url.h>
#include <lucyn/dom/dom.h>
#include <cctype>
#include <sstream>

namespace lucyn::shell {

namespace {
struct Parts {
    std::string scheme;       // "https" (no colon), "" if none
    bool        hasAuthority = false;
    std::string authority;    // host[:port] (userinfo kept as given)
    std::string path;         // may be empty
    std::string query;        // with leading '?', or ""
    std::string fragment;     // with leading '#', or ""
};

bool schemeAt(const std::string& s, size_t* colon) {
    if (s.empty() || !std::isalpha((unsigned char)s[0])) return false;
    for (size_t i = 1; i < s.size(); ++i) {
        char c = s[i];
        if (c == ':') { *colon = i; return true; }
        if (!(std::isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.')) return false;
    }
    return false;
}

Parts split(const std::string& u) {
    Parts p;
    std::string rest = u;
    size_t colon = 0;
    if (schemeAt(rest, &colon)) {
        p.scheme = rest.substr(0, colon);
        for (auto& c : p.scheme) c = (char)std::tolower((unsigned char)c);
        rest = rest.substr(colon + 1);
    }
    size_t h = rest.find('#');
    if (h != std::string::npos) { p.fragment = rest.substr(h); rest.resize(h); }
    size_t q = rest.find('?');
    if (q != std::string::npos) { p.query = rest.substr(q); rest.resize(q); }
    if (rest.rfind("//", 0) == 0) {
        p.hasAuthority = true;
        size_t slash = rest.find('/', 2);
        p.authority = rest.substr(2, slash == std::string::npos ? std::string::npos : slash - 2);
        p.path = slash == std::string::npos ? "" : rest.substr(slash);
    } else {
        p.path = rest;
    }
    return p;
}

std::string removeDots(const std::string& path) {
    if (path.empty()) return path;
    std::vector<std::string> out;
    std::istringstream in(path);
    std::string seg;
    bool trailingSlash = path.back() == '/';
    bool absolute = path[0] == '/';
    while (std::getline(in, seg, '/')) {
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") { if (!out.empty()) out.pop_back(); continue; }
        out.push_back(seg);
    }
    std::string r = absolute ? "/" : "";
    for (size_t i = 0; i < out.size(); ++i) { if (i) r += '/'; r += out[i]; }
    std::string last = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
    if ((trailingSlash || last == "." || last == "..") && !out.empty()) r += '/';
    if (absolute && out.empty()) r = "/";
    return r;
}

std::string build(const Parts& p) {
    std::string s;
    if (!p.scheme.empty()) s += p.scheme + ":";
    if (p.hasAuthority) s += "//" + p.authority;
    return s + p.path + p.query + p.fragment;
}
}  // namespace

std::string resolveUrl(const std::string& base, const std::string& ref) {
    std::string r = ref;
    while (!r.empty() && std::isspace((unsigned char)r.front())) r.erase(r.begin());
    while (!r.empty() && std::isspace((unsigned char)r.back())) r.pop_back();
    Parts b = split(base);
    if (r.empty()) { b.fragment.clear(); return build(b); }

    size_t colon = 0;
    if (schemeAt(r, &colon)) return r;                       // already absolute (http:, data:, javascript:, ...)

    Parts t = split(r);
    Parts out;
    out.scheme = b.scheme;
    if (t.hasAuthority) {                                    // //host/path
        out.hasAuthority = true; out.authority = t.authority;
        out.path = removeDots(t.path); out.query = t.query; out.fragment = t.fragment;
        return build(out);
    }
    out.hasAuthority = b.hasAuthority; out.authority = b.authority;
    if (t.path.empty()) {                                    // "?q" or "#f" only
        out.path = b.path;
        out.query = t.query.empty() ? (r[0] == '#' ? b.query : "") : t.query;
        out.fragment = t.fragment;
    } else if (t.path[0] == '/') {
        out.path = removeDots(t.path); out.query = t.query; out.fragment = t.fragment;
    } else {
        std::string dir = b.path;
        size_t slash = dir.find_last_of('/');
        dir = slash == std::string::npos ? (b.hasAuthority ? "/" : "") : dir.substr(0, slash + 1);
        if (b.hasAuthority && dir.empty()) dir = "/";
        out.path = removeDots(dir + t.path); out.query = t.query; out.fragment = t.fragment;
    }
    return build(out);
}

std::string originOf(const std::string& url) {
    Parts p = split(url);
    if (!p.hasAuthority || p.authority.empty()) return "";
    if (p.scheme != "http" && p.scheme != "https" && p.scheme != "ws" && p.scheme != "wss") return "";
    return p.scheme + "://" + p.authority;
}

std::string hostKey(const std::string& url) {
    Parts p = split(url);
    if (!p.hasAuthority || p.authority.empty() || p.scheme == "file") return "local";
    std::string a = p.authority;
    size_t at = a.rfind('@');                                // userinfo never belongs in a folder name
    if (at != std::string::npos) a = a.substr(at + 1);
    std::string key;
    for (char c : a) {
        if (std::isalnum((unsigned char)c) || c == '.' || c == '-') key += (char)std::tolower((unsigned char)c);
        else key += '_';
    }
    while (!key.empty() && key.front() == '.') key.erase(key.begin());   // no ".." or hidden folders
    return key.empty() ? "local" : key;
}

std::string faviconUrl(const dom::Document& doc, const std::string& pageUrl) {
    std::string base = pageUrl;
    if (auto* b = doc.querySelector("base[href]"))
        if (auto* h = b->attr("href")) base = resolveUrl(pageUrl, *h);

    auto relTokens = [](const dom::Node& n) {
        std::vector<std::string> t;
        if (auto* rel = n.attr("rel")) {
            std::istringstream in(*rel);
            std::string w;
            while (in >> w) { for (auto& c : w) c = (char)std::tolower((unsigned char)c); t.push_back(w); }
        }
        return t;
    };
    std::string icon, touch;
    for (auto* l : doc.getElementsByTagName("link")) {
        auto* href = l->attr("href");
        if (!href || href->empty()) continue;
        for (auto& t : relTokens(*l)) {
            if (t == "icon" && icon.empty()) icon = resolveUrl(base, *href);
            if (t == "apple-touch-icon" && touch.empty()) touch = resolveUrl(base, *href);
        }
    }
    if (!icon.empty()) return icon;
    if (!touch.empty()) return touch;
    std::string origin = originOf(pageUrl);
    return origin.empty() ? "" : origin + "/favicon.ico";
}

}  // namespace lucyn::shell
