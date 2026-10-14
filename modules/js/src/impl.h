// Private to lucyn::js. One State per attach(): a runtime, a context, and everything that points into them.
#pragma once
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <quickjs.h>
#include <lucyn/js/quickjs_host.h>

namespace lucyn::js::detail {

// Shared with the DOM mutation listener. detach() flips it, so a listener that outlives the host (the Document
// can outlive us) turns into a no-op instead of calling into a dead context.
struct Live { bool on = true; };

struct Timer {
    int      id = 0;
    double   due = 0;            // ms since epoch
    double   interval = -1;      // < 0: one-shot
    int      kind = 0;           // 0 setTimeout, 1 setInterval, 2 requestAnimationFrame
    uint64_t seq = 0;
    JSValue  fn = JS_UNDEFINED;
    std::vector<JSValue> args;
};

struct MutRec { int kind; dom::Node* target; dom::Node* node; std::string name; };   // kind: 0 inserted 1 removed 2 attr 3 text

struct State {
    HostOptions            opts;
    JSRuntime*             rt = nullptr;
    JSContext*             ctx = nullptr;
    dom::Document*         doc = nullptr;           // never touched by detach() (it may already be gone)
    shell::NativeHooks*    hooks = nullptr;         // ditto: the Hydrator removes them before detach()
    std::string            pageUrl;
    std::shared_ptr<Live>  live = std::make_shared<Live>();
    dom::Document::ListenerId listener = 0;

    // errors/notes live in the host (they outlive the State)
    std::vector<std::string>* errors = nullptr;
    std::vector<std::string>* notes = nullptr;

    JSClassID nodeClass = 0;
    std::unordered_map<const dom::Node*, JSValue> wrappers;       // el === el
    std::unordered_map<std::string, JSValue>      tagProtoCache;
    JSValue protoNode = JS_UNDEFINED, protoElement = JS_UNDEFINED, protoHtmlElement = JS_UNDEFINED,
            protoText = JS_UNDEFINED, protoComment = JS_UNDEFINED, protoDocument = JS_UNDEFINED,
            tagProtos = JS_UNDEFINED, mutationSink = JS_UNDEFINED, lifecycleFn = JS_UNDEFINED,
            domExc = JS_UNDEFINED, lucynObj = JS_UNDEFINED, jsonStringify = JS_UNDEFINED;

    // <script> handling
    std::unordered_set<const dom::Node*> shellScripts;     // the shell's own: never "the page's"
    std::unordered_set<const dom::Node*> startedScripts;
    std::unordered_set<const dom::Node*> createdScripts;   // made by document.createElement('script') and not run yet
    std::unordered_set<const dom::Node*> queuedScripts;
    std::vector<dom::Node*>              pendingScripts;
    bool                                 scriptJobQueued = false;
    const dom::Node*                     currentScript = nullptr;

    // MutationObserver
    bool                 observing = false;
    std::vector<MutRec>  records;
    bool                 mutJobQueued = false;

    // lifecycle + event loop
    std::string readyState = "loading";
    bool        loadFired = false;
    int         drainDepth = 0;
    std::vector<Timer> timers;
    int         nextTimerId = 1;
    uint64_t    seq = 0;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();

    // watchdog (interrupt handler reads these)
    bool        armed = false;
    bool        timedOut = false;
    int         budgetMs = 0;
    std::chrono::steady_clock::time_point deadline{};

    // ---- host.cpp ----
    double      nowMs() const;
    void        addError(const std::string& e);
    void        addNote(const std::string& n);
    std::string takeException();                       // clears the pending exception, returns readable text
    void        drainJobs();
    bool        runOneDueTimer(double t);
    void        fireLifecycle(const char* stage);
    void        flushMutations();
    void        onMutation(const dom::Document::Mutation& m);
    void        runPendingScripts();
    bool        runScriptNode(dom::Node* script, std::string* err = nullptr);   // false = it threw (also recorded)
    bool        evalGlobal(const std::string& code, const std::string& file, std::string* err);
    bool        buildLucyn();
    void        releaseAll();
};

// RAII time limit. Nested budgets can only shorten the deadline.
struct Budget {
    State& s; bool prevArmed; bool prevTimedOut; int prevMs; std::chrono::steady_clock::time_point prevDl;
    Budget(State& st, int ms);
    ~Budget();
};

// ---- dom_bridge.cpp ----
bool    installDom(State& s);
JSValue wrapNode(State& s, dom::Node* n);              // new reference; JS_NULL for nullptr
JSValue makeCtor(State& s, const char* name, JSValueConst proto);
bool    isConnected(const State& s, const dom::Node* n);

// ---- window.cpp ----
bool installWindow(State& s);                          // timers, console, the private `__n` object, prelude
int  addTimer(State& s, int kind, JSValueConst fn, double delayMs, const std::vector<JSValue>& args);

// ---- shared helpers ----
inline State* S(JSContext* c) { return static_cast<State*>(JS_GetContextOpaque(c)); }
bool        toStr(JSContext* c, JSValueConst v, std::string& out);      // false: a JS exception is pending
std::string toStdOrEmpty(JSContext* c, JSValueConst v);                  // clears any exception it causes
inline JSValue jstr(JSContext* c, const std::string& s) { return JS_NewStringLen(c, s.data(), s.size()); }
JSValue     throwDom(JSContext* c, const char* name, const std::string& msg);
std::string lowerAscii(std::string s);
std::string upperAscii(std::string s);

// ---- fetch_bridge.cpp ----
bool installFetch(State& s, JSValueConst n_obj);      // adds the n.http*/n.utf8*/n.cookie* natives that fetch + XHR + document.cookie use; call from installWindow
void pumpFetch(State& s);                              // deliver finished requests to their JS callbacks; call from drainJobs
bool fetchAndRunScript(State& s, const std::string& absUrl, std::string* err); // download+eval a <script src>
void teardownFetch(JSContext* ctx);                    // free pending promises; call from releaseAll

// ---- storage_bridge.cpp ----
bool installStorage(State& s, JSValueConst n_obj);   // adds n.storage* + n.saveLoadedFile natives; call from installWindow
void teardownStorage(JSContext* ctx);                 // flush to disk; call from releaseAll

// ---- websocket_bridge.cpp ----
bool installWebSocket(State& s, JSValueConst n_obj);  // adds n.wsConnect/wsSend/wsClose/wsSetCallbacks; call from installWindow
void pumpWs(State& s);                                // deliver WebSocket events to JS callbacks; call from drainJobs
void teardownWs(JSContext* ctx);                      // close all sockets and stop the I/O thread; call from releaseAll

// ---- prelude.cpp ----
std::string preludeSource();

}  // namespace lucyn::js::detail
