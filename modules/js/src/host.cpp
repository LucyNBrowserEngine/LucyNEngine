// The ScriptHost implementation: attach/detach, running scripts, the event loop, the watchdog, the `lucyn` object.
#include "impl.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
#include <lucyn/core/paths.h>
#include <lucyn/shell/url.h>

namespace lucyn::js {
namespace detail {



using Clock = std::chrono::steady_clock;

// ------------------------------------------------------------------------------------------------ helpers

std::string lowerAscii(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32); return s; }
std::string upperAscii(std::string s) { for (auto& c : s) if (c >= 'a' && c <= 'z') c = char(c - 32); return s; }

bool toStr(JSContext* c, JSValueConst v, std::string& out) {
    size_t n = 0;
    const char* p = JS_ToCStringLen2(c, &n, v, false);
    if (!p) return false;
    out.assign(p, n);
    JS_FreeCString(c, p);
    return true;
}

std::string toStdOrEmpty(JSContext* c, JSValueConst v) {
    std::string s;
    if (!toStr(c, v, s)) { JS_FreeValue(c, JS_GetException(c)); return {}; }
    return s;
}

JSValue throwDom(JSContext* c, const char* name, const std::string& msg) {
    State* s = S(c);
    if (s && JS_IsFunction(c, s->domExc)) {
        JSValue args[2] = {jstr(c, msg), JS_NewString(c, name)};
        JSValue e = JS_CallConstructor(c, s->domExc, 2, args);
        JS_FreeValue(c, args[0]);
        JS_FreeValue(c, args[1]);
        if (!JS_IsException(e)) return JS_Throw(c, e);
        return e;
    }
    return JS_ThrowInternalError(c, "%s: %s", name, msg.c_str());
}

Budget::Budget(State& st, int ms) : s(st), prevArmed(st.armed), prevTimedOut(st.timedOut), prevMs(st.budgetMs), prevDl(st.deadline) {
    if (ms <= 0) return;                                   // no new limit
    auto dl = Clock::now() + std::chrono::milliseconds(ms);
    if (!s.armed || dl < s.deadline) { s.deadline = dl; s.budgetMs = ms; }
    s.armed = true;
}
Budget::~Budget() { s.armed = prevArmed; s.deadline = prevDl; s.budgetMs = prevMs; (void)prevTimedOut; }

static int interruptCb(JSRuntime*, void* opaque) {
    State* s = static_cast<State*>(opaque);
    if (!s->armed) return 0;
    if (Clock::now() > s->deadline) { s->timedOut = true; return 1; }
    return 0;
}

// ------------------------------------------------------------------------------------------------ State

double State::nowMs() const {
    return std::chrono::duration<double, std::milli>(Clock::now() - epoch).count();
}

void State::addError(const std::string& e) {
    if (!errors || e.empty()) return;
    if (errors->size() >= 100) errors->erase(errors->begin());
    errors->push_back(e);
}
void State::addNote(const std::string& n) {
    if (!notes) return;
    if (std::find(notes->begin(), notes->end(), n) != notes->end()) return;
    if (notes->size() < 200) notes->push_back(n);
}

std::string State::takeException() {
    JSValue ex = JS_GetException(ctx);
    std::string out;
    if (timedOut) {
        out = "script timed out after " + std::to_string(budgetMs) + " ms";
        timedOut = false;
    } else {
        out = toStdOrEmpty(ctx, ex);
        if (out.empty()) out = "uncaught exception";
        if (JS_IsError(ex)) {
            JSValue st = JS_GetPropertyStr(ctx, ex, "stack");
            if (JS_IsString(st)) {
                std::string stack = toStdOrEmpty(ctx, st);
                if (stack.size() > 16384) stack.resize(16384);
                if (!stack.empty()) {
                    // QuickJS includes the error message in Error.stack. Keep
                    // all frames: the first one often points at a generic
                    // runtime wrapper, while later frames identify the site code.
                    if (stack.compare(0, out.size(), out) == 0) out = std::move(stack);
                    else out += "\n" + stack;
                }
            } else if (JS_IsException(st)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
            }
            JS_FreeValue(ctx, st);
        }
    }
    JS_FreeValue(ctx, ex);
    return out;
}

void State::drainJobs() {
    if (!ctx || drainDepth > 0) return;           // a job that runs a script must not drain again: the outer loop continues
    pumpFetch(*this);                              // deliver finished fetch()/XHR requests before running microtasks
    pumpWs(*this);                                 // deliver WebSocket open/message/close/error events
    ++drainDepth;
    Budget b(*this, opts.scriptTimeoutMs);
    unsigned guard = 0;
    for (;;) {
        JSContext* jc = nullptr;
        int r = JS_ExecutePendingJob(rt, &jc);
        if (r == 0) break;
        if (r < 0) addError(takeException());
        if ((++guard & 255u) == 0 && armed && Clock::now() > deadline) {
            addError("microtask queue did not drain within " + std::to_string(budgetMs) + " ms");
            break;
        }
    }
    --drainDepth;
}

bool State::evalGlobal(const std::string& code, const std::string& file, std::string* err) {
    Budget b(*this, opts.scriptTimeoutMs);
    JSValue r = JS_Eval(ctx, code.c_str(), code.size(), file.c_str(), JS_EVAL_TYPE_GLOBAL);
    bool ok = !JS_IsException(r);
    if (!ok) { std::string e = takeException(); if (err) *err = e; }
    JS_FreeValue(ctx, r);
    drainJobs();
    return ok;
}

bool State::runOneDueTimer(double t) {
    int best = -1;
    for (size_t i = 0; i < timers.size(); ++i) {
        if (timers[i].due > t) continue;
        if (best < 0 || timers[i].due < timers[best].due || (timers[i].due == timers[best].due && timers[i].seq < timers[best].seq))
            best = int(i);
    }
    if (best < 0) return false;
    Timer& tr = timers[best];
    JSValue fn = JS_DupValue(ctx, tr.fn);
    std::vector<JSValue> args;
    const int kind = tr.kind;
    if (kind == 1) {                                        // interval: stays, rescheduled
        for (JSValue a : tr.args) args.push_back(JS_DupValue(ctx, a));
        tr.due += tr.interval;
        if (tr.due <= t) tr.due = t + tr.interval;
        tr.seq = ++seq;
    } else {                                                // one-shot: leaves
        args = std::move(tr.args);
        JS_FreeValue(ctx, tr.fn);
        timers.erase(timers.begin() + best);
    }
    if (kind == 2) { for (JSValue a : args) JS_FreeValue(ctx, a); args.clear(); args.push_back(JS_NewFloat64(ctx, t)); }
    {
        Budget b(*this, opts.scriptTimeoutMs);
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, int(args.size()), args.data());
        if (JS_IsException(r)) addError(takeException());
        JS_FreeValue(ctx, r);
    }
    for (JSValue a : args) JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, fn);
    drainJobs();
    return true;
}

void State::fireLifecycle(const char* stage) {
    if (!JS_IsFunction(ctx, lifecycleFn)) return;
    Budget b(*this, opts.scriptTimeoutMs);
    JSValue a = JS_NewString(ctx, stage);
    JSValue r = JS_Call(ctx, lifecycleFn, JS_UNDEFINED, 1, &a);
    if (JS_IsException(r)) addError(takeException());
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, a);
}

void State::flushMutations() {
    mutJobQueued = false;
    if (records.empty()) return;
    std::vector<MutRec> recs;
    recs.swap(records);
    if (!JS_IsFunction(ctx, mutationSink)) return;
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (auto& r : recs) {
        JSValue e = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, e, 0, JS_NewInt32(ctx, r.kind));
        JS_SetPropertyUint32(ctx, e, 1, wrapNode(*this, r.target));
        JS_SetPropertyUint32(ctx, e, 2, wrapNode(*this, r.node));
        JS_SetPropertyUint32(ctx, e, 3, jstr(ctx, r.name));
        JS_SetPropertyUint32(ctx, arr, i++, e);
    }
    JSValue res = JS_Call(ctx, mutationSink, JS_UNDEFINED, 1, &arr);
    if (JS_IsException(res)) addError(takeException());
    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, arr);
}

void State::onMutation(const dom::Document::Mutation& m) {
    using K = dom::Document::Mutation::Kind;
    if (observing && records.size() < 100000) {
        int kind = m.kind == K::ChildInserted ? 0 : m.kind == K::ChildRemoved ? 1 : m.kind == K::Attribute ? 2 : 3;
        records.push_back({kind, m.target, m.node, m.name});
        if (!mutJobQueued) {                                 // delivered as a microtask, never inside the DOM's own callback
            mutJobQueued = true;
            JS_EnqueueJob(ctx, [](JSContext* c, int, JSValueConst*) -> JSValue { S(c)->flushMutations(); return JS_UNDEFINED; }, 0, nullptr);
        }
    }
    // A <script> made by JS and inserted into the live page runs (innerHTML-made and parsed ones do not, like a browser).
    if (m.kind == K::ChildInserted && m.node && !createdScripts.empty() && isConnected(*this, m.target)) {
        std::vector<dom::Node*> st{m.node};
        while (!st.empty()) {
            dom::Node* x = st.back(); st.pop_back();
            if (x->isElement() && x->name == "script" && createdScripts.count(x) && !queuedScripts.count(x)) {
                queuedScripts.insert(x);
                pendingScripts.push_back(x);
            }
            for (auto& k : x->children) st.push_back(k.get());
        }
        if (!pendingScripts.empty() && !scriptJobQueued) {
            scriptJobQueued = true;
            JS_EnqueueJob(ctx, [](JSContext* c, int, JSValueConst*) -> JSValue { S(c)->runPendingScripts(); return JS_UNDEFINED; }, 0, nullptr);
        }
    }
}

void State::runPendingScripts() {
    scriptJobQueued = false;
    std::vector<dom::Node*> list;
    list.swap(pendingScripts);
    for (dom::Node* n : list) {
        queuedScripts.erase(n);
        if (!startedScripts.count(n)) runScriptNode(n);
    }
}

static bool classicScriptType(const std::string* t) {
    if (!t) return true;
    std::string v = lowerAscii(*t);
    size_t b = v.find_first_not_of(" \t\r\n"), e = v.find_last_not_of(" \t\r\n");
    v = b == std::string::npos ? std::string() : v.substr(b, e - b + 1);
    return v.empty() || v == "text/javascript" || v == "application/javascript" || v == "text/ecmascript" ||
           v == "application/ecmascript" || v == "application/x-javascript" || v == "text/jscript";
}

bool State::runScriptNode(dom::Node* n, std::string* err) {
    startedScripts.insert(n);
    createdScripts.erase(n);
    if (n->hasAttr("nomodule")) return true;
    if (const std::string* src = n->attr("src")) {
        std::string absUrl = shell::resolveUrl(pageUrl, *src);
        return fetchAndRunScript(*this, absUrl, err);
    }
    const std::string* type = n->attr("type");
    if (type && lowerAscii(*type) == "module") { addNote("<script type=module> skipped (not supported yet)"); return true; }
    if (!classicScriptType(type)) return true;                // JSON / importmap / templates are data, not code
    std::string code;
    for (auto& c : n->children) if (c->isText()) code += c->text;
    if (code.find_first_not_of(" \t\r\n") == std::string::npos) return true;
    currentScript = n;
    std::string e;
    bool ok = evalGlobal(code, pageUrl.empty() ? "inline" : pageUrl, &e);
    currentScript = nullptr;
    if (!ok) { addError(e); if (err) *err = e; }
    return ok;
}

// ------------------------------------------------------------------------------------------------ `lucyn.*`

static JSValue hookTrampoline(JSContext* c, JSValueConst, int argc, JSValueConst* argv, int, JSValueConst* data) {
    State* s = S(c);
    if (!s || !s->hooks) return JS_ThrowInternalError(c, "lucyn hooks are not available");
    std::string name = toStdOrEmpty(c, data[0]);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        std::string a;
        if (JS_IsString(argv[i])) { if (!toStr(c, argv[i], a)) return JS_EXCEPTION; }
        else if (JS_IsObject(argv[i]) && !JS_IsFunction(c, argv[i]) && JS_IsFunction(c, s->jsonStringify)) {
            // our own copy of JSON.stringify, taken before any page code ran: a page cannot swap it out
            JSValue j = JS_Call(c, s->jsonStringify, JS_UNDEFINED, 1, &argv[i]);
            if (JS_IsException(j)) { JS_FreeValue(c, JS_GetException(c)); if (!toStr(c, argv[i], a)) return JS_EXCEPTION; }
            else if (JS_IsString(j)) { toStr(c, j, a); }
            else { if (!toStr(c, argv[i], a)) { JS_FreeValue(c, j); return JS_EXCEPTION; } }
            JS_FreeValue(c, j);
        } else if (!toStr(c, argv[i], a)) return JS_EXCEPTION;
        args.push_back(std::move(a));
    }
    shell::HookResult r = s->hooks->call(name, args);      // by name, at call time: the Hydrator removes hooks after the run
    if (!r.ok) {
        JSValue e = JS_NewError(c);
        JS_DefinePropertyValueStr(c, e, "message", jstr(c, r.value), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        return JS_Throw(c, e);
    }
    return jstr(c, r.value);
}

bool State::buildLucyn() {
    JSValue root = JS_NewObject(ctx);
    const int flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;       // not enumerable: for..in over it shows nothing
    for (const std::string& full : hooks->names()) {
        std::vector<std::string> parts;
        size_t b = 0;
        for (;;) { size_t d = full.find('.', b); parts.push_back(full.substr(b, d == std::string::npos ? d : d - b)); if (d == std::string::npos) break; b = d + 1; }
        bool bad = false;
        for (auto& p : parts) if (p.empty()) bad = true;
        if (bad) { addNote("hook name skipped (bad name): " + full); continue; }
        JSValue cur = JS_DupValue(ctx, root);
        for (size_t i = 0; i + 1 < parts.size() && !bad; ++i) {
            JSValue nxt = JS_GetPropertyStr(ctx, cur, parts[i].c_str());
            if (JS_IsUndefined(nxt)) {
                nxt = JS_NewObject(ctx);
                JS_DefinePropertyValueStr(ctx, cur, parts[i].c_str(), JS_DupValue(ctx, nxt), flags);
            } else if (JS_IsFunction(ctx, nxt)) { bad = true; }
            JS_FreeValue(ctx, cur);
            cur = nxt;
        }
        if (bad) { addNote("hook name skipped (clashes with another hook): " + full); JS_FreeValue(ctx, cur); continue; }
        JSValue nameVal = jstr(ctx, full);
        JSValue fn = JS_NewCFunctionData2(ctx, hookTrampoline, parts.back().c_str(), 0, 0, 1, &nameVal);
        JS_FreeValue(ctx, nameVal);
        JS_DefinePropertyValueStr(ctx, cur, parts.back().c_str(), fn, flags);
        JS_FreeValue(ctx, cur);
    }
    // freeze the whole tree so a script cannot add to it or replace a hook for the scripts after it
    static const char kFreeze[] = "(function f(o){for(const k of Reflect.ownKeys(o)){const v=o[k];if(typeof v==='object'&&v)f(v);}Object.freeze(o);})";
    JSValue freeze = JS_Eval(ctx, kFreeze, sizeof(kFreeze) - 1, "<lucyn>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(freeze)) { JS_FreeValue(ctx, JS_GetException(ctx)); JS_FreeValue(ctx, root); return false; }
    JSValue r = JS_Call(ctx, freeze, JS_UNDEFINED, 1, &root);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, freeze);
    lucynObj = root;
    return true;
}

// ------------------------------------------------------------------------------------------------ teardown

void State::releaseAll() {
    live->on = false;                                        // the Document may still carry our listener; it is inert from now on
    if (ctx) {
        for (auto& t : timers) { JS_FreeValue(ctx, t.fn); for (JSValue a : t.args) JS_FreeValue(ctx, a); }
        timers.clear();
        for (auto& [k, v] : wrappers) JS_FreeValue(ctx, v);
        wrappers.clear();
        for (auto& [k, v] : tagProtoCache) JS_FreeValue(ctx, v);
        tagProtoCache.clear();
        for (JSValue* v : {&protoNode, &protoElement, &protoHtmlElement, &protoText, &protoComment, &protoDocument, &tagProtos,
                           &mutationSink, &lifecycleFn, &domExc, &lucynObj, &jsonStringify}) {
            JS_FreeValue(ctx, *v);
            *v = JS_UNDEFINED;
        }
        teardownFetch(ctx);     // free in-flight request callbacks (fetch promises, XHR objects)
        teardownStorage(ctx);   // flush localStorage / sessionStorage before the context dies
        teardownWs(ctx);        // close WebSocket connections and stop I/O thread
        JS_FreeContext(ctx);
        ctx = nullptr;
    }
    if (rt) { JS_FreeRuntime(rt); rt = nullptr; }
    doc = nullptr;
    hooks = nullptr;
}

}  // namespace detail

// ================================================================================================ QuickJsHost

using namespace detail;

QuickJsHost::QuickJsHost(HostOptions opts) : opts_(std::move(opts)) {}
QuickJsHost::~QuickJsHost() { detach(); }

bool QuickJsHost::attach(dom::Document& shell, const std::string& pageUrl, shell::NativeHooks& hooks, std::string* err) {
    detach();
    errors_.clear();
    notes_.clear();
    auto s = std::make_unique<State>();
    s->opts = opts_;
    s->doc = &shell;
    s->hooks = &hooks;
    s->pageUrl = pageUrl;
    s->errors = &errors_;
    s->notes = &notes_;
    auto fail = [&](const std::string& m) { if (err) *err = m; s->releaseAll(); return false; };

    s->rt = JS_NewRuntime();
    if (!s->rt) return fail("cannot create the JS runtime");
    if (opts_.memoryLimitBytes) JS_SetMemoryLimit(s->rt, opts_.memoryLimitBytes);
    if (opts_.stackLimitBytes) JS_SetMaxStackSize(s->rt, opts_.stackLimitBytes);
    JS_SetInterruptHandler(s->rt, interruptCb, s.get());
    s->ctx = JS_NewContext(s->rt);
    if (!s->ctx) return fail("cannot create the JS context");
    JS_SetContextOpaque(s->ctx, s.get());

    // Our own JSON.stringify, before any page code can replace it (used to turn object arguments into hook strings).
    {
        JSValue g = JS_GetGlobalObject(s->ctx);
        JSValue json = JS_GetPropertyStr(s->ctx, g, "JSON");
        s->jsonStringify = JS_GetPropertyStr(s->ctx, json, "stringify");
        JS_FreeValue(s->ctx, json);
        JS_FreeValue(s->ctx, g);
    }

    // The shell's own scripts (custom shell.html may have inline ones): never "the page's".
    for (dom::Node* n : shell.getElementsByTagName("script")) s->shellScripts.insert(n);

    if (!installDom(*s)) return fail("DOM bridge failed to initialise");
    if (!installWindow(*s)) { std::string e = s->takeException(); return fail("window bridge failed to initialise: " + e); }
    if (!s->buildLucyn()) return fail("could not build the lucyn object");

    std::shared_ptr<Live> live = s->live;
    State* sp = s.get();
    s->listener = shell.onMutation([live, sp](const dom::Document::Mutation& m) { if (live->on) sp->onMutation(m); });
    p_ = std::move(s);
    return true;
}

shell::ScriptOutcome QuickJsHost::runUserScript(const shell::UserScript& us) {
    if (!p_) return {false, "JS host is not attached"};
    State& s = *p_;
    // `lucyn` is the one parameter of the function the script lives in: a closure, not a global. The wrapper opens and closes on
    // the script's own first/last line, so error line numbers stay the author's.
    std::string wrapped = "(function(lucyn){" + us.source + "\n})";
    std::string file = us.name.empty() ? "user-script" : us.name;
    Budget b(s, s.opts.scriptTimeoutMs);
    JSValue fn = JS_Eval(s.ctx, wrapped.c_str(), wrapped.size(), file.c_str(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) return {false, s.takeException()};
    JSValue g = JS_GetGlobalObject(s.ctx);
    JSValue r = JS_Call(s.ctx, fn, g, 1, &s.lucynObj);
    JS_FreeValue(s.ctx, g);
    JS_FreeValue(s.ctx, fn);
    shell::ScriptOutcome out;
    if (JS_IsException(r)) { out.ok = false; out.error = s.takeException(); }
    JS_FreeValue(s.ctx, r);
    s.drainJobs();
    return out;
}

shell::ScriptOutcome QuickJsHost::runPageScripts() {
    if (!p_) return {false, "JS host is not attached"};
    State& s = *p_;
    s.drainJobs();                                           // observers set up by start scripts see the freshly mounted site
    s.fireLifecycle("parsed");                               // wire inline on* attributes before any page script fires
    int failed = 0;
    std::string first;
    for (dom::Node* n : s.doc->getElementsByTagName("script")) {
        if (s.shellScripts.count(n) || s.startedScripts.count(n)) continue;
        std::string e;
        if (!s.runScriptNode(n, &e)) { if (!failed) first = e; ++failed; }
    }
    s.readyState = "interactive";
    s.fireLifecycle("interactive");                          // readystatechange + DOMContentLoaded
    s.drainJobs();
    if (failed) return {false, std::to_string(failed) + " page script(s) failed; first: " + first};
    return {};
}

bool QuickJsHost::settle(int timeoutMs) {
    if (!p_) return true;
    State& s = *p_;
    const double inf = std::numeric_limits<double>::infinity();
    const double endAt = s.nowMs() + std::max(0, timeoutMs);
    Budget outer(s, timeoutMs);
    if (!s.loadFired) { s.loadFired = true; s.readyState = "complete"; s.fireLifecycle("complete"); }   // readystatechange + load
    for (;;) {
        s.drainJobs();
        bool ranOne = s.runOneDueTimer(s.nowMs());
        // One-shot timers keep the page "busy"; intervals and animation frames run when due but never block idleness.
        // A one-shot due far past the deadline (a 60 s keep-alive) cannot fire in this window: the page counts as idle.
        // One due within 250 ms still counts as pending even past the deadline: that is what a tight setTimeout chain
        // looks like at the moment the window closes, and such a page is not idle.
        double nextBlock = inf, nextAny = inf;
        for (auto& t : s.timers) { nextAny = std::min(nextAny, t.due); if (t.kind == 0) nextBlock = std::min(nextBlock, t.due); }
        const bool quiet = nextBlock == inf || nextBlock > std::max(endAt, s.nowMs() + 250.0);
        if (ranOne) { if (s.nowMs() >= endAt) return quiet; continue; }
        if (quiet) return true;
        double t = s.nowMs();
        if (t >= endAt) return false;
        double wake = std::min(nextAny, endAt);
        if (wake > t) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(wake - t));
    }
}

void QuickJsHost::detach() {
    if (!p_) return;
    p_->releaseAll();       // never touches the Document or the hooks: either may be gone already
    p_.reset();
}

bool QuickJsHost::eval(const std::string& code, std::string* result, std::string* error) {
    if (!p_) { if (error) *error = "JS host is not attached"; return false; }
    State& s = *p_;
    Budget b(s, s.opts.scriptTimeoutMs);
    JSValue r = JS_Eval(s.ctx, code.c_str(), code.size(), "<eval>", JS_EVAL_TYPE_GLOBAL);
    bool ok = !JS_IsException(r);
    if (!ok) {
        std::string e = s.takeException();
        if (error) *error = e;
        JS_FreeValue(s.ctx, r);
        s.drainJobs();
        return false;
    }
    // Convert result to string.
    if (result) {
        JSValue str = JS_ToString(s.ctx, r);
        if (JS_IsException(str)) {
            *result = "";
            JS_FreeValue(s.ctx, JS_GetException(s.ctx));
        } else {
            size_t len = 0;
            const char* p = JS_ToCStringLen2(s.ctx, &len, str, false);
            if (p) { result->assign(p, len); JS_FreeCString(s.ctx, p); }
            else    { *result = ""; }
            JS_FreeValue(s.ctx, str);
        }
    }
    JS_FreeValue(s.ctx, r);
    s.drainJobs();
    return true;
}

void QuickJsHost::replaceHooks(shell::NativeHooks& newHooks) {
    if (p_) p_->hooks = &newHooks;
}

}  // namespace lucyn::js
