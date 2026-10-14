#include <lucyn/security/nullbyte.h>
#include <lucyn/core/paths.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <bcrypt.h>
#endif

namespace lucyn::security {

// ---------------------------------------------------------------- SHA-256 (FIPS 180-4)

namespace {

constexpr std::uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256 {
    std::uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::uint8_t  buf[64];
    std::size_t   bufLen = 0;
    std::uint64_t total = 0;

    void block(const std::uint8_t* p) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(p[i*4]) << 24) | (std::uint32_t(p[i*4+1]) << 16) |
                   (std::uint32_t(p[i*4+2]) << 8) | std::uint32_t(p[i*4+3]);
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            std::uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        std::uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            std::uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    void update(const std::uint8_t* p, std::size_t n) {
        total += n;
        if (bufLen) {
            std::size_t take = std::min<std::size_t>(64 - bufLen, n);
            std::memcpy(buf + bufLen, p, take);
            bufLen += take; p += take; n -= take;
            if (bufLen == 64) { block(buf); bufLen = 0; }
        }
        while (n >= 64) { block(p); p += 64; n -= 64; }
        if (n) { std::memcpy(buf, p, n); bufLen = n; }
    }

    Digest finish() {
        std::uint64_t bits = total * 8;
        std::uint8_t pad = 0x80;
        update(&pad, 1);
        std::uint8_t zero = 0;
        while (bufLen != 56) update(&zero, 1);
        std::uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = std::uint8_t(bits >> (56 - 8 * i));
        update(len, 8);
        Digest d;
        for (int i = 0; i < 8; ++i) {
            d[i*4]   = std::uint8_t(h[i] >> 24);
            d[i*4+1] = std::uint8_t(h[i] >> 16);
            d[i*4+2] = std::uint8_t(h[i] >> 8);
            d[i*4+3] = std::uint8_t(h[i]);
        }
        return d;
    }
};

inline const std::uint8_t* u8(std::string_view s) { return reinterpret_cast<const std::uint8_t*>(s.data()); }

}  // namespace

Digest sha256(std::string_view data) {
    Sha256 s;
    s.update(u8(data), data.size());
    return s.finish();
}

// RFC 2104 / RFC 4231.
Digest hmacSha256(std::string_view key, std::string_view data) {
    std::uint8_t k[64] = {0};
    if (key.size() > 64) {
        Digest kd = sha256(key);
        std::memcpy(k, kd.data(), kd.size());
    } else if (!key.empty()) {
        std::memcpy(k, key.data(), key.size());
    }
    std::uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    Sha256 inner;
    inner.update(ipad, 64);
    inner.update(u8(data), data.size());
    Digest ih = inner.finish();
    Sha256 outer;
    outer.update(opad, 64);
    outer.update(ih.data(), ih.size());
    return outer.finish();
}

std::string toHex(std::string_view bytes) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) { s += hex[c >> 4]; s += hex[c & 15]; }
    return s;
}
std::string toHex(const Digest& d) {
    return toHex(std::string_view(reinterpret_cast<const char*>(d.data()), d.size()));
}

bool fromHex(std::string_view hex, std::string* out) {
    if (hex.size() % 2) return false;
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string r;
    r.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        int a = val(hex[i]), b = val(hex[i+1]);
        if (a < 0 || b < 0) return false;
        r += char((a << 4) | b);
    }
    if (out) *out = std::move(r);
    return true;
}

// ---------------------------------------------------------------- tiny JSON bits for the envelope

namespace {

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
}

// Escapes for embedding in a JSON string. Bytes >= 0x80 pass through untouched: payloads we sign are
// valid UTF-8 JSON text, and an exact byte round trip is the whole point.
void escapeInto(std::string& out, std::string_view s) {
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", unsigned(c)); out += b; }
                else out += char(c);
        }
    }
}

struct Cursor {
    std::string_view s;
    std::size_t i = 0;
    void ws() { while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) ++i; }
    bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }
};

bool hex4(std::string_view s, std::size_t at, std::uint32_t* v) {
    if (at + 4 > s.size()) return false;
    std::uint32_t r = 0;
    for (int k = 0; k < 4; ++k) {
        char c = s[at + k]; int d;
        if (c>='0'&&c<='9') d=c-'0'; else if (c>='a'&&c<='f') d=c-'a'+10; else if (c>='A'&&c<='F') d=c-'A'+10; else return false;
        r = (r << 4) | std::uint32_t(d);
    }
    *v = r; return true;
}

// Cursor sits on the opening quote. Decodes a full JSON string (\uXXXX and surrogate pairs included).
bool readString(Cursor& c, std::string* out) {
    if (c.i >= c.s.size() || c.s[c.i] != '"') return false;
    ++c.i;
    std::string r;
    while (c.i < c.s.size()) {
        char ch = c.s[c.i++];
        if (ch == '"') { if (out) *out = std::move(r); return true; }
        if (ch != '\\') { r += ch; continue; }
        if (c.i >= c.s.size()) return false;
        char e = c.s[c.i++];
        switch (e) {
            case '"': r += '"'; break;   case '\\': r += '\\'; break;  case '/': r += '/'; break;
            case 'b': r += '\b'; break;  case 'f': r += '\f'; break;   case 'n': r += '\n'; break;
            case 'r': r += '\r'; break;  case 't': r += '\t'; break;
            case 'u': {
                std::uint32_t cp;
                if (!hex4(c.s, c.i, &cp)) return false;
                c.i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {          // high surrogate: needs \uDC00..DFFF next
                    std::uint32_t lo;
                    if (c.i + 6 <= c.s.size() && c.s[c.i]=='\\' && c.s[c.i+1]=='u' && hex4(c.s, c.i + 2, &lo) &&
                        lo >= 0xDC00 && lo <= 0xDFFF) {
                        c.i += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else return false;
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) return false;   // lone low surrogate
                appendUtf8(r, cp);
                break;
            }
            default: return false;
        }
    }
    return false;   // unterminated
}

// Skips any JSON value (strings are string-aware, so braces inside strings do not confuse it).
bool skipValue(Cursor& c, int depth = 0) {
    if (depth > 64) return false;
    c.ws();
    if (c.i >= c.s.size()) return false;
    char ch = c.s[c.i];
    if (ch == '"') return readString(c, nullptr);
    if (ch == '{' || ch == '[') {
        char close = ch == '{' ? '}' : ']';
        ++c.i;
        if (c.eat(close)) return true;
        for (;;) {
            if (ch == '{') { c.ws(); if (!readString(c, nullptr) || !c.eat(':')) return false; }
            if (!skipValue(c, depth + 1)) return false;
            if (c.eat(',')) continue;
            return c.eat(close);
        }
    }
    // number / true / false / null
    std::size_t st = c.i;
    while (c.i < c.s.size() && std::string_view(",}] \t\r\n").find(c.s[c.i]) == std::string_view::npos) ++c.i;
    return c.i > st;
}

}  // namespace

// ---------------------------------------------------------------- Nullbyte

Nullbyte::Nullbyte(std::string key) : key_(std::move(key)) {
    if (key_.empty()) throw std::invalid_argument("NULLBYTE: empty key");
}

std::string Nullbyte::sign(std::string_view data) const { return toHex(hmacSha256(key_, data)); }

bool Nullbyte::verify(std::string_view data, std::string_view sigHex) const {
    std::string want = sign(data);
    if (sigHex.size() != want.size()) return false;
    unsigned char diff = 0;                        // constant time: never exit early
    for (std::size_t i = 0; i < want.size(); ++i) {
        char a = sigHex[i];
        if (a >= 'A' && a <= 'F') a = char(a - 'A' + 'a');
        diff |= (unsigned char)(a ^ want[i]);
    }
    return diff == 0;
}

std::string Nullbyte::envelope(std::string_view payload) const {
    std::string out = "{\"v\":1,\"alg\":\"";
    out += kAlgorithm;
    out += "\",\"sig\":\"";
    out += sign(payload);
    out += "\",\"payload\":\"";
    escapeInto(out, payload);
    out += "\"}";
    return out;
}

Nullbyte::Opened Nullbyte::open(std::string_view json) const {
    Opened r;
    Cursor c{json};
    if (!c.eat('{')) { r.error = "not a JSON object"; return r; }
    std::string sig, payload, alg;
    bool haveSig = false, havePayload = false;
    if (!c.eat('}')) {
        for (;;) {
            std::string key;
            c.ws();
            if (!readString(c, &key) || !c.eat(':')) { r.error = "malformed envelope"; return r; }
            c.ws();
            const bool isStr = c.i < c.s.size() && c.s[c.i] == '"';
            if (isStr && (key == "sig" || key == "payload" || key == "alg")) {
                std::string v;
                if (!readString(c, &v)) { r.error = "malformed string in envelope"; return r; }
                if (key == "sig") { sig = std::move(v); haveSig = true; }
                else if (key == "payload") { payload = std::move(v); havePayload = true; }
                else alg = std::move(v);
            } else if (!skipValue(c)) { r.error = "malformed envelope"; return r; }
            if (c.eat(',')) continue;
            if (c.eat('}')) break;
            r.error = "malformed envelope"; return r;
        }
    }
    if (!haveSig || !havePayload) { r.error = "envelope needs \"sig\" and \"payload\" strings"; return r; }
    if (!alg.empty() && alg != kAlgorithm) { r.error = "unsupported algorithm: " + alg; return r; }
    r.parsed = true;
    r.payload = std::move(payload);
    r.ok = verify(r.payload, sig);
    if (!r.ok) r.error = "signature does not match (payload altered, or signed with a different key)";
    return r;
}

std::string Nullbyte::defaultKeyPath() {
    return core::dataDir("security") + "/nullbyte.key";
}

std::string Nullbyte::generateKey() {
    std::string k(32, '\0');
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&k[0]), ULONG(k.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
        return k;
#else
    if (std::FILE* f = std::fopen("/dev/urandom", "rb")) {
        std::size_t got = std::fread(&k[0], 1, k.size(), f);
        std::fclose(f);
        if (got == k.size()) return k;
    }
#endif
    std::random_device rd;   // last resort; on mainstream libstdc++/libc++/MSVC this is also OS entropy
    for (auto& ch : k) ch = char(rd() & 0xFF);
    return k;
}

std::string Nullbyte::loadOrCreateKey(const std::string& path, std::string* err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(path, ec)) {
        std::ifstream in(path, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        while (!text.empty() && (text.back()=='\n' || text.back()=='\r' || text.back()==' ')) text.pop_back();
        std::string raw;
        if (!in.good() && !in.eof()) { if (err) *err = "cannot read NULLBYTE key file: " + path; return {}; }
        if (!fromHex(text, &raw) || raw.size() < 16) {
            if (err) *err = "NULLBYTE key file is corrupt (expected >= 32 hex chars): " + path +
                            " - not replacing it, because that would orphan every earlier signature";
            return {};
        }
        return raw;
    }
    std::string key = generateKey();
    fs::create_directories(fs::path(path).parent_path(), ec);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << toHex(key) << "\n";
        if (!out) { if (err) *err = "cannot write NULLBYTE key file: " + path; return {}; }
    }
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);   // best effort (no-op on Windows)
    return key;
}

}  // namespace lucyn::security
