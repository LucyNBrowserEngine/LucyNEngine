// lucyn/cef_host/browser_host.cpp — CEF engine host implementation (Session 19).
//
// Everything CEF lives in this translation unit; the public header stays CEF-free.
//
// Process/thread notes (engine-history/SESSION19_CEF_MIGRATION.md "Thread model inside cef_host"):
//   * CefExecuteProcess is checked at the top of main() in apps/lucyn/main.cpp,
//     NOT here. By the time we run, we are the browser process.
//   * multi_threaded_message_loop = true: CEF runs its own browser-process UI
//     thread internally, so the main thread is free to drive a stdio command
//     loop (the controller) and call BrowserInstance::pump() between commands.
//   * Windowless (off-screen) rendering: no X/GUI window is ever created. CEF
//     paints into a pixel buffer we receive in OnPaint; screenshots read it.
//   * CEF callbacks fire on CEF's UI thread. They only post a thunk onto a
//     mutex-guarded queue; the caller's pump() drains and runs them. Nothing
//     here calls user code on a CEF thread.

#include "lucyn/cef_host/browser_host.h"

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_cookie.h"
#include "include/cef_display_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_process_message.h"
#include "include/cef_render_handler.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_request_context.h"
#include "include/cef_resource_request_handler.h"
#include "include/cef_string_visitor.h"
#include "include/cef_urlrequest.h"
#include "include/cef_task.h"
#include "include/cef_v8.h"
#include "include/cef_version.h"
#include "include/internal/cef_types.h"
#include "include/internal/cef_time.h"      // cef_time_from_timet / _to_basetime
#include "include/base/cef_callback.h"      // defines base::BindOnce
#include "include/wrapper/cef_closure_task.h"  // CefPostTask(TID_UI, closure)

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <thread>
#include <vector>

#ifdef __APPLE__
  #include <mach-o/dyld.h>   // _NSGetExecutablePath: our own path, for browser_subprocess_path
  #include <filesystem>
#endif

#include "lucyn/core/paths.h"

// Session 19 step 10: third_party/stb was deleted along with the old engine, so the
// screenshot path now uses the tiny dependency-free PNG writer below (RGBA8, zlib
// stored blocks — accepted by every PNG decoder). No external headers, no symbols.
namespace pngwriter {

std::uint32_t crcTable[256];
bool crcReady = false;

void initCrc() {
    if (crcReady) return;
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crcTable[n] = c;
    }
    crcReady = true;
}

// Raw CRC-32 (init 0xFFFFFFFF, no final xor — the caller inverts).
std::uint32_t crc32Raw(const unsigned char* d, size_t n) {
    initCrc();
    std::uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = crcTable[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}

void be32(std::string& s, std::uint32_t v) {
    s += static_cast<char>((v >> 24) & 0xFF);
    s += static_cast<char>((v >> 16) & 0xFF);
    s += static_cast<char>((v >> 8) & 0xFF);
    s += static_cast<char>(v & 0xFF);
}

void chunk(std::string& out, const char* type, const std::string& data) {
    be32(out, static_cast<std::uint32_t>(data.size()));
    const size_t crcStart = out.size();
    out += type;
    out += data;
    const std::uint32_t c = crc32Raw(reinterpret_cast<const unsigned char*>(out.data()) + crcStart,
                                     out.size() - crcStart);
    be32(out, c ^ 0xFFFFFFFFu);
}

// zlib stream of UNCOMPRESSED deflate blocks (valid PNG, no compression library needed).
std::string zlibStored(const unsigned char* data, size_t n) {
    std::string z;
    z += static_cast<char>(0x78);   // CMF: deflate, 32K window
    z += static_cast<char>(0x01);   // FLG: check bits, no dictionary
    size_t off = 0;
    do {
        const size_t blk = std::min<size_t>(65535, n - off);
        const bool last = (off + blk >= n);
        z += static_cast<char>(last ? 1 : 0);            // BFINAL | BTYPE=00, byte-aligned
        z += static_cast<char>(blk & 0xFF);              // LEN
        z += static_cast<char>((blk >> 8) & 0xFF);
        const std::uint16_t nlen = static_cast<std::uint16_t>(~static_cast<std::uint16_t>(blk));
        z += static_cast<char>(nlen & 0xFF);             // NLEN (one's complement of LEN)
        z += static_cast<char>((nlen >> 8) & 0xFF);
        z.append(reinterpret_cast<const char*>(data) + off, blk);
        off += blk;
    } while (off < n);
    std::uint32_t a = 1, b = 0;                          // Adler-32 over the raw bytes
    for (size_t i = 0; i < n; ++i) { a = (a + data[i]) % 65521; b = (b + a) % 65521; }
    be32(z, (b << 16) | a);
    return z;
}

// RGBA8 (one filter byte per scanline) -> a PNG file. Returns false if it cannot write.
bool writePng(const std::string& path, int w, int h, const unsigned char* rgba, int stride) {
    if (w <= 0 || h <= 0 || !rgba) return false;
    std::string raw;
    raw.reserve(static_cast<size_t>(h) * (1 + static_cast<size_t>(w) * 4));
    for (int y = 0; y < h; ++y) {
        raw += static_cast<char>(0);                     // scanline filter: None
        raw.append(reinterpret_cast<const char*>(rgba) + static_cast<size_t>(y) * stride,
                   static_cast<size_t>(w) * 4);
    }
    std::string ihdr;
    be32(ihdr, static_cast<std::uint32_t>(w));
    be32(ihdr, static_cast<std::uint32_t>(h));
    ihdr += static_cast<char>(8);   // bit depth
    ihdr += static_cast<char>(6);   // colour type: truecolour with alpha
    ihdr += static_cast<char>(0);   // compression: deflate
    ihdr += static_cast<char>(0);   // filter method: adaptive
    ihdr += static_cast<char>(0);   // interlace: none
    std::string out("\x89PNG\r\n\x1a\n", 8);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", zlibStored(reinterpret_cast<const unsigned char*>(raw.data()), raw.size()));
    chunk(out, "IEND", std::string());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(out.data(), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}

}  // namespace pngwriter

namespace lucyn::cef_host {
namespace {

// ---------------------------------------------------------------------------
// SharedState — the thread-safe bridge between CEF's UI thread and the caller.
// Owned jointly by BrowserInstance::Impl and each LucynClient via shared_ptr, so
// a late CEF callback can never touch freed memory.
// ---------------------------------------------------------------------------
struct SharedState {
    std::mutex mu;
    std::queue<std::function<void()>> callbacks;  // thunks to run on pump()
    std::function<void(bool, int, std::string, std::string)> loadCb;  // guarded by mu

    std::atomic<bool> loading{false};
    std::atomic<bool> closed{false};
    int width  = 1280;
    int height = 800;

    std::string currentUrl;        // guarded by mu
    int lastStatus = 0;            // guarded by mu
    std::string lastTitle;         // guarded by mu
    CefRefPtr<CefBrowser> browser;  // guarded by mu; set once the browser is created

    // Moonbug feed: called DIRECTLY on CEF's IO thread from the resource-request
    // handler (not posted to pump), so captures stay live while the caller blocks
    // in a wait. Guarded by mu; set once from setResourceHandler().
    std::function<void(const BrowserInstance::ResourceEvent&)> resourceHandler;  // guarded by mu

    // Latest painted frame, BGRA, guarded by paintMu. Screenshot reads it.
    std::mutex paintMu;
    std::vector<unsigned char> pixels;
    int pixW = 0;
    int pixH = 0;

    // A screenshot armed by screenshot(), waiting for the settle deadline to pass
    // (guarded by mu). ScreenshotDeadlineTask pops it after the window and encodes
    // the latest painted frame, so the capture reflects the page (including any
    // eval() mutation) as of the deadline.
    struct PendingShot {
        std::string path;
        BrowserInstance::ResultCallback cb;
    };
    std::optional<PendingShot> pendingShot;

    // In-flight eval round-trips: requestId -> completion callback (guarded by mu).
    // The browser process posts a "lucyn.eval" message to the renderer and parks the
    // callback here; LucynClient::OnProcessMessageReceived pops it when the result
    // comes back and fires it via post()/pump().
    std::map<int, BrowserInstance::ResultCallback> pendingEvals;  // guarded by mu
    int nextEvalId = 1;  // guarded by mu

    // Enqueue a thunk to be run by the next pump() on the caller's thread.
    void post(std::function<void()> thunk) {
        std::lock_guard<std::mutex> lock(mu);
        callbacks.push(std::move(thunk));
    }
};

// ---------------------------------------------------------------------------
// JS helpers shared by the render process and the wrappers below.
// ---------------------------------------------------------------------------

// Escape `s` into a double-quoted JavaScript string literal (safe to embed in a
// wrapper we eval). Needed because selectors reach us from the JSON command.
std::string jsStringLiteral(const std::string& s) {
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
                else o += (char)c;
        }
    }
    o += '"';
    return o;
}

// Render process: convert a V8 value to its JavaScript String(...) form so the
// browser side gets one plain string back (matches the old QuickJS eval contract:
// strings unquoted, numbers/bools as text, arrays/objects via String(val)).
// Must be called with the context entered.
std::string V8ValueToString(CefRefPtr<CefV8Context> ctx, CefRefPtr<CefV8Value> val) {
    if (!val || val->IsUndefined()) return "undefined";
    if (val->IsNull()) return "null";
    if (val->IsString()) return val->GetStringValue().ToString();
    if (val->IsBool()) return val->GetBoolValue() ? "true" : "false";
    if (val->IsInt()) return std::to_string(val->GetIntValue());
    if (val->IsDouble()) { std::ostringstream o; o << val->GetDoubleValue(); return o.str(); }
    // Arrays / objects / functions: call String(val) through V8 itself.
    CefRefPtr<CefV8Value> global = ctx->GetGlobal();
    CefRefPtr<CefV8Value> fn = global->GetValue("String");
    if (fn && fn->IsFunction()) {
        CefV8ValueList args;
        args.push_back(val);
        CefRefPtr<CefV8Value> out = fn->ExecuteFunction(nullptr, args);
        if (out && out->IsString()) return out->GetStringValue().ToString();
    }
    return "";
}

// Encode a captured BGRA frame to a PNG at `path` (stb wants interleaved RGBA).
// Runs on the caller's thread via pump() — never on CEF's UI thread. Fires cb
// with the path on success, or an error text on failure.
void EncodePngFile(const std::string& path,
                   const std::vector<unsigned char>& bgra,
                   int w,
                   int h,
                   BrowserInstance::ResultCallback cb) {
    if (w <= 0 || h <= 0 || bgra.size() < static_cast<size_t>(w) * static_cast<size_t>(h) * 4) {
        cb(false, "", "no frame captured yet");
        return;
    }
    std::vector<unsigned char> rgba(bgra.size());
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
        rgba[i]     = bgra[i + 2];  // R
        rgba[i + 1] = bgra[i + 1];  // G
        rgba[i + 2] = bgra[i];      // B
        rgba[i + 3] = bgra[i + 3];  // A
    }
    if (!pngwriter::writePng(path, w, h, rgba.data(), w * 4)) {
        cb(false, "", "failed to write PNG: " + path);
        return;
    }
    cb(true, path, "");
}

// Screenshot settle window: ScreenshotDeadlineTask encodes the latest painted
// frame this long after the capture is armed. It must outlast the time the
// renderer needs to rasterize + settle any eval() mutation (the ONE-Invalidate
// kick) so the captured frame reflects the live page; measured at ~600-1200ms, so
// 1200 gives headroom. Also bounds the capture time for a static page.
constexpr std::chrono::milliseconds kScreenshotSettle{1200};

// CefHeaderMap (multimap<CefString,CefString>) -> plain string pairs for ResourceEvent.
std::vector<std::pair<std::string, std::string>> headerList(
    const std::multimap<CefString, CefString>& m) {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(m.size());
    for (const auto& kv : m) out.emplace_back(kv.first.ToString(), kv.second.ToString());
    return out;
}

// ---------------------------------------------------------------------------
// LucynResourceRequestHandler — feeds the controller's Moonbug tap (step 8).
// Reports the start and completion of every resource CEF loads. Runs on CEF's IO
// thread and invokes the user's handler DIRECTLY (SharedState::resourceHandler)
// — not via pump() — so a capture lands even while the caller blocks in a wait
// (waitIdle needs a live feed).
// ---------------------------------------------------------------------------
class LucynResourceRequestHandler : public CefResourceRequestHandler {
public:
    explicit LucynResourceRequestHandler(std::shared_ptr<SharedState> state)
        : state_(std::move(state)) {}

    ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser>,
                                      CefRefPtr<CefFrame>,
                                      CefRefPtr<CefRequest> request,
                                      CefRefPtr<CefCallback>) override {
        std::function<void(const BrowserInstance::ResourceEvent&)> h;
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            h = state_->resourceHandler;
        }
        if (h) {
            BrowserInstance::ResourceEvent ev;
            ev.phase     = BrowserInstance::ResourceEvent::Start;
            ev.requestId = request->GetIdentifier();
            ev.url       = request->GetURL().ToString();
            ev.method    = request->GetMethod().ToString();
            CefRequest::HeaderMap hm;
            request->GetHeaderMap(hm);
            ev.requestHeaders = headerList(hm);
            h(ev);  // directly on the IO thread
        }
        return RV_CONTINUE;
    }

    void OnResourceLoadComplete(CefRefPtr<CefBrowser>,
                                CefRefPtr<CefFrame>,
                                CefRefPtr<CefRequest> request,
                                CefRefPtr<CefResponse> response,
                                URLRequestStatus status,
                                int64_t received_content_length) override {
        std::function<void(const BrowserInstance::ResourceEvent&)> h;
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            h = state_->resourceHandler;
        }
        if (!h) return;
        BrowserInstance::ResourceEvent ev;
        ev.phase     = BrowserInstance::ResourceEvent::Finish;
        ev.requestId = request->GetIdentifier();
        ev.method    = request->GetMethod().ToString();
        ev.bytes     = received_content_length;
        {
            CefRequest::HeaderMap hm;
            request->GetHeaderMap(hm);
            ev.requestHeaders = headerList(hm);
        }
        if (response) {
            const std::string respUrl = response->GetURL().ToString();
            ev.url    = respUrl.empty() ? request->GetURL().ToString() : respUrl;
            ev.status = response->GetStatus();
            CefResponse::HeaderMap hm;
            response->GetHeaderMap(hm);
            ev.responseHeaders = headerList(hm);
        } else {
            ev.url = request->GetURL().ToString();
        }
        if (status == UR_CANCELED)     ev.error = "canceled";
        else if (status == UR_FAILED)  ev.error = "request failed";
        h(ev);  // directly on the IO thread
    }

private:
    std::shared_ptr<SharedState> state_;
    IMPLEMENT_REFCOUNTING(LucynResourceRequestHandler);
    DISALLOW_COPY_AND_ASSIGN(LucynResourceRequestHandler);
};

// ---------------------------------------------------------------------------
// LucynFetchClient — a raw body fetch for the `download` command (Session 19
// step 9). Wraps a CefURLRequest so we read the response bytes off Chromium's
// network stack WITHOUT navigating the main page (and using the same global
// cookie jar a visit populates). Created on the caller's thread; the request is
// started on the UI thread (StartFetchTask) and its callbacks fire there; the
// completion is handed to the caller via pump().
// ---------------------------------------------------------------------------
class LucynFetchClient : public CefURLRequestClient {
public:
    LucynFetchClient(std::shared_ptr<SharedState> state,
                     long long maxBytes,
                     const std::string& referer,
                     BrowserInstance::FetchCallback cb)
        : state_(std::move(state)), maxBytes_(maxBytes), referer_(referer), cb_(std::move(cb)) {}

    // Keep the in-flight request alive for its duration (released on completion).
    // Set right after CefURLRequest::Create on the UI thread.
    void attach(CefRefPtr<CefURLRequest> req) { request_ = req; }

    // Fire an immediate failure (e.g. the request could not even be started).
    void fail(const std::string& msg) {
        BrowserInstance::FetchResult r;
        r.error = msg;
        auto cb = cb_;
        auto st = state_;
        st->post([cb, r]() { cb(r); });
    }

    void OnRequestComplete(CefRefPtr<CefURLRequest> request) override {
        BrowserInstance::FetchResult r;
        r.body      = std::move(body_);
        r.bytes     = total_;
        r.truncated = truncated_;
        if (CefRefPtr<CefResponse> resp = request->GetResponse()) {
            r.status   = resp->GetStatus();
            r.finalUrl = resp->GetURL().ToString();
            CefResponse::HeaderMap hm;
            resp->GetHeaderMap(hm);
            for (const auto& kv : hm) {
                std::string k = kv.first.ToString();
                std::string v = kv.second.ToString();
                for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (k == "content-type")             r.contentType = v;
                else if (k == "content-disposition") r.contentDisposition = v;
            }
        }
        if (request->GetRequestStatus() != UR_SUCCESS) {
            r.ok    = false;
            r.error = "request failed (CEF error " + std::to_string(request->GetRequestError()) + ")";
        } else if (r.status >= 200 && r.status < 300) {
            r.ok = true;
        } else {
            r.ok    = false;
            r.error = "HTTP " + std::to_string(r.status);
        }
        request_ = nullptr;   // break the keep-alive cycle; `request` stays valid for this call
        auto cb  = cb_;
        auto st  = state_;
        st->post([cb, r]() { cb(r); });   // deliver on the caller's thread via pump()
    }

    void OnUploadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}
    void OnDownloadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}

    void OnDownloadData(CefRefPtr<CefURLRequest>, const void* data, size_t len) override {
        total_ += static_cast<long long>(len);
        if (maxBytes_ > 0 && static_cast<long long>(body_.size()) + static_cast<long long>(len) > maxBytes_) {
            const size_t room = static_cast<size_t>(maxBytes_) - body_.size();
            body_.append(static_cast<const char*>(data), room);
            truncated_ = true;
        } else {
            body_.append(static_cast<const char*>(data), len);
        }
    }

    bool GetAuthCredentials(bool, const CefString&, int, const CefString&, const CefString&,
                            CefRefPtr<CefAuthCallback>) override {
        return false;   // no interactive auth on a headless fetch
    }

private:
    std::shared_ptr<SharedState> state_;
    long long maxBytes_;
    std::string referer_;
    BrowserInstance::FetchCallback cb_;
    CefRefPtr<CefURLRequest> request_;
    std::string body_;
    long long total_ = 0;
    bool truncated_ = false;

    IMPLEMENT_REFCOUNTING(LucynFetchClient);
    DISALLOW_COPY_AND_ASSIGN(LucynFetchClient);
};

// Start the fetch on the CEF UI thread (CefURLRequest::Create must run on a CEF
// browser-process thread). Refcounted / copyable args only, so a queued task
// cannot dangle even if the BrowserInstance is destroyed before it runs.
void StartFetchTask(CefRefPtr<LucynFetchClient> client, const std::string& url, const std::string& referer) {
    CefRefPtr<CefRequest> req = CefRequest::Create();
    req->SetURL(url);
    req->SetMethod("GET");
    if (!referer.empty()) {   // the `download` command's referer option
        CefRequest::HeaderMap hm;
        hm.insert(std::make_pair(CefString("Referer"), CefString(referer)));
        req->SetHeaderMap(hm);
    }
    // Empty request context = the global one, shared with the browser's cookie jar.
    CefRefPtr<CefURLRequest> r = CefURLRequest::Create(req, client, nullptr);
    if (!r) { client->fail("could not start the request"); return; }
    client->attach(r);
}

// ---------------------------------------------------------------------------
// LucynClient — one browser's CEF client. Implements the handler interfaces the
// engine calls back into. Holds a shared_ptr to SharedState, never a raw Impl*.
// ---------------------------------------------------------------------------
class LucynClient : public CefClient,
                    public CefLoadHandler,
                    public CefDisplayHandler,
                    public CefRenderHandler,
                    public CefRequestHandler {
public:
    explicit LucynClient(std::shared_ptr<SharedState> state) : state_(std::move(state)) {
        resourceReq_ = new LucynResourceRequestHandler(state_);
    }

    // -- CefClient -----------------------------------------------------------
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

    // Provide the resource observer that drives Moonbug. Returning a non-null
    // handler here is what makes OnBeforeResourceLoad / OnResourceLoadComplete fire.
    CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
        CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>,
        bool, bool, const CefString&, bool&) override {
        return resourceReq_;
    }

    // -- CefRenderHandler (windowless painting) ------------------------------
    // Tell CEF the size of our off-screen surface.
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        rect = CefRect(0, 0, state_->width, state_->height);
    }

    // CEF hands us the rendered pixels (BGRA). Keep the latest full-surface
    // copy so screenshots can encode it. PED: paint only ever writes the whole
    // buffer here; a real OSR app would stitch dirty rects.
    void OnPaint(CefRefPtr<CefBrowser>,
                 PaintElementType type,
                 const RectList&,
                 const void* buffer,
                 int width,
                 int height) override {
        if (type != PET_VIEW || buffer == nullptr || width <= 0 || height <= 0) return;
        const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        {
            std::lock_guard<std::mutex> lock(state_->paintMu);
            state_->pixels.resize(bytes);
            std::memcpy(state_->pixels.data(), buffer, bytes);
            state_->pixW = width;
            state_->pixH = height;
        }

        // Screenshot capture is handled entirely by the delayed ScreenshotDeadlineTask
        // (which encodes the latest frame after the settle window). We only store the
        // latest pixels here, and deliberately do NOT re-invalidate: doing so would
        // re-composite the stale raster and stop the renderer from repainting an
        // eval() mutation.
    }

    // -- CefDisplayHandler ---------------------------------------------------
    void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        state_->lastTitle = title.ToString();
    }

    // -- CefLoadHandler ------------------------------------------------------
    void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   int httpStatusCode) override {
        // Only the main frame matters for visit() completion.
        if (!frame->IsMain()) return;
        const std::string url   = browser->GetMainFrame()->GetURL().ToString();
        const int status        = httpStatusCode;
        std::string title;
        std::function<void(bool, int, std::string, std::string)> cb;
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            state_->currentUrl = url;
            state_->lastStatus = status;
            state_->loading    = false;
            title              = state_->lastTitle;
            cb                 = state_->loadCb;
            state_->loadCb     = nullptr;
        }
        if (cb) {
            // Fire the user callback on the caller's thread via pump().
            state_->post([cb, status, title]() { cb(true, status, title, std::string()); });
        }
    }

    void OnLoadError(CefRefPtr<CefBrowser>,
                     CefRefPtr<CefFrame> frame,
                     ErrorCode,
                     const CefString& errorText,
                     const CefString& failedUrl) override {
        if (!frame->IsMain()) return;
        const std::string err  = errorText.ToString();
        std::function<void(bool, int, std::string, std::string)> cb;
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            state_->loading    = false;
            state_->currentUrl = failedUrl.ToString();
            cb                 = state_->loadCb;
            state_->loadCb     = nullptr;
        }
        if (cb) {
            state_->post([cb, err]() { cb(false, 0, std::string(), err); });
        }
    }

    // -- CefClient: eval results coming back from the renderer -------------
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser>,
                                  CefRefPtr<CefFrame>,
                                  CefProcessId,
                                  CefRefPtr<CefProcessMessage> message) override {
        if (message->GetName() != "lucyn.eval.result") return false;
        CefRefPtr<CefListValue> args = message->GetArgumentList();
        const int reqId = args->GetInt(0);
        const bool ok   = args->GetInt(1) != 0;
        const std::string value = args->GetString(2).ToString();
        const std::string error = args->GetString(3).ToString();

        BrowserInstance::ResultCallback cb;
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            auto it = state_->pendingEvals.find(reqId);
            if (it != state_->pendingEvals.end()) {
                cb = std::move(it->second);
                state_->pendingEvals.erase(it);
            }
        }
        if (cb) {
            // Fire on the caller's thread via pump(), like every other completion.
            state_->post([cb, ok, value, error]() { cb(ok, value, error); });
        }
        return true;
    }

private:
    std::shared_ptr<SharedState> state_;
    CefRefPtr<LucynResourceRequestHandler> resourceReq_;  // returned to CEF once, reused

    // CefBaseRefCounted
    IMPLEMENT_REFCOUNTING(LucynClient);
    DISALLOW_COPY_AND_ASSIGN(LucynClient);
};

// Context-init signalling: OnContextInitialized() sets g_contextReady on CEF's
// browser UI thread; initCef() blocks on it. CefShutdown() must not run before
// the context has finished starting up (it segfaults the browser UI thread
// otherwise — a real race the cef_host ctest hit on init->immediate-shutdown).
std::atomic<bool> g_contextReady{false};
std::mutex g_initMu;
std::condition_variable g_initCv;

// ---------------------------------------------------------------------------
// LucynApp — the browser-process CefApp. Sets headless flags before Chromium
// starts processing the command line.
// ---------------------------------------------------------------------------
class LucynApp : public CefApp, public CefBrowserProcessHandler, public CefRenderProcessHandler {
public:
    LucynApp() = default;

    // -- CefApp --------------------------------------------------------------
    void OnBeforeCommandLineProcessing(const CefString&,
                                       CefRefPtr<CefCommandLine> command_line) override {
        // Headless scraping: no GPU, no sandbox friction, tiny footprint.
        // See engine-history/SESSION19_CEF_MIGRATION.md "CEF headless flags for Linux".
        command_line->AppendSwitch("disable-gpu");
        command_line->AppendSwitch("disable-gpu-compositing");
        // Drive frames at windowless_frame_rate so the RENDERER repaints DOM
        // mutations (a screenshot right after eval() captured stale content
        // without it: Invalidate only re-composites the old raster). Required for
        // off-screen rendering to reflect the live page — SESSION19 recommended it.
        command_line->AppendSwitch("enable-begin-frame-scheduling");
        command_line->AppendSwitch("disable-dev-shm-usage");  // /dev/shm is often small
        command_line->AppendSwitch("disable-setuid-sandbox");
        command_line->AppendSwitch("no-sandbox");  // dev/headless; revisit for production
        command_line->AppendSwitch("disable-extensions");
        command_line->AppendSwitch("disable-background-networking");
        command_line->AppendSwitch("disable-sync");
        command_line->AppendSwitch("disable-translate");
        command_line->AppendSwitch("hide-scrollbars");
        command_line->AppendSwitch("mute-audio");
        // Headless ozone platform so no X/Wayland display is required.
        command_line->AppendSwitch("ozone-platform=headless");
    }

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

    // -- CefBrowserProcessHandler -------------------------------------------
    // Called on CEF's browser UI thread once the global context is fully up:
    // browsers may be created and CefShutdown() is now safe. initCef() waits on
    // this before returning.
    void OnContextInitialized() override {
        g_contextReady.store(true);
        std::lock_guard<std::mutex> lock(g_initMu);
        g_initCv.notify_all();
    }

    // -- CefRenderProcessHandler (runs in the RENDERER subprocess) ----------
    // Store the main-frame V8 context when it is created so eval requests have
    // somewhere to run; clear it on release (navigation). All three below run on
    // the renderer main thread, so contexts_ needs no lock.
    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override {
        if (frame->IsMain()) contexts_[browser->GetIdentifier()] = context;
    }

    void OnContextReleased(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefV8Context>) override {
        if (frame->IsMain()) contexts_.erase(browser->GetIdentifier());
    }

    // Receive "lucyn.eval" from the browser process, evaluate `code` in the main
    // frame's stored context, and send String(result) back as "lucyn.eval.result".
    // Always replies (even on error) so the browser-side request can never hang.
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefProcessId,
                                  CefRefPtr<CefProcessMessage> message) override {
        if (message->GetName() != "lucyn.eval") return false;
        CefRefPtr<CefListValue> args = message->GetArgumentList();
        const int reqId        = args->GetInt(0);
        const std::string code = args->GetString(1).ToString();

        bool ok = false;
        std::string value, error;
        auto it = contexts_.find(browser->GetIdentifier());
        CefRefPtr<CefV8Context> ctx = (it != contexts_.end()) ? it->second : nullptr;
        if (!ctx) {
            error = "no V8 context (page not loaded?)";
        } else if (ctx->Enter()) {
            CefRefPtr<CefV8Value> retval;
            CefRefPtr<CefV8Exception> exc;
            if (ctx->Eval(code, "", 0, retval, exc)) {
                ok = true;
                value = V8ValueToString(ctx, retval);
            } else {
                error = exc ? exc->GetMessage().ToString() : "eval failed";
            }
            ctx->Exit();
        } else {
            error = "could not enter V8 context";
        }

        CefRefPtr<CefProcessMessage> reply = CefProcessMessage::Create("lucyn.eval.result");
        reply->GetArgumentList()->SetInt(0, reqId);
        reply->GetArgumentList()->SetInt(1, ok ? 1 : 0);
        reply->GetArgumentList()->SetString(2, value);
        reply->GetArgumentList()->SetString(3, error);
        frame->SendProcessMessage(PID_BROWSER, reply);
        return true;
    }

private:
    // browser id -> main-frame V8 context (renderer main thread only).
    std::map<int, CefRefPtr<CefV8Context>> contexts_;

    IMPLEMENT_REFCOUNTING(LucynApp);
    DISALLOW_COPY_AND_ASSIGN(LucynApp);
};

// ---------------------------------------------------------------------------
// CEF UI-thread tasks — bound with base::BindOnce and posted to TID_UI.
// They capture ONLY refcounted / copyable values (CefRefPtr, shared_ptr,
// std::string), never a raw Impl*, so base::BindOnce's "no raw pointer
// receiver" rule is satisfied and a queued task can never touch a destroyed
// BrowserInstance's internals. The state shared_ptr + client CefRefPtr keep
// everything alive until the task has run.
// ---------------------------------------------------------------------------

// Create the (windowless) browser. Runs on the CEF UI thread, so the synchronous
// CreateBrowserSync API is allowed and hands back the CefBrowser directly.
void CreateBrowserTask(CefRefPtr<LucynClient> client,
                       std::shared_ptr<SharedState> state,
                       const std::string& url) {
    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);  // windowless: parent 0, no real window (off-screen)

    CefBrowserSettings browser_settings;
    // 30 fps (the CEF default). A 1 fps cap starved OnPaint of fresh content
    // frames: screenshots captured only the background color, never the page.
    browser_settings.windowless_frame_rate = 30;
    // Opaque white background so pages without a CSS background aren't transparent.
    browser_settings.background_color = CefColorSetARGB(255, 255, 255, 255);

    CefRefPtr<CefBrowser> b =
        CefBrowserHost::CreateBrowserSync(window_info, client, url, browser_settings, nullptr, nullptr);
    std::lock_guard<std::mutex> lock(state->mu);
    state->browser = b;
}

// Navigate an existing browser's main frame. Runs on the CEF UI thread.
void LoadUrlTask(CefRefPtr<CefBrowser> browser, const std::string& url) {
    if (browser && browser->GetMainFrame()) {
        browser->GetMainFrame()->LoadURL(url);
    }
}

// Kick the compositor to produce one fresh frame (screenshot arming). Runs on the
// UI thread. Exactly one Invalidate is posted per capture: it makes the compositor
// request a BeginFrame so the renderer rasterizes any pending DOM damage (the
// eval() mutation). Re-invalidating on every subsequent paint is harmful — it
// re-composites the stale raster and stops the renderer from ever repainting the
// mutation — so we never do that; the settle deadline then encodes the result.
void InvalidateTask(CefRefPtr<CefBrowser> browser) {
    if (browser) browser->GetHost()->Invalidate(PET_VIEW);
}

// Screenshot settle deadline. Runs on the UI thread one settle-window after the
// capture was armed, and encodes the latest painted frame. By this point the
// ONE-Invalidate kick has made the renderer rasterize any eval() mutation and the
// frame has settled, so the captured pixels reflect the live page (not the stale
// pre-mutation raster). This is the only delivery path — OnPaint just stores
// pixels — so it also covers a static page that never repaints.
void ScreenshotDeadlineTask(std::shared_ptr<SharedState> state) {
    BrowserInstance::ResultCallback shotCb;
    std::string shotPath;
    std::vector<unsigned char> shotPixels;
    int shotW = 0, shotH = 0;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        if (!state->pendingShot) return;  // no armed shot (e.g. overlapped captures)
        shotCb   = std::move(state->pendingShot->cb);
        shotPath = std::move(state->pendingShot->path);
        state->pendingShot.reset();
    }
    {
        std::lock_guard<std::mutex> pl(state->paintMu);
        shotPixels = state->pixels;
        shotW = state->pixW;
        shotH = state->pixH;
    }
    state->post([shotCb, shotPath, px = std::move(shotPixels), shotW, shotH]() mutable {
        EncodePngFile(shotPath, px, shotW, shotH, shotCb);
    });
}

// ---------------------------------------------------------------------------
// Cookies (Session 19 step 9) — backed by Chromium's cookie store, the jar a
// CEF page actually uses.
// ---------------------------------------------------------------------------
// CefCookie (as reported by VisitAllCookies) -> our CEF-free Cookie.
BrowserInstance::Cookie fromCefCookie(const CefCookie& cc) {
    BrowserInstance::Cookie c;
    c.name  = CefString(&cc.name).ToString();
    c.value = CefString(&cc.value).ToString();
    const std::string dom = CefString(&cc.domain).ToString();
    // A domain cookie carries a leading "." (visible to subdomains); a host cookie
    // does not. net/Cookie stores the domain WITHOUT the dot plus a hostOnly flag.
    if (!dom.empty() && dom.front() == '.') { c.hostOnly = false; c.domain = dom.substr(1); }
    else                                     { c.hostOnly = true;  c.domain = dom; }
    c.path = CefString(&cc.path).ToString();
    if (c.path.empty()) c.path = "/";
    c.secure   = cc.secure != 0;
    c.httpOnly = cc.httponly != 0;
    if (cc.has_expires) {
        cef_time_t t;
        time_t sec = 0;
        if (cef_time_from_basetime(cc.expires, &t) && cef_time_to_timet(&t, &sec))
            c.expires = static_cast<long long>(sec);
    }
    return c;
}

// Our Cookie -> a CefCookie to hand to SetCookie(). The caller must clear the
// four string fields afterwards (cef_cookie_t is a POD with no destructor).
CefCookie toCefCookie(const BrowserInstance::Cookie& c) {
    CefCookie cc{};
    CefString(&cc.name).FromString(c.name);
    CefString(&cc.value).FromString(c.value);
    CefString(&cc.path).FromString(c.path.empty() ? "/" : c.path);
    if (!c.domain.empty()) {
        std::string dom = c.domain;
        if (!c.hostOnly && dom.front() != '.') dom.insert(dom.begin(), '.');  // domain cookie
        CefString(&cc.domain).FromString(dom);
    }
    cc.secure   = c.secure ? 1 : 0;
    cc.httponly = c.httpOnly ? 1 : 0;
    if (c.expires > 0) {
        cc.has_expires = 1;
        cef_time_t t;
        if (cef_time_from_timet(static_cast<time_t>(c.expires), &t))
            cef_time_to_basetime(&t, &cc.expires);
    }
    return cc;
}

// Shared state for one VisitAllCookies round-trip. Visit() runs per cookie on the
// UI thread and the LAST one (count+1 == total) completes it. When the store is
// EMPTY Visit is never called, so a delayed fallback completes it instead — both
// paths go through deliver(), which fires the user callback exactly once.
struct CookieCollect {
    std::shared_ptr<SharedState> state;
    BrowserInstance::CookieListCallback cb;
    std::atomic<bool> done{false};
    std::vector<BrowserInstance::Cookie> cookies;  // touched on the UI thread only
    void deliver(bool ok, const std::string& err) {
        bool expected = false;
        if (!done.compare_exchange_strong(expected, true)) return;  // already delivered
        auto c = cb; auto list = cookies; auto st = state;
        st->post([c, list, ok, err]() { c(ok, list, err); });
    }
};

class CookieVisitor : public CefCookieVisitor {
public:
    explicit CookieVisitor(std::shared_ptr<CookieCollect> coll) : coll_(std::move(coll)) {}
    bool Visit(const CefCookie& cookie, int count, int total, bool&) override {
        coll_->cookies.push_back(fromCefCookie(cookie));
        if (count + 1 >= total) coll_->deliver(true, "");  // last cookie
        return true;
    }
private:
    std::shared_ptr<CookieCollect> coll_;
    IMPLEMENT_REFCOUNTING(CookieVisitor);
    DISALLOW_COPY_AND_ASSIGN(CookieVisitor);
};

// Completes the wait when the store was empty (Visit never ran). UI thread.
void CookieVisitFallback(std::shared_ptr<CookieCollect> coll) {
    coll->deliver(true, "");  // no-op if the visitor already delivered
}

// Kick off VisitAllCookies / VisitUrlCookies. Runs on the UI thread.
void DoVisitAllCookies(std::shared_ptr<CookieCollect> coll, std::string url) {
    CefRefPtr<CefCookieManager> mgr = CefCookieManager::GetGlobalManager(nullptr);
    if (!mgr) { coll->deliver(false, "cookie manager unavailable"); return; }
    CefRefPtr<CookieVisitor> v = new CookieVisitor(coll);
    const bool started = url.empty()
        ? mgr->VisitAllCookies(v)
        : mgr->VisitUrlCookies(url, /*includeHttpOnly=*/true, v);
    if (!started) { coll->deliver(false, "cannot access cookies"); return; }
    // Visit*Cookies has no completion callback: schedule a fallback that fires
    // only if the visitor never did (empty result). UI thread; visits are
    // in-memory and run first, so the fallback is a no-op whenever cookies exist.
    CefPostDelayedTask(TID_UI, base::BindOnce(&CookieVisitFallback, coll), 300);
}

// SetCookie completion. UI thread.
class SetCookieDone : public CefSetCookieCallback {
public:
    SetCookieDone(std::shared_ptr<SharedState> state, BrowserInstance::ResultCallback cb)
        : state_(std::move(state)), cb_(std::move(cb)) {}
    void OnComplete(bool success) override {
        state_->post([cb = cb_, success]() {
            cb(success, success ? "ok" : "", success ? "" : "cookie could not be stored");
        });
    }
private:
    std::shared_ptr<SharedState> state_;
    BrowserInstance::ResultCallback cb_;
    IMPLEMENT_REFCOUNTING(SetCookieDone);
    DISALLOW_COPY_AND_ASSIGN(SetCookieDone);
};

// DeleteCookies completion. UI thread.
class DeleteCookiesDone : public CefDeleteCookiesCallback {
public:
    DeleteCookiesDone(std::shared_ptr<SharedState> state, BrowserInstance::ResultCallback cb)
        : state_(std::move(state)), cb_(std::move(cb)) {}
    void OnComplete(int num_deleted) override {
        state_->post([cb = cb_, n = num_deleted]() { cb(true, std::to_string(n), ""); });
    }
private:
    std::shared_ptr<SharedState> state_;
    BrowserInstance::ResultCallback cb_;
    IMPLEMENT_REFCOUNTING(DeleteCookiesDone);
    DISALLOW_COPY_AND_ASSIGN(DeleteCookiesDone);
};

// Perform SetCookie. Runs on the UI thread.
void DoSetCookie(std::shared_ptr<SharedState> state, const std::string& url,
                 BrowserInstance::Cookie c, BrowserInstance::ResultCallback cb) {
    CefRefPtr<CefCookieManager> mgr = CefCookieManager::GetGlobalManager(nullptr);
    if (!mgr) { state->post([cb]() { cb(false, "", "cookie manager unavailable"); }); return; }
    CefCookie cc = toCefCookie(c);
    const bool accepted = mgr->SetCookie(url, cc, new SetCookieDone(state, cb));
    // Free the strings we built (POD cef_cookie_t; CEF copied what it needs by
    // the time SetCookie returns).
    CefString(&cc.name).clear();
    CefString(&cc.value).clear();
    CefString(&cc.domain).clear();
    CefString(&cc.path).clear();
    if (!accepted)  // callback will NOT fire when the call is rejected up front
        state->post([cb]() { cb(false, "", "cookie rejected (bad URL or characters)"); });
}

// Delete every cookie. Runs on the UI thread.
void DoClearCookies(std::shared_ptr<SharedState> state, BrowserInstance::ResultCallback cb) {
    CefRefPtr<CefCookieManager> mgr = CefCookieManager::GetGlobalManager(nullptr);
    if (!mgr) { state->post([cb]() { cb(false, "", "cookie manager unavailable"); }); return; }
    if (!mgr->DeleteCookies("", "", new DeleteCookiesDone(state, cb)))
        state->post([cb]() { cb(false, "", "cannot access cookies"); });
}

// Captures CefFrame::GetSource output and hands it back via pump().
class HtmlVisitor : public CefStringVisitor {
public:
    HtmlVisitor(std::shared_ptr<SharedState> state, BrowserInstance::ResultCallback cb)
        : state_(std::move(state)), cb_(std::move(cb)) {}

    void Visit(const CefString& src) override {
        std::string html = src.ToString();
        state_->post([cb = cb_, html]() { cb(true, html, ""); });
    }

private:
    std::shared_ptr<SharedState> state_;
    BrowserInstance::ResultCallback cb_;
    IMPLEMENT_REFCOUNTING(HtmlVisitor);
    DISALLOW_COPY_AND_ASSIGN(HtmlVisitor);
};

// Deliver an eval request to the renderer. Runs on the CEF UI thread.
void SendEvalTask(CefRefPtr<CefBrowser> browser, int reqId, const std::string& code) {
    if (!browser || !browser->GetMainFrame()) return;
    CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("lucyn.eval");
    msg->GetArgumentList()->SetInt(0, reqId);
    msg->GetArgumentList()->SetString(1, code);
    browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, msg);
}

// Fetch the main frame's serialized HTML source. Runs on the CEF UI thread.
void GetSourceTask(CefRefPtr<CefBrowser> browser,
                   std::shared_ptr<SharedState> state,
                   BrowserInstance::ResultCallback cb) {
    if (!browser || !browser->GetMainFrame()) {
        state->post([cb]() { cb(false, "", "no page loaded"); });
        return;
    }
    browser->GetMainFrame()->GetSource(new HtmlVisitor(std::move(state), std::move(cb)));
}

// Process-wide CEF state.
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_shuttingDown{false};
CefRefPtr<CefApp> g_app;  // keep the app alive for the life of CEF

}  // namespace

// ---------------------------------------------------------------------------
// executeCefProcess / initCef / shutdownCef / isCefInitialized / cefVersion
// ---------------------------------------------------------------------------
int executeCefProcess(int argc, char** argv) {
    CefMainArgs main_args(argc, argv);
    // Pass our LucynApp so renderer/gpu subprocesses get an app: the render-process
    // handler (OnContextCreated + the "lucyn.eval" V8 evaluator) lives on it, and
    // without it eval/extract/click would have no V8 hook in the subprocess. For the
    // browser process CefExecuteProcess returns -1 and this app is discarded (initCef
    // creates the real g_app). MUST be the first CEF call of main() — SESSION19
    // "Process model".
    CefRefPtr<CefApp> app = new LucynApp();
    return CefExecuteProcess(main_args, app, nullptr);
}

bool initCef(int argc, char** argv) {
    if (g_initialized.load()) return true;
    g_contextReady.store(false);
    g_shuttingDown.store(false);

    CefMainArgs main_args(argc, argv);
    g_app = new LucynApp();

    CefSettings settings;
    // Windowless (off-screen) rendering: required for headless + screenshots.
    settings.windowless_rendering_enabled = 1;
    // CEF owns its browser-process UI thread; our main thread stays free to run
    // a stdio command loop and call pump().
    settings.multi_threaded_message_loop = 1;
    // No crash reporter in a scraper.
    settings.no_sandbox = 1;

    // root_cache_path MUST be an absolute path (CEF 120+ enforces a process
    // singleton lock on it). Keep it under nothing-data per the repo RULE
    // (modules/core/paths.h): <exeDir>/nothing-data/cef. A caller can override
    // it via LUCYN_CEF_CACHE (tests/smoke runs use a private temp dir so the
    // singleton lock can't collide across concurrent runs).
    std::string cache;
    if (const char* env = std::getenv("LUCYN_CEF_CACHE"); env && env[0]) {
        cache = env;
    } else {
        cache = core::dataDir("cef");
    }
    if (cache.empty()) {
        // Fall back to a temp dir rather than let CEF crash on an empty path.
        CefString(&settings.root_cache_path).FromString("/tmp/lucyn-cef-cache");
    } else {
        CefString(&settings.root_cache_path).FromString(cache);
    }

    // Where CEF finds icudtl.dat, *.pak and locales/. Left unset, CEF derives these from
    // the directory of the LOADED libcef.so: that works for a build/bin run (the runtime
    // files are copied next to the binary), but a binary copied elsewhere (the cli_download
    // test copies lucyn into a temp dir) loads libcef.so from the distro's Release/ through
    // the INSTALL_RPATH fallback — and icudtl.dat lives in the distro's Resources/, not
    // Release/ — which aborts with the cryptic "Invalid file descriptor to ICU data".
    // So: use the files beside the executable when they are there, else the build-time
    // distro (the same relocation philosophy as the rpath fallback).
#ifdef __APPLE__
    {
        // macOS differs twice over: libcef IS the framework binary, and icudtl.dat, *.pak
        // and the *.lproj locales live INSIDE
        // Chromium Embedded Framework.framework/Resources — there is no flat icudtl.dat
        // beside the executable and the macOS distro has no top-level Resources/ at all.
        // CEF derives resources_dir_path/locales_dir_path from framework_dir_path, so that
        // is the one to set: the framework lucyn_cef_copy_runtime() dropped beside the
        // executable, else the distro's Release/ one.
        std::string framework;
        const std::string exe = core::exeDir();
        if (!exe.empty()) {
            const std::string beside = exe + "/Chromium Embedded Framework.framework";
            if (std::ifstream(beside + "/Resources/icudtl.dat", std::ios::binary).good())
                framework = beside;
        }
        if (framework.empty()) {
#ifdef LUCYN_CEF_FRAMEWORK_DIR
            if (LUCYN_CEF_FRAMEWORK_DIR[0]) framework = LUCYN_CEF_FRAMEWORK_DIR;
#endif
        }
        if (!framework.empty())
            CefString(&settings.framework_dir_path).FromString(framework);

        // AND: browser_subprocess_path. A bare Mach-O executable is not an app bundle, so
        // CEF's macOS default for an empty value — "Contents/Frameworks/<app> Helper.app/..."
        // in the top-level app bundle — cannot resolve (cef_types.h, browser_subprocess_path).
        // Point it at THIS executable instead: main.cpp routes argv through
        // executeCefProcess(), which returns -1 in the browser process and hands the
        // renderer/GPU/network children their CefApp for --type=... invocations.
        uint32_t exeSz = 0;
        _NSGetExecutablePath(nullptr, &exeSz);
        std::vector<char> exeBuf(exeSz + 1);
        if (_NSGetExecutablePath(exeBuf.data(), &exeSz) == 0) {
            std::error_code ec;
            std::filesystem::path self = std::filesystem::canonical(exeBuf.data(), ec);
            if (!ec)
                CefString(&settings.browser_subprocess_path).FromString(self.string());
        }
    }
#else
    {
        std::string resources;
        const std::string exe = core::exeDir();
        if (!exe.empty()) {
            std::ifstream probe(exe + "/icudtl.dat", std::ios::binary);
            if (probe.good()) resources = exe;
        }
        if (resources.empty()) {
#ifdef LUCYN_CEF_RESOURCES_DIR
            resources = LUCYN_CEF_RESOURCES_DIR;
#endif
        }
        if (!resources.empty()) {
            CefString(&settings.resources_dir_path).FromString(resources);
            CefString(&settings.locales_dir_path).FromString(resources + "/locales");
        }
    }
#endif

    if (!CefInitialize(main_args, settings, g_app, nullptr)) {
        g_app = nullptr;
        return false;
    }

    // Wait for CefBrowserProcessHandler::OnContextInitialized() before handing
    // control back to the caller. CefShutdown() racing a still-starting context
    // segfaults the browser UI thread (CrBrowserMain crashes in
    // CefContext::OnContextInitialized). Real failure the cef_host ctest hit:
    // init -> immediate shutdown with no page load to slow it down.
    bool ready = false;
    {
        std::unique_lock<std::mutex> lock(g_initMu);
        ready = g_initCv.wait_for(lock, std::chrono::seconds(15),
                                  [] { return g_contextReady.load(); });
    }
    if (!ready) {
        // Context never came up: treat as init failure and leave g_initialized
        // false so shutdownCef() won't tear down a half-started CEF.
        return false;
    }

    g_initialized.store(true);
    return true;
}

void shutdownCef() {
    if (!g_initialized.load() || g_shuttingDown.exchange(true)) return;
    CefShutdown();
    g_app = nullptr;
    g_initialized.store(false);
}

bool isCefInitialized() { return g_initialized.load(); }

std::string cefVersion() { return CEF_VERSION; }

// ---------------------------------------------------------------------------
// BrowserInstance
// ---------------------------------------------------------------------------
struct BrowserInstance::Impl {
    std::shared_ptr<SharedState> state;
    CefRefPtr<LucynClient> client;
    BrowserOptions opts;
    // NOTE: the CefBrowser itself lives in state->browser (guarded by state->mu),
    // so CEF UI-thread tasks can reach it without holding a raw Impl*.
};

BrowserInstance::BrowserInstance(const BrowserOptions& opts) : impl_(new Impl()) {
    impl_->opts   = opts;
    impl_->state  = std::make_shared<SharedState>();
    impl_->state->width  = opts.viewportWidth;
    impl_->state->height = opts.viewportHeight;
    impl_->client = new LucynClient(impl_->state);
}

BrowserInstance::~BrowserInstance() {
    if (!impl_) return;
    CefRefPtr<CefBrowser> b;
    {
        std::lock_guard<std::mutex> lock(impl_->state->mu);
        impl_->state->closed.store(true);
        b = impl_->state->browser;
    }
    // Close outside the lock: CloseBrowser may re-enter our handlers asynchronously.
    if (b) b->GetHost()->CloseBrowser(true);
}

// Moves are allowed; the source is left empty.
BrowserInstance::BrowserInstance(BrowserInstance&&) noexcept = default;
BrowserInstance& BrowserInstance::operator=(BrowserInstance&&) noexcept = default;

void BrowserInstance::visit(const std::string& url, LoadCallback cb) {
    if (!impl_) return;
    CefRefPtr<CefBrowser> existing;
    {
        std::lock_guard<std::mutex> lock(impl_->state->mu);
        impl_->state->loading    = true;
        impl_->state->currentUrl = url;
        impl_->state->lastTitle.clear();
        impl_->state->lastStatus = 0;
        impl_->state->loadCb     = std::move(cb);
        existing                 = impl_->state->browser;
    }

    if (existing) {
        // Already have a browser: navigate it (UI thread, refcounted args only).
        CefPostTask(TID_UI, base::BindOnce(&LoadUrlTask, existing, url));
    } else {
        // First visit: create the windowless browser on the CEF UI thread. The
        // closure copies the client CefRefPtr + state shared_ptr, so nothing here
        // can dangle even if this BrowserInstance is destroyed before it runs.
        CefPostTask(TID_UI, base::BindOnce(&CreateBrowserTask, impl_->client, impl_->state, url));
    }
}

// Fetch a URL's raw body for `download` without touching the main page.
void BrowserInstance::fetchBody(const std::string& url, long long maxBytes, const std::string& referer, FetchCallback cb) {
    if (!impl_) { FetchResult r; r.error = "instance gone"; cb(r); return; }
    CefRefPtr<LucynFetchClient> client = new LucynFetchClient(impl_->state, maxBytes, referer, std::move(cb));
    CefPostTask(TID_UI, base::BindOnce(&StartFetchTask, client, url, referer));
}

// ---- Session 19 steps 5-6: async page operations ---------------------------
// Each parks a callback and hands work to CEF; completions reach the caller via
// pump(). Nothing below blocks.

// Core eval round-trip: register `cb` under a fresh id, then send the raw `code`
// to the renderer (which evals it in the frame's V8 context and replies with
// String(result)). extract/extractAll/click funnel through this with a wrapper.
void BrowserInstance::evalRaw(const std::string& code, ResultCallback cb) {
    if (!impl_) { cb(false, "", "instance gone"); return; }
    CefRefPtr<CefBrowser> b;
    int reqId = 0;
    bool haveBrowser = false;
    {
        std::lock_guard<std::mutex> lock(impl_->state->mu);
        b = impl_->state->browser;
        if (b) {
            reqId = impl_->state->nextEvalId++;
            impl_->state->pendingEvals[reqId] = cb;
            haveBrowser = true;
        }
    }
    if (!haveBrowser) {
        // No page yet: report it through pump() like every other completion.
        impl_->state->post([cb]() { cb(false, "", "no page loaded; call visit() first"); });
        return;
    }
    CefPostTask(TID_UI, base::BindOnce(&SendEvalTask, b, reqId, code));
}

void BrowserInstance::html(ResultCallback cb) {
    if (!impl_) { cb(false, "", "instance gone"); return; }
    CefRefPtr<CefBrowser> b;
    { std::lock_guard<std::mutex> lock(impl_->state->mu); b = impl_->state->browser; }
    CefPostTask(TID_UI, base::BindOnce(&GetSourceTask, b, impl_->state, cb));
}

void BrowserInstance::eval(const std::string& js, ResultCallback cb) {
    evalRaw(js, std::move(cb));
}

void BrowserInstance::extract(const std::string& selector, FoundCallback cb) {
    // Wrapper returns "1\x1f<innerText>" for the first match, or "0" when none.
    std::string code =
        "(function(){var el=document.querySelector(" + jsStringLiteral(selector) + ");"
        "if(!el)return '0';return '1\\x1f'+(el.innerText||'');})()";
    evalRaw(code, [cb](bool ok, const std::string& value, const std::string&) {
        const std::string kFound = std::string("1") + '\x1f';
        if (ok && value.rfind(kFound, 0) == 0) cb(true, value.substr(kFound.size()));
        else cb(false, "");
    });
}

void BrowserInstance::extractAll(const std::string& selector, ListCallback cb) {
    // Wrapper returns "<count>\x1f<innerText joined by \x1f>"; the count prefix
    // keeps empty strings unambiguous.
    std::string code =
        "(function(){var els=[].slice.call(document.querySelectorAll(" + jsStringLiteral(selector) + "));"
        "var t=els.map(function(e){return e.innerText||'';});"
        "return t.length+'\\x1f'+t.join('\\x1f');})()";
    evalRaw(code, [cb](bool ok, const std::string& value, const std::string& error) {
        if (!ok) { cb(false, {}, error); return; }
        const char sep = '\x1f';
        size_t at = value.find(sep);
        if (at == std::string::npos) { cb(false, {}, "malformed extractAll result"); return; }
        const int count = std::atoi(value.substr(0, at).c_str());
        std::vector<std::string> out;
        if (count > 0) {
            std::string rest = value.substr(at + 1);
            size_t start = 0;
            for (int i = 0; i < count; ++i) {
                size_t nxt = (i + 1 < count) ? rest.find(sep, start) : std::string::npos;
                if (nxt == std::string::npos) { out.push_back(rest.substr(start)); break; }
                out.push_back(rest.substr(start, nxt - start));
                start = nxt + 1;
            }
        }
        cb(true, std::move(out), "");
    });
}

void BrowserInstance::click(const std::string& selector, ResultCallback cb) {
    const std::string sel = jsStringLiteral(selector);
    // Real mousedown/mouseup/click sequence so listeners + default actions fire.
    std::string code =
        "(function(){var el=document.querySelector(" + sel + ");"
        "if(!el)throw new Error('selector not found: ' + " + sel + ");"
        "el.dispatchEvent(new MouseEvent('mousedown',{bubbles:true,cancelable:true}));"
        "el.dispatchEvent(new MouseEvent('mouseup',{bubbles:true,cancelable:true}));"
        "el.click();return el.tagName||'OK';})()";
    evalRaw(code, std::move(cb));
}

// ---- Session 19 step 7: offscreen screenshot --------------------------------
// Flush any pending DOM mutation into layout (a forced reflow via offsetHeight),
// then arm a capture and kick the compositor with ONE Invalidate so the renderer
// rasterizes the mutation. A delayed settle-deadline task then encodes the latest,
// settled frame — reflecting the live page (including any eval() mutation). We
// deliberately never re-invalidate on subsequent paints: doing so re-composites the
// stale raster and stops the renderer from repainting the mutation at all.
void BrowserInstance::screenshot(const std::string& path, ResultCallback cb) {
    if (!impl_) { cb(false, "", "instance gone"); return; }
    auto state = impl_->state;  // keep alive across the flush -> capture chain
    // Forced synchronous reflow: reading offsetHeight flushes style+layout.
    evalRaw("(function(){void document.body.offsetHeight;return 'ok';})()",
            [state, path, cb](bool ok, const std::string&, const std::string& err) {
                if (!ok) { cb(false, "", err); return; }  // no page / eval failed
                CefRefPtr<CefBrowser> b;
                bool haveBrowser = false;
                {
                    std::lock_guard<std::mutex> lock(state->mu);
                    b = state->browser;
                    haveBrowser = (bool)b;
                }
                if (!haveBrowser) {
                    state->post([cb]() { cb(false, "", "no page loaded; call visit() first"); });
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(state->mu);
                    state->pendingShot = SharedState::PendingShot{path, cb};
                }
                // ONE Invalidate kicks the compositor to request a BeginFrame so the
                // renderer rasterizes the pending mutation; we never re-invalidate after
                // that (continuous Invalidate suppresses the repaint). The delayed
                // deadline task then encodes the latest, settled frame — which reflects
                // the page (including any eval() mutation) as of the deadline.
                CefPostTask(TID_UI, base::BindOnce(&InvalidateTask, b));
                CefPostDelayedTask(TID_UI, base::BindOnce(&ScreenshotDeadlineTask, state),
                                   static_cast<uint64_t>(kScreenshotSettle.count()));
            });
}

void BrowserInstance::setResourceHandler(ResourceHandler h) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->state->mu);
    impl_->state->resourceHandler = std::move(h);
}

void BrowserInstance::allCookies(const std::string& url, CookieListCallback cb) {
    if (!impl_) { cb(false, {}, "instance gone"); return; }
    auto coll = std::make_shared<CookieCollect>();
    coll->state = impl_->state;
    coll->cb    = std::move(cb);
    CefPostTask(TID_UI, base::BindOnce(&DoVisitAllCookies, coll, url));
}

void BrowserInstance::setCookie(const std::string& url, const Cookie& c, ResultCallback cb) {
    if (!impl_) { cb(false, "", "instance gone"); return; }
    CefPostTask(TID_UI, base::BindOnce(&DoSetCookie, impl_->state, url, c, std::move(cb)));
}

void BrowserInstance::clearCookies(ResultCallback cb) {
    if (!impl_) { cb(false, "", "instance gone"); return; }
    CefPostTask(TID_UI, base::BindOnce(&DoClearCookies, impl_->state, std::move(cb)));
}

void BrowserInstance::pump() {
    if (!impl_) return;
    // Drain the callback queue. Run each thunk OUTSIDE the lock (a thunk may
    // itself call back into the instance).
    for (;;) {
        std::function<void()> thunk;
        {
            std::lock_guard<std::mutex> lock(impl_->state->mu);
            if (impl_->state->callbacks.empty()) break;
            thunk = std::move(impl_->state->callbacks.front());
            impl_->state->callbacks.pop();
        }
        if (thunk) thunk();
    }
}

bool BrowserInstance::isLoading() const {
    return impl_ ? impl_->state->loading.load() : false;
}

std::string BrowserInstance::currentUrl() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->state->mu);
    return impl_->state->currentUrl;
}

int BrowserInstance::lastStatusCode() const {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lock(impl_->state->mu);
    return impl_->state->lastStatus;
}

std::string BrowserInstance::lastTitle() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->state->mu);
    return impl_->state->lastTitle;
}

std::unique_ptr<BrowserInstance> createInstance(const BrowserOptions& opts) {
    if (!g_initialized.load()) return nullptr;
    // unique_ptr with a private ctor: use a tiny helper so the friend declaration
    // on the class is satisfied.
    struct Maker {
        static std::unique_ptr<BrowserInstance> make(const BrowserOptions& o) {
            return std::unique_ptr<BrowserInstance>(new BrowserInstance(o));
        }
    };
    return Maker::make(opts);
}

}  // namespace lucyn::cef_host
