#include <lucyn/net/moonbug.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace lucyn::net {

// ---------------------------------------------------------------- listeners

Moonbug::ListenerId Moonbug::add(Kind k, Listener fn) {
    std::lock_guard<std::mutex> lk(mu_);
    ListenerId id = nextListener_++;
    listeners_.push_back({id, k, std::move(fn)});
    return id;
}
Moonbug::ListenerId Moonbug::onRequest(Listener fn)  { return add(Kind::Request,  std::move(fn)); }
Moonbug::ListenerId Moonbug::onResponse(Listener fn) { return add(Kind::Response, std::move(fn)); }
Moonbug::ListenerId Moonbug::onCapture(Listener fn)  { return add(Kind::Capture,  std::move(fn)); }

void Moonbug::off(ListenerId id) {
    std::lock_guard<std::mutex> lk(mu_);
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [&](const Entry& e) { return e.id == id; }),
                     listeners_.end());
}

// Copies the matching listeners out under the lock, then calls them with no lock held.
void Moonbug::fire(Kind k, const Capture& c) {
    std::vector<Listener> todo;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& e : listeners_) if (e.kind == k) todo.push_back(e.fn);
    }
    for (auto& fn : todo) {
        try { fn(c); } catch (...) { /* a broken tap must never break a fetch */ }
    }
}

// ---------------------------------------------------------------- begin / finish

Capture Moonbug::begin(const std::string& method, const std::string& url, const std::string& profile, bool impersonating) {
    Capture c;
    {
        std::lock_guard<std::mutex> lk(mu_);
        c.id = nextId_++;
        ++inFlight_;
    }
    c.tsMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    c.method = method;
    c.url = url;
    c.profile = profile;
    c.impersonating = impersonating;
    fire(Kind::Request, c);
    return c;
}

void Moonbug::finish(Capture cap) {
    cap.responseBytes = std::max(cap.responseBytes, cap.responseBody.size());
    if (cap.responseBody.size() > opts_.maxBodyBytes) {
        cap.responseBody.resize(opts_.maxBodyBytes);
        cap.bodyTruncated = true;
    }
    cap.done = true;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ++total_;
        if (inFlight_ > 0) --inFlight_;
        if (opts_.historySize > 0) {
            history_.push_back(cap);
            while (history_.size() > opts_.historySize) history_.pop_front();
        }
    }
    fire(Kind::Response, cap);
    fire(Kind::Capture, cap);
}

std::vector<Capture> Moonbug::recent(std::size_t n) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::size_t take = std::min(n, history_.size());
    return std::vector<Capture>(history_.end() - static_cast<std::ptrdiff_t>(take), history_.end());
}

std::uint64_t Moonbug::total() const { std::lock_guard<std::mutex> lk(mu_); return total_; }
std::uint64_t Moonbug::inFlight() const { std::lock_guard<std::mutex> lk(mu_); return inFlight_; }
void Moonbug::clear() { std::lock_guard<std::mutex> lk(mu_); history_.clear(); }

// ---------------------------------------------------------------- JSON helpers

// Length of the valid UTF-8 sequence starting at s[i], or 0 if it is not valid there
// (rejects overlongs, surrogates and anything above U+10FFFF).
static std::size_t utf8Len(const std::string& s, std::size_t i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    auto cont = [&](std::size_t k) { return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80; };
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return cont(1) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        if (!cont(1) || !cont(2)) return 0;
        unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
        if (c == 0xE0 && c1 < 0xA0) return 0;      // overlong
        if (c == 0xED && c1 > 0x9F) return 0;      // surrogate
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (!cont(1) || !cont(2) || !cont(3)) return 0;
        unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
        if (c == 0xF0 && c1 < 0x90) return 0;      // overlong
        if (c == 0xF4 && c1 > 0x8F) return 0;      // > U+10FFFF
        return 4;
    }
    return 0;
}

bool Moonbug::validUtf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        std::size_t n = utf8Len(s, i);
        if (!n) return false;
        i += n;
    }
    return true;
}

void Moonbug::appendJsonString(std::string& out, const std::string& s) {
    out += '"';
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20 || c == 0x7f) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", unsigned(c)); out += b; }
                    else out += static_cast<char>(c);
            }
            ++i;
            continue;
        }
        std::size_t n = utf8Len(s, i);
        if (!n) { out += "\\ufffd"; ++i; continue; }   // scrub a byte that is not UTF-8
        out.append(s, i, n);
        i += n;
    }
    out += '"';
}

std::string Moonbug::base64(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i+1] << 8 | (unsigned char)in[i+2];
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i+1] << 8;
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
    }
    return out;
}

static void appendHeaders(std::string& out, const HeaderList& h) {
    out += '[';
    for (std::size_t i = 0; i < h.size(); ++i) {
        if (i) out += ',';
        out += '[';
        Moonbug::appendJsonString(out, h[i].first);
        out += ',';
        Moonbug::appendJsonString(out, h[i].second);
        out += ']';
    }
    out += ']';
}

static std::string num(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}

std::string Moonbug::toJson(const Capture& c, std::size_t previewBytes) {
    std::string o;
    o.reserve(512 + std::min(c.responseBody.size(), previewBytes) * 2);
    o += "{\"id\":" + std::to_string(c.id);
    o += ",\"ts\":" + std::to_string(c.tsMs);
    o += ",\"method\":";     appendJsonString(o, c.method);
    o += ",\"url\":";        appendJsonString(o, c.url);
    o += ",\"finalUrl\":";   appendJsonString(o, c.finalUrl);
    o += ",\"profile\":";    appendJsonString(o, c.profile);
    o += ",\"impersonating\":"; o += c.impersonating ? "true" : "false";
    o += ",\"done\":";       o += c.done ? "true" : "false";
    o += ",\"status\":" + std::to_string(c.status);
    o += ",\"error\":";      appendJsonString(o, c.error);
    o += ",\"httpVersion\":"; appendJsonString(o, c.httpVersion);
    o += ",\"remoteIp\":";   appendJsonString(o, c.remoteIp);
    o += ",\"remotePort\":" + std::to_string(c.remotePort);
    o += ",\"redirects\":" + std::to_string(c.redirects);
    o += ",\"timing\":{\"dnsMs\":" + num(c.timing.dnsMs) + ",\"connectMs\":" + num(c.timing.connectMs) +
         ",\"tlsMs\":" + num(c.timing.tlsMs) + ",\"firstByteMs\":" + num(c.timing.firstByteMs) +
         ",\"totalMs\":" + num(c.timing.totalMs) + "}";
    o += ",\"requestHeaders\":";  appendHeaders(o, c.requestHeaders);
    o += ",\"requestBodyBytes\":" + std::to_string(c.requestBody.size());
    o += ",\"responseHeaders\":"; appendHeaders(o, c.responseHeaders);
    o += ",\"bodyBytes\":" + std::to_string(c.responseBytes);

    // body preview: cut on a UTF-8 boundary so a multi-byte character is not split into "binary"
    std::size_t cut = std::min(c.responseBody.size(), previewBytes);
    while (cut > 0 && cut < c.responseBody.size() && (static_cast<unsigned char>(c.responseBody[cut]) & 0xC0) == 0x80) --cut;
    std::string preview = c.responseBody.substr(0, cut);
    const bool shortened = c.bodyTruncated || cut < c.responseBody.size();
    o += ",\"bodyTruncated\":"; o += shortened ? "true" : "false";
    if (validUtf8(preview)) {
        o += ",\"bodyEncoding\":\"utf8\",\"body\":";
        appendJsonString(o, preview);
    } else {
        // binary: ship the raw preview bytes, not a lossy scrubbed copy
        std::string raw = c.responseBody.substr(0, std::min(c.responseBody.size(), previewBytes));
        o += ",\"bodyEncoding\":\"base64\",\"body\":\"" + base64(raw) + "\"";
    }
    o += '}';
    return o;
}

}  // namespace lucyn::net
