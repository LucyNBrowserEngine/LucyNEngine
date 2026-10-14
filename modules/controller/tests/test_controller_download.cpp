// The `download` controller command, end to end: JSON protocol -> controller -> net::HttpClient (streaming) -> a
// loopback HTTP server -> a file in <data root>/downloads. Asserts on the BYTES ON DISK, the file NAMES, and that
// nothing escapes the downloads folder or is left half-written.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>
#include <lucyn/controller/controller.h>
#include <lucyn/core/paths.h>
#include <lucyn/net/http_client.h>
#include <lucyn/profile/profile.h>
#include "mini_server.h"

namespace fs = std::filesystem;
using lucyn::testsupport::Req;
using lucyn::testsupport::Rep;
using lucyn::testsupport::MiniServer;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
#define CONTAINS(hay, needle) do { if (std::string(hay).find(needle) == std::string::npos) { std::printf("FAIL %s:%d  %s\n     does not contain: %s\n     in: %s\n", __FILE__, __LINE__, #hay, std::string(needle).c_str(), std::string(hay).c_str()); ++failures; } } while (0)

static std::string binary256() {
    std::string s;
    for (int i = 0; i < 256; ++i) s += (char)i;
    return s;
}
static std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

static std::string g_escapeTarget;   // an ABSOLUTE path outside downloads/, inside the temp folder (set in main)

static Rep handler(const Req& r) {
    Rep p;
    p.headers = {{"Content-Type", "application/octet-stream"}};
    const std::string& u = r.path;
    if (u == "/bin")                      p.body = binary256();
    else if (u == "/utf8")                { p.body = "u"; p.headers.emplace_back("Content-Disposition", "attachment; filename*=UTF-8''r%C3%A9sum%C3%A9.pdf"); }
    else if (u == "/quoted")              { p.body = "q"; p.headers.emplace_back("Content-Disposition", "attachment; filename=\"quoted name.txt\""); }
    else if (u == "/evil1")               { p.body = "e1"; p.headers.emplace_back("Content-Disposition", "attachment; filename=\"../../evil1.txt\""); }
    else if (u == "/evil2")               { p.body = "e2"; p.headers.emplace_back("Content-Disposition", "attachment; filename=\"..\\\\evil2.txt\""); }
    else if (u == "/evil3")               { p.body = "e3"; p.headers.emplace_back("Content-Disposition", "attachment; filename=\"" + g_escapeTarget + "\""); }
    else if (u == "/dotdot")              { p.body = "dd"; p.headers.emplace_back("Content-Disposition", "attachment; filename=\"..\""); }
    else if (u == "/dir/file%20one.bin")  p.body = "fo";
    else if (u == "/a.tar.gz")            p.body = "tgz";
    else if (u == "/report.pdf?x=1")      { p.body = "%PDF-fake"; p.headers = {{"Content-Type", "application/pdf; charset=binary"}}; }
    else if (u == "/missing")             { p.status = 404; p.body = "this error page must never become a file"; }
    else if (u == "/empty")               p.body = "";
    else if (u == "/redir")               { p.status = 302; p.headers.emplace_back("Location", "/target.bin"); p.body = "moved"; }
    else if (u == "/target.bin")          p.body = "landed";
    else if (u == "/big")                 p.body = std::string(100000, 'x');
    else if (u == "/setc")                { p.body = "cookie set"; p.headers.emplace_back("Set-Cookie", "sid=secret; Path=/"); }
    else if (u == "/needs-cookie")        { if (r.header("Cookie").find("sid=secret") != std::string::npos) p.body = "cookie ok"; else { p.status = 403; p.body = "no cookie"; } }
    else if (u == "/")                    p.body = "root";
    else                                  { p.status = 404; p.body = "?"; }
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
static std::string field(const std::string& line, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    size_t p = line.find(needle);
    if (p == std::string::npos) return {};
    std::string out;
    for (p += needle.size(); p < line.size() && line[p] != '"'; ++p) {
        if (line[p] == '\\' && p + 1 < line.size()) { ++p; out += line[p]; } else out += line[p];
    }
    return out;
}
static bool ok(const std::string& l) { return l.find("\"ok\":true") != std::string::npos; }
static std::string dl(int id, const std::string& url, const std::string& extra = "") {
    return "{\"id\":" + std::to_string(id) + ",\"cmd\":\"download\",\"url\":\"" + url + "\"" + extra + "}";
}

// The streaming contract of net::HttpClient itself (the controller also checks the status, so this is tested directly).
static void testNetStreamingContract(MiniServer& srv) {
    lucyn::profile::Switcher sw("chrome");
    lucyn::net::HttpClient client(sw);
    {
        size_t calls = 0, got = 0;
        lucyn::net::RequestOptions o;
        o.bodySink = [&](const char*, size_t n) { ++calls; got += n; return true; };
        auto r = client.request(srv.url("/missing"), o);
        CHECK(r.ok() && r.status == 404);
        CHECK(calls == 0 && got == 0);                 // an error page is never handed to the sink...
        CHECK(r.body.empty() && r.bodyBytes > 0);      // ...but its size is still reported
    }
    {
        std::string collected;
        lucyn::net::RequestOptions o;
        o.timeoutSec = 0;
        o.bodySink = [&](const char* d, size_t n) { collected.append(d, n); return true; };
        auto r = client.request(srv.url("/big"), o);
        CHECK(r.ok() && r.status == 200 && r.body.empty());
        CHECK(collected == std::string(100000, 'x') && r.bodyBytes == 100000);
    }
    {
        lucyn::net::RequestOptions o;                  // a sink that says "stop" aborts the transfer with an error
        o.bodySink = [&](const char*, size_t) { return false; };
        auto r = client.request(srv.url("/big"), o);
        CHECK(!r.ok());
    }
    {
        lucyn::net::RequestOptions o;                  // buffered requests still work afterwards on the same (reused) handle
        auto r = client.request(srv.url("/bin"), o);
        CHECK(r.ok() && r.status == 200 && r.body == binary256() && r.bodyBytes == 256);
    }
}

int main() {
    fs::path tmp = fs::temp_directory_path() / ("lucyn-dl-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tmp);
    lucyn::core::setDataRootForTests(tmp.string());
    const fs::path dir = fs::path(lucyn::core::dataDir("downloads"));
    g_escapeTarget = (tmp / "outside" / "evil3").string();

    {
        MiniServer srv(handler);
        auto U = [&](const std::string& p) { return srv.url(p); };
        testNetStreamingContract(srv);

        auto L = runScript({
            /* 1  */ dl(1, U("/bin")),
            /* 2  */ dl(2, U("/bin")),                                   // same name again -> "bin (1)"
            /* 3  */ dl(3, U("/bin"), ",\"overwrite\":true"),            // replaces "bin", makes no "bin (2)"
            /* 4  */ dl(4, U("/bin"), ",\"name\":\"my.dat\""),
            /* 5  */ dl(5, U("/bin"), ",\"name\":\"../../evil-name\""),
            /* 6  */ dl(6, U("/utf8")),
            /* 7  */ dl(7, U("/quoted")),
            /* 8  */ dl(8, U("/evil1")),
            /* 9  */ dl(9, U("/evil2")),
            /* 10 */ dl(10, U("/evil3")),
            /* 11 */ dl(11, U("/dir/file%20one.bin")),
            /* 12 */ dl(12, U("/missing")),
            /* 13 */ dl(13, U("/empty")),
            /* 14 */ dl(14, U("/redir")),
            /* 15 */ dl(15, U("/big"), ",\"maxBytes\":1000"),
            /* 16 */ dl(16, U("/big")),
            /* 17 */ dl(17, "file:///etc/passwd"),
            /* 18 */ dl(18, "ftp://example.com/x"),
            /* 19 */ "{\"id\":19,\"cmd\":\"download\"}",
            /* 20 */ dl(20, U("/setc")),
            /* 21 */ dl(21, U("/needs-cookie")),
            /* 22 */ dl(22, U("/a.tar.gz")),
            /* 23 */ dl(23, U("/a.tar.gz")),
            /* 24 */ dl(24, U("/report.pdf?x=1")),
            /* 25 */ dl(25, U("/")),
            /* 26 */ dl(26, U("/bin"), ",\"name\":\"" + std::string(300, 'a') + ".txt\""),
            /* 27 */ dl(27, U("/bin"), ",\"name\":\"NUL.txt\""),
            /* 28 */ dl(28, U("/bin"), ",\"name\":\".bashrc\""),
            /* 29 */ dl(29, U("/dotdot")),
            /* 30 */ dl(30, U("/bin"), ",\"name\":\"..\""),
            /* 31 */ dl(31, U("/bin"), ",\"referer\":\"https://from.example/page\""),
            /* 32 */ "{\"id\":32,\"cmd\":\"captures\",\"limit\":100}",
            /* 33 */ dl(33, U("/bin"), ",\"name\":\"semi;colon:and*star?.bin\""),
            /* 34 */ dl(34, U("/big"), ",\"maxBytes\":20000,\"name\":\"aborted-midway.bin\""),   // aborts AFTER the file was opened and written to
        });

        // 1-3: bytes on disk are exactly what the server sent; collisions never overwrite unless asked
        std::string r1 = reply(L, 1);
        CHECK(ok(r1));
        CONTAINS(r1, "\"bytes\":256");
        CONTAINS(r1, "\"status\":200");
        CONTAINS(r1, "\"type\":\"application/octet-stream\"");
        CHECK(field(r1, "name") == "bin");
        CHECK(fs::path(field(r1, "path")) == dir / "bin");
        CHECK(readFile(dir / "bin") == binary256());
        CHECK(ok(reply(L, 2)) && field(reply(L, 2), "name") == "bin (1)");
        CHECK(readFile(dir / "bin (1)") == binary256());
        CHECK(ok(reply(L, 3)) && field(reply(L, 3), "name") == "bin");
        // (the overwrite in #3 made no extra copy: the next plain /bin download, #31, is the first one to become "bin (2)")

        // 4-5: explicit name wins; any path in it is dropped and the file stays inside downloads/
        CHECK(field(reply(L, 4), "name") == "my.dat" && readFile(dir / "my.dat") == binary256());
        CHECK(ok(reply(L, 5)) && field(reply(L, 5), "name") == "evil-name");
        CHECK(fs::exists(dir / "evil-name"));
        CHECK(!fs::exists(dir.parent_path() / "evil-name") && !fs::exists(dir.parent_path().parent_path() / "evil-name"));

        // 6-10: Content-Disposition (RFC 5987 UTF-8, quoted) and hostile names from the server
        CHECK(field(reply(L, 6), "name") == "r\xC3\xA9sum\xC3\xA9.pdf" && fs::exists(dir / fs::u8path("r\xC3\xA9sum\xC3\xA9.pdf")));
        CHECK(field(reply(L, 7), "name") == "quoted name.txt" && readFile(dir / "quoted name.txt") == "q");
        CHECK(field(reply(L, 8), "name") == "evil1.txt" && fs::exists(dir / "evil1.txt"));
        CHECK(field(reply(L, 9), "name") == "evil2.txt" && fs::exists(dir / "evil2.txt"));
        CHECK(field(reply(L, 10), "name") == "evil3" && fs::exists(dir / "evil3"));
        CHECK(!fs::exists(dir.parent_path() / "evil1.txt") && !fs::exists(dir.parent_path().parent_path() / "evil1.txt"));
        CHECK(!fs::exists(tmp / "outside"));   // an absolute path from the server must not create anything out there

        // 11: name from the URL, percent-decoded
        CHECK(field(reply(L, 11), "name") == "file one.bin" && readFile(dir / "file one.bin") == "fo");

        // 12: a 404 is an error and its body is NOT saved
        CHECK(reply(L, 12).find("\"ok\":false") != std::string::npos);
        CONTAINS(reply(L, 12), "HTTP 404");
        CHECK(!fs::exists(dir / "missing"));

        // 13: an empty 2xx body is a real empty file
        CHECK(ok(reply(L, 13)) && fs::exists(dir / "empty") && fs::file_size(dir / "empty") == 0);
        CONTAINS(reply(L, 13), "\"bytes\":0");

        // 14: redirects are followed; reply carries the FINAL url, the name comes from it
        CHECK(ok(reply(L, 14)));
        CONTAINS(reply(L, 14), "/target.bin\"");
        CHECK(field(reply(L, 14), "name") == "target.bin" && readFile(dir / "target.bin") == "landed");

        // 15-16: maxBytes aborts and deletes; without it the same 100 KB arrives whole (more than one chunk)
        CHECK(reply(L, 15).find("\"ok\":false") != std::string::npos);
        CONTAINS(reply(L, 15), "maxBytes");
        CHECK(ok(reply(L, 16)));
        CONTAINS(reply(L, 16), "\"bytes\":100000");
        CHECK(readFile(dir / "big") == std::string(100000, 'x'));

        // 17-19: not http(s) / missing url
        CONTAINS(reply(L, 17), "only http");
        CONTAINS(reply(L, 18), "only http");
        CONTAINS(reply(L, 19), "'url' required");

        // 20-21: same cookie jar as everything else
        CHECK(ok(reply(L, 20)));
        CHECK(ok(reply(L, 21)));
        CHECK(readFile(dir / "needs-cookie") == "cookie ok");

        // 22-23: "archive.tar.gz" collides as "archive (1).tar.gz"
        CHECK(field(reply(L, 22), "name") == "a.tar.gz");
        CHECK(field(reply(L, 23), "name") == "a (1).tar.gz");

        // 24: query string is not part of the name; Content-Type loses its parameters
        CHECK(field(reply(L, 24), "name") == "report.pdf");
        CONTAINS(reply(L, 24), "\"type\":\"application/pdf\"");
        CHECK(readFile(dir / "report.pdf") == "%PDF-fake");

        // 25: a URL with no file name at all
        CHECK(field(reply(L, 25), "name") == "download.bin");

        // 26-30: sanitising
        std::string longName = field(reply(L, 26), "name");
        CHECK(ok(reply(L, 26)) && longName.size() <= 200 && longName.size() > 100);
        CHECK(longName.size() >= 4 && longName.substr(longName.size() - 4) == ".txt");
        CHECK(field(reply(L, 27), "name") == "_NUL.txt");
        CHECK(field(reply(L, 28), "name") == "_bashrc");
        CHECK(ok(reply(L, 29)));
        CHECK(field(reply(L, 29), "name") == "dotdot");     // ".." from the server is unusable, so the URL name is used
        CONTAINS(reply(L, 30), "not a usable file name");

        // 31: referer option is sent
        CHECK(ok(reply(L, 31)) && field(reply(L, 31), "name") == "bin (2)");
        bool sawReferer = false;
        for (auto& r : srv.log()) if (r.has("Referer") && r.header("Referer") == "https://from.example/page") sawReferer = true;
        CHECK(sawReferer);

        // 32: Moonbug saw the downloads (it captures every request), with the true byte count and no giant body
        std::string caps = reply(L, 32);
        CONTAINS(caps, "/bin");
        CONTAINS(caps, "/big");

        // 33: characters that are illegal on Windows become underscores
        CHECK(field(reply(L, 33), "name") == "semi;colon_and_star_.bin");

        // 34: aborted mid-file: error, and neither the finished name nor a .part is left
        CHECK(reply(L, 34).find("\"ok\":false") != std::string::npos);
        CONTAINS(reply(L, 34), "maxBytes");
        CHECK(!fs::exists(dir / "aborted-midway.bin"));

        // nothing half-written is left behind (the hidden .part temp files)
        int parts = 0;
        for (auto& e : fs::directory_iterator(dir)) {
            std::string n = e.path().filename().string();
            if (n.size() > 5 && n.substr(n.size() - 5) == ".part") ++parts;
        }
        CHECK(parts == 0);
        // and nothing was created next to the downloads folder
        CHECK(!fs::exists(dir.parent_path() / "evil1.txt"));
    }

    lucyn::core::setDataRootForTests("");
    std::error_code ec;
    fs::remove_all(tmp, ec);
    if (failures == 0) std::printf("controller_download: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
