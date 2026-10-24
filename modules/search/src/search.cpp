#include <lucyn/search/search.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <utility>

namespace lucyn::search {
namespace {

// Percent-encode a query (RFC 3986 unreserved characters left literal). Input must be UTF-8.
std::string encodeQuery(const std::string& query) {
    static const char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char c : query) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += static_cast<char>(c);
        } else {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 0x0f];
        }
    }
    return encoded;
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode "%XX" sequences; anything malformed is kept literally.
std::string pctDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hexVal(s[i + 1]), lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

std::string jsonQuote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
                    o += buf;
                } else {
                    o += c;
                }
        }
    }
    return o + "\"";
}

// ---- Tiny JSON reader ------------------------------------------------------
// Just enough to read the engine list this module writes (objects, arrays, strings,
// numbers/bools/null as raw tokens). We do not want a full JSON dependency here.

struct JsonValue {
    enum Type { Null, Literal, String, Array, Object } type = Null;
    std::string scalar;                                        // String (decoded) or Literal (raw token)
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    const JsonValue* find(const std::string& key) const {
        for (auto& kv : object) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string str() const { return (type == String || type == Literal) ? scalar : std::string(); }
};

struct JsonParser {
    const std::string& s;
    size_t i = 0;
    std::string err;

    explicit JsonParser(const std::string& in) : s(in) {}

    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i; }
    bool fail(const char* m) { err = std::string(m) + " at offset " + std::to_string(i); return false; }

    bool parseString(std::string& o) {
        if (i >= s.size() || s[i] != '"') return fail("expected string");
        ++i;
        o.clear();
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { o += c; continue; }
            if (i >= s.size()) return fail("bad escape");
            char e = s[i++];
            switch (e) {
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case '"': o += '"';  break;
                case '\\': o += '\\'; break;
                case '/': o += '/';  break;
                case 'u': {
                    if (i + 4 > s.size()) return fail("bad \\u escape");
                    unsigned v = static_cast<unsigned>(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
                    i += 4;
                    if (v < 0x80) o += static_cast<char>(v);
                    else if (v < 0x800) {
                        o += static_cast<char>(0xC0 | (v >> 6));
                        o += static_cast<char>(0x80 | (v & 0x3F));
                    } else {
                        o += static_cast<char>(0xE0 | (v >> 12));
                        o += static_cast<char>(0x80 | ((v >> 6) & 0x3F));
                        o += static_cast<char>(0x80 | (v & 0x3F));
                    }
                    break;
                }
                default: return fail("bad escape");
            }
        }
        if (i >= s.size()) return fail("unterminated string");
        ++i;  // closing quote
        return true;
    }

    bool parseObject(JsonValue& out) {
        out.type = JsonValue::Object;
        ++i;  // '{'
        ws();
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        for (;;) {
            ws();
            std::string key;
            if (!parseString(key)) return false;
            ws();
            if (i >= s.size() || s[i] != ':') return fail("expected ':'");
            ++i;
            JsonValue val;
            if (!parseValue(val)) return false;
            out.object.emplace_back(std::move(key), std::move(val));
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            return fail("expected ',' or '}'");
        }
    }

    bool parseArray(JsonValue& out) {
        out.type = JsonValue::Array;
        ++i;  // '['
        ws();
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        for (;;) {
            JsonValue val;
            if (!parseValue(val)) return false;
            out.array.push_back(std::move(val));
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool parseValue(JsonValue& out) {
        ws();
        if (i >= s.size()) return fail("unexpected end");
        char c = s[i];
        if (c == '{') return parseObject(out);
        if (c == '[') return parseArray(out);
        if (c == '"') { out.type = JsonValue::String; return parseString(out.scalar); }
        size_t st = i;
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
               !(s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t'))
            ++i;
        out.scalar = s.substr(st, i - st);
        out.type = out.scalar == "null" ? JsonValue::Null : JsonValue::Literal;
        return true;
    }
};

bool parseJsonText(const std::string& text, JsonValue& out, std::string* err) {
    JsonParser p(text);
    if (!p.parseValue(out)) { if (err) *err = p.err; return false; }
    p.ws();
    if (p.i != text.size()) { if (err) *err = "trailing characters after JSON value"; return false; }
    return true;
}

}  // namespace

Registry::Registry() {
    add({"brave", "Brave Search", "https://search.brave.com/search?q={query}", "b"});
    add({"searxng", "SearXNG", ""});
    add({"google", "Google", "https://www.google.com/search?q={query}", "g",
         "", "gl={region}", "hl={language}", "num={count}"});
    add({"duckduckgo", "DuckDuckGo", "https://duckduckgo.com/?q={query}", "d", "kp=1"});
    add({"duckduckgo-lite", "DuckDuckGo Lite (no JavaScript)", "https://lite.duckduckgo.com/lite/?q={query}", "dl"});
    add({"bing", "Bing", "https://www.bing.com/search?q={query}", "bi", "", "", "", "count={count}"});
    add({"ecosia", "Ecosia", "https://www.ecosia.org/search?q={query}", "e"});
    add({"startpage", "Startpage", "https://www.startpage.com/sp/search?q={query}", "sp"});
    add({"mojeek", "Mojeek", "https://www.mojeek.com/search?q={query}", "m"});
    add({"qwant", "Qwant", "https://www.qwant.com/?q={query}", "q"});
    add({"swisscows", "Swisscows", "https://swisscows.com/en/web?query={query}", "sc"});
}

bool Registry::add(Engine engine) {
    if (engine.id.empty() || engine.name.empty()) return false;
    if (!engine.queryTemplate.empty()) {
        const size_t first = engine.queryTemplate.find("{query}");
        if (first == std::string::npos || engine.queryTemplate.find("{query}", first + 7) != std::string::npos)
            return false;
    }
    auto it = std::find_if(engines_.begin(), engines_.end(), [&](const Engine& e) { return e.id == engine.id; });
    if (it == engines_.end()) engines_.push_back(std::move(engine));
    else *it = std::move(engine);
    return true;
}

std::optional<Engine> Registry::find(const std::string& id) const {
    auto it = std::find_if(engines_.begin(), engines_.end(), [&](const Engine& e) { return e.id == id; });
    if (it == engines_.end()) return std::nullopt;
    return *it;
}

std::optional<Engine> Registry::findByKeyword(const std::string& keyword) const {
    if (keyword.empty()) return std::nullopt;
    auto it = std::find_if(engines_.begin(), engines_.end(),
                           [&](const Engine& e) { return !e.keyword.empty() && e.keyword == keyword; });
    if (it == engines_.end()) return std::nullopt;
    return *it;
}

std::vector<Engine> Registry::engines() const { return engines_; }

std::optional<std::string> Registry::url(const std::string& id, const std::string& query,
                                         std::string* error) const {
    return url(id, query, Options{}, error);
}

std::optional<std::string> Registry::url(const std::string& id, const std::string& query,
                                         const Options& opts, std::string* error) const {
    auto engine = find(id);
    if (!engine) {
        if (error) *error = "unknown search engine: " + id;
        return std::nullopt;
    }
    if (engine->queryTemplate.empty()) {
        if (error) *error = "search engine '" + id + "' needs a configured search URL";
        return std::nullopt;
    }
    std::string result = engine->queryTemplate;
    const size_t slot = result.find("{query}");
    result.replace(slot, 7, encodeQuery(query));

    std::vector<std::string> params;
    auto addParam = [&](const std::string& tmpl, const char* placeholder, const std::string& value) {
        if (tmpl.empty()) return;
        std::string p = tmpl;
        const size_t ph = p.find(placeholder);
        if (ph != std::string::npos) p.replace(ph, std::string(placeholder).size(), encodeQuery(value));
        params.push_back(std::move(p));   // a template with no placeholder is a fixed flag
    };
    if (opts.safeSearch)     addParam(engine->safeSearchParam, "{safe}", std::to_string(*opts.safeSearch));
    if (opts.region)         addParam(engine->regionParam, "{region}", *opts.region);
    if (opts.language)       addParam(engine->languageParam, "{language}", *opts.language);
    if (opts.resultsPerPage) addParam(engine->countParam, "{count}", std::to_string(*opts.resultsPerPage));

    for (const auto& p : params) {
        result += (result.find('?') == std::string::npos) ? '?' : '&';
        result += p;
    }
    return result;
}

std::optional<std::string> Registry::url(const std::string& query, std::string* error) const {
    return url(defaultId_, query, error);
}

bool Registry::setDefault(const std::string& id) {
    if (!find(id)) return false;
    defaultId_ = id;
    return true;
}

const std::string& Registry::defaultId() const { return defaultId_; }

std::optional<ParsedSearch> Registry::parseUrl(const std::string& url) const {
    for (const auto& e : engines_) {
        const std::string& t = e.queryTemplate;
        if (t.empty()) continue;
        const size_t slot = t.find("{query}");
        if (slot == std::string::npos) continue;
        const std::string pre = t.substr(0, slot);
        const std::string suf = t.substr(slot + 7);
        if (url.size() < pre.size() + suf.size()) continue;
        if (url.compare(0, pre.size(), pre) != 0) continue;
        if (!suf.empty() && url.compare(url.size() - suf.size(), suf.size(), suf) != 0) continue;
        std::string mid = url.substr(pre.size(), url.size() - pre.size() - suf.size());
        // Options (and anything else) ride along after the query; the query itself never contains
        // a raw '&', '#' or '?' (they are percent-encoded), so cut at the first one.
        const size_t cut = mid.find_first_of("&#");
        if (cut != std::string::npos) mid.erase(cut);
        return ParsedSearch{e.id, pctDecode(mid)};
    }
    return std::nullopt;
}

std::string Registry::toJson() const {
    std::string o = "{\"default\":" + jsonQuote(defaultId_) + ",\"engines\":[";
    bool first = true;
    for (const auto& e : engines_) {
        if (!first) o += ',';
        first = false;
        o += "{\"id\":" + jsonQuote(e.id);
        o += ",\"name\":" + jsonQuote(e.name);
        o += ",\"queryTemplate\":" + jsonQuote(e.queryTemplate);
        if (!e.keyword.empty())        o += ",\"keyword\":" + jsonQuote(e.keyword);
        if (!e.safeSearchParam.empty())o += ",\"safeSearchParam\":" + jsonQuote(e.safeSearchParam);
        if (!e.regionParam.empty())    o += ",\"regionParam\":" + jsonQuote(e.regionParam);
        if (!e.languageParam.empty())  o += ",\"languageParam\":" + jsonQuote(e.languageParam);
        if (!e.countParam.empty())     o += ",\"countParam\":" + jsonQuote(e.countParam);
        o += "}";
    }
    o += "]}";
    return o;
}

bool Registry::loadFromJson(const std::string& json, std::string* error) {
    JsonValue root;
    std::string err;
    if (!parseJsonText(json, root, &err)) {
        if (error) *error = "invalid JSON: " + err;
        return false;
    }
    if (root.type != JsonValue::Object) {
        if (error) *error = "expected a JSON object at the top level";
        return false;
    }
    if (const JsonValue* engines = root.find("engines")) {
        if (engines->type != JsonValue::Array) {
            if (error) *error = "'engines' must be an array";
            return false;
        }
        for (const auto& item : engines->array) {
            if (item.type != JsonValue::Object) {
                if (error) *error = "each engine must be an object";
                return false;
            }
            auto get = [&](const char* k) -> std::string {
                const JsonValue* v = item.find(k);
                return v ? v->str() : std::string();
            };
            Engine e;
            e.id               = get("id");
            e.name             = get("name");
            e.queryTemplate    = get("queryTemplate");
            e.keyword          = get("keyword");
            e.safeSearchParam  = get("safeSearchParam");
            e.regionParam      = get("regionParam");
            e.languageParam    = get("languageParam");
            e.countParam       = get("countParam");
            if (!add(std::move(e))) {
                if (error) *error = "invalid engine entry (empty id/name, or template without exactly one {query})";
                return false;
            }
        }
    }
    if (const JsonValue* def = root.find("default")) {
        if (def->type == JsonValue::String && !def->scalar.empty()) {
            if (!setDefault(def->scalar)) {
                if (error) *error = "default engine unknown: " + def->scalar;
                return false;
            }
        }
    }
    return true;
}

bool Registry::loadFromFile(const std::string& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (error) *error = "cannot open: " + path; return false; }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return loadFromJson(text, error);
}

bool Registry::saveToFile(const std::string& path, std::string* error) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { if (error) *error = "cannot write: " + path; return false; }
    f << toJson();
    if (!f) { if (error) *error = "write failed: " + path; return false; }
    return true;
}

}  // namespace lucyn::search
