// test_moonbug.cpp - Moonbug (the wiretap) unit tests.
// Session 19: the HttpClient capture tests left with the class (libcurl is gone; CEF feeds
// Moonbug through the controller). What remains is the self-contained Capture/JSON/listener
// machinery, tested here with no sockets and no external network. Uses CHECK (not assert) so
// it still checks in Release builds.
#include <lucyn/net/moonbug.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
    CHECK(j.find("\\ufffd") != std::string::npos);                                            // bad header byte scrubbed (the JSON escape)
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

int main() {
    test_base64_and_utf8();
    test_to_json();
    test_listeners_and_history();
    if (g_fail) { std::fprintf(stderr, "%d check(s) FAILED\n", g_fail); return 1; }
    std::puts("moonbug: all passed");
    return 0;
}
