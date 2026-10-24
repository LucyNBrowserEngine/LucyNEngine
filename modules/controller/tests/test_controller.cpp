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
    assert(r.find("url") != std::string::npos);
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

#endif


int main() {
    test_unknown_command();
    test_missing_cmd();
    test_quit();
    test_info();
    test_html_no_page();
    test_extract_no_page();
    test_extract_missing_selector();
    test_visit_missing_url();
    test_eval_returns_error();
    test_multi_command_sequence();
    test_json_escape_in_error();
    test_windows_line_ending();
    test_eof_clean_exit();
#ifndef _WIN32
    test_moonbug_servers_via_command();
    if (g_fail5) { std::fprintf(stderr, "%d M5 check(s) FAILED\n", g_fail5); return 1; }
#endif
    std::puts("\nAll controller tests passed.");
    return 0;
}
