// lucyn-fetch: fetch a URL while switching browser identities.
//
//   lucyn-fetch --probe                          which built-in profiles does the linked lib accept?
//   lucyn-fetch -i safari184 <url>
//   lucyn-fetch -i chrome,safari,firefox <url>   switch identities in one run
//   lucyn-fetch -i chrome --body <url>           also print the response body (first 20000 bytes)
//   lucyn-fetch -i chrome --headers <url>        also print the response headers
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <lucyn/net/http_client.h>

using namespace lucyn;

static std::vector<std::string> splitCommas(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t c = s.find(',', start);
        if (c == std::string::npos) c = s.size();
        if (c > start) out.push_back(s.substr(start, c - start));
        start = c + 1;
    }
    return out;
}

int main(int argc, char** argv) {
    std::string targets = "chrome", url;
    bool probe = false, showBody = false, showHeaders = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--probe")) probe = true;
        else if (!std::strcmp(argv[i], "--body")) showBody = true;
        else if (!std::strcmp(argv[i], "--headers")) showHeaders = true;
        else if (!std::strcmp(argv[i], "-i") && i + 1 < argc) targets = argv[++i];
        else url = argv[i];
    }

    if (probe) {
        if (!net::HttpClient::impersonationBuiltIn()) {
            std::puts("Built against stock libcurl: nothing can be impersonated.\n"
                      "Put libcurl-impersonate in third_party/curl-impersonate/ and re-run cmake (see third_party/README.md)");
            return 2;
        }
        int ok = 0;
        const auto& all = profile::Registry::builtin().all();
        for (const auto& p : all) {
            bool good = net::HttpClient::probe(p);
            ok += good;
            std::printf("%s %s\n", good ? "[ok]  " : "[miss]", p.target.c_str());
        }
        std::printf("\n%d / %zu built-in profiles accepted by the linked library\n", ok, all.size());
        return 0;
    }

    if (url.empty()) {
        std::puts("usage: lucyn-fetch --probe | lucyn-fetch -i <target[,target...]> [--body] [--headers] <url>");
        return 1;
    }

    auto list = splitCommas(targets);
    if (list.empty()) list.push_back("chrome");

    profile::Switcher sw(list[0]);      // the shared identity
    net::HttpClient   client(sw);       // follows it

    int rc = 0;
    for (const auto& t : list) {
        std::string err;
        if (!sw.select(t, &err)) {
            std::printf("== %s: refused: %s\n", t.c_str(), err.c_str());
            rc = 3;
            continue;
        }
        profile::Profile p = sw.current();
        std::printf("== %s (%s %s, %s)%s\n", p.target.c_str(), profile::familyName(p.family),
                    p.version.c_str(), p.os.c_str(),
                    client.impersonating() ? "" : "  [NOT impersonating]");
        if (!client.impersonating() && !client.lastError().empty())
            std::printf("   note: %s\n", client.lastError().c_str());

        net::Response r = client.get(url);
        if (!r.ok()) { std::printf("   error: %s\n", r.error.c_str()); rc = 4; continue; }
        std::printf("   status %ld, %zu bytes, %.2fs\n", r.status, r.body.size(), r.totalTime);
        if (showHeaders)
            for (const auto& h : r.headers) std::printf("   %s: %s\n", h.first.c_str(), h.second.c_str());
        if (showBody) {
            const size_t cap = 20000;
            std::fwrite(r.body.data(), 1, r.body.size() < cap ? r.body.size() : cap, stdout);
            std::printf("\n%s", r.body.size() > cap ? "   [body truncated at 20000 bytes]\n" : "");
        }
    }
    return rc;
}
