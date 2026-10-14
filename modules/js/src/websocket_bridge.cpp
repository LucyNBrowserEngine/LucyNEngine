// websocket_bridge.cpp - the native half of the WebSocket API.
//
// The JS-facing WebSocket class lives in the prelude (p5_websocket.js). It reaches the network
// through these natives on the private `__n` object:
//
//   n.wsConnect(url, protocols) -> numeric id   (0 = not available / immediate error)
//   n.wsSend(id, data, isBinary)
//   n.wsClose(id, code, reason)
//
// Each context gets one WsState, which owns a Mongoose manager on a background thread and a
// pair of lock-protected queues:
//
//   cmds  – commands from the JS thread to the Mongoose thread (SEND, CLOSE)
//   evts  – events from the Mongoose thread to the JS thread (open, message, close, error)
//
// pumpWs() is called from drainJobs() and delivers pending events to the JS callbacks registered
// by the WebSocket JS class via n.__wsCb (a JS array of [id, onopen, onmsg, onclose, onerror]).
//
// Threading rule: the Mongoose thread NEVER touches JSContext/JSRuntime. All JS work happens
// on the JS thread inside pumpWs().

#include "impl.h"
#include <mongoose.h>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define ARGS JSContext* c, [[maybe_unused]] JSValueConst t, [[maybe_unused]] int argc, [[maybe_unused]] JSValueConst* argv

namespace lucyn::js::detail {
namespace {

// ---- event / command types -------------------------------------------------------------------

enum class WsEvtType  { Open, Message, Close, Error };
enum class WsCmdType  { Send, Close };

struct WsEvt {
    int        id;
    WsEvtType  type;
    std::string data;
    bool       binary = false;
    int        code   = 1000;
    std::string reason;
};

struct WsCmd {
    int        id;
    WsCmdType  type;
    std::string data;
    bool       binary = false;
    int        code   = 1000;
    std::string reason;
};

// ---- per-connection state on the Mongoose thread ---------------------------------------------

struct WsConn {
    int            id;
    mg_connection* c   = nullptr;       // null until MG_EV_WS_OPEN
    bool           open = false;
    std::string    url;
    std::string    protocols;
};

// ---- per-JSContext state ---------------------------------------------------------------------

struct WsState {
    // owned by the JS thread (read by Mongoose thread — immutable after init):
    std::thread         thread;

    // shared queues (both threads):
    std::mutex          mu;
    std::queue<WsEvt>   evts;           // Mongoose → JS
    std::queue<WsCmd>   cmds;           // JS → Mongoose
    bool                dead   = false;

    // Mongoose thread only:
    mg_mgr              mgr    = {};
    bool                mgrOk  = false;
    unsigned long       wakeId = 0;     // connection id used with mg_wakeup
    std::unordered_map<int, WsConn> conns;
    int                 nextId = 1;

    // JS thread only: callbacks (JSValue, owned by the JS thread)
    // Map: id -> [onopen, onmessage, onclose, onerror] (each is a JS function or UNDEFINED)
    std::unordered_map<int, std::array<JSValue,4>> cbs;

    // Signal to Mongoose thread that there are new cmds (or dead=true)
    void wakeMongoose() {
        if (mgrOk && wakeId) mg_wakeup(&mgr, wakeId, "x", 1);
    }
};

std::unordered_map<JSContext*, WsState*> g_ws;

// Helper: push event and (optionally) wake the Mongoose thread won't need it — events go JS-ward.
// The Mongoose thread calls this on itself.
void pushEvt(WsState& ws, WsEvt e) {
    std::lock_guard<std::mutex> lk(ws.mu);
    ws.evts.push(std::move(e));
}

// ---- Mongoose event handler (Mongoose thread) ------------------------------------------------

static void wsHandler(mg_connection* c, int ev, void* evData) {
    auto* ws = static_cast<WsState*>(c->fn_data);

    if (ev == MG_EV_WAKEUP) {
        // Process pending commands.
        std::vector<WsCmd> work;
        {
            std::lock_guard<std::mutex> lk(ws->mu);
            while (!ws->cmds.empty()) { work.push_back(std::move(ws->cmds.front())); ws->cmds.pop(); }
        }
        for (auto& cmd : work) {
            auto it = ws->conns.find(cmd.id);
            if (it == ws->conns.end() || !it->second.open) continue;
            mg_connection* cc = it->second.c;
            if (!cc) continue;
            if (cmd.type == WsCmdType::Send) {
                int op = cmd.binary ? WEBSOCKET_OP_BINARY : WEBSOCKET_OP_TEXT;
                mg_ws_send(cc, cmd.data.data(), cmd.data.size(), op);
            } else if (cmd.type == WsCmdType::Close) {
                // Send a CLOSE frame then let mongoose handle the rest.
                uint8_t payload[2] = { uint8_t(cmd.code >> 8), uint8_t(cmd.code & 0xff) };
                mg_ws_send(cc, payload, 2, WEBSOCKET_OP_CLOSE);
                cc->is_draining = 1;
            }
        }
        return;
    }

    // Find which WsConn owns this mg_connection.
    int connId = 0;
    for (auto& kv : ws->conns) {
        if (kv.second.c == c) { connId = kv.first; break; }
        // Before MG_EV_WS_OPEN the conn pointer isn't set yet; match by checking 'c == nullptr'.
        if (!kv.second.c && !kv.second.open) { connId = kv.first; break; }
    }
    if (!connId) return;
    WsConn& wc = ws->conns[connId];

    switch (ev) {
        case MG_EV_OPEN:
            wc.c = c;
            break;

        case MG_EV_WS_OPEN:
            wc.open = true;
            wc.c    = c;
            pushEvt(*ws, { connId, WsEvtType::Open, {}, false, 1000, {} });
            break;

        case MG_EV_WS_MSG: {
            auto* wm = static_cast<mg_ws_message*>(evData);
            bool bin = (wm->flags & WEBSOCKET_OP_BINARY) != 0;
            std::string data(wm->data.buf, wm->data.len);
            pushEvt(*ws, { connId, WsEvtType::Message, std::move(data), bin, 0, {} });
            break;
        }

        case MG_EV_WS_CTL: {
            // Control frames: CLOSE
            auto* wm = static_cast<mg_ws_message*>(evData);
            if ((wm->flags & 0x0f) == WEBSOCKET_OP_CLOSE) {
                int code = 1000;
                std::string reason;
                if (wm->data.len >= 2) {
                    code = (static_cast<unsigned char>(wm->data.buf[0]) << 8) |
                            static_cast<unsigned char>(wm->data.buf[1]);
                    if (wm->data.len > 2) reason.assign(wm->data.buf + 2, wm->data.len - 2);
                }
                pushEvt(*ws, { connId, WsEvtType::Close, {}, false, code, std::move(reason) });
                wc.open = false;
            }
            break;
        }

        case MG_EV_ERROR: {
            const char* msg = static_cast<const char*>(evData);
            std::string emsg = msg ? std::string(msg) : "WebSocket error";
            pushEvt(*ws, { connId, WsEvtType::Error, std::move(emsg), false, 0, {} });
            if (!wc.open) {
                // Never opened — synthetic close so JS state machine ends.
                pushEvt(*ws, { connId, WsEvtType::Close, {}, false, 1006, "Abnormal closure" });
            }
            wc.open = false;
            break;
        }

        case MG_EV_CLOSE:
            if (wc.open) {
                wc.open = false;
                pushEvt(*ws, { connId, WsEvtType::Close, {}, false, 1000, {} });
            }
            wc.c = nullptr;
            // Remove from map so we stop iterating it.
            ws->conns.erase(connId);
            break;

        default:
            break;
    }
}

// ---- JS natives ------------------------------------------------------------------------------

// Connect: called by the JS WebSocket constructor.
// Returns a numeric id (1-based). The connection is started asynchronously.
// Before pumpWs delivers the open event, readyState stays CONNECTING.
JSValue n_wsConnect(ARGS) {
    State* s = S(c);
    auto it = g_ws.find(c);
    if (it == g_ws.end()) return JS_NewInt32(c, 0);   // not available
    WsState* ws = it->second;

    std::string url, protocols;
    if (argc >= 1 && !toStr(c, argv[0], url)) return JS_EXCEPTION;
    if (argc >= 2) { if (!toStr(c, argv[1], protocols)) return JS_EXCEPTION; }

    // Allocate a socket id now (Mongoose thread may still be mid-poll).
    int id;
    {
        std::lock_guard<std::mutex> lk(ws->mu);
        id = ws->nextId++;
    }

    // Create the WsConn entry (Mongoose thread only reads conns in its own handler,
    // and we insert before starting the connection, so this is safe as long as we
    // insert before mg_ws_connect fires).
    WsConn wc;
    wc.id        = id;
    wc.url       = url;
    wc.protocols = protocols;
    ws->conns[id] = wc;

    // mg_ws_connect must be called from the Mongoose thread. Push a special "CONNECT" via a
    // dedicated queue, or — simpler — start the connection directly here since mg_ws_connect is
    // documented to be safe to call from any thread (it just enqueues a new connection):
    // Actually, mongoose docs say connections must be created from the same thread as mg_mgr_poll.
    // So we send a CONNECT command via the cmd queue.
    // We add a placeholder and a separate "CONNECT" command type.
    //
    // Simplification: just have the Mongoose thread start all connections too.
    // The cmd queue gets a CONNECT entry, processed before the next poll.
    {
        // We embed the connect info in a special negative-id cmd.
        WsCmd cmd;
        cmd.id     = -id;               // negative id = CONNECT
        cmd.data   = url;
        cmd.reason = protocols;
        std::lock_guard<std::mutex> lk(ws->mu);
        ws->cmds.push(std::move(cmd));
    }

    // No n.wsCb registration here — the JS class registers callbacks after getting the id.
    (void)s;
    return JS_NewInt32(c, id);
}

// Send data on an open WebSocket.
JSValue n_wsSend(ARGS) {
    if (argc < 2) return JS_UNDEFINED;
    auto it = g_ws.find(c);
    if (it == g_ws.end()) return JS_UNDEFINED;
    WsState* ws = it->second;

    int32_t id = 0;
    if (JS_ToInt32(c, &id, argv[0])) return JS_UNDEFINED;
    bool binary = argc >= 3 && JS_ToBool(c, argv[2]) > 0;

    std::string data;
    if (binary) {
        // argv[1] may be an ArrayBuffer or typed array
        size_t sz = 0;
        uint8_t* p = JS_GetArrayBuffer(c, &sz, argv[1]);
        if (p) { data.assign(reinterpret_cast<char*>(p), sz); }
        else {
            JS_FreeValue(c, JS_GetException(c));
            size_t off = 0, len = 0, per = 0;
            JSValue ab = JS_GetTypedArrayBuffer(c, argv[1], &off, &len, &per);
            if (!JS_IsException(ab)) {
                size_t asz = 0;
                uint8_t* ap = JS_GetArrayBuffer(c, &asz, ab);
                if (ap && off + len <= asz) data.assign(reinterpret_cast<char*>(ap) + off, len);
                JS_FreeValue(c, ab);
            } else JS_FreeValue(c, JS_GetException(c));
        }
    } else {
        if (!toStr(c, argv[1], data)) return JS_EXCEPTION;
    }

    WsCmd cmd;
    cmd.id     = id;
    cmd.type   = WsCmdType::Send;
    cmd.data   = std::move(data);
    cmd.binary = binary;
    {
        std::lock_guard<std::mutex> lk(ws->mu);
        ws->cmds.push(std::move(cmd));
    }
    return JS_UNDEFINED;
}

// Close a WebSocket.
JSValue n_wsClose(ARGS) {
    auto it = g_ws.find(c);
    if (it == g_ws.end()) return JS_UNDEFINED;
    WsState* ws = it->second;

    int32_t id = 0, code = 1000;
    if (argc < 1 || JS_ToInt32(c, &id, argv[0])) return JS_UNDEFINED;
    if (argc >= 2) JS_ToInt32(c, &code, argv[1]);
    std::string reason;
    if (argc >= 3 && !toStr(c, argv[2], reason)) return JS_EXCEPTION;

    WsCmd cmd;
    cmd.id     = id;
    cmd.type   = WsCmdType::Close;
    cmd.code   = code;
    cmd.reason = std::move(reason);
    {
        std::lock_guard<std::mutex> lk(ws->mu);
        ws->cmds.push(std::move(cmd));
    }
    return JS_UNDEFINED;
}

// Register callbacks: n.wsSetCallbacks(id, onopen, onmessage, onclose, onerror)
// Called by the JS WebSocket constructor right after n.wsConnect returns the id.
JSValue n_wsSetCallbacks(ARGS) {
    if (argc < 5) return JS_UNDEFINED;
    auto it = g_ws.find(c);
    if (it == g_ws.end()) return JS_UNDEFINED;
    WsState* ws = it->second;

    int32_t id = 0;
    if (JS_ToInt32(c, &id, argv[0])) return JS_UNDEFINED;

    std::array<JSValue,4> arr;
    for (int i = 0; i < 4; ++i) arr[i] = JS_DupValue(c, argv[1 + i]);

    // Free any previous callbacks for this id.
    auto cit = ws->cbs.find(id);
    if (cit != ws->cbs.end()) {
        for (auto& v : cit->second) JS_FreeValue(c, v);
    }
    ws->cbs[id] = arr;
    return JS_UNDEFINED;
}

// Remove callbacks (called on close).
JSValue n_wsForget(ARGS) {
    if (argc < 1) return JS_UNDEFINED;
    auto it = g_ws.find(c);
    if (it == g_ws.end()) return JS_UNDEFINED;
    WsState* ws = it->second;
    int32_t id = 0;
    if (JS_ToInt32(c, &id, argv[0])) return JS_UNDEFINED;
    auto cit = ws->cbs.find(id);
    if (cit != ws->cbs.end()) {
        for (auto& v : cit->second) JS_FreeValue(c, v);
        ws->cbs.erase(cit);
    }
    return JS_UNDEFINED;
}

}  // namespace

// ---- Mongoose thread (WsState implementation) -----------------------------------------------

// Real Mongoose thread: handles both CONNECT commands and regular I/O.
static void mongooseThreadFull(WsState* ws) {
    while (true) {
        // Process commands before polling.
        std::vector<WsCmd> work;
        {
            std::lock_guard<std::mutex> lk(ws->mu);
            if (ws->dead) break;
            while (!ws->cmds.empty()) { work.push_back(std::move(ws->cmds.front())); ws->cmds.pop(); }
        }

        for (auto& cmd : work) {
            if (cmd.id < 0) {
                // CONNECT command: id is negated, url in data, protocols in reason.
                int realId = -cmd.id;
                auto it = ws->conns.find(realId);
                if (it == ws->conns.end()) continue;
                WsConn& wc = it->second;
                // Build Sec-WebSocket-Protocol header if needed.
                const char* proto = wc.protocols.empty() ? nullptr : wc.protocols.c_str();
                mg_connection* mc = mg_ws_connect(&ws->mgr, cmd.data.c_str(), wsHandler, ws, proto);
                if (!mc) {
                    // Immediate failure: push error + close.
                    pushEvt(*ws, { realId, WsEvtType::Error, "WebSocket connection failed", false, 0, {} });
                    pushEvt(*ws, { realId, WsEvtType::Close, {}, false, 1006, "Abnormal closure" });
                    ws->conns.erase(realId);
                } else {
                    wc.c = mc;
                }
            } else {
                // SEND or CLOSE command.
                auto it = ws->conns.find(cmd.id);
                if (it == ws->conns.end()) continue;
                WsConn& wc = it->second;
                if (!wc.c) continue;
                if (cmd.type == WsCmdType::Send && wc.open) {
                    int op = cmd.binary ? WEBSOCKET_OP_BINARY : WEBSOCKET_OP_TEXT;
                    mg_ws_send(wc.c, cmd.data.data(), cmd.data.size(), op);
                } else if (cmd.type == WsCmdType::Close) {
                    uint8_t payload[2] = { uint8_t(cmd.code >> 8), uint8_t(cmd.code & 0xff) };
                    mg_ws_send(wc.c, payload, 2, WEBSOCKET_OP_CLOSE);
                    wc.c->is_draining = 1;
                }
            }
        }

        mg_mgr_poll(&ws->mgr, 50 /*ms*/);
    }
    mg_mgr_free(&ws->mgr);
}

// ---- public API ------------------------------------------------------------------------------

bool installWebSocket(State& s, JSValueConst n_obj) {
    JSContext* c = s.ctx;

    auto* ws = new WsState();
    g_ws[c]  = ws;

    mg_mgr_init(&ws->mgr);
    ws->mgrOk = true;

    // Start the Mongoose thread.
    ws->thread = std::thread(mongooseThreadFull, ws);

    auto def = [&](const char* name, JSCFunction* f, int len) {
        JS_DefinePropertyValueStr(c, n_obj, name,
            JS_NewCFunction2(c, f, name, len, JS_CFUNC_generic, 0), JS_PROP_C_W_E);
    };
    def("wsConnect",      n_wsConnect,      2);
    def("wsSend",         n_wsSend,         3);
    def("wsClose",        n_wsClose,        3);
    def("wsSetCallbacks", n_wsSetCallbacks, 5);
    def("wsForget",       n_wsForget,       1);
    JS_SetPropertyStr(c, n_obj, "wsAvailable", JS_TRUE);
    return true;
}

void pumpWs(State& s) {
    JSContext* c = s.ctx;
    auto it = g_ws.find(c);
    if (it == g_ws.end()) return;
    WsState* ws = it->second;

    for (;;) {
        WsEvt e;
        {
            std::lock_guard<std::mutex> lk(ws->mu);
            if (ws->evts.empty()) break;
            e = std::move(ws->evts.front());
            ws->evts.pop();
        }

        auto cit = ws->cbs.find(e.id);
        if (cit == ws->cbs.end()) continue;
        auto& cbs = cit->second;   // [onopen, onmessage, onclose, onerror]

        Budget b(s, s.opts.scriptTimeoutMs);
        JSValue fn = JS_UNDEFINED;
        JSValue arg1 = JS_UNDEFINED, arg2 = JS_UNDEFINED;

        switch (e.type) {
            case WsEvtType::Open:
                fn = cbs[0];
                break;
            case WsEvtType::Message:
                fn   = cbs[1];
                arg1 = jstr(c, e.data);
                arg2 = JS_NewBool(c, e.binary);
                break;
            case WsEvtType::Close:
                fn   = cbs[2];
                arg1 = JS_NewInt32(c, e.code);
                arg2 = jstr(c, e.reason);
                break;
            case WsEvtType::Error:
                fn   = cbs[3];
                arg1 = jstr(c, e.data);
                break;
        }

        if (!JS_IsUndefined(fn) && JS_IsFunction(c, fn)) {
            JSValue args[2] = { arg1, arg2 };
            int nargs = (!JS_IsUndefined(arg1) ? 1 : 0) + (!JS_IsUndefined(arg2) ? 1 : 0);
            JSValue r = JS_Call(c, fn, JS_UNDEFINED, nargs, args);
            if (JS_IsException(r)) s.addError(s.takeException());
            JS_FreeValue(c, r);
        }

        JS_FreeValue(c, arg1);
        JS_FreeValue(c, arg2);
    }
}

void teardownWs(JSContext* c) {
    auto it = g_ws.find(c);
    if (it == g_ws.end()) return;
    WsState* ws = it->second;

    // Signal Mongoose thread to stop and wait.
    {
        std::lock_guard<std::mutex> lk(ws->mu);
        ws->dead = true;
    }
    if (ws->thread.joinable()) ws->thread.join();

    // Free JS callbacks on the JS thread.
    for (auto& kv : ws->cbs)
        for (auto& v : kv.second) JS_FreeValue(c, v);
    ws->cbs.clear();

    delete ws;
    g_ws.erase(it);
}

}  // namespace lucyn::js::detail
