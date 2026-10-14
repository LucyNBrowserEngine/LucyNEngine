// `lucyn --download <url>`: the ONE-COMMAND path. Runs the real binary (copied into a temp folder so its
// nothing-data/ lands there) against a loopback server and checks the contract: exactly one line on stdout
// (the saved path), exit code 0 on success and 1 on failure with nothing on stdout, nothing left half-written.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/wait.h>
#include "mini_server.h"

namespace fs = std::filesystem;
using lucyn::testsupport::Req;
using lucyn::testsupport::Rep;
using lucyn::testsupport::MiniServer;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static std::string binary256() { std::string s; for (int i = 0; i < 256; ++i) s += (char)i; return s; }
static std::string readFile(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); }

struct Run { int exitCode = -1; std::string out, err; };
static Run run(const std::string& cmdline, const fs::path& tmp) {
    fs::path errf = tmp / "stderr.txt";
    std::string full = cmdline + " 2>'" + errf.string() + "'";
    Run r;
    FILE* p = popen(full.c_str(), "r");
    if (!p) return r;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) r.out.append(buf, n);
    int st = pclose(p);
    r.exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    r.err = readFile(errf);
    return r;
}
static int lines(const std::string& s) { int n = 0; for (char c : s) if (c == '\n') ++n; return n; }

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: test_cli_download <path-to-lucyn>\n"); return 2; }
    fs::path tmp = fs::temp_directory_path() / ("lucyn-cli-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tmp);
    fs::path bin = tmp / "lucyn";
    fs::copy_file(argv[1], bin, fs::copy_options::overwrite_existing);
    fs::permissions(bin, fs::perms::owner_all, fs::perm_options::add);
    const std::string B = "'" + bin.string() + "'";

    MiniServer srv([](const Req& r) {
        Rep p;
        if (r.path == "/files/data.bin") { p.body = binary256(); p.headers = {{"Content-Type", "application/octet-stream"}}; }
        else if (r.path == "/big")       { p.body = std::string(50000, 'z'); }
        else                             { p.status = 404; p.body = "nope"; }
        return p;
    });

    // 1. plain download: one line on stdout = the saved path, exit 0, bytes identical
    Run a = run(B + " --download '" + srv.url("/files/data.bin") + "'", tmp);
    CHECK(a.exitCode == 0);
    CHECK(lines(a.out) == 1);
    std::string path = a.out.substr(0, a.out.find('\n'));
    CHECK(fs::path(path) == tmp / "nothing-data" / "downloads" / "data.bin");
    CHECK(fs::exists(path) && readFile(path) == binary256());

    // 2. again: never overwrites silently
    Run b = run(B + " --download '" + srv.url("/files/data.bin") + "'", tmp);
    CHECK(b.exitCode == 0);
    CHECK(b.out.find("data (1).bin") != std::string::npos);

    // 3. --name, and --overwrite
    Run c = run(B + " --download '" + srv.url("/files/data.bin") + "' --name mine.bin", tmp);
    CHECK(c.exitCode == 0 && c.out.find("mine.bin") != std::string::npos);
    Run d = run(B + " --download '" + srv.url("/files/data.bin") + "' --name mine.bin --overwrite", tmp);
    CHECK(d.exitCode == 0 && d.out == c.out);

    // 4. failure: exit 1, NOTHING on stdout, the reason on stderr, no file
    Run e = run(B + " --download '" + srv.url("/nope") + "'", tmp);
    CHECK(e.exitCode == 1);
    CHECK(e.out.empty());
    CHECK(e.err.find("HTTP 404") != std::string::npos);
    CHECK(!fs::exists(tmp / "nothing-data" / "downloads" / "nope"));

    // 5. refused scheme
    Run f = run(B + " --download 'file:///etc/passwd'", tmp);
    CHECK(f.exitCode == 1 && f.out.empty() && f.err.find("only http") != std::string::npos);

    // 6. --max-bytes
    Run g = run(B + " --download '" + srv.url("/big") + "' --max-bytes 1000", tmp);
    CHECK(g.exitCode == 1 && g.out.empty());
    CHECK(!fs::exists(tmp / "nothing-data" / "downloads" / "big"));

    // 7. download-only flags without --download are an error, not silently ignored
    Run h = run(B + " --name x", tmp);
    CHECK(h.exitCode == 1 && h.err.find("--download") != std::string::npos);

    // 8. no half-written leftovers
    int parts = 0;
    for (auto& en : fs::directory_iterator(tmp / "nothing-data" / "downloads"))
        if (en.path().extension() == ".part") ++parts;
    CHECK(parts == 0);

    std::error_code ec;
    fs::remove_all(tmp, ec);
    if (failures == 0) std::printf("cli_download: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
