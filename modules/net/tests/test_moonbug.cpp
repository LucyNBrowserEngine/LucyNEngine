// test_moonbug.cpp - Moonbug (the wiretap) and HttpClient capture, against a real loopback server.
// No external network. Uses CHECK (not assert) so it still checks in Release builds.
#include <lucyn/net/http_client.h>
#include <lucyn/net/moonbug.h>
#include <lucyn/profile/profile.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

using namespace lucyn::net;

// ------------------------------------------------------------------ unit tests (no sockets)

static void test_base64_and_utf8() {
    CHECK(Moonbug::base64("") == "");
    CHECK(Moonbug::base64("f") == "Zg==");
    CHECK(Moonbug::base64("fo") == "Zm8=");
    CHECK(Moonbug::base64("foo") == "Zm9v");
    CHECK(Moonbug::base64("foob") == "Zm9vYg==");
    CHECK(Moonbug::base64("fooba") == "Zm9vYmE=");
    CHECK(Moonbug::base64("foobar") == "Zm9vYmFy");
    CHECK(Moonbug::base64(std::string("\x00\xff\xfe", 3)) == "AP/+");

    CHECK(Moonbug::validUtf8(""));
    CHECK(Moonbug::validUtf8("plain ascii"));
    CHECK(Moonbug::validUtf8("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x91\xbb"));
    CHECK(!Moonbug::validUtf8("\xff"));
    CHECK(!Moonbug::validUtf8("\xc3"));                    // truncated sequence
    CHECK(!Moonbug::validUtf8("\xc0\xaf"));                // overlong '/'
    CHECK(!Moonbug::validUtf8("\xed\xa0\x80"));            // surrogate
    CHECK(!Moonbug::validUtf8("\xf4\x90\x80\x80"));        // > U+10FFFF

    std::string o;
    Moonbug::appendJsonString(o, std::string("a\"b\\c\nd\x01") + "\xff" + "e\xc3\xa9");
    CHECK(o == "\"a\\\"b\\\\c\\nd\\u0001\\ufffde\xc3\xa9\"");
    std::puts("PASS: base64_and_utf8");
}

static Capture sample() {
    Capture c;
    c.id = 7; c.tsMs = 1760000000123; c.url = "http://example.test/a?b=\"c\""; c.finalUrl = c.url;
    c.profile = "chrome146"; c.status = 200; c.done = true;
    c.requestHeaders = {{"Host", "example.test"}, {"Accept", "*/*"}, {"Accept", "text/html"}};
    c.responseHeaders = {{"Content-Type", "text/html"}, {"X-Weird", std::string("v\xff")}};
    c.responseBody = "<h1>hi \xc3\xa9</h1>";
    c.responseBytes = c.responseBody.size();
    return c;
}

// Minimal JSON checker: balanced, strings terminated, valid escapes. Enough to prove "always valid JSON".
static bool looksLikeJson(const std::string& s) {
    int depth = 0; bool inStr = false;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = s[i];
        if (inStr) {
            if (c == '\\') { if (++i >= s.size()) return false; if (!std::strchr("\"\\/bfnrtu", s[i])) return false; }
            else if (c == '"') inStr = false;
            else if (c < 0x20) return false;            // raw control char inside a string
        } else if (c == '"') inStr = true;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') { if (--depth < 0) return false; }
    }
    return !inStr && depth == 0;
}

static void test_to_json() {
    Capture c = sample();
    std::string j = Moonbug::toJson(c);
    CHECK(looksLikeJson(j));
    CHECK(j.find("\"id\":7") != std::string::npos);
    CHECK(j.find("\"status\":200") != std::string::npos);
    CHECK(j.find("[\"Accept\",\"*/*\"],[\"Accept\",\"text/html\"]") != std::string::npos);   // order + duplicates kept
    CHECK(j.find("\\ufffd") != std::string::npos);                                            // bad header byte scrubbed
    CHECK(j.find("\"bodyEncoding\":\"utf8\"") != std::string::npos);
    CHECK(Moonbug::validUtf8(j));                                                             // output is always UTF-8

    // binary body -> base64 of the real bytes, not a scrubbed copy
    c.responseBody = std::string("\x89PNG\r\n\x1a\n\x00\x01", 10);
    c.responseBytes = 10;
    j = Moonbug::toJson(c);
    CHECK(looksLikeJson(j));
    CHECK(j.find("\"bodyEncoding\":\"base64\"") != std::string::npos);
    CHECK(j.find(Moonbug::base64(c.responseBody)) != std::string::npos);

    // preview cut must not split a multi-byte char (would wrongly flip a text body to "binary")
    c.responseBody = std::string(9, 'a') + "\xc3\xa9\xc3\xa9";     // cut at 10 lands inside the first "e-acute"
    c.responseBytes = c.responseBody.size();
    j = Moonbug::toJson(c, 10);
    CHECK(j.find("\"bodyEncoding\":\"utf8\"") != std::string::npos);
    CHECK(j.find("\"bodyTruncated\":true") != std::string::npos);
    CHECK(j.find("\"bodyBytes\":13") != std::string::npos);                                   // true size reported
    std::puts("PASS: to_json");
}

static void test_listeners_and_history() {
    Moonbug::Options o; o.historySize = 3; o.maxBodyBytes = 8;
    Moonbug mb(o);
    std::vector<std::string> seen;
    auto a = mb.onRequest([&](const Capture& c) { seen.push_back("req" + std::to_string(c.id) + (c.done ? "!" : "")); });
    mb.onResponse([&](const Capture& c) { seen.push_back("res" + std::to_string(c.id) + (c.done ? "!" : "")); });
    mb.onCapture([&](const Capture&) { throw 42; });                                       // broken tap
    mb.onCapture([&](const Capture& c) { seen.push_back("cap" + std::to_string(c.id)); });  // still runs

    Capture c = mb.begin("GET", "http://x/", "p", false);
    CHECK(c.id == 1 && !c.done && c.tsMs > 1700000000000LL);
    c.responseBody = "0123456789"; c.status = 204;
    mb.finish(c);
    CHECK((seen == std::vector<std::string>{"req1", "res1!", "cap1"}));                    // order, done flag, throw swallowed

    auto r = mb.recent(10);
    CHECK(r.size() == 1);
    CHECK(r[0].responseBody == "01234567" && r[0].bodyTruncated && r[0].responseBytes == 10);   // trimmed, true size kept

    mb.off(a);
    for (int i = 0; i < 5; ++i) mb.finish(mb.begin("GET", "http://x/" + std::to_string(i), "", false));
    CHECK(mb.total() == 6);
    r = mb.recent(10);
    CHECK(r.size() == 3 && r.front().id == 4 && r.back().id == 6);                         // ring keeps the newest, oldest first
    CHECK(mb.recent(2).size() == 2 && mb.recent(2).back().id == 6);
    CHECK(std::count(seen.begin(), seen.end(), "req2") == 0);                              // off() worked
    mb.clear();
    CHECK(mb.recent(5).empty() && mb.total() == 6);
    std::puts("PASS: listeners_and_history");
}

// ------------------------------------------------------------------ loopback HTTP server (POSIX only)

#ifndef _WIN32
struct MiniServer {
    int lfd = -1;
    int port = 0;
    std::atomic<bool> stop{false};
    std::thread th;
    std::vector<std::string> requests;   // raw request texts, in arrival order

    MiniServer() {
        lfd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
        bind(lfd, (sockaddr*)&a, sizeof a);
        listen(lfd, 8);
        socklen_t l = sizeof a; getsockname(lfd, (sockaddr*)&a, &l);
        port = ntohs(a.sin_port);
        th = std::thread([this] { loop(); });
    }
    ~MiniServer() { stop = true; shutdown(lfd, SHUT_RDWR); close(lfd); if (th.joinable()) th.join(); }

    void loop() {
        while (!stop) {
            int fd = accept(lfd, nullptr, nullptr);
            if (fd < 0) return;
            std::string req;
            char buf[4096];
            while (req.find("\r\n\r\n") == std::string::npos) {
                ssize_t n = recv(fd, buf, sizeof buf, 0);
                if (n <= 0) break;
                req.append(buf, size_t(n));
            }
            requests.push_back(req);
            std::string path = req.substr(req.find(' ') + 1, req.find(' ', req.find(' ') + 1) - req.find(' ') - 1);
            std::string resp;
            auto mk = [&](const char* status, const std::string& extra, const std::string& body) {
                return std::string("HTTP/1.1 ") + status + "\r\n" + extra + "Content-Length: " + std::to_string(body.size()) +
                       "\r\nConnection: close\r\n\r\n" + body;
            };
            if (path == "/hello")       resp = mk("200 OK", "Content-Type: text/plain\r\nX-Test: one\r\nX-Test: two\r\n", "hello caf\xc3\xa9");
            else if (path == "/bin")    resp = mk("200 OK", "Content-Type: application/octet-stream\r\n", std::string("\x00\x01\xff\xfe\x80", 5));
            else if (path == "/big")    resp = mk("200 OK", "Content-Type: text/plain\r\n", std::string(5000, 'z'));
            else if (path == "/a:b")    resp = mk("200 OK", "", "colon path");
            else if (path == "/redir")  resp = mk("302 Found", "Location: /hello\r\n", "");
            else                        resp = mk("404 Not Found", "", "nope");
            send(fd, resp.data(), resp.size(), 0);
            close(fd);
        }
    }
    std::string url(const std::string& p) const { return "http://127.0.0.1:" + std::to_string(port) + p; }
};

static std::string headerValue(const HeaderList& h, const std::string& name) {
    for (auto& kv : h) if (kv.first == name) return kv.second;
    return "<none>";
}

static void test_client_capture() {
    MiniServer srv;
    lucyn::profile::Switcher sw("");              // "" -> plain libcurl, no impersonation: works with any build
    HttpClient client(sw);
    Moonbug mb;
    std::vector<Capture> got;
    mb.onCapture([&](const Capture& c) { got.push_back(c); });

    // detached = no capture
    auto r0 = client.get(srv.url("/hello"));
    CHECK(r0.ok() && r0.status == 200 && mb.total() == 0);

    client.setMoonbug(&mb);
    auto r = client.get(srv.url("/hello"));
    CHECK(r.ok() && r.status == 200);
    CHECK(got.size() == 1);
    if (got.size() == 1) {
        const Capture& c = got[0];
        CHECK(c.id == 1 && c.done && c.method == "GET" && c.status == 200 && c.error.empty());
        CHECK(c.url == srv.url("/hello") && c.finalUrl == c.url);
        CHECK(c.responseBody == "hello caf\xc3\xa9" && r.body == c.responseBody);      // passed on untouched
        CHECK(headerValue(c.responseHeaders, "X-Test") == "one");
        int xt = 0; for (auto& kv : c.responseHeaders) if (kv.first == "X-Test") ++xt;
        CHECK(xt == 2);                                                                 // duplicates preserved
        CHECK(headerValue(c.requestHeaders, "Host") == "127.0.0.1:" + std::to_string(srv.port));   // real sent headers
        CHECK(!c.requestHeaders.empty() && c.requestHeaders.front().first == "Host");              // wire order kept
        CHECK(c.httpVersion == "1.1");
        CHECK(c.remoteIp == "127.0.0.1" && c.remotePort == srv.port);
        CHECK(c.timing.totalMs > 0 && c.timing.totalMs >= c.timing.connectMs);
        CHECK(!c.impersonating);
        CHECK(looksLikeJson(Moonbug::toJson(c)));
    }

    // redirect: one capture, final hop headers, redirect count
    got.clear();
    auto rr = client.get(srv.url("/redir"));
    CHECK(rr.status == 200 && got.size() == 1);
    if (got.size() == 1) {
        CHECK(got[0].redirects == 1 && got[0].url == srv.url("/redir") && got[0].finalUrl == srv.url("/hello"));
        CHECK(headerValue(got[0].requestHeaders, "Host") != "<none>");
    }

    // a colon in the path must not be mistaken for a header
    got.clear();
    client.get(srv.url("/a:b"));
    CHECK(got.size() == 1 && got[0].responseBody == "colon path");
    if (got.size() == 1) for (auto& kv : got[0].requestHeaders) CHECK(kv.first.find("GET") == std::string::npos);

    // binary body -> base64 in JSON
    got.clear();
    client.get(srv.url("/bin"));
    CHECK(got.size() == 1);
    if (got.size() == 1) {
        auto j = Moonbug::toJson(got[0]);
        CHECK(looksLikeJson(j) && j.find("\"bodyEncoding\":\"base64\"") != std::string::npos && j.find("AAH//oA=") != std::string::npos);
    }

    // truncation: the CALLER still gets all 5000 bytes, the capture keeps what it is allowed to
    Moonbug small(Moonbug::Options{1000, 10});
    client.setMoonbug(&small);
    auto big = client.get(srv.url("/big"));
    CHECK(big.body.size() == 5000);
    auto rec = small.recent(1);
    CHECK(rec.size() == 1 && rec[0].responseBody.size() == 1000 && rec[0].bodyTruncated && rec[0].responseBytes == 5000);
    client.setMoonbug(&mb);

    // a failed request is still traffic: connection refused is captured with its error
    got.clear();
    int deadPort;
    { MiniServer tmp; deadPort = tmp.port; }       // closed again -> nothing listens
    auto bad = client.get("http://127.0.0.1:" + std::to_string(deadPort) + "/");
    CHECK(!bad.ok() && got.size() == 1);
    if (got.size() == 1) CHECK(got[0].status == 0 && !got[0].error.empty() && got[0].done);

    // a listener may call back into the client (no lock held while listeners run): must not deadlock
    std::atomic<int> nested{0};
    Moonbug nest;
    nest.onRequest([&](const Capture&) { if (nested++ == 0) { client.setMoonbug(nullptr); client.get(srv.url("/hello")); client.setMoonbug(&nest); } });
    client.setMoonbug(&nest);
    auto rn = client.get(srv.url("/hello"));
    CHECK(rn.status == 200 && nested == 1);

    // verbose really is switched off again when detached (no stderr noise): just prove detach works
    client.setMoonbug(nullptr);
    auto rd = client.get(srv.url("/hello"));
    CHECK(rd.status == 200);
    std::puts("PASS: client_capture");
}
#endif

int main() {
    test_base64_and_utf8();
    test_to_json();
    test_listeners_and_history();
#ifndef _WIN32
    test_client_capture();
#else
    std::puts("SKIP: client_capture (POSIX sockets only)");
#endif
    if (g_fail) { std::fprintf(stderr, "%d check(s) FAILED\n", g_fail); return 1; }
    std::puts("moonbug: all passed");
    return 0;
}
