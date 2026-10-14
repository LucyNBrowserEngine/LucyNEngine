// Plain C++ tests: no framework, no network.
#include <cstdio>
#include <set>
#include <thread>
#include <vector>
#include <lucyn/profile/profile.h>

using namespace lucyn::profile;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static void testRegistry() {
    const auto& reg = Registry::builtin();

    std::set<std::string> seen;
    for (const auto& p : reg.all()) CHECK(seen.insert(p.target).second);   // no duplicates

    for (Family f : {Family::Chrome, Family::Edge, Family::Safari, Family::Firefox, Family::Tor})
        CHECK(reg.latest(f) != nullptr);

    CHECK(reg.resolve("chrome")  == "chrome146");
    CHECK(reg.resolve("firefox") == "firefox147");
    CHECK(reg.resolve("safari")  == "safari2601");
    CHECK(!reg.find(reg.resolve("chrome"))->alt);
    CHECK(!reg.find(reg.resolve("safari"))->mobile);

    CHECK(reg.resolve("safari184") == "safari184");   // concrete passes through
    CHECK(reg.resolve("chrome999") == "chrome999");   // so does a target newer than the table

    Profile u = reg.describe("chrome999_android");
    CHECK(u.family == Family::Chrome && u.mobile && u.version == "999");
    CHECK(reg.describe("not_a_browser").family == Family::Unknown);
    CHECK(reg.describe("a").alt == false);            // tiny names must not read out of bounds
    CHECK(reg.describe("").target.empty());

    CHECK(reg.find("chrome133a")->alt);
    CHECK(reg.find("safari184_ios")->mobile);
    CHECK(reg.find("chrome146")->h3);
    CHECK(reg.byFamily(Family::Tor).size() == 1);
}

static void testSwitcher() {
    Switcher sw("chrome");
    CHECK(sw.current().target == "chrome146");

    std::vector<std::string> log;
    auto id = sw.subscribe([&](const Profile& a, const Profile& b) { log.push_back(a.target + ">" + b.target); });

    std::string err;
    CHECK(sw.select("safari184", &err));
    CHECK(sw.current().target == "safari184");
    CHECK(log.size() == 1 && log[0] == "chrome146>safari184");

    CHECK(sw.select("safari184"));          // same target: success, but silent
    CHECK(log.size() == 1);

    CHECK(sw.select("firefox"));            // alias
    CHECK(sw.current().target == "firefox147");

    CHECK(!sw.select("", &err) && !err.empty());   // empty name refused
    CHECK(sw.current().target == "firefox147");

    sw.unsubscribe(id);
    CHECK(sw.select("tor145"));
    CHECK(log.size() == 2);                 // unsubscribed listener stayed quiet

    // validators can veto; the old profile must survive
    Switcher::Id vid = sw.addValidator([](const Profile& p, std::string* e) {
        if (p.family == Family::Edge) { *e = "no edge today"; return false; }
        return true;
    });
    CHECK(!sw.select("edge101", &err));
    CHECK(err == "no edge today");
    CHECK(sw.current().target == "tor145");
    CHECK(sw.select("chrome131"));
    sw.removeValidator(vid);
    CHECK(sw.select("edge101"));

    // strict mode refuses names the table does not know
    Switcher strict("safari");
    strict.setStrict(true);
    CHECK(!strict.select("chrome999", &err));
    CHECK(strict.current().target == "safari2601");
    CHECK(strict.select("chrome124"));

    // lenient mode (default) lets newer targets through
    Switcher lenient("chrome");
    CHECK(lenient.select("chrome999"));
    CHECK(lenient.current().version == "999");

    // a listener may read current() without deadlocking
    Switcher re("chrome");
    std::string seen;
    re.subscribe([&](const Profile&, const Profile&) { seen = re.current().target; });
    re.select("safari180");
    CHECK(seen == "safari180");
}

static void testThreads() {
    Switcher sw("chrome");
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([&sw, t] {
            for (int i = 0; i < 500; ++i) { sw.select((i + t) % 2 ? "safari184" : "firefox147"); (void)sw.current(); }
        });
    for (auto& t : ts) t.join();
    std::string now = sw.current().target;
    CHECK(now == "safari184" || now == "firefox147");
}

static void testJson() {
    Profile p = Registry::builtin().describe("safari184");
    CHECK(toJson(p) == "{\"target\":\"safari184\",\"family\":\"safari\",\"version\":\"18.4\","
                       "\"os\":\"macOS Sequoia\",\"mobile\":false,\"h3\":false,\"alt\":false}");

    Profile evil;
    evil.target = "a\"b\\c\n";
    CHECK(toJson(evil).find("\"a\\\"b\\\\c\\n\"") != std::string::npos);   // escaping works

    CHECK(toJson(std::vector<Profile>{}) == "[]");
    CHECK(toJson(std::vector<Profile>{p, p}).front() == '[');
}

int main() {
    testRegistry();
    testSwitcher();
    testThreads();
    testJson();
    std::puts(failures ? "FAILED" : "profile module: all tests passed");
    return failures ? 1 : 0;
}
