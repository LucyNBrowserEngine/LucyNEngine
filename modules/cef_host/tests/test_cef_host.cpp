// test_cef_host — lightweight CEF host checks (no network, no page load).
//
// Proves the module's contract without touching the network:
//   * cefVersion() reports a real CEF version.
//   * createInstance() returns nullptr before initCef().
//   * initCef() succeeds and createInstance() then returns an instance.
//   * BrowserInstance state defaults are sane (not loading, empty url/title).
//   * shutdownCef() runs clean and createInstance() returns nullptr again.
//
// Page loading (example.com) is exercised by tools/lucyn_cef_smoke, which is a
// tool rather than a ctest so a flaky network can't turn the suite red.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "lucyn/cef_host/browser_host.h"

static int failures = 0;
#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            std::fprintf(stderr, "FAIL: %s (%s)\n", msg, #cond); \
            ++failures;                                         \
        } else {                                                \
            std::fprintf(stderr, "ok: %s\n", msg);              \
        }                                                       \
    } while (0)

int main(int argc, char** argv) {
    // SESSION19 "Process model": subprocess check must be the first thing in
    // main(), before any file I/O (mkdir of the cache dir) or logging.
    const int subprocess_code = lucyn::cef_host::executeCefProcess(argc, argv);
    if (subprocess_code >= 0) return subprocess_code;  // this IS a CEF subprocess

    // Private cache dir: avoids the CEF process-singleton lock colliding with
    // a concurrent smoke run or another ctest.
    std::string tmpl = "/tmp/lucyn-cef-test-XXXXXX";
    if (char* d = mkdtemp(tmpl.data())) {
        setenv("LUCYN_CEF_CACHE", d, 1);
    }

    // Version is available before any init.
    const std::string ver = lucyn::cef_host::cefVersion();
    CHECK(!ver.empty(), "cefVersion() non-empty before init");
    std::fprintf(stderr, "CEF version: %s\n", ver.c_str());

    // Before init: no instances, isCefInitialized false.
    CHECK(!lucyn::cef_host::isCefInitialized(), "not initialised before initCef");
    CHECK(lucyn::cef_host::createInstance() == nullptr,
          "createInstance() is nullptr before initCef");

    // Init.
    CHECK(lucyn::cef_host::initCef(argc, argv), "initCef() succeeds");
    CHECK(lucyn::cef_host::isCefInitialized(), "isCefInitialized() true after init");

    // Instance + default state.
    auto inst = lucyn::cef_host::createInstance();
    CHECK(inst != nullptr, "createInstance() returns an instance after init");
    if (inst) {
        CHECK(!inst->isLoading(), "new instance is not loading");
        CHECK(inst->currentUrl().empty(), "new instance has empty url");
        CHECK(inst->lastStatusCode() == 0, "new instance status == 0");
        CHECK(inst->lastTitle().empty(), "new instance has empty title");
        // pump() with nothing queued must be a no-op (no crash).
        inst->pump();
        CHECK(true, "pump() on an idle instance is safe");
        inst.reset();
    }

    // Shutdown.
    lucyn::cef_host::shutdownCef();
    CHECK(!lucyn::cef_host::isCefInitialized(), "not initialised after shutdownCef");
    CHECK(lucyn::cef_host::createInstance() == nullptr,
          "createInstance() is nullptr after shutdownCef");

    // Double-shutdown must be safe.
    lucyn::cef_host::shutdownCef();
    CHECK(true, "double shutdownCef() is safe");

    if (failures) {
        std::fprintf(stderr, "%d check(s) FAILED\n", failures);
        return 1;
    }
    std::fprintf(stderr, "all cef_host checks passed\n");
    return 0;
}
