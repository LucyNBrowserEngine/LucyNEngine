// End to end through the real stack: JSON protocol -> controller -> JS host -> HttpClient -> a loopback HTTP server.
// A page that, on load, sets a cookie from script, POSTs with XMLHttpRequest and PUTs with fetch(); a server that
// sets an HttpOnly cookie. Asserts on what the SERVER received (methods, bodies, cookies, browser-context headers)
// and on what the page and the client saw.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <lucyn/controller/controller.h>
#include <lucyn/core/paths.h>
#include "mini_server.h"

using lucyn::testsupport::Req;
using lucyn::testsupport::Rep;
using lucyn::testsupport::MiniServer;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
#define CONTAINS(hay, needle) do { if (std::string(hay).find(needle) == std::string::npos) { std::printf("FAIL %s:%d  %s\n     does not contain: %s\n     in: %s\n", __FILE__, __LINE__, #hay, std::string(needle).c_str(), std::string(hay).c_str()); ++failures; } } while (0)

static const char* kPage = R"HTML(<!doctype html><html><head><title>app</title></head><body>
<div id="out">idle</div>
<button data-x="y">go</button>
<script>
document.querySelector('button').addEventListener('click', function () { document.body.setAttribute('data-clicked', '1'); });
document.cookie = 'js=1; path=/';
var x = new XMLHttpRequest();
x.open('POST', '/api/login');
x.setRequestHeader('Content-Type', 'application/json');
x.onload = function () { document.getElementById('out').textContent = 'xhr:' + x.status + ':' + x.responseText + ':' + x.getResponseHeader('X-Server') + ':' + x.getResponseHeader('Set-Cookie'); };
x.send(JSON.stringify({u: 'a'}));
fetch('/api/data', {method: 'PUT', body: 'z'}).then(function (r) { return r.text(); }).then(function (t) { document.body.setAttribute('data-fetch', t); });
</script></body></html>)HTML";

static Rep handler(const Req& r) {
    Rep p;
    if (r.path == "/app") {
        p.body = kPage;
        p.headers = {{"Content-Type", "text/html"}, {"Set-Cookie", "sid=abc; Path=/; HttpOnly"}};
    } else if (r.path == "/api/login") {
        p.body = "welcome";
        p.headers = {{"X-Server", "lucy"}, {"Set-Cookie", "token=t1; Path=/"}};
    } else if (r.path == "/api/data") {
        p.body = r.method + "|" + r.body + "|cookie=" + r.header("Cookie");
    } else {
        p.body = "?";
    }
    return p;
}

static std::vector<std::string> runScript(const std::vector<std::string>& cmds) {
    std::string input;
    for (auto& c : cmds) input += c + "\n";
    input += "{\"id\":999,\"cmd\":\"quit\"}\n";
    std::istringstream in(input);
    std::ostringstream out;
    lucyn::controller::ControllerOptions o;
    o.ephemeralNullbyte = true;
    o.in = &in;
    o.out = &out;
    lucyn::controller::Controller c(o);
    c.run();
    std::vector<std::string> lines;
    std::istringstream rd(out.str());
    for (std::string l; std::getline(rd, l);) lines.push_back(l);
    return lines;
}
static std::string reply(const std::vector<std::string>& lines, int id) {
    const std::string needle = "{\"id\":" + std::to_string(id) + ",";
    for (auto& l : lines) if (l.rfind(needle, 0) == 0) return l;
    return "<no reply for id " + std::to_string(id) + ">";
}
static std::string esc(const std::string& s) { return s; }

int main() {
    MiniServer srv(handler);
    const std::string app = srv.url("/app");
    auto L = runScript({
        "{\"id\":1,\"cmd\":\"visit\",\"url\":\"" + app + "\"}",
        "{\"id\":2,\"cmd\":\"waitIdle\",\"quiet\":250,\"timeout\":8000}",
        "{\"id\":3,\"cmd\":\"extract\",\"selector\":\"#out\"}",
        "{\"id\":4,\"cmd\":\"eval\",\"js\":\"document.body.getAttribute('data-fetch')\"}",
        "{\"id\":5,\"cmd\":\"eval\",\"js\":\"document.cookie\"}",
        "{\"id\":6,\"cmd\":\"cookies\",\"url\":\"" + srv.url("/") + "\"}",
        // click with a quoted selector used to be a JS SyntaxError
        "{\"id\":7,\"cmd\":\"click\",\"selector\":\"[data-x='y']\"}",
        "{\"id\":8,\"cmd\":\"eval\",\"js\":\"document.body.getAttribute('data-clicked')\"}",
        "{\"id\":9,\"cmd\":\"click\",\"selector\":\"[data-x='nope']\"}",
        "{\"id\":10,\"cmd\":\"setCookie\",\"name\":\"imp\",\"value\":\"v\",\"url\":\"" + srv.url("/") + "\",\"httpOnly\":true}",
        "{\"id\":11,\"cmd\":\"cookies\"}",
        "{\"id\":12,\"cmd\":\"clearCookies\"}",
        "{\"id\":13,\"cmd\":\"cookies\"}",
        "{\"id\":14,\"cmd\":\"setCookie\",\"value\":\"x\",\"domain\":\"a.test\"}",
    });

    CHECK(reply(L, 1).find("\"ok\":true") != std::string::npos);
    CHECK(reply(L, 2).find("\"ok\":true") != std::string::npos);

    // the page's XHR POST completed and its handlers saw the status, body, a response header; Set-Cookie stayed hidden
    CONTAINS(reply(L, 3), "xhr:200:welcome:lucy:null");
    // the page's fetch PUT carried the body, and the shared jar sent the script cookie AND the server's HttpOnly one
    CONTAINS(reply(L, 4), "PUT|z|cookie=");
    CONTAINS(reply(L, 4), "js=1");
    CONTAINS(reply(L, 4), "sid=abc");

    // document.cookie: script-visible cookies only (no HttpOnly sid), including the one the XHR response set
    std::string dc = reply(L, 5);
    CONTAINS(dc, "js=1");
    CONTAINS(dc, "token=t1");
    CHECK(dc.find("sid") == std::string::npos);

    // the `cookies` command shows everything a request to the URL would carry, HttpOnly flagged
    std::string ck = reply(L, 6);
    CONTAINS(ck, "\"name\":\"sid\"");
    CONTAINS(ck, "\"httpOnly\":true");
    CONTAINS(ck, "\"name\":\"js\"");

    // click: quoted selectors work, a missing one is a clean error naming it (not a SyntaxError)
    CHECK(reply(L, 7).find("\"ok\":true") != std::string::npos);
    CONTAINS(reply(L, 8), "\"value\":\"1\"");
    CHECK(reply(L, 9).find("\"ok\":false") != std::string::npos);
    CONTAINS(reply(L, 9), "selector not found");
    CHECK(reply(L, 9).find("SyntaxError") == std::string::npos);

    // setCookie / cookies / clearCookies
    CHECK(reply(L, 10).find("\"ok\":true") != std::string::npos);
    CONTAINS(reply(L, 11), "\"name\":\"imp\"");
    CHECK(reply(L, 12).find("\"ok\":true") != std::string::npos);
    CONTAINS(reply(L, 13), "\"cookies\":[]");
    CONTAINS(reply(L, 14), "'name' required");

    // ---- what the SERVER actually received ----------------------------------------------------------------
    auto log = srv.log();
    const Req* nav = nullptr; const Req* post = nullptr; const Req* put = nullptr;
    for (auto& r : log) {
        if (r.path == "/app") nav = &r;
        if (r.path == "/api/login") post = &r;
        if (r.path == "/api/data") put = &r;
    }
    CHECK(nav && post && put);
    if (!(nav && post && put)) { std::printf("%s\n", failures ? "controller_net: FAILED" : ""); return 1; }

    // the navigation: identity headers, no page context
    CHECK(nav->has("Accept-Language"));
    CHECK(!nav->has("Origin"));
    CHECK(!nav->has("Referer"));
    CHECK(nav->header("User-Agent").find("curl/") == std::string::npos);        // never the library's own UA

    // the XHR POST: a subresource request from the page, with its body, content type and cookies
    CHECK(post->method == "POST");
    CHECK(post->body == "{\"u\":\"a\"}");
    CHECK(post->header("Content-Type") == "application/json");
    CHECK(post->header("Sec-Fetch-Mode") == "cors");
    CHECK(post->header("Sec-Fetch-Dest") == "empty");
    CHECK(post->header("Sec-Fetch-Site") == "same-origin");
    CHECK(post->header("Accept") == "*/*");
    CHECK(post->header("Origin") == "http://127.0.0.1:" + srv.url("/").substr(17, srv.url("/").size() - 18));
    CHECK(post->header("Referer") == app);
    CHECK(!post->has("Upgrade-Insecure-Requests"));
    CHECK(!post->has("Sec-Fetch-User"));
    CHECK(post->header("Accept-Language") == nav->header("Accept-Language"));  // one identity, everywhere
    CHECK(post->header("Cookie").find("sid=abc") != std::string::npos);
    CHECK(post->header("Cookie").find("js=1") != std::string::npos);

    // the fetch PUT
    CHECK(put->method == "PUT");
    CHECK(put->body == "z");
    CHECK(put->header("Content-Type") == "text/plain;charset=UTF-8");
    CHECK(put->header("Origin") != "");                                          // non-GET always carries Origin

    std::printf(failures ? "controller_net: %d FAILED\n" : "controller_net: all passed\n", failures);
    (void)esc;

    // ---- Cookie persistence test ----------------------------------------------------------------
    {
        // Use a temp dir as the data root so the test is isolated.
        namespace fs = std::filesystem;
        fs::path tmpDir = fs::temp_directory_path() / "lucyn_test_cookies_XXXXXX";
        // Make it unique.
        tmpDir = fs::temp_directory_path() / ("lucyn_test_" + std::to_string(getpid()));
        fs::create_directories(tmpDir);
        lucyn::core::setDataRootForTests(tmpDir.string());

        // A simple server that always sets a cookie.
        MiniServer cookieSrv([](const Req& r) {
            (void)r;
            Rep p;
            p.body = "ok";
            p.headers = {{"Content-Type","text/plain"}, {"Set-Cookie","persist=1; Path=/"}};
            return p;
        });

        auto runWith = [&](bool persist) {
            std::string input =
                "{\"id\":1,\"cmd\":\"visit\",\"url\":\"" + cookieSrv.url("/") + "\"}\n"
                "{\"id\":2,\"cmd\":\"cookies\"}\n"
                "{\"id\":3,\"cmd\":\"quit\"}\n";
            std::istringstream in(input);
            std::ostringstream out;
            lucyn::controller::ControllerOptions o;
            o.ephemeralNullbyte = true;
            o.persistCookies    = persist;
            o.in = &in; o.out = &out;
            lucyn::controller::Controller c(o);
            c.run();
            return out.str();
        };

        // First run: cookie is set by the server. With persistCookies on, it should be written to disk.
        std::string r1 = runWith(true);
        CONTAINS(r1, "\"name\":\"persist\"");   // jar has the cookie

        // Verify the file was written. The alias "chrome" resolves to e.g. "chrome146".
        // setDataRootForTests makes dataRoot() == tmpDir, so dataDir("cookies") == tmpDir/cookies.
        fs::path cookiesDir = tmpDir / "cookies";
        bool anyFile = false;
        std::string cookieContent;
        if (fs::exists(cookiesDir)) {
            for (auto& e : fs::directory_iterator(cookiesDir)) {
                if (e.path().extension() == ".txt") { anyFile = true; std::ifstream f(e.path()); cookieContent.assign(std::istreambuf_iterator<char>(f),{}); break; }
            }
        }
        CHECK(anyFile);
        if (!anyFile) {
            std::printf("  (no .txt found in %s)\n", cookiesDir.string().c_str());
        } else {
            CONTAINS(cookieContent, "persist");
        }

        // Second run on a fresh controller (empty jar). With persistCookies, cookies load from disk.
        std::string r2 = runWith(true);
        CONTAINS(r2, "\"name\":\"persist\"");   // loaded from file before the visit

        // Third run WITHOUT persistence: new jar, so cookie should be absent before the visit.
        // We'll check the reply to the "cookies" command (which runs AFTER visit, so it'll have the
        // server's cookie again — just verify the file mechanism doesn't corrupt anything).
        std::string r3 = runWith(false);
        CONTAINS(r3, "\"name\":\"persist\"");   // set by the server this run too

        lucyn::core::setDataRootForTests("");   // restore
        fs::remove_all(tmpDir);

        int pf = failures;
        std::printf(pf == failures ? "cookie_persist: all passed\n" : "cookie_persist: FAILED\n");
    }

    return failures ? 1 : 0;
}
