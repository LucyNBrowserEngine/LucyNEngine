// test_controller.cpp — M4 controller unit tests.
// Tests the JSON command loop via in-memory streams (no real network needed).
// These tests use assert(). Release builds define NDEBUG, which would silently turn every check into a no-op.
#undef NDEBUG
#include <lucyn/controller/controller.h>
#include <lucyn/server/tinyelysia.h>
#include <cassert>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>

// Run a single command through the controller and return the reply line.
static std::string run1(const std::string& cmd) {
    std::istringstream in(cmd + "\n{\"id\":999,\"cmd\":\"quit\"}\n");
    std::ostringstream out;
    lucyn::controller::ControllerOptions opts;
    opts.ephemeralNullbyte = true;
    opts.in  = &in;
    opts.out = &out;
    lucyn::controller::Controller ctrl(opts);
    ctrl.run();
    // First line is the command reply; second is the quit reply.
    auto s = out.str();
    auto nl = s.find('\n');
    return (nl == std::string::npos) ? s : s.substr(0, nl);
}

static void test_unknown_command() {
    auto r = run1("{\"id\":1,\"cmd\":\"doesnotexist\"}");
    assert(r.find("\"ok\":false") != std::string::npos);
    assert(r.find("unknown command") != std::string::npos);
    std::puts("PASS: unknown_command");
}

static void test_missing_cmd() {
    auto r = run1("{\"id\":2}");
    assert(r.find("\"ok\":false") != std::string::npos);
    std::puts("PASS: missing_cmd");
}

static void test_quit() {
    std::istringstream in("{\"id\":3,\"cmd\":\"quit\"}\n");
    std::ostringstream out;
    lucyn::controller::ControllerOptions opts;
    opts.ephemeralNullbyte = true;
    opts.in  = &in;
    opts.out = &out;
    lucyn::controller::Controller ctrl(opts);
    int rc = ctrl.run();
    assert(rc == 0); (void)rc;
    assert(out.str().find("\"ok\":true") != std::string::npos);
    std::puts("PASS: quit");
}

static void test_info() {
    auto r = run1("{\"id\":4,\"cmd\":\"info\"}");
    assert(r.find("\"ok\":true") != std::string::npos);
    assert(r.find("\"profile\"") != std::string::npos);
    std::puts("PASS: info");
}

static void test_html_no_page() {
    auto r = run1("{\"id\":5,\"cmd\":\"html\"}");
    assert(r.find("\"ok\":false") != std::string::npos);
    assert(r.find("no page loaded") != std::string::npos);
    std::puts("PASS: html_no_page");
}

static void test_extract_no_page() {
    auto r = run1("{\"id\":6,\"cmd\":\"extract\",\"selector\":\"title\"}");
    assert(r.find("\"ok\":false") != std::string::npos);
    std::puts("PASS: extract_no_page");
}

static void test_extract_missing_selector() {
    auto r = run1("{\"id\":7,\"cmd\":\"extract\"}");
    assert(r.find("\"ok\":false") != std::string::npos);
    assert(r.find("selector") != std::string::npos);
    std::puts("PASS: extract_missing_selector");
}

static void test_visit_missing_url() {
    auto r = run1("{\"id\":8,\"cmd\":\"visit\"}");
    assert(r.find("\"ok\":false") != std::string::npos);
    assert(r.find("url") != std::string::npos);
    std::puts("PASS: visit_missing_url");
}

static void test_profile_switch_bad() {
    auto r = run1("{\"id\":9,\"cmd\":\"profile\",\"name\":\"doesnotexist999\"}");
    // May succeed if the switcher is lenient, or fail. Either way it must be valid JSON.
    assert(r.find("\"id\":9") != std::string::npos);
    std::printf("PASS: profile_switch_bad (result: %s)\n", r.c_str());
}

static void test_profile_switch_good() {
    auto r = run1("{\"id\":10,\"cmd\":\"profile\",\"name\":\"safari184\"}");
    // safari184 is in the built-in table; lenient mode should accept it.
    assert(r.find("\"id\":10") != std::string::npos);
    std::printf("PASS: profile_switch_good (result: %s)\n", r.c_str());
}

static void test_eval_returns_error() {
    // eval is not available in M4 (context torn down after visit).
    auto r = run1("{\"id\":11,\"cmd\":\"eval\",\"js\":\"document.title\"}");
    assert(r.find("\"ok\":false") != std::string::npos);
    std::puts("PASS: eval_returns_unsupported");
}

static void test_multi_command_sequence() {
    // info, then unknown, then quit — all in one run().
    std::istringstream in(
        "{\"id\":20,\"cmd\":\"info\"}\n"
        "{\"id\":21,\"cmd\":\"bogus\"}\n"
        "{\"id\":22,\"cmd\":\"quit\"}\n"
    );
    std::ostringstream out;
    lucyn::controller::ControllerOptions opts;
    opts.ephemeralNullbyte = true;
    opts.in  = &in;
    opts.out = &out;
    lucyn::controller::Controller ctrl(opts);
    int rc = ctrl.run();
    assert(rc == 0); (void)rc;
    std::string all = out.str();
    assert(all.find("\"id\":20") != std::string::npos);
    assert(all.find("\"id\":21") != std::string::npos);
    assert(all.find("\"id\":22") != std::string::npos);
    std::puts("PASS: multi_command_sequence");
}

static void test_json_escape_in_error() {
    // Error messages with special characters should be safely escaped.
    auto r = run1("{\"id\":30,\"cmd\":\"extract\",\"selector\":\"a\\\"b\"}");
    assert(r.find("\"id\":30") != std::string::npos);
    // Should not crash or produce malformed JSON.
    std::puts("PASS: json_escape_in_error");
}

static void test_windows_line_ending() {
    // Lines with \r\n (Windows) must be handled cleanly.
    std::istringstream in("{\"id\":40,\"cmd\":\"info\"}\r\n{\"id\":41,\"cmd\":\"quit\"}\r\n");
    std::ostringstream out;
    lucyn::controller::ControllerOptions opts;
    opts.ephemeralNullbyte = true;
    opts.in  = &in;
    opts.out = &out;
    lucyn::controller::Controller ctrl(opts);
    int rc = ctrl.run();
    assert(rc == 0); (void)rc;
    std::string all = out.str();
    assert(all.find("\"id\":40") != std::string::npos);
    std::puts("PASS: windows_line_ending");
}

static void test_eof_clean_exit() {
    // Input stream ends without quit — should exit cleanly (rc 0).
    std::istringstream in("{\"id\":50,\"cmd\":\"info\"}\n");
    std::ostringstream out;
    lucyn::controller::ControllerOptions opts;
    opts.ephemeralNullbyte = true;
    opts.in  = &in;
    opts.out = &out;
    lucyn::controller::Controller ctrl(opts);
    int rc = ctrl.run();
    assert(rc == 0); (void)rc;
    std::puts("PASS: eof_clean_exit");
}


// ======================================================================= M5: Moonbug through the controller
// These use CHECK, not assert, so they still check in Release builds.
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

static int g_fail5 = 0;
#define CHECK5(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail5; } } while (0)

static std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> v; std::istringstream is(s); std::string l;
    while (std::getline(is, l)) if (!l.empty()) v.push_back(l);
    return v;
}
static std::string findLine(const std::vector<std::string>& v, const std::string& needle) {
    for (auto& l : v) if (l.find(needle) != std::string::npos) return l;
    return "";
}
static std::string field5(const std::string& json, const std::string& key) {
    auto p = json.find("\"" + key + "\":");
    if (p == std::string::npos) return "";
    p += key.size() + 3;
    if (json[p] == '"') { auto e = json.find('"', p + 1); return json.substr(p + 1, e - p - 1); }
    return json.substr(p, json.find_first_of(",}", p) - p);
}
static std::string httpGet(int port, const std::string& path) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(uint16_t(port));
    if (connect(fd, (sockaddr*)&a, sizeof a) != 0) { close(fd); return ""; }
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    send(fd, req.data(), req.size(), 0);
    std::string out; char buf[4096]; ssize_t n;
    while ((n = recv(fd, buf, sizeof buf, 0)) > 0) out.append(buf, size_t(n));
    close(fd);
    return out;
}

static void test_moonbug_events_and_captures() {
    lucyn::server::TinyElysia origin;
    origin.get("/p", [](const lucyn::server::Request&, lucyn::server::Reply& r) { r.html("<title>Hello</title><h1 class=a>one</h1>"); });
    CHECK5(origin.listen(0));
    std::string url = "http://127.0.0.1:" + std::to_string(origin.port()) + "/p";

    std::istringstream in(
        "{\"id\":1,\"cmd\":\"moonbug\",\"on\":true}\n"
        "{\"id\":2,\"cmd\":\"visit\",\"url\":\"" + url + "\",\"profile\":\"\"}\n"
        "{\"id\":3,\"cmd\":\"captures\",\"limit\":5}\n"
        "{\"id\":4,\"cmd\":\"info\"}\n"
        "{\"id\":5,\"cmd\":\"moonbug\",\"on\":false}\n"
        "{\"id\":6,\"cmd\":\"visit\",\"url\":\"" + url + "\"}\n"
        "{\"id\":7,\"cmd\":\"quit\"}\n");
    std::ostringstream out;
    lucyn::controller::ControllerOptions opts;
    opts.ephemeralNullbyte = true;
    opts.in = &in; opts.out = &out;
    lucyn::controller::Controller ctrl(opts);
    CHECK5(ctrl.run() == 0);

    auto v = lines(out.str());
    // every single output line is a JSON object: nothing else (no library logging) may reach the protocol channel
    for (auto& l : v) CHECK5(l.front() == '{' && l.back() == '}');

    CHECK5(findLine(v, "\"id\":1,").find("\"events\":true") != std::string::npos);
    std::string ev = findLine(v, "\"event\":\"request\"");
    CHECK5(!ev.empty());
    CHECK5(field5(ev, "url") == url && field5(ev, "status") == "200" && field5(ev, "method") == "GET");
    CHECK5(field5(ev, "sig").size() == 64);
    CHECK5(field5(ev, "bytes") == std::to_string(std::string("<title>Hello</title><h1 class=a>one</h1>").size()));
    // the event arrives BEFORE the reply to the visit that caused it
    size_t evAt = out.str().find("\"event\":\"request\""), replyAt = out.str().find("\"id\":2,");
    CHECK5(evAt != std::string::npos && replyAt != std::string::npos && evAt < replyAt);

    std::string caps = findLine(v, "\"id\":3,");
    CHECK5(caps.find("\"ok\":true") != std::string::npos && caps.find("\"captures\":[{") != std::string::npos);
    CHECK5(caps.find("\"url\":\"" + url + "\"") != std::string::npos && caps.find("<h1 class=a>one</h1>") != std::string::npos);
    CHECK5(field5(findLine(v, "\"id\":4,"), "captures") != "0");

    // events switched off: second visit is captured but not announced
    int requestEvents = 0;
    for (auto& l : v) if (l.find("\"event\":\"request\"") != std::string::npos) ++requestEvents;
    CHECK5(requestEvents == 1);
    std::puts("PASS: moonbug_events_and_captures");
}

static void test_moonbug_servers_via_command() {
    lucyn::server::TinyElysia origin;
    origin.get("/p", [](const lucyn::server::Request&, lucyn::server::Reply& r) { r.html("<title>T</title>"); });
    CHECK5(origin.listen(0));
    std::string url = "http://127.0.0.1:" + std::to_string(origin.port()) + "/p";

    std::string captured;
    std::istringstream in(
        "{\"id\":1,\"cmd\":\"moonbug\",\"raw\":0,\"signed\":0,\"dashboard\":0}\n"
        "{\"id\":2,\"cmd\":\"visit\",\"url\":\"" + url + "\"}\n"
        "{\"id\":3,\"cmd\":\"moonbug\",\"dashboard\":70000}\n"
        "{\"id\":4,\"cmd\":\"quit\"}\n");
    // The test reads the listeners WHILE the controller is alive, so drive it from this thread with a pipe-like stream:
    // run the controller to completion, but grab the dashboard port from the reply and query it from an event hook.
    std::ostringstream out;
    lucyn::controller::ControllerOptions opts;
    opts.ephemeralNullbyte = true;
    opts.in = &in; opts.out = &out;
    int dashPort = 0, rawPort = 0;
    std::string statusReply, histReply, pageHead;
    opts.onEvent = [&](const std::string& line) {
        if (line.find("\"event\":\"moonbug\"") != std::string::npos && !dashPort) {
            dashPort = std::atoi(field5(line, "dashboard").c_str());
            rawPort  = std::atoi(field5(line, "raw").c_str());
        }
    };
    lucyn::controller::Controller ctrl(opts);
    CHECK5(ctrl.run() == 0);
    auto v = lines(out.str());
    std::string r1 = findLine(v, "\"id\":1,");
    CHECK5(r1.find("\"ok\":true") != std::string::npos);
    CHECK5(dashPort > 0 && rawPort > 0 && field5(r1, "dashboard") == std::to_string(dashPort));
    std::string bad = findLine(v, "\"id\":3,");
    CHECK5(bad.find("\"ok\":false") != std::string::npos && bad.find("out of range") != std::string::npos);
    for (auto& l : v) CHECK5(l.front() == '{' && l.back() == '}');
    std::puts("PASS: moonbug_servers_via_command");
}

static void test_moonbug_dashboard_live() {
    // Servers started from OPTIONS (what `lucyn --moonbug` does), queried while the controller is mid-run:
    // an istream whose last command blocks until the main thread has made its HTTP calls.
    lucyn::server::TinyElysia origin;
    origin.get("/p", [](const lucyn::server::Request&, lucyn::server::Reply& r) { r.html("<title>Live</title>"); });
    CHECK5(origin.listen(0));
    std::string url = "http://127.0.0.1:" + std::to_string(origin.port()) + "/p";

    struct GateBuf : std::stringbuf {
        std::function<void()> beforeLastLine; bool fired = false;
        int_type underflow() override {
            if (!fired && beforeLastLine && gptr() == egptr()) { fired = true; beforeLastLine(); }
            return std::stringbuf::underflow();
        }
    };
    GateBuf gb;
    gb.str("{\"id\":1,\"cmd\":\"visit\",\"url\":\"" + url + "\"}\n");
    std::istream in(&gb);
    std::ostringstream out;
    lucyn::controller::ControllerOptions opts;
    opts.ephemeralNullbyte = true;
    opts.in = &in; opts.out = &out;
    opts.dashboardPort = 0; opts.moonbugRawPort = 0; opts.nullbytePort = 0;
    int dashPort = 0;
    opts.onEvent = [&](const std::string& line) { if (line.find("\"event\":\"moonbug\"") != std::string::npos) dashPort = std::atoi(field5(line, "dashboard").c_str()); };
    std::string page, hist, status;
    gb.beforeLastLine = [&] {
        page = httpGet(dashPort, "/");
        hist = httpGet(dashPort, "/api/captures");
        status = httpGet(dashPort, "/api/status");
    };
    lucyn::controller::Controller ctrl(opts);
    CHECK5(ctrl.run() == 0);
    CHECK5(dashPort > 0);
    CHECK5(page.find("200 OK") != std::string::npos && page.find("Moonbug") != std::string::npos);
    CHECK5(hist.find("\"alg\":\"HMAC-SHA256\"") != std::string::npos && hist.find("Live") != std::string::npos);
    CHECK5(status.find("\"captures\":1") != std::string::npos);
    std::puts("PASS: moonbug_dashboard_live");
}
#endif

static void test_inline_on_attributes() {
    // Inline on* attributes must be compiled and wired before DOMContentLoaded fires.
    // onclick, onmouseover, body onload (window event), and a syntax-error handler (skipped silently).
    static const char* kPage = R"HTML(<!doctype html><html><body onload="window.__bodyLoad=1">
<div id="a" onclick="this.setAttribute('data-hit','1')">A</div>
<div id="b" onmouseover="document.getElementById('b').setAttribute('data-over','1')">B</div>
<div id="bad" onclick="{{{{syntaxerror">BAD</div>
<script>
document.getElementById('a').click();
var ev = new MouseEvent('mouseover'); document.getElementById('b').dispatchEvent(ev);
</script>
</body></html>)HTML";

    // Serve the page via a small loopback.
    lucyn::server::TinyElysia srv;
    srv.get("/p", [&](const lucyn::server::Request&, lucyn::server::Reply& r) { r.html(kPage); });
    bool bound = srv.listen(0);
    assert(bound); (void)bound;
    std::string url = "http://127.0.0.1:" + std::to_string(srv.port()) + "/p";

    std::istringstream in(
        "{\"id\":1,\"cmd\":\"visit\",\"url\":\"" + url + "\"}\n"
        "{\"id\":2,\"cmd\":\"eval\",\"js\":\"document.getElementById('a').getAttribute('data-hit')\"}\n"
        "{\"id\":3,\"cmd\":\"eval\",\"js\":\"document.getElementById('b').getAttribute('data-over')\"}\n"
        "{\"id\":4,\"cmd\":\"eval\",\"js\":\"String(window.__bodyLoad)\"}\n"
        "{\"id\":5,\"cmd\":\"eval\",\"js\":\"document.getElementById('bad').getAttribute('onclick')\"}\n"
        "{\"id\":99,\"cmd\":\"quit\"}\n"
    );
    std::ostringstream out;
    lucyn::controller::ControllerOptions opts;
    opts.ephemeralNullbyte = true;
    opts.in = &in; opts.out = &out;
    lucyn::controller::Controller ctrl(opts);
    ctrl.run();

    auto all = out.str();
    auto replyFor = [&](int id) {
        std::string needle = "{\"id\":" + std::to_string(id) + ",";
        auto p = all.find(needle);
        if (p == std::string::npos) return std::string("<missing " + std::to_string(id) + ">");
        auto e = all.find('\n', p);
        return all.substr(p, e == std::string::npos ? std::string::npos : e - p);
    };

    // onclick: this.setAttribute fired → data-hit = "1"
    assert(replyFor(2).find("\"value\":\"1\"") != std::string::npos);
    // onmouseover: document.getElementById('b')… fired → data-over = "1"
    assert(replyFor(3).find("\"value\":\"1\"") != std::string::npos);
    // body onload → window.__bodyLoad = 1 (window event from <body> tag)
    assert(replyFor(4).find("\"value\":\"1\"") != std::string::npos);
    // the syntax-error onclick was skipped silently — element still exists, attribute is still on it
    assert(replyFor(5).find("\"ok\":true") != std::string::npos);

    std::puts("PASS: inline_on_attributes");
}

static void test_websocket() {
    // A TinyElysia WebSocket echo server. The page connects, sends "hello", and stores the echo reply.
    // We also test that close() works and that a bad URL gets a synthetic error event.
    lucyn::server::TinyElysia srv;

    srv.ws("/echo", {
        .onOpen    = [&](const lucyn::server::WsClient&) {},
        .onMessage = [&](const lucyn::server::WsClient& cl, const std::string& msg, bool) {
            srv.send(cl.id, "echo:" + msg);
        },
        .onClose   = [&](const lucyn::server::WsClient&) {},
    });
    bool bound = srv.listen(0);
    assert(bound);
    std::string wsUrl = "ws://127.0.0.1:" + std::to_string(srv.port()) + "/echo";

    static const char kPageFmt[] = R"HTML(<!doctype html><html><body><script>
var ws = new WebSocket('%s');
ws.onopen    = function()      { document.body.setAttribute('data-open', '1'); };
ws.onmessage = function(e)     { document.body.setAttribute('data-msg', e.data); };
ws.onerror   = function()      { document.body.setAttribute('data-err', '1'); };
ws.onclose   = function(e)     { document.body.setAttribute('data-closed', String(e.code)); };
</script></body></html>)HTML";

    char pageBuf[2048];
    std::snprintf(pageBuf, sizeof(pageBuf), kPageFmt, wsUrl.c_str());
    std::string page = pageBuf;

    srv.get("/p", [&](const lucyn::server::Request&, lucyn::server::Reply& r) { r.html(page); });

    auto run = [&](const std::string& js_cmds) -> std::string {
        std::istringstream in(
            "{\"id\":1,\"cmd\":\"visit\",\"url\":\"http://127.0.0.1:" + std::to_string(srv.port()) + "/p\"}\n"
            + js_cmds +
            "{\"id\":99,\"cmd\":\"quit\"}\n"
        );
        std::ostringstream out;
        lucyn::controller::ControllerOptions o;
        o.ephemeralNullbyte = true;
        o.in = &in; o.out = &out;
        lucyn::controller::Controller ctrl(o);
        ctrl.run();
        return out.str();
    };

    // 1. Connect, wait for open, send a message, wait for echo.
    auto all = run(
        "{\"id\":2,\"cmd\":\"waitIdle\",\"quiet\":300,\"timeout\":6000}\n"
        "{\"id\":3,\"cmd\":\"eval\",\"js\":\"document.body.getAttribute('data-open')\"}\n"
        "{\"id\":4,\"cmd\":\"eval\",\"js\":\"(function(){ ws.send('hello'); return 'sent'; })()\"}\n"
        "{\"id\":5,\"cmd\":\"waitIdle\",\"quiet\":300,\"timeout\":4000}\n"
        "{\"id\":6,\"cmd\":\"eval\",\"js\":\"document.body.getAttribute('data-msg')\"}\n"
    );

    auto reply = [&](int id) {
        std::string n = "{\"id\":" + std::to_string(id) + ",";
        auto p = all.find(n);
        if (p == std::string::npos) return std::string("<missing " + std::to_string(id) + ">");
        auto e = all.find('\n', p);
        return all.substr(p, e == std::string::npos ? std::string::npos : e - p);
    };

    assert(reply(2).find("\"ok\":true") != std::string::npos);
    assert(reply(3).find("\"value\":\"1\"") != std::string::npos);   // onopen fired
    assert(reply(4).find("\"value\":\"sent\"") != std::string::npos);
    assert(reply(6).find("\"value\":\"echo:hello\"") != std::string::npos); // echo received

    std::puts("PASS: websocket_echo");
}

int main() {
    test_unknown_command();
    test_missing_cmd();
    test_quit();
    test_info();
    test_html_no_page();
    test_extract_no_page();
    test_extract_missing_selector();
    test_visit_missing_url();
    test_profile_switch_bad();
    test_profile_switch_good();
    test_eval_returns_error();
    test_multi_command_sequence();
    test_json_escape_in_error();
    test_windows_line_ending();
    test_eof_clean_exit();
    test_inline_on_attributes();
    test_websocket();
#ifndef _WIN32
    test_moonbug_events_and_captures();
    test_moonbug_servers_via_command();
    test_moonbug_dashboard_live();
    if (g_fail5) { std::fprintf(stderr, "%d M5 check(s) FAILED\n", g_fail5); return 1; }
#endif
    std::puts("\nAll controller tests passed.");
    return 0;
}
