// Timers, console, queueMicrotask, and the private `__n` object through which the prelude reaches native code.
// `__n` is a parameter of the prelude function and is captured by its closures; it is never a property of any global.
#include "impl.h"
#include <algorithm>
#include <cmath>
#include <random>
#include <lucyn/shell/url.h>

namespace lucyn::js::detail {
namespace {

#define ARGS JSContext* c, [[maybe_unused]] JSValueConst t, [[maybe_unused]] int argc, [[maybe_unused]] JSValueConst* argv
#define MARGS JSContext* c, [[maybe_unused]] JSValueConst t, [[maybe_unused]] int argc, [[maybe_unused]] JSValueConst* argv, int magic

// ---- timers --------------------------------------------------------------------------------------------------------------------
JSValue timerFn(MARGS) {
    State* s = S(c);
    static const char* names[] = {"setTimeout", "setInterval", "requestAnimationFrame"};
    if (argc < 1 || !JS_IsFunction(c, argv[0]))
        return JS_ThrowTypeError(c, "Failed to execute '%s' on 'Window': The callback provided as parameter 1 is not a function.", names[magic]);
    double delay = 0;
    if (magic != 2 && argc > 1 && JS_ToFloat64(c, &delay, argv[1]) < 0) return JS_EXCEPTION;
    std::vector<JSValue> args;
    if (magic != 2) for (int i = 2; i < argc; ++i) args.push_back(argv[i]);
    return JS_NewInt32(c, addTimer(*s, magic, argv[0], delay, args));
}
JSValue clearTimer(ARGS) {
    State* s = S(c);
    int32_t id = 0;
    if (argc < 1 || JS_ToInt32(c, &id, argv[0]) < 0) { if (argc >= 1) JS_FreeValue(c, JS_GetException(c)); return JS_UNDEFINED; }
    for (size_t i = 0; i < s->timers.size(); ++i) {
        if (s->timers[i].id != id) continue;
        JS_FreeValue(c, s->timers[i].fn);
        for (JSValue a : s->timers[i].args) JS_FreeValue(c, a);
        s->timers.erase(s->timers.begin() + long(i));
        break;
    }
    return JS_UNDEFINED;
}
JSValue microtaskJob(JSContext* c, int, JSValueConst* argv) { return JS_Call(c, argv[0], JS_UNDEFINED, 0, nullptr); }
JSValue queueMicrotask(ARGS) {
    if (argc < 1 || !JS_IsFunction(c, argv[0]))
        return JS_ThrowTypeError(c, "Failed to execute 'queueMicrotask' on 'Window': The callback provided as parameter 1 is not a function.");
    JS_EnqueueJob(c, microtaskJob, 1, argv);
    return JS_UNDEFINED;
}

// ---- console -------------------------------------------------------------------------------------------------------------------
static std::string formatConsoleArg(JSContext* c, JSValueConst value) {
    std::string text = toStdOrEmpty(c, value);
    if (!JS_IsError(value)) return text;

    // Error#toString() omits its stack. Preserve it in console output so a
    // caught page exception still reveals the script and call site that failed.
    JSValue stackValue = JS_GetPropertyStr(c, value, "stack");
    if (JS_IsString(stackValue)) {
        std::string stack = toStdOrEmpty(c, stackValue);
        if (stack.size() > 16384) stack.resize(16384);
        if (!stack.empty() && stack != text) {
            if (stack.compare(0, text.size(), text) == 0) text = std::move(stack);
            else text += "\n" + stack;
        }
    } else if (JS_IsException(stackValue)) {
        JS_FreeValue(c, JS_GetException(c));
    }
    JS_FreeValue(c, stackValue);
    return text;
}

JSValue consoleFn(MARGS) {
    State* s = S(c);
    static const char* levels[] = {"log", "info", "warn", "error", "debug", "assert"};
    if (!s->opts.onConsole) return JS_UNDEFINED;
    int first = 0;
    if (magic == 5) { if (argc > 0 && JS_ToBool(c, argv[0]) > 0) return JS_UNDEFINED; first = 1; }
    std::string line = magic == 5 ? "Assertion failed:" : "";
    for (int i = first; i < argc; ++i) { if (!line.empty()) line += ' '; line += formatConsoleArg(c, argv[i]); }
    if (magic == 3) {
        // Some frameworks stringify caught exceptions before calling
        // console.error, which loses Error.stack. Capture the active JS frame
        // at the logging boundary to retain at least the original call site.
        JSValue probe = JS_NewError(c);
        JSValue stackValue = JS_GetPropertyStr(c, probe, "stack");
        if (JS_IsString(stackValue)) {
            std::string stack = toStdOrEmpty(c, stackValue);
            const size_t firstLine = stack.find('\n');
            if (firstLine != std::string::npos) {
                stack.erase(0, firstLine + 1);
                if (stack.size() > 8192) stack.resize(8192);
                if (!stack.empty()) line += "\n" + stack;
            }
        } else if (JS_IsException(stackValue)) {
            JS_FreeValue(c, JS_GetException(c));
        }
        JS_FreeValue(c, stackValue);
        JS_FreeValue(c, probe);
    }
    s->opts.onConsole(magic == 5 ? "error" : levels[magic], line);
    return JS_UNDEFINED;
}
JSValue noopFn(ARGS) { (void)c; return JS_UNDEFINED; }

// ---- __n natives ---------------------------------------------------------------------------------------------------------------
void keep(JSContext* c, JSValue& slot, JSValueConst v) { JS_FreeValue(c, slot); slot = JS_DupValue(c, v); }

JSValue n_setTagProtos(ARGS) { State* s = S(c); if (argc < 2) return JS_UNDEFINED;
    keep(c, s->tagProtos, argv[0]); keep(c, s->protoHtmlElement, argv[1]);
    for (auto& [k, v] : s->tagProtoCache) JS_FreeValue(c, v);
    s->tagProtoCache.clear();
    return JS_UNDEFINED; }
JSValue n_setMutationSink(ARGS) { if (argc) keep(c, S(c)->mutationSink, argv[0]); return JS_UNDEFINED; }
JSValue n_setLifecycle(ARGS) { if (argc) keep(c, S(c)->lifecycleFn, argv[0]); return JS_UNDEFINED; }
JSValue n_setDomException(ARGS) { if (argc) keep(c, S(c)->domExc, argv[0]); return JS_UNDEFINED; }
JSValue n_setObserving(ARGS) { State* s = S(c); s->observing = argc && JS_ToBool(c, argv[0]) > 0; if (!s->observing) s->records.clear(); return JS_UNDEFINED; }
JSValue n_resolveUrl(ARGS) { std::string b, r; if (argc < 2 || !toStr(c, argv[0], b) || !toStr(c, argv[1], r)) return JS_EXCEPTION;
    return jstr(c, shell::resolveUrl(b, r)); }
JSValue n_now(ARGS) { return JS_NewFloat64(c, S(c)->nowMs()); }
JSValue n_randomValues(ARGS) {
    if (argc < 1 || JS_GetTypedArrayType(argv[0]) < 0)
        return JS_ThrowTypeError(c, "Failed to execute 'getRandomValues' on 'Crypto': parameter 1 is not an integer TypedArray.");
    const int type = JS_GetTypedArrayType(argv[0]);
    if (type == JS_TYPED_ARRAY_FLOAT16 || type == JS_TYPED_ARRAY_FLOAT32 || type == JS_TYPED_ARRAY_FLOAT64)
        return JS_ThrowTypeError(c, "Failed to execute 'getRandomValues' on 'Crypto': floating-point arrays are not supported.");

    size_t offset = 0, length = 0, elementSize = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(c, argv[0], &offset, &length, &elementSize);
    if (JS_IsException(buffer)) return JS_EXCEPTION;
    if (length > 65536) {
        JS_FreeValue(c, buffer);
        return JS_ThrowTypeError(c, "Failed to execute 'getRandomValues' on 'Crypto': quota exceeded.");
    }
    size_t bufferSize = 0;
    uint8_t* bytes = JS_GetArrayBuffer(c, &bufferSize, buffer);
    JS_FreeValue(c, buffer);
    if (!bytes || offset > bufferSize || length > bufferSize - offset)
        return JS_ThrowTypeError(c, "Failed to execute 'getRandomValues' on 'Crypto': invalid TypedArray buffer.");
    try {
        static std::random_device source;
        std::uniform_int_distribution<unsigned int> byte(0, 255);
        for (size_t i = 0; i < length; ++i) bytes[offset + i] = static_cast<uint8_t>(byte(source));
    } catch (...) {
        return JS_ThrowInternalError(c, "Crypto.getRandomValues could not obtain random data.");
    }
    return JS_DupValue(c, argv[0]);
}
JSValue n_reportError(ARGS) { State* s = S(c); if (!argc) return JS_UNDEFINED;
    JS_Throw(c, JS_DupValue(c, argv[0])); s->addError(s->takeException()); return JS_UNDEFINED; }
JSValue n_note(ARGS) { std::string x; if (argc && toStr(c, argv[0], x)) S(c)->addNote(x); return JS_UNDEFINED; }
JSValue n_getDocument(ARGS) { State* s = S(c); return wrapNode(*s, &s->doc->root()); }

// ---- persona -> JSON (the prelude builds navigator / screen from it) ----------------------------------------------------------
std::string jq(const std::string& s) {
    std::string o = "\"";
    for (unsigned char ch : s) {
        switch (ch) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (ch < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", ch); o += b; } else o += char(ch);
        }
    }
    return o + "\"";
}
std::string brandsJson(const std::vector<profile::Surface::Brand>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) { if (i) o += ","; o += "{\"brand\":" + jq(v[i].brand) + ",\"version\":" + jq(v[i].version) + "}"; }
    return o + "]";
}
std::string personaJson(const Persona& p) {
    const auto& s = p.surface; const auto& d = p.device;
    std::string langs = "[";
    for (size_t i = 0; i < s.languages.size(); ++i) { if (i) langs += ","; langs += jq(s.languages[i]); }
    langs += "]";
    auto b = [](bool x) { return x ? "true" : "false"; };
    std::string o = "{\"surface\":{";
    o += "\"userAgent\":" + jq(s.userAgent) + ",\"platform\":" + jq(s.platform) + ",\"vendor\":" + jq(s.vendor) +
         ",\"productSub\":" + jq(s.productSub) + ",\"languages\":" + langs + ",\"maxTouchPoints\":" + std::to_string(s.maxTouchPoints) +
         ",\"mobile\":" + b(s.mobile) + ",\"brands\":" + brandsJson(s.brands) + ",\"fullVersionList\":" + brandsJson(s.fullVersionList) +
         ",\"uaPlatform\":" + jq(s.secChUaPlatform) + ",\"uaPlatformVersion\":" + jq(s.uaPlatformVersion) +
         ",\"uaArchitecture\":" + jq(s.uaArchitecture) + ",\"uaBitness\":" + jq(s.uaBitness) + ",\"uaModel\":" + jq(s.uaModel) +
         ",\"hasUserAgentData\":" + b(s.hasUserAgentData) + ",\"hasDeviceMemory\":" + b(s.hasDeviceMemory) +
         ",\"hasWindowChrome\":" + b(s.hasWindowChrome) + "},\"device\":{";
    o += "\"cpuCores\":" + std::to_string(d.cpuCores) + ",\"memoryGb\":" + std::to_string(d.memoryGb) +
         ",\"screenW\":" + std::to_string(d.screenW) + ",\"screenH\":" + std::to_string(d.screenH) +
         ",\"pixelRatio\":" + std::to_string(d.pixelRatio > 0 ? d.pixelRatio : 1.0) + ",\"reservedBottom\":" + std::to_string(d.reservedBottom) + "}}";
    return o;
}

void fn(JSContext* c, JSValueConst obj, const char* name, JSCFunction* f, int len, int magic = -1, bool enumerable = true) {
    JSValue v = magic < 0 ? JS_NewCFunction2(c, f, name, len, JS_CFUNC_generic, 0)
                          : JS_NewCFunction2(c, f, name, len, JS_CFUNC_generic_magic, magic);
    JS_DefinePropertyValueStr(c, obj, name, v, enumerable ? JS_PROP_C_W_E : (JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE));
}
JSCFunction* asPlain(JSValue (*f)(MARGS)) { return reinterpret_cast<JSCFunction*>(reinterpret_cast<void (*)()>(f)); }

}  // namespace

int addTimer(State& s, int kind, JSValueConst f, double delay, const std::vector<JSValue>& args) {
    if (!(delay >= 0)) delay = 0;                               // NaN, negatives
    if (kind == 2) delay = 16;
    if (kind == 1 && delay < 4) delay = 4;                      // an interval of 0 would spin forever
    Timer t;
    t.id = s.nextTimerId++;
    t.kind = kind;
    t.interval = kind == 1 ? delay : -1;
    t.due = s.nowMs() + delay;
    t.seq = ++s.seq;
    t.fn = JS_DupValue(s.ctx, f);
    for (JSValue a : args) t.args.push_back(JS_DupValue(s.ctx, a));
    s.timers.push_back(std::move(t));
    return s.timers.back().id;
}

bool installWindow(State& s) {
    JSContext* c = s.ctx;
    JSValue g = JS_GetGlobalObject(c);

    fn(c, g, "setTimeout", asPlain(timerFn), 2, 0);
    fn(c, g, "setInterval", asPlain(timerFn), 2, 1);
    fn(c, g, "requestAnimationFrame", asPlain(timerFn), 1, 2);
    fn(c, g, "clearTimeout", clearTimer, 1);
    fn(c, g, "clearInterval", clearTimer, 1);
    fn(c, g, "cancelAnimationFrame", clearTimer, 1);
    fn(c, g, "queueMicrotask", queueMicrotask, 1);

    JSValue con = JS_NewObject(c);
    static const char* lv[] = {"log", "info", "warn", "error", "debug"};
    for (int i = 0; i < 5; ++i) fn(c, con, lv[i], asPlain(consoleFn), 0, i);
    fn(c, con, "assert", asPlain(consoleFn), 0, 5);
    for (const char* nm : {"trace", "dir", "dirxml", "table", "group", "groupCollapsed", "groupEnd", "time", "timeEnd", "timeLog",
                           "count", "countReset", "clear", "profile", "profileEnd"})
        fn(c, con, nm, noopFn, 0);
    JS_DefinePropertyValueStr(c, g, "console", con, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);

    JSValue n = JS_NewObject(c);
    fn(c, n, "setTagProtos", n_setTagProtos, 2);        fn(c, n, "setMutationSink", n_setMutationSink, 1);
    fn(c, n, "setLifecycle", n_setLifecycle, 1);        fn(c, n, "setDomException", n_setDomException, 1);
    fn(c, n, "setObserving", n_setObserving, 1);        fn(c, n, "resolveUrl", n_resolveUrl, 2);
    fn(c, n, "now", n_now, 0);                          fn(c, n, "reportError", n_reportError, 1);
    fn(c, n, "randomValues", n_randomValues, 1);
    fn(c, n, "note", n_note, 1);                        fn(c, n, "getDocument", n_getDocument, 0);
    JSValue protos = JS_NewObject(c);
    JS_SetPropertyStr(c, protos, "Node", JS_DupValue(c, s.protoNode));
    JS_SetPropertyStr(c, protos, "Element", JS_DupValue(c, s.protoElement));
    JS_SetPropertyStr(c, protos, "Text", JS_DupValue(c, s.protoText));
    JS_SetPropertyStr(c, protos, "Comment", JS_DupValue(c, s.protoComment));
    JS_SetPropertyStr(c, protos, "Document", JS_DupValue(c, s.protoDocument));
    JS_SetPropertyStr(c, n, "protos", protos);
    JS_SetPropertyStr(c, n, "pageUrl", jstr(c, s.pageUrl));
    std::optional<Persona> persona;
    if (s.opts.persona) persona = s.opts.persona();
    JS_SetPropertyStr(c, n, "persona", persona ? jstr(c, personaJson(*persona)) : JS_NULL);

    if (!installStorage(s, n)) return false;
    if (!installFetch(s, n)) return false;
    if (!installWebSocket(s, n)) return false;

    std::string src = preludeSource();
    JSValue prelude = JS_Eval(c, src.c_str(), src.size(), "<lucyn-prelude>", JS_EVAL_TYPE_GLOBAL);
    bool ok = !JS_IsException(prelude);
    if (ok) {
        JSValue args[2] = {n, g};
        JSValue r = JS_Call(c, prelude, JS_UNDEFINED, 2, args);
        ok = !JS_IsException(r);
        JS_FreeValue(c, r);
    }
    JS_FreeValue(c, prelude);
    JS_FreeValue(c, n);
    JS_FreeValue(c, g);
    return ok;      // on failure the exception is still pending: attach() reads it
}

}  // namespace lucyn::js::detail
