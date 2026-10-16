// fetch_bridge.cpp - the native half of fetch(), XMLHttpRequest and <script src> loading.
//
// The JS-facing classes (fetch, Headers, Request, Response, XMLHttpRequest, AbortController, FormData,
// TextEncoder/TextDecoder) live in the prelude (prelude/p4_net.js). They reach the network through these
// natives on the private `__n` object:
//
//   n.httpSend(spec, cb) -> id    asynchronous: cb(result) runs on the JS thread from the event loop
//   n.httpSendSync(spec) -> result  blocking (synchronous XHR)
//   n.httpCancel(id)              the result of a cancelled request is dropped
//   n.utf8Decode(bytes) / n.utf8Encode(str)
//   n.cookieGet(href) / n.cookieSet(href, str)    document.cookie, when the host wired the callbacks
//
// spec   = { method, url, body (string | ArrayBuffer | typed array | undefined), headers: [[name, value]...],
//            pageUrl, mode, dest }
// result = { status, statusText, url, redirected, headers: [[name, value]...], body: ArrayBuffer, error }
//          (error is "" on success; on failure status is 0)
//
// Threading rule: a worker thread NEVER touches JSContext/JSRuntime. It runs the host's HTTP callback on
// plain C++ data and pushes the result into a locked queue; pumpFetch() (JS thread, from drainJobs) turns
// results into JS values and calls the callbacks.
//
// <script src> loading: runScriptNode calls fetchAndRunScript(), a synchronous download (the JS engine is
// paused waiting for settle() anyway) followed by an eval.

#include "impl.h"
#include <lucyn/core/paths.h>
#include <lucyn/shell/url.h>
#include <filesystem>
#include <fstream>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_set>

#define ARGS JSContext* c, [[maybe_unused]] JSValueConst t, [[maybe_unused]] int argc, [[maybe_unused]] JSValueConst* argv

namespace fs = std::filesystem;

namespace lucyn::js::detail {
namespace {

// ---- the host's HTTP callbacks, copied so worker threads never touch State -----------------------------

struct HttpFns {
    std::function<HttpGetResult(const HttpRequest&)> fetch;
    std::function<HttpGetResult(const std::string&, const std::string&, const std::string&,
                                const std::vector<std::pair<std::string, std::string>>&)> request;
    std::function<HttpGetResult(const std::string&)> get;
    bool any() const { return fetch || request || get; }
};

HttpFns fnsOf(const HostOptions& o) { return HttpFns{o.httpFetch, o.httpRequest, o.httpGet}; }

// Precedence: httpFetch (full context) > httpRequest (method/body/headers) > httpGet (GET only).
HttpGetResult runHttp(const HttpFns& f, const HttpRequest& rq) {
    HttpGetResult r;
    try {
        if (f.fetch)   return f.fetch(rq);
        if (f.request) return f.request(rq.method, rq.url, rq.body, rq.headers);
        if (f.get && rq.method == "GET") return f.get(rq.url);
        r.error = f.get ? "HTTP method " + rq.method + " is not available in this build" : "no HTTP callback is wired";
    } catch (const std::exception& e) {
        r.error = e.what();
    } catch (...) {
        r.error = "unknown error";
    }
    return r;
}

std::string standardPhrase(long s) {
    switch (s) {
        case 100: return "Continue";            case 101: return "Switching Protocols";
        case 200: return "OK";                  case 201: return "Created";
        case 202: return "Accepted";            case 204: return "No Content";
        case 206: return "Partial Content";     case 301: return "Moved Permanently";
        case 302: return "Found";               case 303: return "See Other";
        case 304: return "Not Modified";        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";  case 400: return "Bad Request";
        case 401: return "Unauthorized";        case 403: return "Forbidden";
        case 404: return "Not Found";           case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";     case 409: return "Conflict";
        case 410: return "Gone";                case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type"; case 418: return "I'm a teapot";
        case 422: return "Unprocessable Entity"; case 429: return "Too Many Requests";
        case 500: return "Internal Server Error"; case 501: return "Not Implemented";
        case 502: return "Bad Gateway";         case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "";
    }
}

// Chrome reports the reason phrase the server sent, which is empty on HTTP/2 and HTTP/3. A host that
// tells us the protocol gets exactly that; an older callback that tells us nothing gets the standard phrase.
std::string reasonPhrase(const HttpGetResult& r) {
    if (!r.statusText.empty()) return r.statusText;
    if (r.httpVersion == "2" || r.httpVersion == "3") return "";
    return standardPhrase(r.status);
}

// ---- spec <-> JS --------------------------------------------------------------------------------------

bool getStrProp(JSContext* c, JSValueConst obj, const char* name, std::string& out) {
    JSValue v = JS_GetPropertyStr(c, obj, name);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(c, v); return true; }
    bool ok = toStr(c, v, out);
    JS_FreeValue(c, v);
    return ok;
}

// Raw bytes of an ArrayBuffer or typed array. False if `v` is neither.
bool bytesOf(JSContext* c, JSValueConst v, std::string& out) {
    size_t size = 0;
    if (uint8_t* p = JS_GetArrayBuffer(c, &size, v)) { out.assign(reinterpret_cast<char*>(p), size); return true; }
    JS_FreeValue(c, JS_GetException(c));   // JS_GetArrayBuffer throws for non-buffers; that is not an error here
    size_t off = 0, len = 0, per = 0;
    JSValue ab = JS_GetTypedArrayBuffer(c, v, &off, &len, &per);
    if (JS_IsException(ab)) { JS_FreeValue(c, JS_GetException(c)); return false; }
    uint8_t* p = JS_GetArrayBuffer(c, &size, ab);
    bool ok = p && off + len <= size;
    if (ok) out.assign(reinterpret_cast<char*>(p) + off, len);
    JS_FreeValue(c, ab);
    return ok;
}

bool parseSpec(JSContext* c, JSValueConst spec, HttpRequest& rq) {
    if (!JS_IsObject(spec)) { JS_ThrowTypeError(c, "http: spec must be an object"); return false; }
    if (!getStrProp(c, spec, "method", rq.method) || !getStrProp(c, spec, "url", rq.url) ||
        !getStrProp(c, spec, "pageUrl", rq.pageUrl) || !getStrProp(c, spec, "mode", rq.mode) ||
        !getStrProp(c, spec, "dest", rq.dest)) return false;
    if (rq.method.empty()) rq.method = "GET";
    rq.method = upperAscii(rq.method);

    JSValue body = JS_GetPropertyStr(c, spec, "body");
    if (!JS_IsUndefined(body) && !JS_IsNull(body)) {
        rq.hasBody = true;
        if (JS_IsString(body)) { if (!toStr(c, body, rq.body)) { JS_FreeValue(c, body); return false; } }
        else if (!bytesOf(c, body, rq.body)) { if (!toStr(c, body, rq.body)) { JS_FreeValue(c, body); return false; } }
    }
    JS_FreeValue(c, body);

    JSValue hs = JS_GetPropertyStr(c, spec, "headers");
    if (JS_IsObject(hs)) {
        JSValue lenV = JS_GetPropertyStr(c, hs, "length");
        uint32_t len = 0;
        JS_ToUint32(c, &len, lenV);
        JS_FreeValue(c, lenV);
        for (uint32_t i = 0; i < len; ++i) {
            JSValue pair = JS_GetPropertyUint32(c, hs, i);
            JSValue k = JS_GetPropertyUint32(c, pair, 0), v = JS_GetPropertyUint32(c, pair, 1);
            std::string ks, vs;
            if (toStr(c, k, ks) && toStr(c, v, vs)) rq.headers.emplace_back(ks, vs);
            else JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, k); JS_FreeValue(c, v); JS_FreeValue(c, pair);
        }
    }
    JS_FreeValue(c, hs);
    return true;
}

JSValue makeResult(JSContext* c, const HttpGetResult& r, const std::string& requestedUrl) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "error", jstr(c, r.error));
    JS_SetPropertyStr(c, o, "status", JS_NewInt32(c, static_cast<int>(r.status)));
    JS_SetPropertyStr(c, o, "statusText", jstr(c, r.error.empty() ? reasonPhrase(r) : ""));
    JS_SetPropertyStr(c, o, "url", jstr(c, r.effectiveUrl.empty() ? requestedUrl : r.effectiveUrl));
    JS_SetPropertyStr(c, o, "redirected", JS_NewBool(c, r.redirects > 0));
    JSValue hs = JS_NewArray(c);
    uint32_t i = 0;
    for (auto& kv : r.headers) {
        JSValue pair = JS_NewArray(c);
        JS_SetPropertyUint32(c, pair, 0, jstr(c, kv.first));
        JS_SetPropertyUint32(c, pair, 1, jstr(c, kv.second));
        JS_SetPropertyUint32(c, hs, i++, pair);
    }
    JS_SetPropertyStr(c, o, "headers", hs);
    JS_SetPropertyStr(c, o, "body", JS_NewArrayBufferCopy(c, reinterpret_cast<const uint8_t*>(r.body.data()), r.body.size()));
    return o;
}

// ---- async requests -----------------------------------------------------------------------------------

struct HttpDone {
    int         id = 0;
    JSValue     cb = JS_UNDEFINED;
    std::string url;               // as requested (the fallback for result.url)
    HttpGetResult res;
};

// Per-context queue. Held as shared_ptr so in-flight threads can safely push results even after the
// context has been torn down.
struct FetchState {
    std::mutex              mu;
    std::condition_variable workersDone;
    std::queue<HttpDone>    done;
    std::unordered_set<int> cancelled;
    size_t                  activeWorkers = 0;
    int                     nextId = 1;
    bool                    dead = false;
};
std::unordered_map<JSContext*, std::shared_ptr<FetchState>> g_fetch;

std::shared_ptr<FetchState> stateFor(JSContext* c) {
    auto it = g_fetch.find(c);
    if (it == g_fetch.end()) it = g_fetch.emplace(c, std::make_shared<FetchState>()).first;
    return it->second;
}

// Runs microtasks to completion (a checkpoint after every delivered result, like a browser after each task).
void microtaskCheckpoint(State& s) {
    for (;;) {
        JSContext* jc = nullptr;
        int r = JS_ExecutePendingJob(s.rt, &jc);
        if (r == 0) break;
        if (r < 0) s.addError(s.takeException());
    }
}

void drainFetch(State& s) {
    auto it = g_fetch.find(s.ctx);
    if (it == g_fetch.end()) return;
    auto fs = it->second;
    for (;;) {
        HttpDone d;
        bool skip = false;
        {
            std::lock_guard<std::mutex> lk(fs->mu);
            if (fs->done.empty()) break;
            d = std::move(fs->done.front());
            fs->done.pop();
            skip = fs->cancelled.erase(d.id) > 0;
        }
        if (!skip) {
            if (!d.res.error.empty()) s.addNote("request failed (" + d.res.error + "): " + d.url);
            Budget b(s, s.opts.scriptTimeoutMs);      // a runaway handler must not hang the page
            JSValue res = makeResult(s.ctx, d.res, d.url);
            JSValue r = JS_Call(s.ctx, d.cb, JS_UNDEFINED, 1, &res);
            if (JS_IsException(r)) s.addError(s.takeException());
            JS_FreeValue(s.ctx, r);
            JS_FreeValue(s.ctx, res);
            microtaskCheckpoint(s);
        }
        JS_FreeValue(s.ctx, d.cb);
    }
}

JSValue n_httpSend(ARGS) {
    State* s = S(c);
    HttpFns fns = fnsOf(s->opts);
    if (!fns.any()) return JS_ThrowTypeError(c, "network access is not available in this build");
    if (argc < 2 || !JS_IsFunction(c, argv[1])) return JS_ThrowTypeError(c, "httpSend(spec, callback): callback required");
    HttpRequest rq;
    if (!parseSpec(c, argv[0], rq)) return JS_EXCEPTION;

    auto fstate = stateFor(c);
    int id;
    {
        std::lock_guard<std::mutex> lk(fstate->mu);
        id = fstate->nextId++;
        ++fstate->activeWorkers;
    }
    JSValue cb = JS_DupValue(c, argv[1]);

    std::thread([fns, rq, fstate, id, cb]() mutable {
        HttpDone d;
        d.id = id;
        d.cb = cb;
        d.url = rq.url;
        d.res = runHttp(fns, rq);
        {
            std::lock_guard<std::mutex> lk(fstate->mu);
            // Always hand the JS value to the queue. If teardown has started it frees it after waiting for us.
            fstate->done.push(std::move(d));
            --fstate->activeWorkers;
        }
        fstate->workersDone.notify_all();
    }).detach();

    return JS_NewInt32(c, id);
}

JSValue n_httpSendSync(ARGS) {
    State* s = S(c);
    HttpFns fns = fnsOf(s->opts);
    if (!fns.any()) return JS_ThrowTypeError(c, "network access is not available in this build");
    HttpRequest rq;
    if (argc < 1 || !parseSpec(c, argv[0], rq)) return argc < 1 ? JS_ThrowTypeError(c, "httpSendSync(spec)") : JS_EXCEPTION;
    HttpGetResult res = runHttp(fns, rq);     // blocks the JS thread, exactly what a synchronous XHR does
    if (!res.error.empty()) s->addNote("request failed (" + res.error + "): " + rq.url);
    return makeResult(c, res, rq.url);
}

JSValue n_httpCancel(ARGS) {
    int32_t id = 0;
    if (argc < 1 || JS_ToInt32(c, &id, argv[0])) return JS_UNDEFINED;
    auto it = g_fetch.find(c);
    if (it == g_fetch.end()) return JS_UNDEFINED;
    std::lock_guard<std::mutex> lk(it->second->mu);
    it->second->cancelled.insert(id);
    return JS_UNDEFINED;
}

// ---- text <-> bytes -----------------------------------------------------------------------------------

JSValue n_utf8Decode(ARGS) {
    if (argc < 1) return jstr(c, "");
    std::string bytes;
    if (!bytesOf(c, argv[0], bytes)) return JS_ThrowTypeError(c, "utf8Decode: expected an ArrayBuffer or a typed array");
    size_t skip = (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
                   static_cast<unsigned char>(bytes[2]) == 0xBF) ? 3 : 0;      // a BOM is not part of the text
    return JS_NewStringLen(c, bytes.data() + skip, bytes.size() - skip);
}

JSValue n_utf8Encode(ARGS) {
    std::string str;
    if (argc >= 1 && !toStr(c, argv[0], str)) return JS_EXCEPTION;
    return JS_NewArrayBufferCopy(c, reinterpret_cast<const uint8_t*>(str.data()), str.size());
}

// ---- document.cookie ----------------------------------------------------------------------------------

JSValue n_cookieGet(ARGS) {
    State* s = S(c);
    if (!s->opts.cookieGet) return JS_NULL;                 // JS falls back to its own in-memory jar
    std::string href;
    if (argc >= 1 && !toStr(c, argv[0], href)) return JS_EXCEPTION;
    std::string out;
    try { out = s->opts.cookieGet(href); } catch (...) {}
    return jstr(c, out);
}

JSValue n_cookieSet(ARGS) {
    State* s = S(c);
    if (!s->opts.cookieSet) return JS_FALSE;
    std::string href, cookie;
    if (argc < 2 || !toStr(c, argv[0], href) || !toStr(c, argv[1], cookie)) return argc < 2 ? JS_FALSE : JS_EXCEPTION;
    try { s->opts.cookieSet(href, cookie); } catch (...) {}
    return JS_TRUE;
}

// ---- loadedfiles stub writer (reused from storage_bridge pattern) -----------

static std::string sanOrig(std::string s) {
    auto p = s.find("://"); if (p != std::string::npos) s = s.substr(p + 3);
    auto sl = s.find('/'); if (sl != std::string::npos) s = s.substr(0, sl);
    for (char& c : s) if (!std::isalnum((unsigned char)c) && c != '-' && c != '.') c = '_';
    return s.empty() ? "_unknown_" : s;
}
static std::string sanFile(std::string u) {
    auto sl = u.rfind('/'); if (sl != std::string::npos) u = u.substr(sl + 1);
    for (char ch : {'?','#'}) { auto p = u.find(ch); if (p != std::string::npos) u = u.substr(0,p); }
    for (char& c : u) if (!std::isalnum((unsigned char)c) && c != '-' && c != '.' && c != '_') c = '_';
    return u.empty() ? "index.js" : u;
}
static void writeLoadedFile(const std::string& absUrl, const std::string& content) {
    std::string origin;
    auto proto = absUrl.find("://");
    if (proto != std::string::npos) {
        auto slash = absUrl.find('/', proto + 3);
        origin = slash != std::string::npos ? absUrl.substr(0, slash) : absUrl;
    } else origin = "_local_";
    std::string dir = core::dataDir("loadedfiles/" + sanOrig(origin));
    if (dir.empty()) return;
    fs::path fp = fs::path(dir) / sanFile(absUrl);
    std::ofstream f(fp, std::ios::binary);
    if (f) {
        if (content.empty()) f << "// loadedfiles stub — content not fetched yet\n// url: " << absUrl << "\n";
        else f << content;
    }
}

}  // namespace

// ---- public API ------------------------------------------------------------

bool installFetch(State& s, JSValueConst n) {
    stateFor(s.ctx);
    JSContext* c = s.ctx;
    auto def = [&](const char* name, JSCFunction* f, int len) {
        JS_DefinePropertyValueStr(c, n, name, JS_NewCFunction2(c, f, name, len, JS_CFUNC_generic, 0), JS_PROP_C_W_E);
    };
    def("httpSend", n_httpSend, 2);
    def("httpSendSync", n_httpSendSync, 1);
    def("httpCancel", n_httpCancel, 1);
    def("utf8Decode", n_utf8Decode, 1);
    def("utf8Encode", n_utf8Encode, 1);
    def("cookieGet", n_cookieGet, 1);
    def("cookieSet", n_cookieSet, 2);
    JS_SetPropertyStr(c, n, "httpAvailable", JS_NewBool(c, fnsOf(s.opts).any()));
    JS_SetPropertyStr(c, n, "cookieAvailable", JS_NewBool(c, static_cast<bool>(s.opts.cookieGet) && static_cast<bool>(s.opts.cookieSet)));
    return true;
}

void pumpFetch(State& s) {
    drainFetch(s);
}

// Fetch source code for a script (HTTP or file://). Returns false only on a hard failure; missing
// remote files are non-fatal (return true with empty body, like a 404'd classic script). `note`
// receives engine notes for the operator; `err` receives the error string thrown back to the caller
// for diagnostic logs. Used by both classic <script src> and <script type="module" src> paths,
// and by the QuickJS module loader for `import` resolution.
bool fetchScriptSource(State& s, const std::string& absUrl, std::string& body, std::string* err) {
    bool isLocal = absUrl.rfind("file://", 0) == 0;
    if (isLocal) {
        std::string path = absUrl.substr(7);   // strip "file://"
        std::ifstream f(path, std::ios::binary);
        if (!f) { s.addNote("script file not found: " + absUrl); return true; }
        body.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    }
    HttpFns fns = fnsOf(s.opts);
    if (!fns.any()) {
        writeLoadedFile(absUrl, "");
        s.addNote("script not loaded (no HTTP callback wired): " + absUrl);
        return true;
    }
    HttpRequest rq;
    rq.url      = absUrl;
    rq.pageUrl   = s.pageUrl;
    rq.mode     = "no-cors";
    rq.dest     = "script";
    auto resp = runHttp(fns, rq);
    if (!resp.error.empty()) {
        writeLoadedFile(absUrl, "");
        s.addNote("script fetch failed (" + resp.error + "): " + absUrl);
        if (err) *err = resp.error;
        return true;   // a missing remote script is non-fatal
    }
    if (resp.status < 200 || resp.status >= 300) {
        writeLoadedFile(absUrl, "");
        s.addNote("script HTTP " + std::to_string(resp.status) + ": " + absUrl);
        if (err) *err = "HTTP " + std::to_string(resp.status);
        return true;
    }
    body = std::move(resp.body);
    writeLoadedFile(absUrl, body);
    return true;
}

// Called from host.cpp::runScriptNode when a <script src> is found:
// downloads synchronously (blocks the JS engine, which is idle waiting for settle() anyway),
// evals the source as a classic global script, and saves the real content to loadedfiles.
bool fetchAndRunScript(State& s, const std::string& absUrl, std::string* err) {
    std::string body;
    if (!fetchScriptSource(s, absUrl, body, err)) return false;
    if (body.empty()) return true;
    bool ok = s.evalGlobal(body, absUrl, err);
    if (!ok) s.addError("<script src> " + absUrl + ": " + (err ? *err : ""));
    return ok;
}

// Same as fetchAndRunScript but evaluates the source as an ES module. Used for
// <script type="module" src=...>. The QuickJS module loader (installed in attach())
// handles any `import` statements encountered in the source; for that to find the
// module by URL, the module name passed to JS_Eval MUST be the absolute URL.
bool fetchAndRunModuleScript(State& s, const std::string& absUrl, std::string* err) {
    std::string body;
    if (!fetchScriptSource(s, absUrl, body, err)) return false;
    if (body.empty()) return true;
    Budget b(s, s.opts.scriptTimeoutMs);
    JSValue r = JS_Eval(s.ctx, body.c_str(), body.size(), absUrl.c_str(), JS_EVAL_TYPE_MODULE);
    bool ok = !JS_IsException(r);
    if (!ok) { std::string e = s.takeException(); if (err) *err = e; s.addError("<script type=module> " + absUrl + ": " + e); }
    JS_FreeValue(s.ctx, r);
    s.drainJobs();
    return ok;
}

// QuickJS module loader: invoked when an ES module hits an `import` statement.
// `module_name` is already normalized (URL-resolved) by moduleNormalize below.
// Returns a compiled-but-not-evaluated module (JS_EVAL_FLAG_COMPILE_ONLY); QuickJS
// handles instantiation and top-level evaluation. Failure throws a JS exception.
JSModuleDef* moduleLoaderCb(JSContext* c, const char* module_name, void* opaque) {
    State* s = S(c);
    if (!s) return nullptr;
    std::string name = module_name ? module_name : "";
    if (name.empty()) { JS_ThrowTypeError(c, "module: empty module name"); return nullptr; }

    std::string body;
    std::string err;
    // The loader runs synchronously inside JS_Eval — the engine is blocked, so a
    // blocking HTTP fetch here is fine (same as <script src>).
    if (!fetchScriptSource(*s, name, body, &err)) {
        JS_ThrowTypeError(c, "module loader failed for %s: %s", name.c_str(), err.c_str());
        return nullptr;
    }
    if (body.empty()) {
        JS_ThrowTypeError(c, "module not loaded (empty body): %s", name.c_str());
        return nullptr;
    }
    JSValue v = JS_Eval(c, body.c_str(), body.size(), name.c_str(),
                       JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(v)) {
        // The exception is already pending on the context — let it propagate.
        JS_FreeValue(c, v);
        return nullptr;
    }
    JSModuleDef* m = (JSModuleDef*)JS_VALUE_GET_PTR(v);
    JS_FreeValue(c, v);
    return m;
}

// QuickJS module normalizer: resolves an import specifier against the importing
// module's URL using the same URL resolver as <a href>/fetch/etc. Returns a
// malloc'd string (per QuickJS's contract); NULL throws.
char* moduleNormalizeCb(JSContext* c, const char* base, const char* name, void* opaque) {
    std::string b = base ? base : "";
    std::string n = name ? name : "";
    std::string resolved = lucyn::shell::resolveUrl(b, n);
    char* out = js_strdup(c, resolved.c_str());
    if (!out) JS_ThrowTypeError(c, "module: out of memory normalizing %s", n.c_str());
    return out;
}

void teardownFetch(JSContext* ctx) {
    auto it = g_fetch.find(ctx);
    if (it == g_fetch.end()) return;
    auto fs = it->second;
    {
        std::unique_lock<std::mutex> lk(fs->mu);
        fs->dead = true;
        // The HTTP callback may capture objects owned by the host's caller.
        // Do not let detached workers outlive that callback's owner.
        fs->workersDone.wait(lk, [&] { return fs->activeWorkers == 0; });
        // Workers only enqueue results; JS values are freed here, on the JS thread, while the context is alive.
        while (!fs->done.empty()) {
            JS_FreeValue(ctx, fs->done.front().cb);
            fs->done.pop();
        }
        fs->cancelled.clear();
    }
    g_fetch.erase(it);
}

}  // namespace lucyn::js::detail
