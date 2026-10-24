#include <cstdio>
#include <filesystem>
#include <lucyn/search/search.h>

namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    using namespace lucyn::search;

    // ---- URL encoding (RFC 3986 unreserved chars stay literal) ----
    {
        Registry r;
        CHECK(r.url("google", "") == "https://www.google.com/search?q=");
        CHECK(r.url("google", "a&b=c") == "https://www.google.com/search?q=a%26b%3Dc");
        CHECK(r.url("google", "日本語") == "https://www.google.com/search?q=%E6%97%A5%E6%9C%AC%E8%AA%9E");
        CHECK(r.url("google", "hello world") == "https://www.google.com/search?q=hello%20world");
        CHECK(r.url("google", "AZaz09-_.~") == "https://www.google.com/search?q=AZaz09-_.~");
    }

    // ---- add() validation & upsert ----
    {
        Registry r;
        CHECK(!r.add({"", "Name", "https://x/?q={query}"}));             // empty id
        CHECK(!r.add({"id", "", "https://x/?q={query}"}));               // empty name
        CHECK(!r.add({"id", "Name", "https://x/?q="}));                  // missing {query}
        CHECK(!r.add({"id", "Name", "https://x/?q={query}&r={query}"})); // two {query}
        CHECK(r.add({"empty", "Empty", ""}));                            // empty template allowed

        const size_t before = r.engines().size();
        CHECK(r.add({"ok", "OK", "https://x/?q={query}"}));
        CHECK(r.engines().size() == before + 1);
        CHECK(r.add({"ok", "OK2", "https://y/?q={query}"}));             // upsert, no growth
        CHECK(r.engines().size() == before + 1);
        auto e = r.find("ok");
        CHECK(e && e->name == "OK2" && e->queryTemplate == "https://y/?q={query}");
    }

    // ---- url() error paths ----
    {
        Registry r;
        std::string err;
        CHECK(!r.url("nope", "q", &err));
        CHECK(err.find("unknown search engine") != std::string::npos);
        CHECK(!r.url("searxng", "q", &err));                              // needs configuration
        CHECK(err.find("needs a configured search URL") != std::string::npos);
    }

    // ---- Options: region / language / count / safe search ----
    {
        Registry r;
        Options o;
        o.region = "us"; o.language = "en"; o.resultsPerPage = 100;
        auto u = r.url("google", "cats", o);
        CHECK(u && u->find("q=cats") != std::string::npos);
        CHECK(u && u->find("gl=us") != std::string::npos);
        CHECK(u && u->find("hl=en") != std::string::npos);
        CHECK(u && u->find("num=100") != std::string::npos);

        Options s; s.safeSearch = 2;
        auto u2 = r.url("google", "cats", s);                             // google has no numeric safe param
        CHECK(u2 && u2->find("safe=") == std::string::npos);

        auto u3 = r.url("duckduckgo", "cats", s);                         // ddg: fixed flag kp=1
        CHECK(u3 && u3->find("kp=1") != std::string::npos);

        Options b; b.resultsPerPage = 50;
        auto u4 = r.url("bing", "cats", b);                               // bing: count=50
        CHECK(u4 && u4->find("count=50") != std::string::npos);
    }

    // ---- default engine ----
    {
        Registry r;
        CHECK(r.defaultId() == "brave");
        auto u = r.url("hello");                                          // default overload
        CHECK(u && u->find("search.brave.com") != std::string::npos);
        CHECK(r.setDefault("google"));
        CHECK(r.defaultId() == "google");
        CHECK(!r.setDefault("nope"));                                     // unknown -> false, unchanged
        CHECK(r.defaultId() == "google");
        auto u2 = r.url("hello");
        CHECK(u2 && u2->find("google.com") != std::string::npos);
    }

    // ---- keyword shortcuts ----
    {
        Registry r;
        auto g = r.findByKeyword("g");
        CHECK(g && g->id == "google");
        CHECK(!r.findByKeyword(""));                                      // empty never matches
        CHECK(!r.findByKeyword("zzz"));
        r.add({"custom", "Custom", "https://c/?q={query}", "c"});
        auto c = r.findByKeyword("c");
        CHECK(c && c->id == "custom");
    }

    // ---- parseUrl reverse lookup ----
    {
        Registry r;
        auto p = r.parseUrl("https://www.google.com/search?q=hello%20world");
        CHECK(p && p->engineId == "google" && p->query == "hello world");
        auto p2 = r.parseUrl("https://www.google.com/search?q=cats&gl=us&hl=en");   // options ride along
        CHECK(p2 && p2->engineId == "google" && p2->query == "cats");
        auto p3 = r.parseUrl("https://example.com/not-a-search");
        CHECK(!p3);
        // Round-trip through url() with options.
        Options o; o.region = "us";
        auto u = r.url("google", "dogs & cats", o);
        CHECK(u);
        auto p4 = r.parseUrl(*u);
        CHECK(p4 && p4->engineId == "google" && p4->query == "dogs & cats");
    }

    // ---- persistence: save/load round-trip ----
    {
        Registry r;
        r.add({"custom", "Custom", "https://c/?q={query}", "c", "", "gl={region}"});
        r.setDefault("custom");
        fs::path tmp = fs::temp_directory_path() / "lucyn_search_test.json";
        std::string err;
        CHECK(r.saveToFile(tmp.string(), &err));
        Registry r2;
        CHECK(r2.loadFromFile(tmp.string(), &err));
        CHECK(r2.defaultId() == "custom");
        auto e = r2.find("custom");
        CHECK(e && e->queryTemplate == "https://c/?q={query}" && e->keyword == "c" && e->regionParam == "gl={region}");
        CHECK(r2.find("google"));                                          // defaults still present (load merges)
        fs::remove(tmp);
    }

    // ---- loadFromJson validation ----
    {
        Registry r;
        std::string err;
        CHECK(!r.loadFromJson("{ not json", &err));
        CHECK(!r.loadFromJson("[1,2]", &err));                             // not an object
        CHECK(!r.loadFromJson("{\"engines\":\"x\"}", &err));               // engines not an array
        CHECK(!r.loadFromJson("{\"default\":\"nope\"}", &err));            // unknown default
        CHECK(!r.loadFromJson(
            "{\"engines\":[{\"id\":\"\",\"name\":\"n\",\"queryTemplate\":\"https://x/?q={query}\"}]}", &err)); // empty id
        CHECK(r.loadFromJson(
            "{\"default\":\"google\",\"engines\":[{\"id\":\"mine\",\"name\":\"Mine\","
            "\"queryTemplate\":\"https://m/?q={query}\",\"keyword\":\"mi\"}]}", &err));
        CHECK(r.defaultId() == "google");
        auto m = r.find("mine");
        CHECK(m && m->keyword == "mi");
    }

    std::printf(failures ? "search: %d FAILED\n" : "search: all passed\n", failures);
    return failures ? 1 : 0;
}
