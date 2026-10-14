// Identity tests: generation, consistency, client hints, storage, manager.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <lucyn/profile/identity.h>

using namespace lucyn::profile;
namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static Profile P(const char* target) { return Registry::builtin().describe(target); }

static void testClientHints() {
    // Real Chrome headers for these versions, from memory of captured traffic.
    // If this fails, the brand algorithm drifted from Chromium's.
    struct { int v; const char* want; } cases[] = {
        {107, "\"Google Chrome\";v=\"107\", \"Chromium\";v=\"107\", \"Not=A?Brand\";v=\"24\""},
        {110, "\"Chromium\";v=\"110\", \"Not A(Brand\";v=\"24\", \"Google Chrome\";v=\"110\""},
        {116, "\"Chromium\";v=\"116\", \"Not)A;Brand\";v=\"24\", \"Google Chrome\";v=\"116\""},
        {120, "\"Not_A Brand\";v=\"8\", \"Chromium\";v=\"120\", \"Google Chrome\";v=\"120\""},
        {124, "\"Chromium\";v=\"124\", \"Google Chrome\";v=\"124\", \"Not-A.Brand\";v=\"99\""},
        {131, "\"Google Chrome\";v=\"131\", \"Chromium\";v=\"131\", \"Not_A Brand\";v=\"24\""},
        {133, "\"Not(A:Brand\";v=\"99\", \"Google Chrome\";v=\"133\", \"Chromium\";v=\"133\""},
        {136, "\"Chromium\";v=\"136\", \"Google Chrome\";v=\"136\", \"Not.A/Brand\";v=\"99\""},
    };
    for (const auto& c : cases) {
        std::string got = formatSecChUa(clientHintBrands(Family::Chrome, c.v));
        if (got != c.want) std::printf("   chrome %d: got %s\n", c.v, got.c_str());
        CHECK(got == c.want);
    }
    CHECK(clientHintBrands(Family::Safari, 18).empty());
    CHECK(clientHintBrands(Family::Firefox, 147).empty());
    CHECK(formatSecChUa(clientHintBrands(Family::Edge, 101)).find("Microsoft Edge") != std::string::npos);

    CHECK(acceptLanguageFor(Family::Chrome,  {"en-US", "en"}) == "en-US,en;q=0.9");
    CHECK(acceptLanguageFor(Family::Chrome,  {"de-DE", "de", "en-US", "en"}) == "de-DE,de;q=0.9,en-US;q=0.8,en;q=0.7");
    CHECK(acceptLanguageFor(Family::Firefox, {"en-US", "en"}) == "en-US,en;q=0.5");
    CHECK(acceptLanguageFor(Family::Firefox, {"de", "en-US", "en"}) == "de,en-US;q=0.7,en;q=0.3");
}

static void testDeterminism() {
    Profile p = P("chrome146");
    std::string err;
    auto a = generateIdentity(p, "default", 12345, {}, &err);
    auto b = generateIdentity(p, "default", 12345, {}, &err);
    auto c = generateIdentity(p, "default", 12346, {}, &err);
    CHECK(a && b && c);
    CHECK(toJson(*a) == toJson(*b));        // same seed -> same person
    CHECK(toJson(*a) != toJson(*c));
    CHECK(a->id != c->id);
    CHECK(a->id.size() == 36 && a->id[14] == '4');   // uuid v4 shape

    auto bad = generateIdentity(P("tor145"), "default", 1, {}, &err);
    CHECK(!bad && !err.empty());
    IdentityOptions o; o.region = "atlantis";
    CHECK(!generateIdentity(p, "default", 1, o, &err));
    CHECK(!generateIdentity(P("not_a_browser"), "default", 1, {}, &err));

    // region + overrides
    IdentityOptions de; de.region = "de";
    auto d = generateIdentity(p, "default", 1, de);
    CHECK(d->device.timezone == "Europe/Berlin" && d->device.languages[0] == "de-DE");
    IdentityOptions tz; tz.timezone = "Asia/Seoul"; tz.languages = {"ko-KR", "ko"};
    auto k = generateIdentity(p, "default", 1, tz);
    CHECK(k->device.timezone == "Asia/Seoul" && k->region == "custom");
}

static void testEveryPersonaIsConsistent() {
    int checked = 0;
    for (const auto& prof : Registry::builtin().all()) {
        if (prof.family == Family::Tor) continue;
        for (uint64_t seed = 1; seed <= 200; ++seed) {
            auto id = generateIdentity(prof, "default", seed * 7919);
            CHECK(id.has_value());
            if (!id) continue;
            auto issues = checkConsistency(prof, *id);
            if (!issues.empty()) std::printf("   %s seed %llu: %s\n", prof.target.c_str(), (unsigned long long)seed, issues[0].c_str());
            CHECK(issues.empty());
            ++checked;
        }
    }
    CHECK(checked > 5000);
}

static void testBrowsersLookLikeThemselves() {
    auto sf = [](const char* t) { Profile p = P(t); auto id = generateIdentity(p, "x", 42); return surfaceFor(p, *id); };
    Surface ch = sf("chrome146"), sa = sf("safari184"), ff = sf("firefox147"), an = sf("chrome131_android"), ip = sf("safari184_ios");

    CHECK(ch.vendor == "Google Inc." && ch.hasWindowChrome && ch.hasUserAgentData && ch.hasDeviceMemory);
    CHECK(sa.vendor == "Apple Computer, Inc." && !sa.hasWindowChrome && !sa.hasUserAgentData && sa.secChUa.empty());
    CHECK(ff.vendor.empty() && !ff.hasDeviceMemory && !ff.hasBattery && ff.productSub == "20100101");
    CHECK(sa.webglRenderer == "Apple GPU" && ip.webglRenderer == "Apple GPU");

    CHECK(ch.userAgent.find("Chrome/146.0.0.0") != std::string::npos);
    CHECK(sa.userAgent.find("Version/18.4 Safari/605.1.15") != std::string::npos);
    CHECK(ip.userAgent.find("iPhone OS 18_4") != std::string::npos && ip.platform == "iPhone");
    CHECK(an.userAgent.find("Android 10; K") != std::string::npos && an.secChUaMobile == "?1" && an.maxTouchPoints == 5);
    CHECK(ff.userAgent.find("Firefox/147.0") != std::string::npos);

    // JS-visible and HTTP-visible data come from ONE place and must agree
    CHECK(ch.brands.size() == 3 && formatSecChUa(ch.brands) == ch.secChUa);
    CHECK(ch.secChUaPlatform == "macOS" || ch.secChUaPlatform == "Windows");

    IdentityOptions fv; fv.chromeFullVersion = "146.0.7680.80";
    Profile p = P("chrome146"); auto id = generateIdentity(p, "x", 42);
    Surface full = surfaceFor(p, *id, fv);
    bool found = false;
    for (const auto& b : full.fullVersionList) if (b.brand == "Chromium") found = b.version == "146.0.7680.80";
    CHECK(found);
}

static void testPeopleDiffer() {
    // The flaw in the sample files: regenerating gave the SAME machine with new noise.
    {   // a Mac persona also varies (chip, screen), but cores follow the chip
        std::set<std::string> chips;
        for (uint64_t s = 1; s <= 300; ++s) chips.insert(generateIdentity(P("safari184"), "default", s)->device.gpuName);
        CHECK(chips.size() >= 4);
    }
    Profile p = P("chrome116");   // Windows 10 persona
    std::set<std::string> gpus, screens, cores;
    for (uint64_t s = 1; s <= 500; ++s) {
        auto id = generateIdentity(p, "default", s);
        gpus.insert(id->device.gpuName);
        screens.insert(std::to_string(id->device.screenW) + "x" + std::to_string(id->device.screenH));
        cores.insert(std::to_string(id->device.cpuCores));
    }
    CHECK(gpus.size() >= 4);
    CHECK(screens.size() >= 4);
    CHECK(cores.size() >= 4);
}

static void testJsonRoundTrip() {
    for (const char* t : {"chrome146", "safari184", "firefox147", "chrome131_android", "safari260_ios"}) {
        auto id = generateIdentity(P(t), "work-1", 987654321987654321ull);
        std::string err;
        auto back = identityFromJson(toJson(*id), &err);
        CHECK(back.has_value());
        if (back) CHECK(toJson(*back) == toJson(*id));        // exact, including 17-digit doubles and the 64-bit seed
        if (back) CHECK(back->seed == 987654321987654321ull);
    }
    std::string err;
    CHECK(!identityFromJson("not json", &err) && !err.empty());
    CHECK(!identityFromJson("{}", &err) && err.find("missing") != std::string::npos);
    CHECK(!identityFromJson("{\"schema\":2}", &err));
}

static void testStore() {
    fs::path dir = fs::temp_directory_path() / ("lucyn_identity_test_" + std::to_string(newSeed() % 1000000));
    IdentityStore store(dir.string());
    std::string err;
    bool created = false;

    CHECK(IdentityStore::validName("default") && IdentityStore::validName("work_2-b"));
    CHECK(!IdentityStore::validName("") && !IdentityStore::validName("../evil") && !IdentityStore::validName("a/b") && !IdentityStore::validName("a.b"));
    CHECK(!store.loadOrCreate("../evil", P("chrome116"), {}, &err));

    auto first = store.loadOrCreate("default", P("chrome116"), {}, &err, &created);
    CHECK(first && created);
    auto again = store.loadOrCreate("default", P("chrome116"), {}, &err, &created);
    CHECK(again && !created);
    if (first && again) CHECK(toJson(*first) == toJson(*again));          // same person tomorrow

    auto upgraded = store.loadOrCreate("default", P("chrome110"), {}, &err, &created);   // same OS: same machine
    CHECK(upgraded && !created);
    if (first && upgraded) CHECK(upgraded->id == first->id);

    auto mac = store.loadOrCreate("default", P("safari184"), {}, &err, &created);        // other OS: its own machine
    CHECK(mac && created && mac->os == OsClass::MacOS);
    if (first && mac) CHECK(mac->id != first->id);
    auto mac2 = store.loadOrCreate("default", P("firefox147"), {}, &err, &created);      // Firefox on that same Mac
    CHECK(mac2 && !created);
    if (mac && mac2) CHECK(mac2->id == mac->id);

    auto other = store.loadOrCreate("alt", P("chrome116"), {}, &err, &created);          // different person
    CHECK(other && created);
    if (first && other) CHECK(other->id != first->id);

    auto fresh = store.regenerate("default", P("chrome116"), {}, &err);
    CHECK(fresh && first && fresh->id != first->id);

    // a damaged file is reported, never silently replaced
    { std::ofstream f(store.pathFor("alt", OsClass::Windows), std::ios::trunc); f << "{ broken"; }
    err.clear();
    CHECK(!store.loadOrCreate("alt", P("chrome116"), {}, &err) && !err.empty());

    // a file moved under the wrong name is rejected
    fs::copy_file(store.pathFor("default", OsClass::MacOS), store.pathFor("copy", OsClass::MacOS));
    Identity tmp;
    CHECK(store.load("copy", OsClass::MacOS, tmp, &err) == IdentityStore::Status::Corrupt);

    CHECK(!fs::exists(store.pathFor("default", OsClass::Windows).append(".tmp")));       // no temp litter
    std::error_code ec;
    fs::remove_all(dir, ec);
}

static void testManager() {
    fs::path dir = fs::temp_directory_path() / ("lucyn_manager_test_" + std::to_string(newSeed() % 1000000));
    Switcher sw("chrome116");
    {
        IdentityManager mgr(sw, IdentityStore(dir.string()), "default");
        auto win = mgr.identity();
        CHECK(win && win->os == OsClass::Windows);
        CHECK(mgr.surface() && mgr.surface()->userAgent.find("Chrome/116") != std::string::npos);

        std::string err;
        CHECK(sw.select("safari184", &err));
        CHECK(mgr.identity() && mgr.identity()->os == OsClass::MacOS);
        CHECK(mgr.surface() && mgr.surface()->secChUa.empty() && mgr.surface()->vendor == "Apple Computer, Inc.");

        CHECK(sw.select("chrome110"));                                    // back to the Windows machine
        CHECK(mgr.identity() && win && mgr.identity()->id == win->id);

        CHECK(!sw.select("tor145", &err) && !err.empty());                // cannot build an identity: refused
        CHECK(sw.current().target == "chrome110");
    }
    CHECK(sw.select("tor145"));                                           // manager gone, so its validator is gone too
    std::error_code ec;
    fs::remove_all(dir, ec);
}

int main() {
    testClientHints();
    testDeterminism();
    testEveryPersonaIsConsistent();
    testBrowsersLookLikeThemselves();
    testPeopleDiffer();
    testJsonRoundTrip();
    testStore();
    testManager();
    std::puts(failures ? "FAILED" : "identity module: all tests passed");
    return failures ? 1 : 0;
}
