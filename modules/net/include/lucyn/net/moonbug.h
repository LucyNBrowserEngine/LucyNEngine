// lucyn/net/moonbug.h - Moonbug: the wiretap that sits between libcurl and the engine.
//
// Every request HttpClient makes passes through here. Moonbug records method, URL, the request
// headers as they went out (in wire order), the response, status and timing, hands it to whoever
// listens, and passes everything on UNTOUCHED. LucyN *is* the HTTP client, so it sees decrypted
// traffic natively: no MITM certificate, no proxy, no CDP, nothing reachable from page JavaScript.
//
//   engine side:   HttpClient::setMoonbug(&mb)         (a null pointer = no capture, zero overhead)
//   listener side: mb.onCapture([](const Capture& c) { ... })
//
// Threading: capture() may be called from any thread (the JS fetch bridge uses a worker thread).
// Listeners are called on the thread that made the request, with NO Moonbug or HttpClient lock held,
// so a listener may call back into either. A listener that throws is ignored: a broken tap must never
// break a fetch.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace lucyn::net {

using HeaderList = std::vector<std::pair<std::string, std::string>>;   // wire order, duplicates kept

struct Capture {
    std::uint64_t id = 0;               // 1, 2, 3 ... per Moonbug
    std::int64_t  tsMs = 0;             // unix milliseconds when the request started
    std::string   method = "GET";
    std::string   url;                  // as requested
    std::string   finalUrl;             // after redirects
    std::string   profile;              // curl-impersonate target the switcher asked for ("" = none)
    bool          impersonating = false;// ... and whether the handle was really wearing it

    HeaderList    requestHeaders;       // final hop, as sent (empty if libcurl reported none)
    std::string   requestBody;
    int           redirects = 0;

    long          status = 0;           // 0 = no response (see error)
    HeaderList    responseHeaders;      // final hop
    std::string   responseBody;         // first maxBodyBytes only
    std::size_t   responseBytes = 0;    // true size, even when responseBody was cut
    bool          bodyTruncated = false;

    std::string   httpVersion;          // "1.0" "1.1" "2" "3" or ""
    std::string   remoteIp;
    int           remotePort = 0;
    struct Timing { double dnsMs = 0, connectMs = 0, tlsMs = 0, firstByteMs = 0, totalMs = 0; } timing;   // cumulative, like curl

    std::string   error;                // empty on success
    bool          done = false;         // false inside onRequest, true from onResponse on
};

class Moonbug {
public:
    struct Options {
        std::size_t maxBodyBytes = 1u << 20;   // response bytes kept per capture (1 MiB)
        std::size_t historySize  = 500;        // captures kept for late joiners / the dashboard
    };

    Moonbug() : Moonbug(Options{}) {}
    explicit Moonbug(Options o) : opts_(o) {}
    Moonbug(const Moonbug&) = delete;
    Moonbug& operator=(const Moonbug&) = delete;

    using Listener = std::function<void(const Capture&)>;
    using ListenerId = std::uint64_t;

    ListenerId onRequest(Listener fn);    // before the request is sent (request fields only)
    ListenerId onResponse(Listener fn);   // response arrived (or failed), capture complete
    ListenerId onCapture(Listener fn);    // every request lands here, after onResponse
    void       off(ListenerId id);

    // ---- called by HttpClient --------------------------------------------------------------
    // Starts a capture: assigns the id, stamps the time, fires onRequest. Returns the capture so far.
    Capture    begin(const std::string& method, const std::string& url, const std::string& profile, bool impersonating);
    // Completes it: trims the body to maxBodyBytes, stores it in the history, fires onResponse then onCapture.
    void       finish(Capture cap);

    // ---- reading ---------------------------------------------------------------------------
    std::vector<Capture> recent(std::size_t n) const;   // oldest first, at most n
    std::uint64_t        total() const;                 // captures finished so far
    std::uint64_t        inFlight() const;              // begun but not finished yet (what waitIdle must wait for)
    void                 clear();                       // forget the history (ids keep counting)
    const Options&       options() const { return opts_; }

    // One capture as a JSON object (always valid JSON, always valid UTF-8):
    //   header names/values and the body preview are scrubbed of invalid UTF-8; a binary body preview
    //   is sent as base64 ("bodyEncoding":"base64"). The preview is at most `previewBytes` long.
    static std::string toJson(const Capture& c, std::size_t previewBytes = 16384);

    // helpers, exposed for tests and for other JSON producers
    static std::string base64(const std::string& bytes);
    static bool        validUtf8(const std::string& s);
    static void        appendJsonString(std::string& out, const std::string& s);   // quotes + escapes + scrubs

private:
    enum class Kind { Request, Response, Capture };
    ListenerId add(Kind k, Listener fn);
    void       fire(Kind k, const Capture& c);

    Options opts_;
    mutable std::mutex mu_;
    struct Entry { ListenerId id; Kind kind; Listener fn; };
    std::vector<Entry>   listeners_;
    ListenerId           nextListener_ = 1;
    std::uint64_t        nextId_ = 1;
    std::uint64_t        total_ = 0;
    std::uint64_t        inFlight_ = 0;
    std::deque<Capture>  history_;
};

}  // namespace lucyn::net
