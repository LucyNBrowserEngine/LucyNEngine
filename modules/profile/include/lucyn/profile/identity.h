// lucyn/profile/identity.h - generates and stores believable device identities.
//
// Two layers (this is the important idea):
//
//   PERSONA  = which browser LucyN claims to be. It comes from a Profile
//              ("safari184"). Everything the browser itself reports is DERIVED
//              from it on demand (see Surface) and is never stored. So the TLS
//              handshake and the JavaScript can never disagree.
//
//   DEVICE   = the machine the "person" sits at: cores, screen, GPU, timezone,
//              language, noise seeds. It is generated once, stored, and reused,
//              so the same person comes back tomorrow with the same machine.
//
// A device is stored per (profile name, OS class). The profile "default" can be
// a Windows machine for chrome146 and a Mac for safari184, each one stable, and
// Chrome-on-Mac and Safari-on-Mac share the same Mac (same person, same laptop).
#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <lucyn/profile/profile.h>

namespace lucyn::profile {

// ------------------------------------------------------------------ basics ----

enum class OsClass { Windows, MacOS, Android, IOS, Unknown };

const char* osClassName(OsClass o);                 // "windows" | "macos" | "android" | "ios"
OsClass     parseOsClass(std::string_view name);
OsClass     osClassFor(const Profile& persona);     // from the persona row, else a family default

// A believable place to be. Pick the one that matches your proxy's exit IP:
// timezone and language that disagree with the IP are a classic giveaway.
struct Region {
    std::string id;                      // "us-east", "de", "jp", ...
    std::string timezone;                // IANA name
    std::vector<std::string> languages;  // navigator.languages, best first
};
const std::vector<Region>& regions();
const Region* findRegion(std::string_view id);

struct IdentityOptions {
    std::string region = "us-east";
    std::string timezone;                // overrides the region's timezone
    std::vector<std::string> languages;  // overrides the region's languages
    std::string chromeFullVersion;       // e.g. "146.0.7680.80"; default "<major>.0.0.0"
    std::string createdAt;               // filled in by the store
};

// ------------------------------------------------------------------ device ----

struct Device {
    int    cpuCores = 0;                 // navigator.hardwareConcurrency
    int    memoryGb = 0;                 // navigator.deviceMemory (Chrome caps it at 8)
    int    screenW = 0, screenH = 0;     // CSS pixels, like screen.width / screen.height
    double pixelRatio = 1.0;
    int    reservedBottom = 0;           // taskbar / menu bar: screen.availHeight = screenH - this
    std::string timezone;
    std::vector<std::string> languages;
    // Hardware names. The WebGL strings a page sees are derived per browser (Surface).
    std::string gpuVendor;               // "Intel", "NVIDIA", "AMD", "Apple", "Qualcomm", "ARM"
    std::string gpuName;                 // "Intel(R) UHD Graphics 630", "Apple M2", ...
    std::string gpuId;                   // PCI device id for Windows/ANGLE, else empty
    std::string arch;                    // "x86" | "arm"
    std::string model;                   // phone model (Android), else empty
    double canvasSeed = 0, audioSeed = 0, webglSeed = 0, fontSeed = 0;   // each in [0,1)
};

struct Identity {
    int         schema = 1;
    std::string id;                      // uuid, stable for the life of the file
    std::string profile;                 // profile name, e.g. "default"
    OsClass     os = OsClass::Unknown;
    uint64_t    seed = 0;                // what generated it (informational: values are stored in full)
    std::string personaTarget;           // persona it was FIRST generated for (informational)
    std::string region;
    std::string createdAt;
    Device      device;
};

// ----------------------------------------------------------------- surface ----
// What the browser actually reports to a page and to the server, derived from
// persona + device. Never stored.

struct Surface {
    struct Brand { std::string brand, version; };

    std::string userAgent, platform, vendor, productSub;
    std::string acceptLanguage;                   // header value
    std::vector<std::string> languages;
    int         maxTouchPoints = 0;
    bool        mobile = false;
    // WebGL UNMASKED_VENDOR_WEBGL / UNMASKED_RENDERER_WEBGL
    std::string webglVendor, webglRenderer;
    // Chromium client hints (empty / false for Safari and Firefox)
    std::string secChUa, secChUaMobile, secChUaPlatform;   // platform is the bare name, e.g. Windows
    std::vector<Brand> brands;                    // navigator.userAgentData.brands
    std::vector<Brand> fullVersionList;           // getHighEntropyValues().fullVersionList
    std::string uaPlatformVersion, uaArchitecture, uaBitness, uaModel;
    // Which JS surface exists at all. A Safari persona has no window.chrome, no
    // userAgentData, no getBattery; Firefox has no deviceMemory. Presence is a signal.
    bool hasWindowChrome = false, hasUserAgentData = false, hasDeviceMemory = false, hasBattery = false;
};

// ---------------------------------------------------------------- generate ----

uint64_t newSeed();   // random, for creating a brand-new person

// Same (persona OS class, name, seed, options) -> same identity, on every machine.
// Returns nullopt with *err set if no believable identity can be made (e.g. Tor,
// whose values are fixed by the browser and are not generated yet).
std::optional<Identity> generateIdentity(const Profile& persona, std::string_view profileName,
                                         uint64_t seed, const IdentityOptions& opts = {},
                                         std::string* err = nullptr);

Surface surfaceFor(const Profile& persona, const Identity& id, const IdentityOptions& opts = {});

// Chromium's client-hint brand list (the "Not?A_Brand" decoy and the brand order
// are computed from the major version, exactly like Chrome does).
std::vector<Surface::Brand> clientHintBrands(Family f, int major);
std::string formatSecChUa(const std::vector<Surface::Brand>& brands);
std::string acceptLanguageFor(Family f, const std::vector<std::string>& languages);

// The README rule, as code: "a believable human is one whose details all agree".
// Empty vector = consistent. Otherwise one human-readable problem per entry.
std::vector<std::string> checkConsistency(const Profile& persona, const Identity& id);

std::string toJson(const Identity& id);
std::optional<Identity> identityFromJson(std::string_view json, std::string* err = nullptr);

// ------------------------------------------------------------------- store ----

class IdentityStore {
public:
    enum class Status { Ok, NotFound, Corrupt };

    explicit IdentityStore(std::string dir);
    static std::string defaultDir();        // <running binary>/nothing-data/identities (always)
    const std::string& dir() const { return dir_; }

    static bool validName(std::string_view name);                 // [A-Za-z0-9_-], 1..64
    std::string pathFor(std::string_view name, OsClass os) const; // <dir>/<name>.<os>.identity.json

    Status load(std::string_view name, OsClass os, Identity& out, std::string* err = nullptr) const;
    bool   save(const Identity& id, std::string* err = nullptr) const;   // atomic: temp file + rename

    // Reuse the stored device for (name, persona OS) or create and save one.
    // A corrupt file is reported, NOT silently replaced: replacing it would turn
    // "the same person" into a new one without anyone noticing.
    std::optional<Identity> loadOrCreate(std::string_view name, const Profile& persona,
                                         const IdentityOptions& opts = {},
                                         std::string* err = nullptr, bool* created = nullptr) const;

    // Throw the stored device away and roll a new person.
    std::optional<Identity> regenerate(std::string_view name, const Profile& persona,
                                       const IdentityOptions& opts = {}, std::string* err = nullptr) const;

private:
    std::string dir_;
};

// ----------------------------------------------------------------- manager ----
// The glue an engine uses: follows a Switcher and always holds the identity and
// surface that belong to the CURRENT persona. Refuses switches to personas it
// cannot build an identity for.

class IdentityManager {
public:
    IdentityManager(Switcher& sw, IdentityStore store, std::string profileName = "default",
                    IdentityOptions opts = {});
    ~IdentityManager();
    IdentityManager(const IdentityManager&) = delete;
    IdentityManager& operator=(const IdentityManager&) = delete;

    std::optional<Identity> identity() const;   // empty if the last (re)load failed
    std::optional<Surface>  surface() const;
    std::string             lastError() const;

private:
    void reload(const Profile& persona);

    Switcher&       sw_;
    IdentityStore   store_;
    std::string     name_;
    IdentityOptions opts_;
    mutable std::mutex m_;
    std::optional<Identity> id_;
    std::optional<Surface>  surface_;
    std::string     err_;
    Switcher::Id    validator_ = 0, listener_ = 0;
};

}  // namespace lucyn::profile
