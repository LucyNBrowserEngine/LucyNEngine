// lucyn/cef_host/browser_host.h — the CEF engine host (Session 19, steps 2-4).
//
// This is the only new module of the CEF migration. It wraps Chromium Embedded
// Framework behind a small, CEF-free public surface so the controller never has
// to include a CEF header. CEF types live behind the pimpl in the .cpp.
//
// Threading model (see engine-history/SESSION19_CEF_MIGRATION.md "Thread model inside cef_host"):
//   * CEF runs its own browser-process UI thread (multi_threaded_message_loop).
//   * CEF callbacks (OnLoadEnd, OnTitleChange, OnPaint, ...) fire on that UI
//     thread. They never call your code directly; they post a thunk onto a
//     mutex-guarded queue.
//   * Your thread calls BrowserInstance::pump() between commands to drain that
//     queue and run the stored callbacks. Nothing here blocks.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lucyn::cef_host {

// Viewport + per-instance preferences. Defaults suit a headless scraper.
struct BrowserOptions {
    int viewportWidth  = 1280;
    int viewportHeight = 800;
    std::string userAgent;  // empty = CEF default
    bool javascript  = true;
    bool images      = true;
    bool followRedirects = true;
};

// One scraping instance. Create up to ~4 of these per process.
// Not thread-safe for concurrent method calls from multiple of YOUR threads —
// drive it from one thread (the controller thread) and let CEF's own threads
// feed it through the callback queue.
class BrowserInstance {
public:
    // Fired by pump() once the main frame finishes loading (ok=true, with the
    // HTTP status and the page title) or fails (ok=false, with an error text).
    using LoadCallback =
        std::function<void(bool ok, int statusCode, const std::string& title, const std::string& error)>;

    ~BrowserInstance();
    BrowserInstance(BrowserInstance&&) noexcept;
    BrowserInstance& operator=(BrowserInstance&&) noexcept;
    BrowserInstance(const BrowserInstance&) = delete;
    BrowserInstance& operator=(const BrowserInstance&) = delete;

    // Begin loading `url`. The optional callback fires once for the main frame
    // (delivered on the thread that calls pump()).
    void visit(const std::string& url, LoadCallback cb = nullptr);

    // ---- Async page operations (Session 19 steps 5-6) --------------------
    // All of these are asynchronous: they queue work into CEF and their callback
    // is delivered on the thread that calls pump(). `value` for eval/click is a
    // display string (JavaScript ToString semantics).
    using ResultCallback = std::function<void(bool ok, const std::string& value, const std::string& error)>;
    using ListCallback   = std::function<void(bool ok, const std::vector<std::string>& values, const std::string& error)>;
    using FoundCallback  = std::function<void(bool found, const std::string& value)>;

    // Full serialized source of the main frame (the document as HTML).
    void html(std::function<void(bool ok, const std::string& value, const std::string& error)> cb);

    // Evaluate `js` in the page and return String(result).
    void eval(const std::string& js, ResultCallback cb);

    // innerText of the first element matching `selector`, or found=false when it
    // matches nothing.
    void extract(const std::string& selector, FoundCallback cb);

    // innerText of every element matching `selector`, in document order.
    void extractAll(const std::string& selector, ListCallback cb);

    // Fire a real click (mousedown, mouseup, click) on the first match; value is
    // the element's tagName. ok=false + error when the selector matches nothing.
    void click(const std::string& selector, ResultCallback cb);

    // ---- Offscreen screenshot (Session 19 step 7) -------------------------
    // Capture the current painted frame as a PNG at `path` (BGRA -> RGBA, stb
    // encode). `value` is the path on success. Drives Chromium's offscreen
    // renderer — no window ever exists. Forces one fresh paint so the frame
    // reflects the fully loaded page. Viewport = the instance's BrowserOptions.
    void screenshot(const std::string& path, ResultCallback cb);

    // ---- Raw body fetch (Session 19 step 9, the `download` command) -----------
    // Fetch `url` and hand back the raw response bytes WITHOUT navigating the main
    // page. Driven by CefURLRequest on CEF's network stack — the same Chromium jar
    // and identity the page uses — so cookies set by a visit are sent. `maxBytes`
    // caps the retained body (<= 0 = unlimited; the rest is discarded, but `bytes`
    // still reports the true size). Delivered on the thread that calls pump().
    struct FetchResult {
        bool ok = false;              // 2xx and no transport error
        long status = 0;              // HTTP status (0 on a transport failure)
        std::string finalUrl;         // after redirects
        std::string contentType;
        std::string contentDisposition;
        long long bytes = 0;          // total bytes received
        std::string body;             // up to maxBytes of the response
        bool truncated = false;       // true when the body was cut at maxBytes
        std::string error;            // empty on success
    };
    using FetchCallback = std::function<void(const FetchResult&)>;
    void fetchBody(const std::string& url, long long maxBytes, const std::string& referer, FetchCallback cb);

    // ---- Network capture feed (Session 19 step 8) -----------------------------
    // Every resource CEF requests and loads is reported here so the controller can
    // feed Moonbug (the wiretap + dashboard). Delivered DIRECTLY on CEF's IO
    // thread — not via pump() — so a capture lands even while your loop is blocked
    // in a wait (waitIdle needs that). `requestId` correlates a Start with its
    // Finish. Set once after createInstance(); pass nullptr to stop.
    struct ResourceEvent {
        // Start/Finish come from Chromium's network stack (one pair per resource).
        // Body is extra: Chromium does not hand the response body of a page load to
        // the browser process, so cef_host taps what the PAGE reads (fetch/XHR) in
        // the renderer and forwards it as its own event — the only way to show an
        // API response's payload in Moonbug without a debug port.
        enum Phase { Start = 0, Finish = 1, Body = 2 };
        int phase = Start;
        unsigned long long requestId = 0;
        std::string url;       // Finish: final URL after redirects
        std::string method;
        // Finish only:
        long status = 0;                     // HTTP status (0 on transport failure)
        long long bytes = 0;                 // received content length
        int redirects = 0;
        std::string httpVersion;
        std::string error;                   // empty on success
        std::vector<std::pair<std::string, std::string>> requestHeaders;
        std::vector<std::pair<std::string, std::string>> responseHeaders;
        // Body phase only:
        std::string body;           // the response bytes, text/code resources only (capped, see bodyTruncated)
        bool bodyTruncated = false; // true when the text was cut at the engine's cap
        std::string contentType;    // response Content-Type (the controller's asset classification; may be empty)
    };
    using ResourceHandler = std::function<void(const ResourceEvent&)>;
    void setResourceHandler(ResourceHandler h);

    // Engine messages meant for a human ("downloaded <file> into ..."). Called
    // DIRECTLY on the CEF callback thread that produced them — keep the handler
    // quick and never block. nullptr (the default) drops them.
    using NoteHandler = std::function<void(const std::string&)>;
    void setNoteHandler(NoteHandler h);

    // ---- Cookies (Session 19 step 9) ------------------------------------------
    // Backed by Chromium's cookie store — the jar a CEF page actually uses (the
    // old libcurl HttpClient jar is separate and unused once CEF is driving).
    struct Cookie {
        std::string name, value, domain, path = "/";
        bool secure = false, httpOnly = false, hostOnly = true;
        long long expires = 0;  // unix seconds; 0 = session cookie
    };
    using CookieListCallback =
        std::function<void(bool ok, const std::vector<Cookie>& cookies, const std::string& error)>;

    // Cookies in the store, optionally filtered to those applicable to `url`
    // (empty `url` = every cookie; non-empty = like the old cookiesFor(url)).
    void allCookies(const std::string& url, CookieListCallback cb);
    // Set one cookie scoped to `url`. ResultCallback::value is "ok" on success.
    void setCookie(const std::string& url, const Cookie& c, ResultCallback cb);
    // Delete every cookie. ResultCallback::value is the number deleted (as text).
    void clearCookies(ResultCallback cb);

    // Drain pending callbacks queued by CEF threads and run them. Call from your loop.
    void pump();

    bool isLoading() const;
    std::string currentUrl() const;
    int lastStatusCode() const;   // HTTP status of the last completed main-frame load
    std::string lastTitle() const;

private:
    friend std::unique_ptr<BrowserInstance> createInstance(const BrowserOptions&);
    explicit BrowserInstance(const BrowserOptions& opts);

    // Core eval round-trip: send `code` to the renderer, fire `cb` with the
    // String(result) (or an error) via pump(). extract/extractAll/click build a
    // JS wrapper around this; eval() calls it directly.
    void evalRaw(const std::string& code, ResultCallback cb);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Call this as the VERY FIRST statement of main(), before any logging, file I/O,
// or initCef() (SESSION19 "Process model"). CEF re-execs this binary as
// gpu/renderer/utility subprocesses; they run main() again and must exit here.
// Returns >= 0 if this process is a CEF subprocess — exit immediately with that
// value. Returns -1 if this is the browser process; continue to initCef().
int executeCefProcess(int argc, char** argv);

// Call once at process start, AFTER the executeCefProcess() subprocess check in
// main(). Returns false if CEF could not be initialised (do not continue).
bool initCef(int argc, char** argv);

// Call once at process end. Safe to call even if initCef() failed or was never
// called. Do not call any other CEF function afterwards.
void shutdownCef();

// True between a successful initCef() and shutdownCef().
bool isCefInitialized();

// Headful mode: show the page in a real window while keeping the off-screen
// surface (so screenshots and every other command still work). Default OFF —
// libraries, tests and `lucyn --headless` never open one. Call BEFORE initCef().
// The window itself is created with the first browser; if the machine has no
// display it silently stays headless.
void setPreviewWindow(bool on);
bool previewWindowEnabled();

// Listen mode (lucyn --listen): inject a small "listen bar" overlay — back
// button + URL field — into every page, so a human can drive the window by
// hand (the viewer window itself has no chrome). Call BEFORE initCef().
void setListenBar(bool on);
bool listenBarEnabled();

// CEF version string, e.g. "156.0.7+g47dbc1d+chromium-156.0.8078.17".
std::string cefVersion();

// Create a browser instance. Returns nullptr if CEF is not initialised.
std::unique_ptr<BrowserInstance> createInstance(const BrowserOptions& opts = {});

}  // namespace lucyn::cef_host
