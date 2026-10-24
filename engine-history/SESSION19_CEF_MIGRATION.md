# Session 19 — Replace the engine with CEF

Read `HANDOFF.md` and `SESSION18.md` first. Then read this entire file before touching a single line of code.

## What this session is

We are ripping out the old browser engine (QuickJS, libcurl, libcurl-impersonate, modules/dom, modules/shell,
modules/js, modules/net, modules/layout, modules/paint) and replacing it with CEF (Chromium Embedded Framework).

The goal at the end of this session: `./bin/lucyn` accepts the same JSON stdin/stdout commands it always has
(`visit`, `eval`, `extract`, `extractAll`, `html`, `screenshot`, `click`, `waitFor`, `waitIdle`, `download`,
`search`, `cookies`, `setCookie`, `clearCookies`, `info`, `quit`) but Chromium is doing ALL the heavy lifting
underneath. Real JS. Real DOM. Real rendering. Multiple concurrent browser instances (up to 3-4 per run).

---

## What DIES — delete these, no ceremony

| Path | Why |
|---|---|
| `modules/dom/` | CEF has a real DOM via CefDOMVisitor |
| `modules/shell/` | Hydrator/ScriptRegistry replaced by CefV8Handler injection |
| `modules/js/` | QuickJS gone, V8 is Chromium's engine now |
| `modules/net/` | HttpClient/libcurl gone, CEF network stack takes over |
| `modules/layout/` | CEF renders, litehtml gone |
| `modules/paint/` | CEF renders offscreen, stb gone |
| `third_party/quickjs/` | Gone |
| `third_party/curl-impersonate/` | Gone |
| `third_party/stb/` | Gone |
| `third_party/fonts/` | Gone |
| `third_party/litehtml/` | Gone |

Do NOT delete yet — do it after the new build wiring is confirmed to compile.

---

## What SURVIVES — do not touch these

| Path | Status |
|---|---|
| `modules/security/` | NULLBYTE is pure crypto, zero changes |
| `modules/server/` | TinyElysia/dashboard survives intact |
| `modules/core/` | dataRoot(), exeDir() still needed |
| `modules/search/` | Provider registry, URL building, untouched |
| `modules/adblock/` | Keep for now, wire into CEF request handler later |
| `modules/profile/` | Keep the identity store, profiles still make sense |
| `client/src/index.js` | Node client is protocol-level, survives completely |
| `apps/lucyn/main.cpp` | Gutted and rewritten but the file stays |

Moonbug (`modules/net/moonbug.h/.cpp`) — keep the DATA STRUCTURES and the Capture struct.
Delete HttpClient. Keep Moonbug. It rehooks into CEF's CefResourceRequestHandler.

---

## CEF version to use

Use **CEF 120 or newer** (stable branch). Download the binary distribution for Linux x86_64 from:
https://cef-builds.spotifyopen.com/index.html

The binary distribution gives you:
- `libcef.so` (the main library)
- `libcef_dll_wrapper` (the C++ wrapper you link against)
- All the Chromium resource files, ICU data, V8 snapshots

Put it at `third_party/cef/`. The CMake find logic goes in `cmake/FindCEF.cmake`.

CEF binary distro has its own `CMakeLists.txt` inside it. Use it. Do not reinvent it.

---

## Process model — READ THIS CAREFULLY

CEF is multi-process. When your binary starts, CEF may re-exec itself as a renderer/gpu/utility subprocess.
This MUST be handled in `main()` before anything else or nothing works.

```cpp
// apps/lucyn/main.cpp — this must be the FIRST thing in main()
CefMainArgs main_args(argc, argv);
int exit_code = CefExecuteProcess(main_args, nullptr, nullptr);
if (exit_code >= 0) return exit_code; // this IS a subprocess, exit now
// only the browser process continues past here
```

If you forget this the whole thing hangs or crashes immediately. It is not optional.

---

## New module: `modules/cef_host/`

This is the only new module. It wraps CEF and exposes what the controller needs.
Shape it like every other module: `include/lucyn/cef_host/`, `src/`, `tests/`, `CMakeLists.txt`.

### Public API (`include/lucyn/cef_host/browser_host.h`)

```cpp
namespace lucyn::cef_host {

struct BrowserOptions {
    int viewportWidth  = 1280;
    int viewportHeight = 800;
    std::string userAgent; // from profile if set
    bool javascript = true;
    bool images     = true;
};

// One scraping instance. Create up to ~4 of these per process.
class BrowserInstance {
public:
    // All callbacks fire on the controller thread via a thread-safe queue.
    // None of these block — results come back through the queue.

    void visit(const std::string& url);
    void eval(const std::string& js, std::function<void(std::string result, std::string error)>);
    void extractText(const std::string& selector, std::function<void(std::string)>);
    void extractAll(const std::string& selector, std::function<void(std::vector<std::string>)>);
    void html(std::function<void(std::string)>);
    void click(const std::string& selector);
    void screenshot(const std::string& path, std::function<void(std::string path, std::string error)>);
    void waitFor(const std::string& selector, int timeoutMs, std::function<void(bool found)>);
    void setCookie(const std::string& url, const std::string& name,
                   const std::string& value, std::function<void(bool)>);
    void cookies(const std::string& url, std::function<void(std::string json)>);
    void clearCookies(std::function<void()>);

    // Pump pending callbacks — call this from the controller's command loop
    void pump();

    bool isLoading() const;
    std::string currentUrl() const;
    int lastStatusCode() const;
};

// Call once at process start (after CefExecuteProcess check).
bool initCef(int argc, char** argv);

// Call once at process end.
void shutdownCef();

// Create a browser instance. Returns nullptr if CEF not initialised.
std::unique_ptr<BrowserInstance> createInstance(const BrowserOptions& opts = {});

} // namespace lucyn::cef_host
```

### Key CEF classes you implement inside cef_host

**`LucynApp : public CefApp, public CefBrowserProcessHandler`**
- `OnBeforeCommandLineProcessing` — set headless flags:
  `--disable-gpu`, `--disable-gpu-compositing`, `--enable-begin-frame-scheduling`,
  `--disable-dev-shm-usage` (important on Linux)
- `GetBrowserProcessHandler` returns `this`

**`LucynClient : public CefClient, public CefLoadHandler, public CefDisplayHandler,
                  public CefRequestHandler, public CefResourceRequestHandler,
                  public CefRenderHandler`**
- `OnLoadEnd` — fire the visit completion callback, grab status code
- `OnLoadError` — fire visit error callback
- `GetRenderHandler` returns `this` (needed for offscreen/screenshot)
- `GetPaint` — CEF calls this with the rendered pixels; copy into a buffer for screenshot
- `OnBeforeResourceLoad` / `OnResourceLoadComplete` — THIS IS WHERE MOONBUG HOOKS IN
  Create a `lucyn::Capture` from the CEF request/response and fire Moonbug callbacks

**`LucynRenderHandler : public CefRenderHandler`**
- `GetViewRect` — return your viewport size
- `OnPaint` — receive pixel buffer, encode to PNG with a minimal encoder
  (stb_image_write is gone but you can use libpng which is always on Linux, or re-vendor
  just stb_image_write.h — it is a single header, 1200 lines, MIT licensed, worth keeping)

**`LucynV8Handler : public CefV8Handler`** (in the RENDER process)
- Inject a `lucyn` object with any native hooks you want pages to call
- This runs in the renderer subprocess — use CefV8Context carefully

### Thread model inside cef_host

CEF has its own thread pool. Rules:
- CefBrowser methods: call from any thread, CEF posts to the right one
- CefV8 / DOM visitors: ONLY from the renderer thread (CefV8Context::GetCurrentContext)
- UI thread on Linux = CEF's browser process main thread
- Your controller command loop runs on the main thread; use `CefPostTask(TID_UI, ...)` to
  dispatch into CEF, and a `std::queue` + `std::mutex` to get results back out

`BrowserInstance::pump()` drains that result queue and fires the stored callbacks.
The controller calls `pump()` in its idle loop between commands.

---

## Rewrite `modules/controller/`

The controller keeps the same JSON stdin/stdout loop. The internals change:

**Old:** controller → QuickJsHost → libcurl  
**New:** controller → BrowserInstance → CEF

Each command maps to a BrowserInstance method. The controller can hold a pool:

```cpp
// Up to 4 instances, lazily created
std::vector<std::unique_ptr<cef_host::BrowserInstance>> instances_;
```

For now, single-instance commands (visit, eval, extract etc.) use `instances_[0]`.
Multi-instance scraping (`{"cmd":"visitAll","urls":[...]}`) is a stretch goal for this session
— get single instance working first, then parallelise.

Commands that map trivially:
- `visit` → `instance->visit(url)` then wait for OnLoadEnd callback
- `eval` → `instance->eval(js, cb)`
- `extract` → `instance->extractText(selector, cb)`
- `extractAll` → `instance->extractAll(selector, cb)`
- `html` → `instance->html(cb)`
- `click` → `instance->eval("document.querySelector('"+esc(sel)+"')?.click()", cb)`
- `screenshot` → `instance->screenshot(path, cb)`
- `waitFor` → `instance->waitFor(selector, timeout, cb)`
- `cookies` → `instance->cookies(url, cb)`
- `setCookie` → `instance->setCookie(...)`
- `clearCookies` → `instance->clearCookies(cb)`
- `download` → visit the URL, intercept via OnBeforeResourceLoad, stream body to disk
- `search` → use modules/search to build URL, then `visit`
- `info` → return CEF version, current URL, loading state
- `quit` → CefQuitMessageLoop(), shutdownCef()

---

## Moonbug rehook

In `LucynClient::OnResourceLoadComplete`:

```cpp
void LucynClient::OnResourceLoadComplete(CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request,
    CefRefPtr<CefResponse> response, URLRequestStatus status,
    int64 received_content_length) {

    lucyn::Capture cap;
    cap.url     = request->GetURL().ToString();
    cap.method  = request->GetMethod().ToString();
    cap.status  = response->GetStatus();
    cap.bytes   = received_content_length;
    // response->GetHeaderMap() -> cap.responseHeaders
    // request->GetHeaderMap()  -> cap.requestHeaders

    if (moonbug_) moonbug_->record(cap);
}
```

Moonbug data structures (`Capture`, `Moonbug` class) survive. Only the feed point moves.

---

## CMake changes

Root `CMakeLists.txt`:

```cmake
# REMOVE these add_subdirectory calls:
# modules/dom, modules/shell, modules/js, modules/net,
# modules/layout, modules/paint

# ADD:
include(cmake/FindCEF.cmake)
add_subdirectory(modules/cef_host)

# controller now links lucyn::cef_host instead of lucyn::js etc.
```

`cmake/FindCEF.cmake` — finds `third_party/cef/`, sets up include paths,
links `libcef_dll_wrapper`, adds the `libcef.so` rpath.

CEF also needs its resource files (icudtl.dat, v8_context_snapshot.bin, etc.)
copied next to the binary at install/run time. Add a CMake custom command for this.

---

## CEF headless flags for Linux (Deepin/Debian)

These go in `OnBeforeCommandLineProcessing`:

```
--headless=new              (CEF 112+ headless mode, no display needed)
--disable-gpu
--disable-gpu-compositing
--disable-software-rasterizer
--disable-dev-shm-usage     (critical: /dev/shm is often too small in containers)
--no-sandbox                (only if running as root; prefer --disable-setuid-sandbox)
--disable-setuid-sandbox
--single-process            DO NOT USE — unsupported and breaks sites
--disable-extensions
--disable-background-networking
--disable-sync
--disable-translate
--hide-scrollbars
--mute-audio
```

---

## eval / DOM access pattern

CEF's equivalent of your old `jsHost_->eval()`:

```cpp
// Must be called from the RENDERER thread via CefPostTask or a CefV8Handler
CefRefPtr<CefFrame> frame = browser_->GetMainFrame();
frame->ExecuteJavaScript(code, frame->GetURL(), 0);
```

For return values, use the message-passing pattern:
- In renderer: `CefV8Context::GetCurrentContext()->Eval(...)` → result → `CefProcessMessage` → browser process
- In browser process: `OnProcessMessageReceived` → fire your callback

This is the standard CEF pattern. Do not fight it.

For `extract(selector)`:
```cpp
std::string js = R"(
    (function() {
        var el = document.querySelector(')" + escapedSelector + R"(');
        return el ? el.innerText : '';
    })()
)";
frame->ExecuteJavaScript(js, ...);
```

---

## screenshot via offscreen rendering

CEF offscreen rendering = `CefWindowInfo::SetAsWindowless(0)` + `CefBrowserSettings` with
`windowless_frame_rate = 1` (we only need one frame per screenshot, not 60fps).

`OnPaint` receives a `const void* buffer` of BGRA pixels at `width × height`.
Convert BGRA → RGBA, then encode to PNG. Re-vendor ONLY `stb_image_write.h` (single header,
zero other deps) into `third_party/stb/stb_image_write.h` — it is the smallest PNG encoder
available and you already know how it works.

---

## What to build FIRST (order matters)

1. `cmake/FindCEF.cmake` + download CEF binary distro to `third_party/cef/` + confirm it links
2. `apps/lucyn/main.cpp` — add `CefExecuteProcess` at the top, confirm subprocesses work
3. `modules/cef_host/` skeleton — `initCef()`, `shutdownCef()`, `createInstance()`, nothing else
4. Get a blank browser to load `https://example.com` and call `OnLoadEnd` — confirm CEF works
5. Wire `visit` + `html` commands in the controller — confirm end to end
6. Wire `eval`, `extract`, `extractAll`, `click`
7. Wire `screenshot` (offscreen rendering)
8. Rehook Moonbug into `OnResourceLoadComplete`
9. Wire remaining commands (`cookies`, `download`, `search`, `waitFor`, `waitIdle`)
10. Delete the dead modules

Do NOT attempt steps 8-10 before 1-5 are green. CEF init problems will waste your entire session.

---

## Known hard parts (do not get surprised)

- **CEF message loop on Linux**: call `CefRunMessageLoop()` — this blocks. Your JSON stdin loop
  must run on a separate thread that posts commands into CEF via `CefPostTask(TID_UI, ...)`.
  This is the biggest architecture change from the old synchronous controller.

- **Renderer subprocess**: CEF will re-exec your binary with `--type=renderer` etc. The
  `CefExecuteProcess` check at the top of main handles this. Your main.cpp must not do ANYTHING
  (no file I/O, no logging, no nothing) before that check.

- **Resource files**: `icudtl.dat` must be next to the binary or CEF crashes with a cryptic
  locale error. The CMake install step must copy all of `third_party/cef/Resources/` and
  `third_party/cef/Release/` next to `bin/lucyn`.

- **`/dev/shm` on Deepin**: if CEF crashes immediately on Linux, add `--disable-dev-shm-usage`.
  Deepin defaults may have a small `/dev/shm`.

---

## What "done" means for this session

- `ctest`: all SURVIVING suites pass (security, server, core, search, profile, adblock)
- `./bin/lucyn` accepts JSON commands and a real `visit` loads a real page through Chromium
- `eval("document.title")` returns the actual page title
- `extract("h1")` returns actual text
- `screenshot` produces a real PNG of the page
- Moonbug captures show up in TinyElysia dashboard
- The Node client (`client/src/index.js`) works unchanged against the new binary

---

## Opening message for Session 20

> "Read HANDOFF.md and SESSION19_CEF_MIGRATION.md first. CEF is now the engine.
> Here is what works / what broke: [paste]. Next I want: [parallelism / cookies / download / whatever]."
