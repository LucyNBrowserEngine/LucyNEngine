// lucyn-cef-smoke — Session 19 steps 2-4: prove CEF initialises, creates a real
// off-screen browser, loads a URL, and fires OnLoadEnd with a page title.
//
//   ./bin/lucyn-cef-smoke [url]        (default https://example.com)
//
// It sets a private cache dir (LUCYN_CEF_CACHE) so it never contends with the
// lucyn binary or another run for CEF's process-singleton lock, initialises CEF,
// visits the URL, then pumps the callback queue until OnLoadEnd/OnLoadError
// posts the completion thunk. The title is printed to stderr from inside that
// callback (i.e. it arrived via OnLoadEnd, not polled separately).
//
// Exit code: 0 = page loaded and a title was reported; 1 = failure.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "lucyn/cef_host/browser_host.h"

int main(int argc, char** argv) {
    // SESSION19 "Process model": this MUST be the first thing in main(). CEF
    // re-execs this binary as gpu/renderer subprocesses; they run main() again
    // and must exit here instead of executing the browser-path logic below.
    const int subprocess_code = lucyn::cef_host::executeCefProcess(argc, argv);
    if (subprocess_code >= 0) return subprocess_code;  // this IS a subprocess

    const std::string url = (argc > 1) ? argv[1] : "https://example.com";

    // Private CEF cache so concurrent runs never fight over the singleton lock.
    // (A real invocation would use the shared nothing-data/cef; this tool isolates.)
    std::string cache_template = "/tmp/lucyn-cef-smoke-XXXXXX";
    std::string cacheDir;
    if (char* d = mkdtemp(cache_template.data())) {
        cacheDir = d;
        setenv("LUCYN_CEF_CACHE", cacheDir.c_str(), 1);
    }

    // Step 2: initCef — must succeed or nothing else is real.
    if (!lucyn::cef_host::initCef(argc, argv)) {
        std::fprintf(stderr, "[smoke] initCef FAILED\n");
        return 1;
    }
    std::fprintf(stderr, "[smoke] CEF initialised, version %s\n",
                 lucyn::cef_host::cefVersion().c_str());

    // Step 3: createInstance.
    auto inst = lucyn::cef_host::createInstance();
    if (!inst) {
        std::fprintf(stderr, "[smoke] createInstance FAILED (nullptr)\n");
        lucyn::cef_host::shutdownCef();
        return 1;
    }

    // Step 4: load the page. The callback fires from pump() after OnLoadEnd (or
    // OnLoadError) posted it from CEF's UI thread.
    bool done   = false;
    bool ok     = false;
    int status  = 0;
    std::string title;
    std::string error;

    std::fprintf(stderr, "[smoke] visiting %s\n", url.c_str());
    inst->visit(url, [&](bool succeeded, int statusCode, const std::string& t, const std::string& err) {
        done  = true;
        ok    = succeeded;
        status = statusCode;
        title = t;
        error = err;
    });

    // Pump until the load settles or we time out. 30s budget: CEF first-launch
    // plus a real network fetch.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!done && std::chrono::steady_clock::now() < deadline) {
        inst->pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    inst->pump();  // one last drain in case it landed on the final tick

    if (!done) {
        std::fprintf(stderr, "[smoke] TIMEOUT: no OnLoadEnd within 30s (loading=%d)\n",
                     inst->isLoading() ? 1 : 0);
        lucyn::cef_host::shutdownCef();
        return 1;
    }

    if (!ok) {
        std::fprintf(stderr, "[smoke] OnLoadError: %s\n", error.c_str());
        lucyn::cef_host::shutdownCef();
        return 1;
    }

    // The proof the task asked for: title, from OnLoadEnd, on stderr.
    std::fprintf(stderr, "[smoke] OnLoadEnd: status=%d title=\"%s\" url=%s\n",
                 status, title.c_str(), inst->currentUrl().c_str());

    // ---- Steps 5-6: page operations through the renderer eval round-trip ----
    // Validates html / eval / extract / extractAll / click against the real page
    // before the controller depends on them.
    auto waitFor = [&](std::function<bool()> pred, int timeoutMs) -> bool {
        const auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!pred() && std::chrono::steady_clock::now() < dl) {
            inst->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        inst->pump();
        return pred();
    };

    int opFail = 0;

    bool d = false, eok = false;
    std::string val, err;
    inst->html([&](bool ok2, const std::string& v, const std::string& e) { d = true; eok = ok2; val = v; err = e; });
    waitFor([&] { return d; }, 15000);
    const bool htmlOk = d && eok && val.find("<html") != std::string::npos;
    std::fprintf(stderr, "[smoke] html: ok=%d len=%zu \"%.40s\" -> %s\n",
                 eok ? 1 : 0, val.size(), val.c_str(), htmlOk ? "ok" : "FAIL");
    if (!htmlOk) ++opFail;

    d = false; eok = false; val.clear(); err.clear();
    inst->eval("document.title", [&](bool ok2, const std::string& v, const std::string& e) { d = true; eok = ok2; val = v; err = e; });
    waitFor([&] { return d; }, 15000);
    const bool evalOk = d && eok && val == title;
    std::fprintf(stderr, "[smoke] eval(document.title) = \"%s\" (expect \"%s\") -> %s\n",
                 val.c_str(), title.c_str(), evalOk ? "ok" : "FAIL");
    if (!evalOk) ++opFail;

    bool found = false;
    d = false; val.clear();
    inst->extract("p", [&](bool f, const std::string& v) { d = true; found = f; val = v; });
    waitFor([&] { return d; }, 15000);
    const bool extOk = d && found && !val.empty();
    std::fprintf(stderr, "[smoke] extract(p) found=%d text=\"%.40s\" -> %s\n",
                 found ? 1 : 0, val.c_str(), extOk ? "ok" : "FAIL");
    if (!extOk) ++opFail;

    bool aok = false;
    std::vector<std::string> vals;
    d = false;
    inst->extractAll("p", [&](bool ok2, const std::vector<std::string>& v, const std::string&) { d = true; aok = ok2; vals = v; });
    waitFor([&] { return d; }, 15000);
    const bool allOk = d && aok && !vals.empty() && !vals[0].empty();
    std::fprintf(stderr, "[smoke] extractAll(p) count=%zu first=\"%.40s\" -> %s\n",
                 vals.size(), vals.empty() ? "" : vals[0].c_str(), allOk ? "ok" : "FAIL");
    if (!allOk) ++opFail;

    d = false; eok = false; val.clear(); err.clear();
    inst->click("body", [&](bool ok2, const std::string& v, const std::string& e) { d = true; eok = ok2; val = v; err = e; });
    waitFor([&] { return d; }, 15000);
    const bool clickOk = d && eok && !val.empty();
    std::fprintf(stderr, "[smoke] click(body) tag=\"%s\" err=\"%s\" -> %s\n",
                 val.c_str(), err.c_str(), clickOk ? "ok" : "FAIL");
    if (!clickOk) ++opFail;

    inst.reset();
    lucyn::cef_host::shutdownCef();
    if (opFail) {
        std::fprintf(stderr, "[smoke] %d page-op(s) FAILED\n", opFail);
        return 1;
    }
    std::fprintf(stderr, "[smoke] OK (load + html/eval/extract/extractAll/click)\n");
    return 0;
}
