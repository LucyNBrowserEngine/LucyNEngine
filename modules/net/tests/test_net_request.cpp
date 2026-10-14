// HttpClient::request(): methods and bodies, the cookie jar, page-context headers, identity headers.
// Runs against a tiny scripted HTTP server on loopback, so it needs no network and passes on BOTH
// build flavours (stock libcurl and libcurl-impersonate).
//
// The first test below is a regression test for a real bug: post() used to set CURLOPT_POSTFIELDSIZE
// after CURLOPT_COPYPOSTFIELDS, which made libcurl drop the body and then block reading it from stdin.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <thread>
#include <lucyn/net/http_client.h>
#include "mini_server.h"

using namespace lucyn;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

using lucyn::testsupport::Req;
using lucyn::testsupport::Rep;
using lucyn::testsupport::MiniServer;

// echo: body = "<METHOD>|<Content-Type>|<request body>"
static Rep echo(const Req& r) {
    Rep p;
    p.body = r.method + "|" + r.header("Content-Type") + "|" + r.body;
    return p;
}

static bool hasHeader(const net::HeaderList& l, const std::string& name, const std::string& value) {
    for (auto& h : l) if (h.first == name && h.second == value) return true;
    return false;
}
static bool hasName(const net::HeaderList& l, const std::string& name) {
    for (auto& h : l) if (h.first == name) return true;
    return false;
}

// ------------------------------------------------------------------ tests

static void testPostBodyArrives(net::HttpClient& c) {
    MiniServer srv(echo);
    net::PostOptions po;
    po.contentType = "application/json";
    auto r = c.post(srv.url("/p"), "{\"x\":1}", po);
    CHECK(r.ok());
    CHECK(r.status == 200);
    CHECK(r.body == "POST|application/json|{\"x\":1}");
    CHECK(srv.last().header("Content-Length") == "7");

    // default content type, and a body with an embedded NUL (must be sent by length, not as a C string)
    std::string bin("a\0b", 3);
    auto r2 = c.post(srv.url("/p"), bin);
    CHECK(r2.ok());
    CHECK(srv.last().body == bin);
    CHECK(srv.last().header("Content-Type") == "application/x-www-form-urlencoded");

    // big body
    std::string big(300 * 1024, 'z');
    auto r3 = c.post(srv.url("/p"), big);
    CHECK(r3.ok());
    CHECK(srv.last().body.size() == big.size());
}

static void testVerbs(net::HttpClient& c) {
    MiniServer srv(echo);
    net::RequestOptions o;

    o.method = "PUT"; o.body = "put-body"; o.hasBody = true; o.headers = {{"Content-Type", "text/plain"}};
    auto r = c.request(srv.url("/r"), o);
    CHECK(r.ok() && r.body == "PUT|text/plain|put-body");
    CHECK(r.statusText == "OK");            // the reason phrase from the status line, not invented
    CHECK(r.httpVersion == "1.1");

    o.method = "patch"; o.body = "{\"a\":1}"; o.headers = {{"content-type", "application/json"}};   // lower-case verb and header name
    r = c.request(srv.url("/r"), o);
    CHECK(r.ok() && r.body == "PATCH|application/json|{\"a\":1}");

    net::RequestOptions d; d.method = "DELETE";
    r = c.request(srv.url("/r"), d);
    CHECK(r.ok() && srv.last().method == "DELETE" && srv.last().body.empty());

    net::RequestOptions op; op.method = "OPTIONS";
    r = c.request(srv.url("/r"), op);
    CHECK(r.ok() && srv.last().method == "OPTIONS");

    net::RequestOptions hd; hd.method = "HEAD";
    r = c.request(srv.url("/r"), hd);
    CHECK(r.ok() && r.status == 200 && r.body.empty() && srv.last().method == "HEAD");

    // an empty POST still announces Content-Length: 0
    net::RequestOptions ep; ep.method = "POST";
    r = c.request(srv.url("/r"), ep);
    CHECK(r.ok() && srv.last().header("Content-Length") == "0");

    // the reused handle must not leak the previous method/body into a plain GET
    r = c.get(srv.url("/after"));
    CHECK(r.ok() && srv.last().method == "GET" && srv.last().body.empty());
    CHECK(!srv.last().has("Content-Type"));
    CHECK(!srv.last().has("Content-Length"));
}

static void testHeaderMerge(net::HttpClient& c) {
    MiniServer srv(echo);
    net::RequestOptions o;
    o.headers = {{"X-One", "1"}, {"x-one", "2"}, {"X-Empty", ""}};   // later same-name (any case) wins
    auto r = c.request(srv.url("/h"), o);
    CHECK(r.ok());
    CHECK(srv.last().header("X-One") == "2");
    CHECK(srv.last().has("X-Empty") && srv.last().header("X-Empty").empty());
    int ones = 0;
    for (auto& h : srv.last().headers) if (h.first == "X-One" || h.first == "x-one") ++ones;
    CHECK(ones == 1);
}

static void testCookieJar(net::HttpClient& c) {
    MiniServer srv([](const Req& r) {
        Rep p;
        if (r.path.rfind("/set", 0) == 0) {
            p.headers = {{"Set-Cookie", "sid=abc123; Path=/; HttpOnly"},
                         {"Set-Cookie", "theme=dark; Path=/"},
                         {"Set-Cookie", "scoped=1; Path=/admin"}};
        } else if (r.path.rfind("/redir", 0) == 0) {
            p.status = 302;
            p.body = "";
            p.headers = {{"Location", "/final"}, {"Set-Cookie", "hop=1; Path=/"}};
        } else if (r.path.rfind("/final", 0) == 0) {
            p.body = "cookie=" + r.header("Cookie");
        } else {
            p.body = "cookie=" + r.header("Cookie");
        }
        return p;
    });

    c.clearCookies();
    CHECK(c.cookies().empty());
    auto r = c.get(srv.url("/set"));
    CHECK(r.ok());

    auto jar = c.cookies();
    CHECK(jar.size() == 3);
    bool sidHttpOnly = false;
    for (auto& k : jar) if (k.name == "sid") sidHttpOnly = k.httpOnly;
    CHECK(sidHttpOnly);

    // sent back automatically
    r = c.get(srv.url("/echo"));
    CHECK(r.body.find("sid=abc123") != std::string::npos);
    CHECK(r.body.find("theme=dark") != std::string::npos);
    CHECK(r.body.find("scoped=1") == std::string::npos);        // Path=/admin only
    r = c.get(srv.url("/admin/x"));
    CHECK(r.body.find("scoped=1") != std::string::npos);

    // document.cookie view: no HttpOnly, no foreign path
    std::string vis = c.cookieStringFor(srv.url("/page"));
    CHECK(vis == "theme=dark");
    std::string all = c.cookieStringFor(srv.url("/page"), true);
    CHECK(all.find("sid=abc123") != std::string::npos && all.find("theme=dark") != std::string::npos);

    // Set-Cookie on a redirect hop is stored, and sent on the next hop
    r = c.get(srv.url("/redir"));
    CHECK(r.ok() && r.redirects == 1);
    CHECK(r.body.find("hop=1") != std::string::npos);

    c.clearCookies();
    CHECK(c.cookies().empty());
    r = c.get(srv.url("/echo"));
    CHECK(r.body == "cookie=");
}

static void testScriptCookies(net::HttpClient& c) {
    MiniServer srv([](const Req& r) {
        Rep p;
        if (r.path == "/http-only") p.headers = {{"Set-Cookie", "locked=server; Path=/; HttpOnly"}};
        p.body = "cookie=" + r.header("Cookie");
        return p;
    });
    const std::string page = srv.url("/dir/page.html");
    c.clearCookies();

    CHECK(c.setCookieFromScript(page, "a=1"));
    CHECK(c.setCookieFromScript(page, "b=two words; path=/"));
    // default path is the page's directory
    auto jar = c.cookies();
    std::string aPath, bPath;
    for (auto& k : jar) { if (k.name == "a") aPath = k.path; if (k.name == "b") bPath = k.path; }
    CHECK(aPath == "/dir");
    CHECK(bPath == "/");
    CHECK(c.cookieStringFor(page) == "a=1; b=two words");          // longer path first
    CHECK(c.cookieStringFor(srv.url("/other")) == "b=two words");

    // sent on the wire
    auto r = c.get(srv.url("/dir/x"));
    CHECK(r.body.find("a=1") != std::string::npos && r.body.find("b=two words") != std::string::npos);

    // overwrite, then delete via max-age=0 and via a past Expires
    CHECK(c.setCookieFromScript(page, "b=three; path=/"));
    CHECK(c.cookieStringFor(page) == "a=1; b=three");
    CHECK(c.setCookieFromScript(page, "b=; path=/; max-age=0"));
    CHECK(c.cookieStringFor(page) == "a=1");
    CHECK(c.setCookieFromScript(page, "a=gone; path=/dir; expires=Thu, 01 Jan 1970 00:00:00 GMT"));
    CHECK(c.cookieStringFor(page).empty());

    // future max-age is a persistent cookie
    CHECK(c.setCookieFromScript(page, "p=1; path=/; max-age=3600"));
    long long exp = 0;
    for (auto& k : c.cookies()) if (k.name == "p") exp = k.expires;
    CHECK(exp > static_cast<long long>(std::time(nullptr)) + 3000);

    // rejected by the browser rules
    CHECK(!c.setCookieFromScript(page, "h=1; HttpOnly"));                        // script may not create HttpOnly
    CHECK(!c.setCookieFromScript(page, "d=1; domain=example.org"));               // not our domain
    CHECK(!c.setCookieFromScript("http://example.test/p", "s=1; secure"));        // Secure from plain http
    CHECK(c.setCookieFromScript(page, "s=1; secure; max-age=0"));                  // loopback IS a secure context (as in Chrome)
    CHECK(!c.setCookieFromScript(page, "__Secure-x=1"));                          // prefix needs Secure
    CHECK(!c.setCookieFromScript(page, "__Host-x=1; path=/"));
    CHECK(!c.setCookieFromScript(page, ""));
    CHECK(!c.setCookieFromScript("not a url", "a=1"));

    // script cannot overwrite an HttpOnly cookie the server set
    c.get(srv.url("/http-only"));
    CHECK(!c.setCookieFromScript(srv.url("/"), "locked=hacked; path=/"));
    CHECK(c.cookieStringFor(srv.url("/")).find("locked") == std::string::npos);        // and it stays invisible
    CHECK(c.cookieStringFor(srv.url("/"), true).find("locked=server") != std::string::npos);

    // setCookie(): import a session, expired = delete
    net::Cookie imp;
    imp.domain = "127.0.0.1"; imp.name = "imported"; imp.value = "yes"; imp.httpOnly = true;
    CHECK(c.setCookie(imp));
    r = c.get(srv.url("/echo"));
    CHECK(r.body.find("imported=yes") != std::string::npos);
    imp.expires = 1;
    CHECK(c.setCookie(imp));
    r = c.get(srv.url("/echo"));
    CHECK(r.body.find("imported") == std::string::npos);
    c.clearCookies();
}

static void testPlanHeaders() {
    net::RequestOptions o;
    net::HeaderList none;

    // navigation (no page): the browser's headers are left alone
    auto nav = net::planHeaders("https://a.example.com/x", o, none);
    CHECK(nav.set.empty() && nav.remove.empty());

    // same-origin XHR GET
    o.pageUrl = "https://a.example.com/dir/page?q=1#frag";
    auto p = net::planHeaders("https://a.example.com/api/data", o, none);
    CHECK(hasHeader(p.set, "Accept", "*/*"));
    CHECK(hasHeader(p.set, "Sec-Fetch-Site", "same-origin"));
    CHECK(hasHeader(p.set, "Sec-Fetch-Mode", "cors"));
    CHECK(hasHeader(p.set, "Sec-Fetch-Dest", "empty"));
    CHECK(hasHeader(p.set, "Referer", "https://a.example.com/dir/page?q=1"));   // full URL, fragment stripped
    CHECK(!hasName(p.set, "Origin"));                                            // same-origin GET: none
    CHECK(std::find(p.remove.begin(), p.remove.end(), "Upgrade-Insecure-Requests") != p.remove.end());
    CHECK(std::find(p.remove.begin(), p.remove.end(), "Sec-Fetch-User") != p.remove.end());

    // same-origin POST gets Origin
    o.method = "POST";
    p = net::planHeaders("https://a.example.com/api", o, none);
    CHECK(hasHeader(p.set, "Origin", "https://a.example.com"));

    // same-site, other subdomain: Origin even for GET, Referer cut to the origin
    o.method = "GET";
    p = net::planHeaders("https://b.example.com/api", o, none);
    CHECK(hasHeader(p.set, "Sec-Fetch-Site", "same-site"));
    CHECK(hasHeader(p.set, "Referer", "https://a.example.com/"));
    CHECK(hasHeader(p.set, "Origin", "https://a.example.com"));

    // cross-site
    p = net::planHeaders("https://other.net/api", o, none);
    CHECK(hasHeader(p.set, "Sec-Fetch-Site", "cross-site"));

    // two-label public suffix: a.co.uk and b.co.uk are NOT the same site
    o.pageUrl = "https://a.co.uk/";
    p = net::planHeaders("https://b.co.uk/x", o, none);
    CHECK(hasHeader(p.set, "Sec-Fetch-Site", "cross-site"));

    // https page -> http target: no Referer at all
    o.pageUrl = "https://a.example.com/p";
    p = net::planHeaders("http://a.example.com/x", o, none);
    CHECK(!hasName(p.set, "Referer"));

    // port makes a different origin
    o.pageUrl = "http://localhost:3000/";
    p = net::planHeaders("http://localhost:4000/x", o, none);
    CHECK(hasHeader(p.set, "Sec-Fetch-Site", "same-site"));   // same host => same site, different origin
    CHECK(hasHeader(p.set, "Origin", "http://localhost:3000"));

    // <script src>: no-cors, dest script, and no Origin for a cross-origin GET
    o.pageUrl = "https://a.example.com/";
    o.fetchMode = "no-cors"; o.fetchDest = "script";
    p = net::planHeaders("https://cdn.other.net/lib.js", o, none);
    CHECK(hasHeader(p.set, "Sec-Fetch-Mode", "no-cors"));
    CHECK(hasHeader(p.set, "Sec-Fetch-Dest", "script"));
    CHECK(!hasName(p.set, "Origin"));

    // caller headers beat derived ones; persona headers are included; explicit value beats a removal
    o = net::RequestOptions{};
    o.pageUrl = "https://a.example.com/";
    o.headers = {{"accept", "application/json"}, {"Upgrade-Insecure-Requests", "1"}};
    net::HeaderList persona = {{"Accept-Language", "de-DE,de;q=0.9"}};
    p = net::planHeaders("https://a.example.com/api", o, persona);
    CHECK(hasHeader(p.set, "accept", "application/json") || hasHeader(p.set, "Accept", "application/json"));
    int accepts = 0;
    for (auto& h : p.set) if (h.first == "Accept" || h.first == "accept") ++accepts;
    CHECK(accepts == 1);
    CHECK(hasHeader(p.set, "Accept-Language", "de-DE,de;q=0.9"));
    CHECK(std::find(p.remove.begin(), p.remove.end(), "Upgrade-Insecure-Requests") == p.remove.end());
    CHECK(hasHeader(p.set, "Upgrade-Insecure-Requests", "1"));
}

static void testWireHeaders(net::HttpClient& c) {
    MiniServer srv(echo);
    c.setHeaderProvider([](bool) { return net::HeaderList{{"Accept-Language", "de-DE,de;q=0.9"}}; });

    // navigation GET: persona header only
    auto r = c.get(srv.url("/nav"));
    CHECK(r.ok());
    CHECK(srv.last().header("Accept-Language") == "de-DE,de;q=0.9");

    // page-initiated POST: subresource headers, no navigation leftovers
    net::RequestOptions o;
    o.method = "POST"; o.body = "x=1"; o.hasBody = true;
    o.headers = {{"Content-Type", "text/plain;charset=UTF-8"}};
    o.pageUrl = "http://127.0.0.1/app/page";           // different port => different origin
    r = c.request(srv.url("/api"), o);
    CHECK(r.ok());
    Req q = srv.last();
    CHECK(q.header("Sec-Fetch-Mode") == "cors");
    CHECK(q.header("Sec-Fetch-Dest") == "empty");
    CHECK(q.header("Sec-Fetch-Site") == "same-site");
    CHECK(q.header("Accept") == "*/*");
    CHECK(q.header("Origin") == "http://127.0.0.1");
    CHECK(q.header("Referer") == "http://127.0.0.1/");
    CHECK(!q.has("Upgrade-Insecure-Requests"));
    CHECK(!q.has("Sec-Fetch-User"));
    CHECK(q.header("Accept-Language") == "de-DE,de;q=0.9");
    CHECK(q.body == "x=1");

    // and a plain navigation after that is not polluted by the page-context request
    r = c.get(srv.url("/nav2"));
    CHECK(!srv.last().has("Origin"));
    CHECK(!srv.last().has("Referer"));
    CHECK(srv.last().header("Sec-Fetch-Mode") != "cors");

    c.setHeaderProvider(nullptr);
}

static void testInFlightCounter() {
    profile::Switcher sw("chrome");
    net::Moonbug mb;
    net::HttpClient c(sw);
    c.setMoonbug(&mb);
    MiniServer srv([](const Req&) { Rep p; p.delayMs = 400; return p; });
    CHECK(mb.inFlight() == 0);
    std::thread t([&] { c.get(srv.url("/slow")); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(mb.inFlight() == 1);          // a request that is still running counts
    t.join();
    CHECK(mb.inFlight() == 0);
    CHECK(mb.total() == 1);
}

int main() {
    profile::Switcher sw(std::string("chrome"));
    net::HttpClient client(sw);
    testPostBodyArrives(client);
    testVerbs(client);
    testHeaderMerge(client);
    testCookieJar(client);
    testScriptCookies(client);
    testPlanHeaders();
    testWireHeaders(client);
    testInFlightCounter();
    if (failures == 0) std::printf("net_request: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
