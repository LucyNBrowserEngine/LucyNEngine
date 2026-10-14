// lucyn-moonbug - poke the capture pipeline without the whole engine.
//
//   lucyn-moonbug [options] [url ...]
//
//   --profile <name>    browser persona (default: chrome)
//   --once              fetch the urls, print the captures, exit (no servers)
//   --raw <port>        raw stream port        (default 2005)
//   --signed <port>     NULLBYTE stream port   (default 2006)
//   --dashboard <port>  dashboard port         (default 6969)
//   --expose <file>     serve this HTML instead of the embedded dashboard
//
// Without --once it starts the three listeners (127.0.0.1 only), fetches the urls given, then keeps running and
// fetches every url you type (one per line; empty line or EOF quits). Open the dashboard and watch.
#include <lucyn/net/http_client.h>
#include <lucyn/net/moonbug.h>
#include <lucyn/profile/profile.h>
#include <lucyn/security/nullbyte.h>
#include <lucyn/server/capture_server.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::string profile = "chrome", expose;
    int raw = 2005, sgn = 2006, dash = 6969;
    bool once = false;
    std::vector<std::string> urls;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "--profile") profile = next("--profile");
        else if (a == "--once") once = true;
        else if (a == "--raw") raw = std::atoi(next("--raw").c_str());
        else if (a == "--signed") sgn = std::atoi(next("--signed").c_str());
        else if (a == "--dashboard") dash = std::atoi(next("--dashboard").c_str());
        else if (a == "--expose") expose = next("--expose");
        else if (a == "--help" || a == "-h") { std::puts("usage: lucyn-moonbug [--profile p] [--once] [--raw n] [--signed n] [--dashboard n] [--expose file] [url ...]"); return 0; }
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
        else urls.push_back(a);
    }

    std::string kerr;
    std::string key = lucyn::security::Nullbyte::loadOrCreateKey(lucyn::security::Nullbyte::defaultKeyPath(), &kerr);
    if (key.empty()) { std::fprintf(stderr, "NULLBYTE: %s\n(using a throwaway key for this run)\n", kerr.c_str()); key = lucyn::security::Nullbyte::generateKey(); }
    lucyn::security::Nullbyte nb(key);

    lucyn::profile::Switcher sw(profile);
    lucyn::net::HttpClient client(sw);
    lucyn::net::Moonbug mb;
    client.setMoonbug(&mb);
    std::printf("[%s] impersonating: %s\n", sw.current().target.c_str(), client.impersonating() ? "yes" : "NO (stock libcurl or rejected)");

    mb.onCapture([&](const lucyn::net::Capture& c) {
        std::string sig = nb.sign(lucyn::net::Moonbug::toJson(c));
        std::printf("#%llu  %s  %ld  %.0fms  %zuB  seal %.12s  %s%s\n", (unsigned long long)c.id, c.method.c_str(), c.status,
                    c.timing.totalMs, c.responseBytes, sig.c_str(), c.url.c_str(), c.error.empty() ? "" : ("   ERROR: " + c.error).c_str());
        std::fflush(stdout);
    });

    lucyn::server::CaptureServer srv(mb, nb);
    if (!once) {
        std::string err;
        if (!expose.empty()) srv.expose(expose);
        if (raw  > 0 && !srv.activate(std::uint16_t(raw), &err))          std::fprintf(stderr, "raw stream: %s\n", err.c_str());
        if (sgn  > 0 && !srv.alsoUseNullbyte(std::uint16_t(sgn), &err))   std::fprintf(stderr, "signed stream: %s\n", err.c_str());
        if (dash > 0 && !srv.serveDashboard(std::uint16_t(dash), &err))   std::fprintf(stderr, "dashboard: %s\n", err.c_str());
        if (srv.rawPort())       std::printf("raw      ws://127.0.0.1:%u/stream\n", srv.rawPort());
        if (srv.signedPort())    std::printf("signed   ws://127.0.0.1:%u/stream   (POST /verify)\n", srv.signedPort());
        if (srv.dashboardPort()) std::printf("dashboard  http://127.0.0.1:%u/\n", srv.dashboardPort());
        std::fflush(stdout);
    }

    for (auto& u : urls) client.get(u);
    if (once) return 0;

    std::puts("type a url and press enter to fetch it (empty line quits)");
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) break;
        client.get(line);
    }
    client.setMoonbug(nullptr);
    return 0;
}
