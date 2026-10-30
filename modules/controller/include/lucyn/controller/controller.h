// lucyn/controller/controller.h - JSON command loop that drives the engine over stdio.
//
// The controller is the "front door" described in M4. It reads newline-delimited JSON
// commands from stdin, executes them, and writes newline-delimited JSON replies to stdout.
//
// Commands:
//   {"v":1, "id":N, "cmd":"visit",   "url":"https://example.com"}
//   {"v":1, "id":N, "cmd":"search",  "query":"LucyN browser", "engine":"brave"}
//   {"v":1, "id":N, "cmd":"html"}
//   {"v":1, "id":N, "cmd":"extract", "selector":"title"}
//   {"v":1, "id":N, "cmd":"extractAll", "selector":".price"}
//   {"v":1, "id":N, "cmd":"eval",    "js":"document.title"}
//   {"v":1, "id":N, "cmd":"info"}                          // last URL + capture count
//   {"v":1, "id":N, "cmd":"download", "url":"https://host/file.pdf"}   // save into nothing-data/downloads/
//   {"v":1, "id":N, "cmd":"download", "url":"...", "name":"my.pdf", "overwrite":false, "maxBytes":0, "referer":"https://host/"}
//        -> {"id":N,"ok":true,"path":"<abs path>","name":"my.pdf","bytes":123,"status":200,"type":"application/pdf","url":"<final url>"}
//        Streams to a hidden .part file and renames on success. Non-2xx / error / over maxBytes = ok:false and NO file left.
//        File name: "name", else Content-Disposition, else the URL's last segment, else download.bin; always one safe
//        segment inside downloads/. An existing name becomes "name (1).ext" unless overwrite is true. http(s) only.
//   {"v":1, "id":N, "cmd":"screenshot"}                                // render the current page offscreen (no window ever opens)
//   {"v":1, "id":N, "cmd":"screenshot", "name":"page.png", "width":1024}
//        -> {"id":N,"ok":true,"path":"<abs path in nothing-data/screenshots/>"}   layout+SoftwarePainter, PNG out.
//        Only present when built with LUCYN_MODULE_LAYOUT + LUCYN_MODULE_PAINT (both on by default).
//   {"v":1, "id":N, "cmd":"quit"}
//
// Replies:
//   {"id":N, "ok":true}                              // generic success
//   {"id":N, "ok":true, "value":"..."}               // single string result
//   {"id":N, "ok":true, "values":["a","b"]}          // array result (extractAll)
//   {"id":N, "ok":false, "error":"..."}              // error
//
//   {"v":1, "id":N, "cmd":"moonbug", "on":true}                       // stream "request" events on stdout
//   {"v":1, "id":N, "cmd":"moonbug", "raw":2005, "signed":2006, "dashboard":6969}   // start listeners (0 = any free port)
//   {"v":1, "id":N, "cmd":"captures", "limit":20}                     // recent Moonbug captures
//
// Events (unprompted):
//   {"event":"pageError", "message":"..."}
//   {"event":"note",      "message":"..."}
//   {"event":"moonbug",   "raw":2005, "signed":2006, "dashboard":6969}   // listeners are up (0 = that one is off)
//   {"event":"request",   "id":N, "method":"GET", "url":"...", "status":200, "ms":123.4, "bytes":5678,
//                         "error":"", "sig":"<NULLBYTE HMAC-SHA256 of that capture>"}   // only after "moonbug" on:true
//
// The controller dispatches one command at a time, reading lines synchronously. There is
// no window and no event pump: stdin/stdout are the entire interface.
#pragma once
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>

namespace lucyn::controller {

struct ControllerOptions {
    // JS settle timeout per visit, ms. 0 = 5000.
    int settleTimeoutMs = 5000;
    // Request filtering is on by default. Add a local EasyList/uBO-format file
    // on top of the small built-in starter rules with this path.
    bool adblockEnabled = true;
    std::string adblockFilterListPath;
    // Page console output. Empty: silent.
    std::function<void(const std::string& level, const std::string& line)> onConsole;
    // Line-level event emitter (for notes, page errors, future Moonbug events).
    // Called with a complete JSON line, ready to write to stdout.
    std::function<void(const std::string& jsonLine)> onEvent;
    // When this returns true, unprompted event lines are NOT written to the
    // output stream (onEvent still sees them). Listen mode uses it so a human's
    // terminal gets logger-formatted captures instead of raw JSON, while an
    // attached script — which makes this return false — still receives them.
    std::function<bool()> suppressEvents;
    // ---- Moonbug (M5) ----------------------------------------------------------------------------
    // Ports for the capture listeners (all bind 127.0.0.1 only). -1 = off, 0 = any free port.
    int moonbugRawPort = -1;      // raw JSON capture stream           (README: 2005)
    int nullbytePort   = -1;      // NULLBYTE-signed capture stream    (README: 2006)
    int dashboardPort  = -1;      // capture dashboard + its API       (README: 6969)
    std::string dashboardFile;    // serve this HTML instead of the embedded dashboard
    bool captureEvents = false;   // write a {"event":"request"...} line per capture to the output stream
    // Use a throwaway NULLBYTE key instead of <nothing-data>/security/nullbyte.key. For tests.
    bool ephemeralNullbyte = false;
    // Persist the cookie jar across runs (nothing-data/cookies/). Currently unwired: CEF owns the jar (see HANDOFF).
    bool persistCookies = false;
    // Search defaults from `engine.config.search`; SearXNG needs a user-selected instance.
    std::string searchDefault = "brave";
    std::string searxngUrl;
    // Headful launch page. When the engine starts a window (lucyn without
    // --headless) this URL is loaded as soon as Chromium is up, so the window
    // shows something before the first `visit` arrives — and it is the SAME
    // browser, so the next visit simply navigates the window there.
    std::string startUrl;

    // Custom in/out streams for testing. Null = use stdin/stdout.
    std::istream* in  = nullptr;
    std::ostream* out = nullptr;
};

class Controller {
public:
    explicit Controller(ControllerOptions opts = {});
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    // Run until "quit" is received or the input stream ends. Returns 0 on clean quit, 1 on error.
    int run();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace lucyn::controller
