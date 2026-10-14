// storage_bridge.cpp - localStorage / sessionStorage (file-backed, per origin) + loadedfiles saver.
//
// localStorage  -> nothing-data/storage/<origin>/local.json   (persists across runs)
// sessionStorage-> nothing-data/storage/<origin>/session.json (written each run, not read back)
// loadedfiles   -> nothing-data/loadedfiles/<origin>/<filename>  (stub per external script URL)

#include "impl.h"
#include <lucyn/core/paths.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#define ARGS JSContext* c, [[maybe_unused]] JSValueConst t, [[maybe_unused]] int argc, [[maybe_unused]] JSValueConst* argv

namespace fs = std::filesystem;

namespace lucyn::js::detail {
namespace {

// ---- minimal flat JSON (string->string) ------------------------------------

using KVMap = std::vector<std::pair<std::string, std::string>>;

static std::string jsonEscape(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += char(c);
        }
    }
    return o + "\"";
}

static std::string kvToJson(const KVMap& m) {
    std::string o = "{";
    for (size_t i = 0; i < m.size(); ++i) {
        if (i) o += ",";
        o += "\n  " + jsonEscape(m[i].first) + ": " + jsonEscape(m[i].second);
    }
    return o + "\n}";
}

static bool parseKvJson(const std::string& src, KVMap& out) {
    size_t i = 0;
    auto skip = [&] { while (i < src.size() && std::isspace((unsigned char)src[i])) ++i; };
    auto expect = [&](char ch) -> bool { skip(); if (i >= src.size() || src[i] != ch) return false; ++i; return true; };
    auto readStr = [&](std::string& s) -> bool {
        if (!expect('"')) return false;
        s.clear();
        while (i < src.size()) {
            char c2 = src[i++];
            if (c2 == '"') return true;
            if (c2 == '\\' && i < src.size()) {
                char e = src[i++];
                switch (e) {
                    case '"': s += '"'; break; case '\\': s += '\\'; break;
                    case 'n': s += '\n'; break; case 'r': s += '\r'; break;
                    case 't': s += '\t'; break;
                    case 'u': {
                        if (i + 4 > src.size()) return false;
                        unsigned v = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = src[i++]; v <<= 4;
                            if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                            else return false;
                        }
                        if (v < 0x80) s += char(v);
                        else if (v < 0x800) { s += char(0xC0|(v>>6)); s += char(0x80|(v&0x3F)); }
                        else { s += char(0xE0|(v>>12)); s += char(0x80|((v>>6)&0x3F)); s += char(0x80|(v&0x3F)); }
                        break;
                    }
                    default: s += e;
                }
            } else s += c2;
        }
        return false;
    };
    skip(); if (!expect('{')) return false;
    skip();
    if (i < src.size() && src[i] == '}') return true;
    while (true) {
        std::string k, v;
        if (!readStr(k)) return false;
        if (!expect(':')) return false;
        if (!readStr(v)) return false;
        out.push_back({k, v});
        skip();
        if (i >= src.size()) return false;
        if (src[i] == '}') { ++i; return true; }
        if (src[i] != ',') return false;
        ++i;
    }
}

// ---- StorageArea -----------------------------------------------------------

struct StorageArea {
    KVMap    items;
    fs::path path;
    bool     dirty = false;

    void load() {
        if (path.empty() || !fs::exists(path)) return;
        std::ifstream f(path);
        if (!f) return;
        std::string src((std::istreambuf_iterator<char>(f)), {});
        KVMap tmp;
        if (parseKvJson(src, tmp)) items = std::move(tmp);
    }
    void flush() {
        if (!dirty || path.empty()) return;
        std::ofstream f(path);
        if (f) f << kvToJson(items);
        dirty = false;
    }
    int  length() const { return (int)items.size(); }
    const std::string* get(const std::string& k) const {
        for (auto& p : items) if (p.first == k) return &p.second;
        return nullptr;
    }
    void set(const std::string& k, const std::string& v) {
        for (auto& p : items) if (p.first == k) { p.second = v; dirty = true; return; }
        items.push_back({k, v}); dirty = true;
    }
    bool remove(const std::string& k) {
        auto it = std::find_if(items.begin(), items.end(), [&](auto& p){ return p.first == k; });
        if (it == items.end()) return false;
        items.erase(it); dirty = true; return true;
    }
    void clear() { if (!items.empty()) { items.clear(); dirty = true; } }
    const std::string* keyAt(int idx) const {
        if (idx < 0 || idx >= (int)items.size()) return nullptr;
        return &items[idx].first;
    }
};

struct StorageSlots { StorageArea local, session; };
static std::unordered_map<JSContext*, StorageSlots> g_storage;

static std::string sanOrigin(std::string s) {
    auto p = s.find("://"); if (p != std::string::npos) s = s.substr(p + 3);
    auto sl = s.find('/'); if (sl != std::string::npos) s = s.substr(0, sl);
    for (char& c : s) if (!std::isalnum((unsigned char)c) && c != '-' && c != '.') c = '_';
    return s.empty() ? std::string("_unknown_") : s;
}

static void initStorage(State& s, const std::string& origin) {
    std::string dir = core::dataDir("storage/" + sanOrigin(origin));
    auto& sl = g_storage[s.ctx];
    sl.local.path   = fs::path(dir) / "local.json";
    sl.session.path = fs::path(dir) / "session.json";
    sl.local.load();
}

// ---- native functions ------------------------------------------------------

JSValue n_storageLength(ARGS) {
    if (argc < 1) return JS_NewInt32(c, 0);
    bool loc = JS_ToBool(c, argv[0]) > 0;
    auto it = g_storage.find(c); if (it == g_storage.end()) return JS_NewInt32(c, 0);
    return JS_NewInt32(c, loc ? it->second.local.length() : it->second.session.length());
}
JSValue n_storageKey(ARGS) {
    if (argc < 2) return JS_NULL;
    bool loc = JS_ToBool(c, argv[0]) > 0;
    int32_t idx = 0; JS_ToInt32(c, &idx, argv[1]);
    auto it = g_storage.find(c); if (it == g_storage.end()) return JS_NULL;
    auto& area = loc ? it->second.local : it->second.session;
    const auto* k = area.keyAt(idx); return k ? jstr(c, *k) : JS_NULL;
}
JSValue n_storageGet(ARGS) {
    if (argc < 2) return JS_NULL;
    bool loc = JS_ToBool(c, argv[0]) > 0;
    std::string k; if (!toStr(c, argv[1], k)) return JS_EXCEPTION;
    auto it = g_storage.find(c); if (it == g_storage.end()) return JS_NULL;
    auto& area = loc ? it->second.local : it->second.session;
    const auto* v = area.get(k); return v ? jstr(c, *v) : JS_NULL;
}
JSValue n_storageSet(ARGS) {
    if (argc < 3) return JS_UNDEFINED;
    bool loc = JS_ToBool(c, argv[0]) > 0;
    std::string k, v;
    if (!toStr(c, argv[1], k) || !toStr(c, argv[2], v)) return JS_EXCEPTION;
    auto it = g_storage.find(c); if (it == g_storage.end()) return JS_UNDEFINED;
    auto& area = loc ? it->second.local : it->second.session;
    area.set(k, v);
    if (loc) area.flush();
    return JS_UNDEFINED;
}
JSValue n_storageRemove(ARGS) {
    if (argc < 2) return JS_UNDEFINED;
    bool loc = JS_ToBool(c, argv[0]) > 0;
    std::string k; if (!toStr(c, argv[1], k)) return JS_EXCEPTION;
    auto it = g_storage.find(c); if (it == g_storage.end()) return JS_UNDEFINED;
    auto& area = loc ? it->second.local : it->second.session;
    area.remove(k);
    if (loc) area.flush();
    return JS_UNDEFINED;
}
JSValue n_storageClear(ARGS) {
    if (argc < 1) return JS_UNDEFINED;
    bool loc = JS_ToBool(c, argv[0]) > 0;
    auto it = g_storage.find(c); if (it == g_storage.end()) return JS_UNDEFINED;
    auto& area = loc ? it->second.local : it->second.session;
    area.clear();
    if (loc) area.flush();
    return JS_UNDEFINED;
}
// n.saveLoadedFile(absoluteUrl) — called from JS prelude when it would load a <script src>
// The C++ host also calls saveLoadedFileStub() directly for the same purpose.
JSValue n_saveLoadedFile(ARGS) {
    if (argc < 1) return JS_UNDEFINED;
    std::string url; if (!toStr(c, argv[0], url)) return JS_EXCEPTION;
    std::string content;
    if (argc >= 2 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1]))
        toStr(c, argv[1], content);  // ignore failure — empty content is fine
    std::string origin;
    auto proto = url.find("://");
    if (proto != std::string::npos) {
        auto slash = url.find('/', proto + 3);
        origin = slash != std::string::npos ? url.substr(0, slash) : url;
    } else origin = "_local_";
    std::string dir = core::dataDir("loadedfiles/" + sanOrigin(origin));
    if (dir.empty()) return JS_UNDEFINED;
    // filename: last path segment, sanitised
    std::string fname = url;
    auto sl = fname.rfind('/'); if (sl != std::string::npos) fname = fname.substr(sl + 1);
    for (char ch : {'?','#'}) { auto p = fname.find(ch); if (p != std::string::npos) fname = fname.substr(0,p); }
    for (char& ch : fname) if (!std::isalnum((unsigned char)ch) && ch != '-' && ch != '.' && ch != '_') ch = '_';
    if (fname.empty()) fname = "index.js";
    fs::path fp = fs::path(dir) / fname;
    if (!content.empty() || !fs::exists(fp)) {
        std::ofstream f(fp, std::ios::binary);
        if (f) {
            if (content.empty()) f << "// loadedfiles stub — content not fetched yet\n// url: " << url << "\n";
            else                 f << content;
        }
    }
    return JS_UNDEFINED;
}

static void reg(JSContext* c, JSValueConst obj, const char* name, JSCFunction* f, int len) {
    JSValue v = JS_NewCFunction2(c, f, name, len, JS_CFUNC_generic, 0);
    JS_DefinePropertyValueStr(c, obj, name, v, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

}  // namespace

// ---- public API ------------------------------------------------------------

bool installStorage(State& s, JSValueConst n_obj) {
    std::string origin;
    const auto& u = s.pageUrl;
    auto proto = u.find("://");
    if (proto != std::string::npos) {
        auto slash = u.find('/', proto + 3);
        origin = slash != std::string::npos ? u.substr(0, slash) : u;
    } else origin = "_local_";
    initStorage(s, origin);

    JSContext* c = s.ctx;
    reg(c, n_obj, "storageLength",  n_storageLength,  1);
    reg(c, n_obj, "storageKey",     n_storageKey,     2);
    reg(c, n_obj, "storageGet",     n_storageGet,     2);
    reg(c, n_obj, "storageSet",     n_storageSet,     3);
    reg(c, n_obj, "storageRemove",  n_storageRemove,  2);
    reg(c, n_obj, "storageClear",   n_storageClear,   1);
    reg(c, n_obj, "saveLoadedFile", n_saveLoadedFile, 2);
    return true;
}

void teardownStorage(JSContext* ctx) {
    auto it = g_storage.find(ctx);
    if (it == g_storage.end()) return;
    it->second.local.flush();
    it->second.session.flush();
    g_storage.erase(it);
}

}  // namespace lucyn::js::detail
