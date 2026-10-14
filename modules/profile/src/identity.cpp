#include <lucyn/profile/identity.h>
#include "detail.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

namespace lucyn::profile {

// ------------------------------------------------------------------ os class --

const char* osClassName(OsClass o) {
    switch (o) {
        case OsClass::Windows: return "windows";
        case OsClass::MacOS:   return "macos";
        case OsClass::Android: return "android";
        case OsClass::IOS:     return "ios";
        default:               return "unknown";
    }
}

OsClass parseOsClass(std::string_view s) {
    for (OsClass o : {OsClass::Windows, OsClass::MacOS, OsClass::Android, OsClass::IOS})
        if (s == osClassName(o)) return o;
    return OsClass::Unknown;
}

OsClass osClassFor(const Profile& p) {
    auto has = [&](const char* s) { return p.os.find(s) != std::string::npos; };
    if (has("Windows")) return OsClass::Windows;
    if (has("macOS"))   return OsClass::MacOS;
    if (has("Android")) return OsClass::Android;
    if (has("iOS"))     return OsClass::IOS;
    switch (p.family) {   // unknown target: fall back to the most common home of that browser
        case Family::Chrome:
        case Family::Edge:    return p.mobile ? OsClass::Android : OsClass::Windows;
        case Family::Safari:  return p.mobile ? OsClass::IOS : OsClass::MacOS;
        case Family::Firefox:
        case Family::Tor:     return OsClass::Windows;
        default:              return OsClass::Unknown;
    }
}

// ------------------------------------------------------------------- regions --

const std::vector<Region>& regions() {
    static const std::vector<Region> r = {
        {"us-east", "America/New_York",      {"en-US", "en"}},
        {"us-west", "America/Los_Angeles",   {"en-US", "en"}},
        {"uk",      "Europe/London",         {"en-GB", "en"}},
        {"de",      "Europe/Berlin",         {"de-DE", "de", "en-US", "en"}},
        {"fr",      "Europe/Paris",          {"fr-FR", "fr", "en-US", "en"}},
        {"jp",      "Asia/Tokyo",            {"ja-JP", "ja", "en-US", "en"}},
        {"kr",      "Asia/Seoul",            {"ko-KR", "ko", "en-US", "en"}},
        {"in",      "Asia/Kolkata",          {"en-IN", "en"}},
        {"br",      "America/Sao_Paulo",     {"pt-BR", "pt", "en-US", "en"}},
        {"au",      "Australia/Sydney",      {"en-AU", "en"}},
    };
    return r;
}

const Region* findRegion(std::string_view id) {
    for (const auto& r : regions())
        if (r.id == id) return &r;
    return nullptr;
}

// ----------------------------------------------------------------------- rng --
// SplitMix64: tiny, fast, and identical on every platform. We do NOT use
// std::uniform_*_distribution because its output differs between standard
// libraries, and "same seed -> same person" must hold everywhere.
// The ORDER of draws below is part of the format: change it and every
// freshly generated identity changes (stored ones are unaffected, they keep their values).

namespace {

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }   // [0,1)
    size_t below(size_t n) { return n ? static_cast<size_t>(next() % n) : 0; }
};

template <class T, class W>
const T& pickWeighted(Rng& r, const std::vector<T>& v, W weightOf) {
    int total = 0;
    for (const auto& x : v) total += weightOf(x);
    int roll = static_cast<int>(r.below(static_cast<size_t>(total)));
    for (const auto& x : v) {
        roll -= weightOf(x);
        if (roll < 0) return x;
    }
    return v.back();
}

// ------------------------------------------------------------------- tables --
// Real-world hardware, grouped into whole machines so the pieces always belong
// together (an Apple M1 never gets 16 cores, a GTX 1650 never sits in a 1366x768 tablet).
// These tables are the part to keep fresh and to verify against real traces.
// The mobile tables are coarse; treat them as a starting point.

struct Screen  { int w, h; double dpr; int reserved; int weight; };
struct Machine {
    int weight;
    const char* gpuVendor; const char* gpuName; const char* gpuId;
    const char* arch; const char* model;
    std::vector<int>    cores;
    std::vector<Screen> screens;
};

const std::vector<Machine>& windowsMachines() {
    static const std::vector<Machine> m = {
        {25, "Intel",  "Intel(R) UHD Graphics 630",   "0x00003E92", "x86", "", {4, 6, 8, 12},
            {{1920, 1080, 1.0, 40, 5}, {1366, 768, 1.0, 40, 3}, {1536, 864, 1.25, 40, 3}}},
        {20, "Intel",  "Intel(R) Iris(R) Xe Graphics", "0x000046A6", "x86", "", {12, 16},
            {{1536, 864, 1.25, 40, 5}, {1920, 1080, 1.0, 40, 3}, {1280, 720, 1.5, 40, 2}}},
        {20, "NVIDIA", "NVIDIA GeForce GTX 1650",      "0x00001F82", "x86", "", {8, 12},
            {{1920, 1080, 1.0, 40, 5}, {1536, 864, 1.25, 40, 3}}},
        {20, "NVIDIA", "NVIDIA GeForce RTX 3060",      "0x00002504", "x86", "", {12, 16, 20},
            {{1920, 1080, 1.0, 40, 5}, {2560, 1440, 1.0, 40, 3}}},
        {15, "AMD",    "AMD Radeon(TM) Graphics",      "0x00001638", "x86", "", {8, 12, 16},
            {{1536, 864, 1.25, 40, 4}, {1920, 1080, 1.0, 40, 4}}},
    };
    return m;
}

const std::vector<Machine>& macMachines() {
    static const std::vector<Machine> m = {
        {20, "Apple", "Apple M1",     "", "arm", "", {8},  {{1440, 900,  2.0, 25, 1}}},
        {25, "Apple", "Apple M2",     "", "arm", "", {8},  {{1470, 956,  2.0, 37, 3}, {1440, 900, 2.0, 25, 1}}},
        {20, "Apple", "Apple M3",     "", "arm", "", {8},  {{1512, 982,  2.0, 37, 3}, {1470, 956, 2.0, 37, 2}}},
        {15, "Apple", "Apple M1 Pro", "", "arm", "", {10}, {{1512, 982,  2.0, 37, 3}, {1728, 1117, 2.0, 37, 2}}},
        {15, "Apple", "Apple M2 Pro", "", "arm", "", {12}, {{1512, 982,  2.0, 37, 3}, {1728, 1117, 2.0, 37, 2}}},
    };
    return m;
}

const std::vector<Machine>& androidMachines() {
    static const std::vector<Machine> m = {
        {40, "ARM",      "Mali-G710",       "", "", "Pixel 7",  {8}, {{412, 915, 2.625, 0, 1}}},
        {30, "ARM",      "Mali-G715-Immortalis MC10", "", "", "Pixel 8", {8}, {{412, 915, 2.625, 0, 1}}},
        {30, "Qualcomm", "Adreno (TM) 740", "", "", "SM-S911U", {8}, {{360, 780, 3.0, 0, 1}}},
    };
    return m;
}

const std::vector<Machine>& iosMachines() {
    static const std::vector<Machine> m = {
        {40, "Apple", "Apple GPU", "", "arm", "", {6}, {{390, 844, 3.0, 0, 1}}},
        {35, "Apple", "Apple GPU", "", "arm", "", {6}, {{393, 852, 3.0, 0, 1}}},
        {25, "Apple", "Apple GPU", "", "arm", "", {6}, {{430, 932, 3.0, 0, 1}}},
    };
    return m;
}

const std::vector<Machine>& machinesFor(OsClass os) {
    switch (os) {
        case OsClass::MacOS:   return macMachines();
        case OsClass::Android: return androidMachines();
        case OsClass::IOS:     return iosMachines();
        default:               return windowsMachines();
    }
}

int memoryFor(OsClass os, Rng& r) {   // deviceMemory: Chrome never reports more than 8
    switch (os) {
        case OsClass::Windows: return r.below(100) < 15 ? 4 : 8;
        case OsClass::MacOS:   return 8;
        case OsClass::Android: return r.below(100) < 35 ? 4 : 8;
        default:               return 4;
    }
}

std::string uuidFrom(Rng& r) {
    uint64_t a = r.next(), b = r.next();
    a = (a & 0xFFFFFFFFFFFF0FFFull) | 0x4000ull;                  // version 4
    unsigned long long tl = a >> 32, tm = (a >> 16) & 0xFFFF, th = a & 0xFFFF;
    unsigned long long cs = ((b >> 48) & 0x3FFF) | 0x8000, node = b & 0xFFFFFFFFFFFFull;   // variant 10xx
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08llx-%04llx-%04llx-%04llx-%012llx", tl, tm, th, cs, node);
    return buf;
}

int majorOf(const Profile& p) { return std::atoi(p.version.c_str()); }

std::string majorMinor(const std::string& v) {   // "26.0.1" -> "26.0", "17" -> "17.0"
    size_t d1 = v.find('.');
    if (d1 == std::string::npos) return v + ".0";
    size_t d2 = v.find('.', d1 + 1);
    return d2 == std::string::npos ? v : v.substr(0, d2);
}

std::string macVersion(const std::string& os) {
    if (os.find("Tahoe")    != std::string::npos) return "26.0.0";
    if (os.find("Sequoia")  != std::string::npos) return "15.0.0";
    if (os.find("Sonoma")   != std::string::npos) return "14.0.0";
    if (os.find("Monterey") != std::string::npos) return "12.0.0";
    if (os.find("Big Sur")  != std::string::npos) return "11.0.0";
    return "15.0.0";
}

std::string androidVersion(const std::string& os) {   // "Android 14" -> "14.0.0"
    size_t at = os.find("Android ");
    if (at == std::string::npos) return "14.0.0";
    int v = std::atoi(os.c_str() + at + 8);
    return std::to_string(v > 0 ? v : 14) + ".0.0";
}

std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);   // 17 digits round-trips a double exactly
    return buf;
}

}  // namespace

// ----------------------------------------------------------------- generate --

uint64_t newSeed() {
    std::random_device rd;
    uint64_t a = (static_cast<uint64_t>(rd()) << 32) ^ rd();
    a ^= static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    return a;
}

std::optional<Identity> generateIdentity(const Profile& persona, std::string_view name, uint64_t seed,
                                         const IdentityOptions& opts, std::string* err) {
    auto fail = [&](std::string m) -> std::optional<Identity> {
        if (err) *err = std::move(m);
        return std::nullopt;
    };

    if (persona.family == Family::Tor)
        return fail("the tor persona has fixed, browser-defined values and is not generated yet");
    const OsClass os = osClassFor(persona);
    if (os == OsClass::Unknown)
        return fail("cannot tell which OS '" + persona.target + "' runs on");

    const Region* region = findRegion(opts.region);
    std::string tz = opts.timezone;
    std::vector<std::string> langs = opts.languages;
    if (!region && (tz.empty() || langs.empty()))
        return fail("unknown region '" + opts.region + "' (try us-east, us-west, uk, de, fr, jp, kr, in, br, au)");
    if (tz.empty())    tz = region->timezone;
    if (langs.empty()) langs = region->languages;

    Rng r(seed);
    const Machine& m = pickWeighted(r, machinesFor(os), [](const Machine& x) { return x.weight; });

    Identity id;
    id.profile       = std::string(name);
    id.os            = os;
    id.seed          = seed;
    id.personaTarget = persona.target;
    id.region        = (region && opts.timezone.empty() && opts.languages.empty()) ? region->id : "custom";
    id.createdAt     = opts.createdAt;

    Device& d = id.device;
    d.cpuCores = m.cores[r.below(m.cores.size())];
    const Screen& sc = pickWeighted(r, m.screens, [](const Screen& x) { return x.weight; });
    d.screenW = sc.w;  d.screenH = sc.h;  d.pixelRatio = sc.dpr;  d.reservedBottom = sc.reserved;
    d.memoryGb  = memoryFor(os, r);
    d.gpuVendor = m.gpuVendor;  d.gpuName = m.gpuName;  d.gpuId = m.gpuId;
    d.arch = m.arch;            d.model = m.model;
    d.timezone = tz;            d.languages = langs;
    d.canvasSeed = r.uniform();
    d.audioSeed  = r.uniform();
    d.webglSeed  = r.uniform();
    d.fontSeed   = r.uniform();
    id.id = uuidFrom(r);
    return id;
}

// ------------------------------------------------------------ client hints ----

std::vector<Surface::Brand> clientHintBrands(Family f, int major) {
    if (f != Family::Chrome && f != Family::Edge) return {};
    // Chromium builds the "Not?A_Brand" decoy and the brand order from the major
    // version alone. Reproduces the real headers of Chrome 107..136 (see tests).
    static const char* chars[]   = {" ", "(", ":", "-", ".", "/", ")", ";", "=", "?", "_"};
    static const char* greaseV[] = {"8", "99", "24"};
    static const int   order[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};

    const std::string grease = std::string("Not") + chars[major % 11] + "A" + chars[(major + 1) % 11] + "Brand";
    std::vector<Surface::Brand> out(3);
    const int* o = order[major % 6];
    out[o[0]] = {grease, greaseV[major % 3]};
    out[o[1]] = {"Chromium", std::to_string(major)};
    out[o[2]] = {f == Family::Edge ? "Microsoft Edge" : "Google Chrome", std::to_string(major)};
    return out;
}

std::string formatSecChUa(const std::vector<Surface::Brand>& brands) {
    std::string o;
    for (size_t i = 0; i < brands.size(); ++i) {
        if (i) o += ", ";
        o += "\"" + brands[i].brand + "\";v=\"" + brands[i].version + "\"";
    }
    return o;
}

std::string acceptLanguageFor(Family f, const std::vector<std::string>& langs) {
    if (langs.empty()) return "en-US,en;q=0.9";
    const bool gecko = f == Family::Firefox || f == Family::Tor;
    const size_t n = langs.size();
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        if (i) out += ",";
        out += langs[i];
        if (i == 0) continue;
        // Chromium/Safari step down by 0.1; Firefox spreads the list over 1.0..0
        double q = gecko ? 1.0 - static_cast<double>(i) / static_cast<double>(n) : 1.0 - 0.1 * static_cast<double>(i);
        if (q < 0.1) q = 0.1;
        char buf[16];
        std::snprintf(buf, sizeof buf, ";q=%.1f", q);
        out += buf;
    }
    return out;
}

// ------------------------------------------------------------------ surface ---

Surface surfaceFor(const Profile& p, const Identity& id, const IdentityOptions& opts) {
    Surface s;
    const Device& d = id.device;
    const OsClass os = osClassFor(p);
    const int major = majorOf(p);
    const std::string majS = std::to_string(major);
    const bool firefox  = p.family == Family::Firefox || p.family == Family::Tor;
    const bool safari   = p.family == Family::Safari;
    const bool chromium = !firefox && !safari;   // Chrome, Edge, and unknown names

    s.mobile         = os == OsClass::Android || os == OsClass::IOS;
    s.maxTouchPoints = s.mobile ? 5 : 0;
    s.languages      = d.languages;
    s.acceptLanguage = acceptLanguageFor(p.family, d.languages);

    switch (os) {
        case OsClass::MacOS:   s.platform = "MacIntel";     break;
        case OsClass::Android: s.platform = "Linux armv81"; break;
        case OsClass::IOS:     s.platform = "iPhone";       break;
        default:               s.platform = "Win32";        break;
    }

    // ---- User-Agent (all of these are the "reduced" UA formats modern browsers send)
    if (firefox) {
        s.userAgent = os == OsClass::MacOS
            ? "Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:" + majS + ".0) Gecko/20100101 Firefox/" + majS + ".0"
            : "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:" + majS + ".0) Gecko/20100101 Firefox/" + majS + ".0";
    } else if (safari) {
        const std::string v = majorMinor(p.version);
        if (os == OsClass::IOS) {
            std::string u = v;
            std::replace(u.begin(), u.end(), '.', '_');
            s.userAgent = "Mozilla/5.0 (iPhone; CPU iPhone OS " + u + " like Mac OS X) AppleWebKit/605.1.15 "
                          "(KHTML, like Gecko) Version/" + v + " Mobile/15E148 Safari/604.1";
        } else {
            s.userAgent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
                          "(KHTML, like Gecko) Version/" + v + " Safari/605.1.15";
        }
    } else {
        std::string tok = "Windows NT 10.0; Win64; x64";
        if (os == OsClass::MacOS)   tok = "Macintosh; Intel Mac OS X 10_15_7";
        if (os == OsClass::Android) tok = "Linux; Android 10; K";     // Chrome freezes both fields
        s.userAgent = "Mozilla/5.0 (" + tok + ") AppleWebKit/537.36 (KHTML, like Gecko) Chrome/" + majS + ".0.0.0 " +
                      (s.mobile ? "Mobile " : "") + "Safari/537.36" +
                      (p.family == Family::Edge ? " Edg/" + majS + ".0.0.0" : "");
    }

    s.vendor      = chromium ? "Google Inc." : safari ? "Apple Computer, Inc." : "";
    s.productSub  = firefox ? "20100101" : "20030107";

    // ---- WebGL strings: same hardware, shown differently by each browser
    if (chromium) {
        s.webglVendor = "Google Inc. (" + d.gpuVendor + ")";
        if (os == OsClass::MacOS)
            s.webglRenderer = "ANGLE (Apple, ANGLE Metal Renderer: " + d.gpuName + ", Unspecified Version)";
        else if (os == OsClass::Android)
            s.webglRenderer = "ANGLE (" + d.gpuVendor + ", " + d.gpuName + ", OpenGL ES 3.2)";
        else
            s.webglRenderer = "ANGLE (" + d.gpuVendor + ", " + d.gpuName + " (" + d.gpuId + ") Direct3D11 vs_5_0 ps_5_0, D3D11)";
    } else if (safari) {
        s.webglVendor = "Apple Inc.";
        s.webglRenderer = "Apple GPU";
    } else {                                   // Firefox: unverified, refine against real traces
        s.webglVendor = d.gpuVendor;
        s.webglRenderer = d.gpuName;
    }

    // ---- Which JS surface exists at all
    s.hasWindowChrome = s.hasUserAgentData = s.hasDeviceMemory = s.hasBattery = chromium;

    // ---- Chromium client hints
    if (chromium) {
        s.brands          = clientHintBrands(p.family, major);
        s.secChUa         = formatSecChUa(s.brands);
        s.secChUaMobile   = s.mobile ? "?1" : "?0";
        s.secChUaPlatform = os == OsClass::MacOS ? "macOS" : os == OsClass::Android ? "Android" : "Windows";

        const std::string full = opts.chromeFullVersion.empty() ? majS + ".0.0.0" : opts.chromeFullVersion;
        for (const auto& b : s.brands) {
            const bool decoy = b.brand.rfind("Not", 0) == 0;
            s.fullVersionList.push_back({b.brand, decoy ? b.version + ".0.0.0" : full});
        }
        switch (os) {
            case OsClass::MacOS:   s.uaPlatformVersion = macVersion(p.os);     s.uaArchitecture = d.arch; s.uaBitness = "64"; break;
            case OsClass::Android: s.uaPlatformVersion = androidVersion(p.os); s.uaArchitecture = "";     s.uaBitness = "";   s.uaModel = d.model; break;
            default:               s.uaPlatformVersion = "10.0.0";             s.uaArchitecture = "x86";  s.uaBitness = "64"; break;
        }
    }
    return s;
}

// -------------------------------------------------------------- consistency ---

std::vector<std::string> checkConsistency(const Profile& p, const Identity& id) {
    std::vector<std::string> out;
    const Device& d = id.device;
    const OsClass want = osClassFor(p);
    const Surface s = surfaceFor(p, id);
    auto has = [](const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; };

    if (id.os != want)
        out.push_back(std::string("device is ") + osClassName(id.os) + " but persona " + p.target + " runs on " + osClassName(want));
    if (d.cpuCores < 1 || d.cpuCores > 64) out.push_back("cpu core count " + std::to_string(d.cpuCores) + " is not believable");
    if (d.pixelRatio <= 0)                 out.push_back("pixel ratio must be positive");
    if (d.timezone.empty())                out.push_back("timezone is empty");
    if (d.languages.empty())               out.push_back("languages are empty");

    if (s.hasDeviceMemory && d.memoryGb != 1 && d.memoryGb != 2 && d.memoryGb != 4 && d.memoryGb != 8)
        out.push_back("deviceMemory " + std::to_string(d.memoryGb) + " does not exist in Chrome (values stop at 8)");

    if (s.mobile && d.screenW > 600)   out.push_back("a phone persona with a " + std::to_string(d.screenW) + "px wide screen");
    if (!s.mobile && d.screenW < 1024) out.push_back("a desktop persona with a " + std::to_string(d.screenW) + "px wide screen");

    if (has(s.webglRenderer, "Mesa"))  out.push_back("renderer mentions Mesa, a Linux driver, on " + std::string(osClassName(want)));
    if (p.family == Family::Chrome || p.family == Family::Edge) {
        if (s.webglRenderer.rfind("ANGLE (", 0) != 0) out.push_back("Chromium WebGL renderer should start with 'ANGLE ('");
        if (!has(s.webglRenderer, d.gpuName.c_str())) out.push_back("renderer does not contain the GPU name");
        if (!has(s.webglVendor, d.gpuVendor.c_str())) out.push_back("WebGL vendor does not match the GPU vendor");
        if (!has(s.userAgent, ("Chrome/" + std::to_string(majorOf(p)) + ".").c_str())) out.push_back("User-Agent does not carry the persona's Chrome version");
        if (!has(s.secChUa, ("v=\"" + std::to_string(majorOf(p)) + "\"").c_str()))   out.push_back("sec-ch-ua does not carry the persona's major version");
    } else if (!s.secChUa.empty() || s.hasUserAgentData) {
        out.push_back("Safari and Firefox send no client hints");
    }

    const bool uaWin = has(s.userAgent, "Windows NT"), uaMac = has(s.userAgent, "Macintosh"),
               uaAnd = has(s.userAgent, "Android"),    uaIos = has(s.userAgent, "iPhone");
    if (uaWin != (want == OsClass::Windows) || uaMac != (want == OsClass::MacOS) ||
        uaAnd != (want == OsClass::Android) || uaIos != (want == OsClass::IOS))
        out.push_back("User-Agent OS does not match the persona OS");
    return out;
}

// --------------------------------------------------------------------- json ---

std::string toJson(const Identity& id) {
    using detail::jsonQuote;
    const Device& d = id.device;
    std::string langs = "[";
    for (size_t i = 0; i < d.languages.size(); ++i) langs += (i ? "," : "") + jsonQuote(d.languages[i]);
    langs += "]";

    std::string o = "{\n";
    auto line = [&](const char* k, const std::string& v, bool last = false) {
        o += "  \"" + std::string(k) + "\": " + v + (last ? "\n" : ",\n");
    };
    line("schema",          std::to_string(id.schema));
    line("identity_id",     jsonQuote(id.id));
    line("profile",         jsonQuote(id.profile));
    line("os",              jsonQuote(osClassName(id.os)));
    line("seed",            jsonQuote(std::to_string(id.seed)));   // string: JSON numbers lose 64-bit precision
    line("persona_target",  jsonQuote(id.personaTarget));
    line("region",          jsonQuote(id.region));
    line("created_at",      jsonQuote(id.createdAt));
    line("cpu_cores",       std::to_string(d.cpuCores));
    line("memory_gb",       std::to_string(d.memoryGb));
    line("screen_w",        std::to_string(d.screenW));
    line("screen_h",        std::to_string(d.screenH));
    line("pixel_ratio",     num(d.pixelRatio));
    line("reserved_bottom", std::to_string(d.reservedBottom));
    line("timezone",        jsonQuote(d.timezone));
    line("languages",       langs);
    line("gpu_vendor",      jsonQuote(d.gpuVendor));
    line("gpu_name",        jsonQuote(d.gpuName));
    line("gpu_id",          jsonQuote(d.gpuId));
    line("arch",            jsonQuote(d.arch));
    line("model",           jsonQuote(d.model));
    line("canvas_seed",     num(d.canvasSeed));
    line("audio_seed",      num(d.audioSeed));
    line("webgl_seed",      num(d.webglSeed));
    line("font_seed",       num(d.fontSeed), true);
    return o + "}\n";
}

std::optional<Identity> identityFromJson(std::string_view json, std::string* err) {
    detail::FlatJson fj;
    std::string perr;
    auto fail = [&](const std::string& m) -> std::optional<Identity> {
        if (err) *err = m;
        return std::nullopt;
    };
    if (!detail::parseFlatJson(json, fj, &perr)) return fail("not valid identity JSON: " + perr);

    std::string missing;
    auto str = [&](const char* k, bool required = true) -> std::string {
        auto it = fj.scalars.find(k);
        if (it == fj.scalars.end()) { if (required && missing.empty()) missing = k; return {}; }
        return it->second;
    };
    auto integer = [&](const char* k) -> int {
        auto it = fj.scalars.find(k);
        if (it == fj.scalars.end()) { if (missing.empty()) missing = k; return 0; }
        return std::atoi(it->second.c_str());
    };
    auto real = [&](const char* k) -> double {
        auto it = fj.scalars.find(k);
        if (it == fj.scalars.end()) { if (missing.empty()) missing = k; return 0; }
        return std::strtod(it->second.c_str(), nullptr);
    };

    Identity id;
    Device& d = id.device;
    id.schema        = integer("schema");
    id.id            = str("identity_id");
    id.profile       = str("profile");
    id.os            = parseOsClass(str("os"));
    id.seed          = std::strtoull(str("seed").c_str(), nullptr, 10);
    id.personaTarget = str("persona_target", false);
    id.region        = str("region", false);
    id.createdAt     = str("created_at", false);
    d.cpuCores       = integer("cpu_cores");
    d.memoryGb       = integer("memory_gb");
    d.screenW        = integer("screen_w");
    d.screenH        = integer("screen_h");
    d.pixelRatio     = real("pixel_ratio");
    d.reservedBottom = integer("reserved_bottom");
    d.timezone       = str("timezone");
    d.gpuVendor      = str("gpu_vendor");
    d.gpuName        = str("gpu_name");
    d.gpuId          = str("gpu_id", false);
    d.arch           = str("arch", false);
    d.model          = str("model", false);
    d.canvasSeed     = real("canvas_seed");
    d.audioSeed      = real("audio_seed");
    d.webglSeed      = real("webgl_seed");
    d.fontSeed       = real("font_seed");
    auto langs = fj.arrays.find("languages");
    if (langs == fj.arrays.end()) { if (missing.empty()) missing = "languages"; }
    else d.languages = langs->second;

    if (!missing.empty())                  return fail("missing field '" + missing + "'");
    if (id.schema != 1)                    return fail("unsupported schema " + std::to_string(id.schema));
    if (id.os == OsClass::Unknown)         return fail("unknown os class");
    if (d.cpuCores < 1 || d.screenW < 1 || d.screenH < 1 || d.languages.empty())
        return fail("device values are out of range");
    return id;
}

}  // namespace lucyn::profile
