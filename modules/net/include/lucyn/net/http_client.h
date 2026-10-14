// lucyn/net/http_client.h - libcurl wrapper that follows a profile::Switcher.
//
// The net module does NOT own the identity. It listens to a shared Switcher:
// whenever anyone (controller, ui, a test) selects a new profile, the client
// builds a fresh curl handle for it. Nothing in here is "switch" logic.
#pragma once
#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <lucyn/net/moonbug.h>
#include <lucyn/profile/profile.h>
#include <request.h>

namespace lucyn::net {

struct Response {
    long        status = 0;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;   // final hop only
    std::string effectiveUrl;
    double      totalTime = 0;                                   // seconds
    std::string error;                                           // empty on success
    bool        blocked = false;                                 // request filter prevented network I/O
    int         redirects = 0;                                   // hops libcurl followed (fetch's `redirected`)
    std::string statusText;                                      // reason phrase of the final status line; empty on HTTP/2+ (as in Chrome)
    std::string httpVersion;                                     // "1.0" | "1.1" | "2" | "3" | "" if unknown
    unsigned long long bodyBytes = 0;                            // bytes of body received (== body.size() unless a bodySink was used)
    bool ok() const { return error.empty(); }
};

// One cookie in the client's jar. `expires` is unix seconds, 0 = session cookie.
struct Cookie {
    std::string domain;          // without the leading dot
    std::string path = "/";
    std::string name;
    std::string value;
    bool        hostOnly = true; // true: sent to exactly `domain`; false: also to its subdomains
    bool        secure   = false;
    bool        httpOnly = false;
    long long   expires  = 0;
};

// A request in full. Plain get()/post() are thin wrappers over HttpClient::request().
struct RequestOptions {
    std::string method = "GET";            // any token; upper-cased by request()
    std::string body;
    bool        hasBody = false;           // true even for an empty body (POST with "")
    // Merged on top of the browser's own headers BY NAME (the original header keeps its place in the
    // fingerprint-ordered list, only its value changes). An empty value sends the header empty.
    HeaderList  headers;
    std::vector<std::string> removeHeaders;   // headers to drop from the browser's default set

    // Page context. Empty pageUrl = top-level navigation: the browser's navigation headers are left alone.
    // Non-empty = a request made BY the page at pageUrl (XHR, fetch, <script src>): Accept, Sec-Fetch-*,
    // Referer and Origin are set the way the browser sets them for subresources, and the navigation-only
    // headers (Upgrade-Insecure-Requests, Sec-Fetch-User) are removed.
    std::string pageUrl;
    std::string fetchMode = "cors";        // Sec-Fetch-Mode: "cors" | "no-cors" | "same-origin"
    std::string fetchDest = "empty";       // Sec-Fetch-Dest: "empty" | "script" | "style" | "image" ...
    adblock::RequestType requestType = adblock::RequestType::Document;

    // ---- Streaming (used by downloads) -------------------------------------------------------------
    // If set, the body of a 2xx response is handed to `bodySink` chunk by chunk instead of being collected
    // in Response::body (which stays empty; Response::bodyBytes has the count). Return false to abort the
    // transfer: request() then returns an error. Bodies of non-2xx responses are DISCARDED, never passed
    // to the sink, so an error page can never end up in a file. Runs on the calling thread.
    std::function<bool(const char* data, size_t len)> bodySink;
    // Time limit for the whole transfer in seconds. -1 = the client's default (30 s). 0 = no limit; a
    // transfer that stalls (under 1 byte/s for 60 s) is still aborted. Restored after the request.
    long timeoutSec = -1;
};

// What request() will do to the browser's default headers for this request. Public so it can be tested
// without a network.
struct HeaderPlan {
    HeaderList               set;          // name -> value overrides / additions
    std::vector<std::string> remove;       // names to drop
};
HeaderPlan planHeaders(const std::string& url, const RequestOptions& o, const HeaderList& persona);

// Headers every request from a client should carry for the active identity (Accept-Language, ...).
// `impersonating` tells the provider whether the library already supplies a full browser header set.
using HeaderProvider = std::function<HeaderList(bool impersonating)>;

// Options for HttpClient::post().  Defined outside HttpClient so GCC 13 accepts
// a default-argument of PostOptions{} in the post() declaration.
struct PostOptions {
    std::string contentType = "application/x-www-form-urlencoded";
    std::vector<std::pair<std::string,std::string>> extraHeaders;
};

class HttpClient {
public:
    using RequestFilter = std::function<std::string(const std::string& url,
                                                    const std::string& sourceUrl,
                                                    adblock::RequestType type)>;
    // Follows `sw` (which must outlive the client). Installs a validator, so
    // sw.select() refuses targets the linked libcurl cannot impersonate, and a
    // listener, so the curl handle is rebuilt on every successful switch.
    explicit HttpClient(profile::Switcher& sw);
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // True if we were linked against libcurl-impersonate (vs stock libcurl).
    static bool impersonationBuiltIn();

    // Asks the linked library whether it accepts this profile. Works for ANY
    // target it knows, including ones newer than the profile table.
    static bool probe(const profile::Profile& p, std::string* err = nullptr);

    // Is the live handle really wearing the switcher's current profile?
    // False on stock libcurl, or if the library rejected the initial profile.
    bool impersonating() const { return impersonating_; }
    const std::string& lastError() const { return lastError_; }

    Response get(const std::string& url);

    Response post(const std::string& url, const std::string& body, const PostOptions& opts = PostOptions{});

    // The general entry point: any method, body, headers, page context. Cookies are handled by libcurl's
    // engine (stored from Set-Cookie on every hop, sent back on matching requests).
    Response request(const std::string& url, const RequestOptions& opts);

    // Identity headers. Called for every request, outside the client's lock. nullptr = none.
    void setHeaderProvider(HeaderProvider p);
    // Return an empty string to allow, or the matching rule/reason to block.
    void setRequestFilter(RequestFilter filter);

    // ---- Cookie jar (lives in the curl handle, so a profile switch starts with an empty jar) ----
    std::vector<Cookie> cookies();                                       // everything, HttpOnly included
    // The cookies that would be sent to `url`, in the order they are sent (longest path first).
    std::vector<Cookie> cookiesFor(const std::string& url, bool includeHttpOnly = true);
    // "a=1; b=2" for `url`, exactly what document.cookie returns: HttpOnly cookies are left out.
    std::string         cookieStringFor(const std::string& url, bool includeHttpOnly = false);
    // document.cookie = "..." : parsed and validated like a browser does (rejects HttpOnly, bad Domain,
    // Secure on http, __Secure-/__Host- violations, overwriting an HttpOnly cookie). True if stored/deleted.
    bool                setCookieFromScript(const std::string& url, const std::string& cookieString);
    // Stores `c` as given (an expired `expires` deletes). For importing a session.
    bool                setCookie(const Cookie& c);
    void                clearCookies();

    // Persistence: export the jar as Netscape cookie-file lines (one per line, no header).
    std::string         exportCookieFile();
    // Import cookies from a Netscape cookie-file string (lines from exportCookieFile).
    // Existing cookies are kept; imported ones are merged on top (same name+domain+path = replace).
    void                importCookieFile(const std::string& text);

    // Moonbug: when set, every get() is captured (request headers as sent, response, timing, errors)
    // and handed to the tap's listeners. nullptr (the default) = no capture and no extra libcurl work.
    // The tap must outlive the client, or be detached with setMoonbug(nullptr) first.
    void     setMoonbug(Moonbug* m) { moonbug_.store(m); }
    Moonbug* moonbug() const        { return moonbug_.load(); }

private:
    void rebuild(const profile::Profile& p);

    profile::Switcher&    sw_;
    void*                 easy_ = nullptr;   // CURL*, opaque so curl.h stays out of the header
    // libcurl keeps callback userdata installed on the reusable easy handle.
    // Keep these buffers alive for the lifetime of that handle rather than
    // pointing libcurl at the stack-local Response returned by get().
    std::string           callbackBody_;
    std::string           callbackReqHeaders_;   // raw "sent" header text from libcurl's debug channel (Moonbug)
    std::atomic<Moonbug*> moonbug_{nullptr};
    HeaderProvider        headerProvider_;       // guarded by mu_
    RequestFilter         requestFilter_;        // guarded by mu_
    std::vector<std::pair<std::string, std::string>> callbackHeaders_;
    std::mutex            mu_;
    bool                  impersonating_ = false;
    std::string           lastError_;
    profile::Switcher::Id validator_ = 0, listener_ = 0;
};

}  // namespace lucyn::net
