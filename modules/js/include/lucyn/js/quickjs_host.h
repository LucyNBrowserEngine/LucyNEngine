// lucyn/js/quickjs_host.h - the real shell::ScriptHost: QuickJS + a DOM bridge over lucyn::dom.
//
// Call order (driven by shell::Hydrator, see shell/host.h):
//   attach -> runUserScript(start...) -> [site is mounted] -> runPageScripts -> settle -> runUserScript(end...) -> detach
//
// What a page sees: window/document/Node/Element/Event/MutationObserver/timers/location/history/navigator/screen/console.
// What only injected scripts see: `lucyn.*` (the NativeHooks). It is a parameter of the function every script is wrapped in,
// so it is never a global, never on `window`, never enumerable, and never prints anything.
#pragma once
#include <functional>
#include <vector>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <lucyn/profile/identity.h>
#include <lucyn/shell/host.h>

namespace lucyn::js {

// Who the page believes it is. Read at every attach(), so a page always matches the CURRENT persona.
struct Persona {
    profile::Surface surface;
    profile::Device  device;
};

// Generic HTTP GET result — mirrors net::Response but keeps js/ independent of net/.
struct HttpGetResult {
    long        status = 0;
    std::string body;
    std::vector<std::pair<std::string,std::string>> headers;
    std::string effectiveUrl;
    std::string error;   // empty on success
    // Optional extras. A callback that does not know them leaves them at their defaults:
    int         redirects = 0;      // hops followed (fetch's `redirected`, XHR's responseURL)
    std::string statusText;         // reason phrase from the status line; empty on HTTP/2+
    std::string httpVersion;        // "1.1" | "2" | "3" | "" (unknown: statusText falls back to the standard phrase)
};

// A request issued BY page code (fetch(), XMLHttpRequest, <script src>). Mirrors net::RequestOptions
// without making js/ depend on net/.
struct HttpRequest {
    std::string method = "GET";
    std::string url;                // absolute
    std::string body;
    bool        hasBody = false;
    std::vector<std::pair<std::string,std::string>> headers;   // as set by the page (may be empty)
    std::string pageUrl;            // the document making the request (Referer / Origin / Sec-Fetch-Site)
    std::string mode = "cors";      // "cors" | "no-cors" | "same-origin"
    std::string dest = "empty";     // "empty" | "script" | ...
};

struct HostOptions {
    int    scriptTimeoutMs  = 10000;        // per script / timer callback / microtask drain. <= 0: no limit
    size_t memoryLimitBytes = 256u << 20;   // per page. 0: no limit
    size_t stackLimitBytes  = 1u << 20;
    // Empty: navigator/screen exist but are blank (LucyN never invents an identity on its own).
    std::function<std::optional<Persona>()> persona;
    // HTTP GET callback (legacy). When set, window.fetch() and <script src> downloading are enabled.
    // The callback is called on a background thread; it must be thread-safe.
    std::function<HttpGetResult(const std::string& url)> httpGet;
    // Full HTTP callback (POST, custom headers, etc.). When set it takes precedence over httpGet.
    // method = any HTTP method; body is empty when there is none; headers = extra request headers.
    // Called on a background thread; must be thread-safe.
    std::function<HttpGetResult(const std::string& method, const std::string& url,
                                const std::string& body,
                                const std::vector<std::pair<std::string,std::string>>& headers)> httpRequest;
    // Preferred HTTP callback: gets the whole request including page context, so the host can send the
    // browser's subresource headers (Sec-Fetch-*, Referer, Origin) and attach cookies. Takes precedence over
    // httpRequest and httpGet. Called on a background thread; must be thread-safe.
    std::function<HttpGetResult(const HttpRequest&)> httpFetch;
    // document.cookie. Both are called on the JS thread with the document's current URL.
    //   cookieGet -> "a=1; b=2" (no HttpOnly cookies);  cookieSet <- "name=value; Path=/; ..." (document.cookie = ...)
    // Unset: document.cookie works as a simple per-page in-memory jar (nothing is sent on requests).
    std::function<std::string(const std::string& pageUrl)> cookieGet;
    std::function<void(const std::string& pageUrl, const std::string& cookie)> cookieSet;
    // Page console output. Empty: the page's console is silent. (`lucyn.log` is separate and always goes to the hook.)
    std::function<void(const std::string& level, const std::string& line)> onConsole;
};

namespace detail { struct State; }

class QuickJsHost : public shell::ScriptHost {
public:
    explicit QuickJsHost(HostOptions opts = {});
    ~QuickJsHost() override;
    QuickJsHost(const QuickJsHost&) = delete;
    QuickJsHost& operator=(const QuickJsHost&) = delete;

    bool available() const override { return true; }
    bool attach(dom::Document& shell, const std::string& pageUrl, shell::NativeHooks& hooks, std::string* err) override;
    shell::ScriptOutcome runUserScript(const shell::UserScript& script) override;
    shell::ScriptOutcome runPageScripts() override;
    bool settle(int timeoutMs) override;
    void detach() override;      // safe even when the Document has already been destroyed

    // Evaluate JS in the live context (only valid between attach and detach).
    // Returns true on success; *result = JS string representation of the value.
    // Returns false on error; *error = readable error string.
    bool eval(const std::string& code, std::string* result, std::string* error = nullptr);

    // Replace the NativeHooks pointer without detaching. Called by the controller to swap the
    // Hydrator's stack-local hooks with a longer-lived object after hydrate() returns.
    void replaceHooks(shell::NativeHooks& newHooks);

    // Uncaught errors from timers, listeners, microtasks and page scripts (newest last, at most 100).
    // They survive detach() so a tool can print them after the run.
    const std::vector<std::string>& pageErrors() const { return errors_; }
    // Things LucyN could not do and says so ("<script src> not loaded", "navigation ignored", ...).
    const std::vector<std::string>& notes() const { return notes_; }

private:
    HostOptions opts_;
    std::unique_ptr<detail::State> p_;
    std::vector<std::string> errors_, notes_;
};

}  // namespace lucyn::js
