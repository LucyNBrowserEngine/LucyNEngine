#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <fstream>
#include <functional>
#include <map>
#include <lucyn/core/paths.h>
#include <lucyn/shell/assets.h>
#include <lucyn/shell/hydrator.h>
#include <lucyn/shell/snapshot.h>
#include <lucyn/shell/url.h>

using namespace lucyn;
using namespace lucyn::shell;
namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

// A pretend JS engine: records what the Hydrator asks of it, and lets a test change the DOM per script.
struct FakeHost : ScriptHost {
    std::vector<std::string> calls;
    std::map<std::string, std::function<void(dom::Document&, NativeHooks&)>> effects;
    std::function<void(dom::Document&)> pageEffect;
    bool attachOk = true, idle = true, pageOk = true, throwInPage = false;
    std::weak_ptr<int> token;            // captured by a listener that lives INSIDE the Document: expires when the Document dies
    bool docAliveAtDetach = false;
    dom::Document* doc = nullptr;
    NativeHooks* hooks = nullptr;
    size_t bodyKidsAtStart = 999, bodyKidsAtEnd = 0;

    bool available() const override { return true; }
    bool attach(dom::Document& d, const std::string& url, NativeHooks& h, std::string* err) override {
        calls.push_back("attach " + url);
        if (!attachOk) { if (err) *err = "nope"; return false; }
        doc = &d; hooks = &h;
        auto t = std::make_shared<int>(0);
        token = t;
        d.onMutation([t](const dom::Document::Mutation&) {});
        return true;
    }
    ScriptOutcome runUserScript(const UserScript& s) override {
        calls.push_back("run " + s.name);
        size_t elems = doc->body()->querySelectorAll("*").size();   // whitespace text does not count
        if (s.runAt == RunAt::Start) bodyKidsAtStart = elems;
        else bodyKidsAtEnd = elems;
        if (s.source.find("THROW") != std::string::npos) return {false, "boom"};
        auto it = effects.find(s.name);
        if (it != effects.end()) it->second(*doc, *hooks);
        return {};
    }
    ScriptOutcome runPageScripts() override {
        calls.push_back("page");
        if (throwInPage) throw std::runtime_error("host exploded");
        if (pageEffect) pageEffect(*doc);
        return pageOk ? ScriptOutcome{} : ScriptOutcome{false, "page script threw"};
    }
    bool settle(int) override { calls.push_back("settle"); return idle; }
    void detach() override { calls.push_back("detach"); docAliveAtDetach = !token.expired(); doc = nullptr; }
};

static const char* kSite = R"(<!doctype html><html lang="en" class="dark"><head>
<title>  My  Site </title><meta name="x" content="y">
<link rel="shortcut icon" href="/static/fav.png"><style>p{}</style></head>
<body class="app" data-x="1"><div id="root"><p>hello</p></div><script>var a = 1 < 2;</script></body></html>)";

static Fetcher fetchOf(std::string body, long status = 200, std::string finalUrl = "") {
    return [=](const std::string&) { FetchResult r; r.ok = true; r.status = status; r.body = body; r.finalUrl = finalUrl; return r; };
}

int main() {
    fs::path tmp = fs::temp_directory_path() / "lucyn_shell_test";
    fs::remove_all(tmp);
    core::setDataRootForTests((tmp / "nothing-data").string());

    // ---- glob -----------------------------------------------------------------------------------------
    CHECK(globMatch("*", "anything"));
    CHECK(globMatch("https://*.example.com/*", "https://www.example.com/a/b?c=1"));
    CHECK(!globMatch("https://*.example.com/*", "https://example.org/"));
    CHECK(globMatch("a?c", "abc") && !globMatch("a?c", "ac"));
    CHECK(globMatch("", "") && !globMatch("", "x") && globMatch("*", ""));
    CHECK(!globMatch("abc", "ABC"));                                    // case-sensitive
    CHECK(!globMatch("*a*a*a*a*a*a*a*a*b", std::string(5000, 'a')));    // no catastrophic backtracking

    // ---- script headers ---------------------------------------------------------------------------
    ScriptRegistry reg;
    std::string err;
    auto s1 = reg.add("// ==LucyScript==\n// @name hide-banner\n// @run-at end\n// @match https://*.example.com/*\n// @match http://localhost*\n// @priority -5\n// ==/LucyScript==\nconsole.log(1)", "file1", &err);
    CHECK(s1 && s1->name == "hide-banner" && s1->runAt == RunAt::End && s1->match.size() == 2 && s1->priority == -5);
    auto s2 = reg.add("console.log(2)", "plain", &err);                 // no header: defaults
    CHECK(s2 && s2->name == "plain" && s2->runAt == RunAt::Start && s2->match.empty() && s2->priority == 0);
    auto s3 = reg.add("\xEF\xBB\xBF\r\n\r\n// note\r\n// ==LucyScript==\r\n// @run-at start\r\n// ==/LucyScript==\r\nx()", "bom", &err);
    CHECK(s3 && s3->runAt == RunAt::Start && s3->name == "bom");        // BOM, CRLF, leading comment tolerated
    CHECK(!reg.add("// ==LucyScript==\n// @run-at later\n// ==/LucyScript==\n", "bad1", &err) && err.find("run-at") != std::string::npos);
    CHECK(!reg.add("// ==LucyScript==\n// @priority abc\n// ==/LucyScript==\n", "bad2", &err) && err.find("priority") != std::string::npos);
    CHECK(!reg.add("// ==LucyScript==\n// @name x\n", "bad3", &err) && err.find("missing") != std::string::npos);
    CHECK(!reg.add("x()", "plain", &err) && err.find("already exists") != std::string::npos);   // duplicate name
    CHECK(!reg.add("x()", "", &err));                                    // nameless
    auto s4 = reg.add("// ==LucyScript==\n// @future-key whatever\n// ==/LucyScript==\n", "future", &err);
    CHECK(s4.has_value());                                               // unknown keys are ignored
    auto s5 = reg.add("var s = '// ==LucyScript==\\n// @run-at end';", "code-first", &err);
    CHECK(s5 && s5->runAt == RunAt::Start);                              // a header after code is just text
    CHECK(reg.size() == 5);
    CHECK(reg.remove("future") && !reg.remove("future") && reg.size() == 4);

    // ---- ordering: 22 scripts ---------------------------------------------------------------------------
    ScriptRegistry many;
    for (int i = 0; i < 20; ++i) {
        std::string hdr = "// ==LucyScript==\n// @name s" + std::to_string(i) + "\n// @run-at " + (i % 2 ? "end" : "start") +
                          "\n// @priority " + std::to_string(i % 4 == 0 ? -1 : 0) + "\n// ==/LucyScript==\n";
        CHECK(many.add(hdr, "x").has_value());
    }
    CHECK(many.add("// ==LucyScript==\n// @name only-example\n// @match https://example.com/*\n// ==/LucyScript==\n", "x").has_value());
    CHECK(many.add("// ==LucyScript==\n// @name only-other\n// @match https://other.com/*\n// ==/LucyScript==\n", "x").has_value());
    auto starts = many.forUrl("https://example.com/p", RunAt::Start);
    auto ends = many.forUrl("https://example.com/p", RunAt::End);
    CHECK(starts.size() == 11 && ends.size() == 10);                    // 10 even + only-example; 10 odd; only-other excluded
    CHECK(starts[0].name == "s0" && starts[1].name == "s4" && starts[2].name == "s8");   // priority -1 first, in add order
    CHECK(starts[5].name == "s2" && starts[6].name == "s6");            // then priority 0 in add order
    CHECK(starts.back().name == "only-example");                        // added last, so last
    CHECK(many.forUrl("https://other.com/", RunAt::Start).back().name == "only-other");

    // ---- loadDir ---------------------------------------------------------------------------------------------
    fs::path sdir = tmp / "scripts_in";
    fs::create_directories(sdir);
    std::ofstream(sdir / "b.js") << "// ==LucyScript==\n// @run-at end\n// ==/LucyScript==\nb()";
    std::ofstream(sdir / "a.js") << "a()";
    std::ofstream(sdir / "broken.js") << "// ==LucyScript==\n// @run-at never\n// ==/LucyScript==\n";
    std::ofstream(sdir / "notes.txt") << "ignored";
    ScriptRegistry fromDir;
    std::vector<std::string> errs;
    CHECK(fromDir.loadDir(sdir.string(), &errs) == 2);
    CHECK(errs.size() == 1 && errs[0].find("broken") != std::string::npos);   // bad one reported, others still load
    CHECK(fromDir.list()[0].name == "a" && fromDir.list()[1].name == "b");    // sorted by file name
    CHECK(fromDir.loadDir((tmp / "no_such_dir").string()) == 0);

    // ---- URLs ---------------------------------------------------------------------------------------------------
    const std::string base = "https://example.com:8080/a/b/page.html?q=1#top";
    CHECK(resolveUrl(base, "https://other.org/x") == "https://other.org/x");
    CHECK(resolveUrl(base, "//cdn.net/x.js") == "https://cdn.net/x.js");
    CHECK(resolveUrl(base, "/root.png") == "https://example.com:8080/root.png");
    CHECK(resolveUrl(base, "img/p.png") == "https://example.com:8080/a/b/img/p.png");
    CHECK(resolveUrl(base, "../up.png") == "https://example.com:8080/a/up.png");
    CHECK(resolveUrl(base, "../../../../way/up.png") == "https://example.com:8080/way/up.png");
    CHECK(resolveUrl(base, "./same.png?x=2#f") == "https://example.com:8080/a/b/same.png?x=2#f");
    CHECK(resolveUrl(base, "?z=9") == "https://example.com:8080/a/b/page.html?z=9");
    CHECK(resolveUrl(base, "#frag") == "https://example.com:8080/a/b/page.html?q=1#frag");
    CHECK(resolveUrl(base, "") == "https://example.com:8080/a/b/page.html?q=1");
    CHECK(resolveUrl(base, "data:image/png;base64,AAA") == "data:image/png;base64,AAA");
    CHECK(resolveUrl("https://example.com", "a.png") == "https://example.com/a.png");
    CHECK(resolveUrl("file:///home/u/index.html", "fav.ico") == "file:///home/u/fav.ico");
    CHECK(originOf(base) == "https://example.com:8080");
    CHECK(originOf("file:///x") == "" && originOf("data:text/html,hi") == "" && originOf("garbage") == "");
    CHECK(hostKey(base) == "example.com_8080");
    CHECK(hostKey("https://user:pw@Example.COM/") == "example.com");
    CHECK(hostKey("file:///x/y") == "local" && hostKey("nonsense") == "local");
    CHECK(hostKey("https://../") == "local");                           // never ".." as a folder name

    // ---- favicon ------------------------------------------------------------------------------------------------
    auto fav1 = dom::parseHtml("<head><link rel='shortcut icon' href='/s/f.png'></head>");
    CHECK(faviconUrl(*fav1, "https://a.com/x/y") == "https://a.com/s/f.png");
    auto fav2 = dom::parseHtml("<head><base href='https://cdn.a.com/assets/'><link rel='icon' href='i.png'></head>");
    CHECK(faviconUrl(*fav2, "https://a.com/") == "https://cdn.a.com/assets/i.png");
    auto fav3 = dom::parseHtml("<head><link rel='apple-touch-icon' href='/t.png'></head>");
    CHECK(faviconUrl(*fav3, "https://a.com/") == "https://a.com/t.png");
    auto fav4 = dom::parseHtml("<head><link rel='stylesheet' href='/x.css'><link rel='ICON' href=''></head>");
    CHECK(faviconUrl(*fav4, "https://a.com/p") == "https://a.com/favicon.ico");   // fallback
    CHECK(faviconUrl(*fav4, "file:///x.html") == "");                              // no origin, no link

    // ---- snapshots (memory + disk) ----------------------------------------------------------------------------------
    {
        Snapshots mem(false, "https://example.com/");
        CHECK(mem.take("one", "<p>1</p>") && mem.take("two", "<p>1</p><b>2</b>"));
        CHECK(!mem.take("../evil", "x", &err) && !mem.take("", "x", &err) && !mem.take("a b", "x", &err));
        CHECK(mem.take("one", "<p>again</p>") && mem.labels().size() == 2 && *mem.get("one") == "<p>again</p>");   // overwrite
        CHECK(mem.runDir().empty() && !fs::exists(tmp / "nothing-data" / "snapshots"));
        auto d = mem.diff("one", "two");
        CHECK(d && d->newTags() == std::vector<std::string>{"b"});
        CHECK(!mem.diff("one", "nope").has_value());
        Snapshots capped(false, "x");
        for (int i = 0; i < 64; ++i) CHECK(capped.take("s" + std::to_string(i), "x"));
        CHECK(!capped.take("s64", "x", &err));                             // spam guard
        CHECK(capped.take("s3", "y"));                                      // but existing labels still overwrite

        Snapshots disk(true, "https://example.com:8080/p");
        CHECK(disk.take("before", "<p>b</p>") && disk.take("after", "<p>a</p>") && disk.saveNote("diff.txt", "d"));
        CHECK(fs::path(disk.runDir()).parent_path().filename() == "example.com_8080");
        CHECK(slurp(fs::path(disk.runDir()) / "before.html") == "<p>b</p>");
        CHECK(slurp(fs::path(disk.runDir()) / "diff.txt") == "d");
        CHECK(!disk.saveNote("../x", "d", &err));
        Snapshots disk2(true, "https://example.com:8080/p");               // same second: must not collide
        CHECK(disk2.take("before", "z") && disk2.runDir() != disk.runDir());
    }

    // ---- native hooks ---------------------------------------------------------------------------------------------------
    {
        NativeHooks h;
        CHECK(h.add("a.b", [](const std::vector<std::string>& a) { return HookResult{true, std::to_string(a.size())}; }));
        CHECK(!h.add("a.b", [](auto&) { return HookResult{}; }) && !h.add("", [](auto&) { return HookResult{}; }));
        CHECK(h.call("a.b", {"x", "y"}).value == "2");
        CHECK(!h.call("nope", {}).ok);
        h.add("boom", [](const std::vector<std::string>&) -> HookResult { throw std::runtime_error("kaboom"); });
        auto r = h.call("boom", {});
        CHECK(!r.ok && r.value.find("kaboom") != std::string::npos);        // exceptions never cross the boundary
        CHECK((h.names() == std::vector<std::string>{"a.b", "boom"}));
    }

    // ---- mountSite ---------------------------------------------------------------------------------------------------------
    {
        auto shell = dom::parseHtml(defaultShellHtml());
        size_t headBefore = shell->head()->children.size();
        auto site = dom::parseHtml(kSite);
        CHECK(mountSite(*shell, *site));
        CHECK(shell->title() == "My Site");
        CHECK(shell->querySelector("link[rel='shortcut icon']") != nullptr);
        CHECK(shell->head()->children.size() > headBefore);
        CHECK(shell->querySelectorAll("script").size() >= 2);               // the shell's own inline scripts survived
        CHECK(*shell->documentElement()->attr("lang") == "en" && shell->documentElement()->hasClass("dark"));
        CHECK(shell->body()->hasClass("app") && *shell->body()->attr("data-x") == "1");
        CHECK(shell->getElementById("root") != nullptr && shell->getElementById("root")->ownerDocument == shell.get());
        CHECK(shell->querySelector("body > script") != nullptr);
        CHECK(site->getElementById("root") != nullptr);                     // source document untouched

        auto tpl = dom::parseHtml("<html><head><title>shell title</title></head><body><nav id=bar>chrome</nav></body></html>");
        CHECK(mountSite(*tpl, *site));
        CHECK(tpl->getElementsByTagName("title").size() == 1 && tpl->title() == "My Site");   // site's title wins
        CHECK(tpl->body()->firstChild()->id() == "bar");                     // shell chrome stays first
        auto noTitle = dom::parseHtml("<p>x</p>");
        CHECK(mountSite(*tpl, *noTitle) && tpl->title() == "My Site");       // a titleless site keeps what is there
    }

    // ---- Hydrator: order, hooks, diff, files ---------------------------------------------------------------------------------------
    {
        ScriptRegistry sr;
        sr.add("// ==LucyScript==\n// @name late\n// @run-at end\n// ==/LucyScript==\n", "x");
        sr.add("// ==LucyScript==\n// @name early\n// @run-at start\n// @priority -1\n// ==/LucyScript==\n", "x");
        sr.add("// ==LucyScript==\n// @name bad\n// @run-at start\n// ==/LucyScript==\nTHROW", "x");
        sr.add("// ==LucyScript==\n// @name other-site\n// @match https://nope.org/*\n// ==/LucyScript==\n", "x");

        FakeHost host;
        host.effects["early"] = [](dom::Document& d, NativeHooks& h) {
            h.call("snapshot.take", {"empty"});                              // start: page not mounted yet
            h.call("log", {"hello", "from", "early"});
            CHECK(h.call("page.url", {}).value == "https://site.test/index.html");
            CHECK(!h.call("snapshot.take", {"before"}).ok);                   // reserved
            CHECK(!h.call("snapshot.take", {"../x"}).ok);
            CHECK(!h.call("snapshot.get", {"missing"}).ok);
            (void)d;
        };
        host.effects["late"] = [](dom::Document& d, NativeHooks& h) {
            dom::Node* root = d.getElementById("root");
            root->appendChild(d.createElement("canvas"));
            dom::Node* sec = d.createElement("section");
            sec->setAttr("class", "added");
            root->appendChild(sec);
            d.head()->appendChild(d.createElement("noscript"));
            CHECK(h.call("snapshot.take", {"mid"}).ok);
            auto diff = h.call("snapshot.diff", {"before", "mid"});
            CHECK(diff.ok && diff.value.find("new tags: canvas") != std::string::npos);
            CHECK(h.call("snapshot.labels", {}).value == "before\nempty\nmid");
            CHECK(h.call("snapshot.get", {"mid"}).value.find("<canvas>") != std::string::npos);
        };
        host.pageEffect = [](dom::Document& d) { d.getElementById("root")->setAttr("data-hydrated", "1"); };

        std::vector<std::string> phases;
        Hydrator hy(fetchOf(kSite, 200, "https://site.test/index.html"), sr, host);
        hy.hooks.onFetchStart = [&](const std::string&) { phases.push_back("fetchstart"); };
        hy.hooks.onFetched = [&](const FetchResult& f) { phases.push_back("fetched"); CHECK(f.ok); };
        hy.hooks.onStartScripts = [&](dom::Document&) { phases.push_back("start"); };
        hy.hooks.onMounted = [&](dom::Document& d) { phases.push_back("mounted"); CHECK(d.getElementById("root") != nullptr); };
        hy.hooks.onSettled = [&](dom::Document&) { phases.push_back("settled"); };
        hy.hooks.onEndScripts = [&](dom::Document&) { phases.push_back("end"); };
        hy.hooks.onDone = [&](HydrateResult& r) { phases.push_back("done"); CHECK(r.ok); };
        hy.setShellHtml(defaultShellHtml());
        // a stale hook of the same name must not survive or break the next run
        hy.native().add("log", [](auto&) { return HookResult{true, "stale"}; });

        auto r = hy.hydrate("https://site.test/");
        CHECK(r.ok && r.error.empty());
        CHECK(r.url == "https://site.test/index.html");                      // redirect target is what scripts see
        CHECK(r.jsAvailable && r.settled);
        CHECK((phases == std::vector<std::string>{"fetchstart", "fetched", "start", "mounted", "settled", "end", "done"}));
        CHECK((host.calls == std::vector<std::string>{"attach https://site.test/index.html", "run early", "run bad", "page", "settle", "run late", "detach"}));
        CHECK(host.bodyKidsAtStart == 0);                                    // `start` = before the site exists
        CHECK(host.bodyKidsAtEnd > 0);                                       // `end` = site is there
        CHECK(r.scripts.size() == 3);                                        // other-site never matched
        CHECK(r.scripts[0].name == "early" && r.scripts[0].status == ScriptReport::Status::Ran);
        CHECK(r.scripts[1].name == "bad" && r.scripts[1].status == ScriptReport::Status::Failed && r.scripts[1].detail == "boom");
        CHECK(r.scripts[2].name == "late" && r.scripts[2].status == ScriptReport::Status::Ran);   // failure did not stop it
        CHECK(r.log.size() == 1 && r.log[0] == "hello from early");
        CHECK(r.title == "My Site");
        CHECK(r.faviconUrl == "https://site.test/static/fav.png");
        CHECK(r.document && r.document->getElementById("root") && *r.document->getElementById("root")->attr("data-hydrated") == "1");

        // before = the server's bytes; after = live page without shell scaffolding.
        CHECK(r.beforeHtml == kSite);
        CHECK(r.afterHtml.find("inject.js") == std::string::npos && r.afterHtml.find("hydrator.js") == std::string::npos);
        CHECK(r.afterHtml.find("<canvas>") != std::string::npos);
        auto nt = r.diff.newTags();
        CHECK((nt == std::vector<std::string>{"canvas", "noscript", "section"}));
        CHECK(r.diff.goneTags().empty());

        // files on disk
        CHECK(!r.snapshotDir.empty());
        CHECK(fs::path(r.snapshotDir).parent_path().filename() == "site.test");
        CHECK(slurp(fs::path(r.snapshotDir) / "before.html") == kSite);
        CHECK(slurp(fs::path(r.snapshotDir) / "after.html") == r.afterHtml);
        CHECK(slurp(fs::path(r.snapshotDir) / "diff.txt").find("new tags: canvas noscript section") != std::string::npos);
        CHECK(fs::exists(fs::path(r.snapshotDir) / "mid.html") && fs::exists(fs::path(r.snapshotDir) / "empty.html"));
        CHECK(slurp(fs::path(r.snapshotDir) / "empty.html").find("id=\"root\"") == std::string::npos);   // taken before the mount

        // hooks must not outlive the run (they point into its stack)
        CHECK(!hy.native().has("page.url") && !hy.native().has("snapshot.take") && !hy.native().has("log"));
    }

    // ---- Hydrator without JS (what ships today): honest, and mounting is lossless -------------------------------------------------------
    {
        ScriptRegistry sr;
        sr.add("// ==LucyScript==\n// @name s1\n// ==/LucyScript==\n", "x");
        sr.add("// ==LucyScript==\n// @name s2\n// @run-at end\n// ==/LucyScript==\n", "x");
        NullHost nul;
        Hydrator hy(fetchOf(kSite, 404, ""), sr, nul);
        hy.setShellHtml(defaultShellHtml());
        auto r = hy.hydrate("https://site.test/missing");
        CHECK(r.ok && !r.jsAvailable);
        CHECK(r.scripts.size() == 2);
        for (auto& s : r.scripts) CHECK(s.status == ScriptReport::Status::Skipped && s.detail == "no JS engine");
        CHECK(r.diff.empty());                                              // nothing ran, so mounting must change nothing
        CHECK(r.status == 404 && r.warnings.size() == 1 && r.warnings[0] == "HTTP 404");   // error pages still hydrate
        CHECK(r.title == "My Site");
    }

    // ---- Hydrator: failures never crash --------------------------------------------------------------------------------------------
    {
        ScriptRegistry sr;
        NullHost nul;
        Hydrator bad([](const std::string&) { FetchResult r; r.error = "connection refused"; return r; }, sr, nul);
        auto r = bad.hydrate("https://down.test/");
        CHECK(!r.ok && r.error == "connection refused" && !r.document);
        CHECK(!bad.hydrate("").ok);
        Hydrator none(Fetcher{}, sr, nul);
        CHECK(!none.hydrate("https://x/").ok);
        Hydrator thrower([](const std::string&) -> FetchResult { throw std::runtime_error("kaput"); }, sr, nul);
        auto t = thrower.hydrate("https://x/");
        CHECK(!t.ok && t.error.find("kaput") != std::string::npos);

        Hydrator empty(fetchOf(""), sr, nul);                               // empty body is a page, not a crash
        auto e = empty.hydrate("https://empty.test/");
        CHECK(e.ok && e.warnings.size() == 1);

        FakeHost fh; fh.attachOk = false;
        ScriptRegistry sr2;
        sr2.add("// ==LucyScript==\n// @name s\n// ==/LucyScript==\n", "x");
        Hydrator na(fetchOf(kSite), sr2, fh);
        auto a = na.hydrate("https://site.test/");
        CHECK(a.ok && !a.jsAvailable && a.scripts.size() == 1 && a.scripts[0].detail == "JS host could not attach");
        CHECK(fh.calls == std::vector<std::string>{"attach https://site.test/"});     // no detach for an attach that failed

        FakeHost slow; slow.idle = false; slow.pageOk = false;
        Hydrator sl(fetchOf(kSite), sr, slow);
        auto s = sl.hydrate("https://site.test/");
        CHECK(s.ok && !s.settled && s.warnings.size() >= 2);                // reported, not fatal

        // detach() must always see a LIVE Document: on the normal path...
        FakeHost okHost;
        Hydrator okH(fetchOf(kSite), sr2, okHost);
        auto okR = okH.hydrate("https://site.test/");
        CHECK(okR.ok && okHost.docAliveAtDetach);
        // ...and on the exception path, where the shell Document used to be destroyed before the guard called detach()
        FakeHost boom; boom.throwInPage = true;
        Hydrator bm(fetchOf(kSite), sr2, boom);
        auto bR = bm.hydrate("https://site.test/");
        CHECK(!bR.ok && bR.error.find("host exploded") != std::string::npos);
        CHECK(boom.calls.back() == "detach" && boom.docAliveAtDetach);
    }

    // ---- shell files ---------------------------------------------------------------------------------------------------------------------
    {
        CHECK(std::string(loadShellHtml()) == defaultShellHtml());          // nothing on disk yet: built-in
        CHECK(ensureShellFiles(&err));
        fs::path sd = tmp / "nothing-data" / "shell";
        CHECK(fs::exists(sd / "shell.html") && fs::exists(sd / "inject.js") && fs::exists(sd / "hydrator.js"));
        CHECK(fs::exists(tmp / "nothing-data" / "scripts" / "example.js.disabled"));
        CHECK(slurp(sd / "shell.html").find("<script>") != std::string::npos);   // shell.html has inline scripts
        std::ofstream(sd / "shell.html", std::ios::trunc) << "<html><head></head><body><div id=chrome></div></body></html>";
        CHECK(ensureShellFiles(&err));                                       // never overwrites the user's edit
        CHECK(loadShellHtml().find("id=chrome") != std::string::npos);
        ScriptRegistry sr;
        CHECK(sr.loadDir(ScriptRegistry::defaultDir()) == 0);                // .disabled is not loaded
        NullHost nul;
        Hydrator hy(fetchOf(kSite), sr, nul);                                // no setShellHtml: picks the user's file
        auto r = hy.hydrate("https://site.test/");
        CHECK(r.ok && r.document->getElementById("chrome") != nullptr && r.afterHtml.find("id=\"chrome\"") == std::string::npos);
    }

    core::setDataRootForTests("");
    fs::remove_all(tmp);
    std::printf(failures ? "shell: %d FAILED\n" : "shell: all passed\n", failures);
    return failures ? 1 : 0;
}
