#include <lucyn/profile/profile.h>
#include "detail.h"
#include <cctype>
#include <cstdio>

namespace lucyn::profile {

const char* version() { return "0.2.0"; }

// ------------------------------------------------------------- helpers ----

static bool startsWith(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(0, p.size()) == p;
}
static bool endsWith(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

const char* familyName(Family f) {
    switch (f) {
        case Family::Chrome:  return "chrome";
        case Family::Edge:    return "edge";
        case Family::Safari:  return "safari";
        case Family::Firefox: return "firefox";
        case Family::Tor:     return "tor";
        default:              return "unknown";
    }
}

Family parseFamily(std::string_view n) {
    if (n == "chrome")  return Family::Chrome;
    if (n == "edge")    return Family::Edge;
    if (n == "safari")  return Family::Safari;
    if (n == "firefox") return Family::Firefox;
    if (n == "tor")     return Family::Tor;
    return Family::Unknown;
}

static Family inferFamily(std::string_view t) {
    for (Family f : {Family::Chrome, Family::Edge, Family::Safari, Family::Firefox, Family::Tor})
        if (startsWith(t, familyName(f))) return f;
    return Family::Unknown;
}

// ------------------------------------------------------------ registry ----
// Keep each family in release order (oldest -> newest): latest() relies on it.
// Columns: target, family, version, os, mobile, h3, alt

Registry::Registry() {
    using F = Family;
    profiles_ = {
        // ---- Chrome (desktop) ----
        {"chrome99",   F::Chrome, "99",  "Windows 10",    false, false, false},
        {"chrome100",  F::Chrome, "100", "Windows 10",    false, false, false},
        {"chrome101",  F::Chrome, "101", "Windows 10",    false, false, false},
        {"chrome104",  F::Chrome, "104", "Windows 10",    false, false, false},
        {"chrome107",  F::Chrome, "107", "Windows 10",    false, false, false},
        {"chrome110",  F::Chrome, "110", "Windows 10",    false, false, false},
        {"chrome116",  F::Chrome, "116", "Windows 10",    false, false, false},
        {"chrome119",  F::Chrome, "119", "macOS Sonoma",  false, false, false},
        {"chrome120",  F::Chrome, "120", "macOS Sonoma",  false, false, false},
        {"chrome123",  F::Chrome, "123", "macOS Sonoma",  false, false, false},
        {"chrome124",  F::Chrome, "124", "macOS Sonoma",  false, false, false},
        {"chrome131",  F::Chrome, "131", "macOS Sonoma",  false, false, false},
        {"chrome133a", F::Chrome, "133", "macOS Sequoia", false, false, true },
        {"chrome136",  F::Chrome, "136", "macOS Sequoia", false, false, false},
        {"chrome142",  F::Chrome, "142", "macOS Tahoe",   false, false, false},
        {"chrome145",  F::Chrome, "145", "macOS Tahoe",   false, true,  false},
        {"chrome146",  F::Chrome, "146", "macOS Tahoe",   false, true,  false},
        // ---- Chrome (Android) ----
        {"chrome99_android",  F::Chrome, "99",  "Android 12", true, false, false},
        {"chrome131_android", F::Chrome, "131", "Android 14", true, false, false},
        // ---- Edge ----
        {"edge99",  F::Edge, "99",  "Windows 10", false, false, false},
        {"edge101", F::Edge, "101", "Windows 10", false, false, false},
        // ---- Safari (desktop) ----
        {"safari153",  F::Safari, "15.3",   "macOS Big Sur",  false, false, false},
        {"safari155",  F::Safari, "15.5",   "macOS Monterey", false, false, false},
        {"safari170",  F::Safari, "17.0",   "macOS Sonoma",   false, false, false},
        {"safari180",  F::Safari, "18.0",   "macOS Sequoia",  false, false, false},
        {"safari184",  F::Safari, "18.4",   "macOS Sequoia",  false, false, false},
        {"safari260",  F::Safari, "26.0",   "macOS Tahoe",    false, false, false},
        {"safari2601", F::Safari, "26.0.1", "macOS Tahoe",    false, false, false},
        // ---- Safari (iOS) ----
        {"safari172_ios", F::Safari, "17.2", "iOS 17.2", true, false, false},
        {"safari180_ios", F::Safari, "18.0", "iOS 18.0", true, false, false},
        {"safari184_ios", F::Safari, "18.4", "iOS 18.4", true, false, false},
        {"safari260_ios", F::Safari, "26.0", "iOS 26.0", true, false, false},
        // ---- Firefox ----
        {"firefox133", F::Firefox, "133", "macOS Sonoma", false, false, false},
        {"firefox135", F::Firefox, "135", "macOS Sonoma", false, false, false},
        {"firefox144", F::Firefox, "144", "macOS Tahoe",  false, false, false},
        {"firefox147", F::Firefox, "147", "macOS Tahoe",  false, true,  false},
        // ---- Tor Browser ----
        {"tor145", F::Tor, "14.5", "macOS Sonoma", false, false, false},
    };
}

const Registry& Registry::builtin() {
    static const Registry r;
    return r;
}

std::vector<const Profile*> Registry::byFamily(Family f) const {
    std::vector<const Profile*> out;
    for (const auto& p : profiles_)
        if (p.family == f) out.push_back(&p);
    return out;
}

const Profile* Registry::find(std::string_view target) const {
    for (const auto& p : profiles_)
        if (p.target == target) return &p;
    return nullptr;
}

const Profile* Registry::latest(Family f) const {
    const Profile* best = nullptr;
    for (const auto& p : profiles_)
        if (p.family == f && !p.mobile && !p.alt) best = &p;   // last wins
    return best;
}

std::string Registry::resolve(std::string_view s) const {
    Family f = parseFamily(s);
    if (f != Family::Unknown)
        if (const Profile* p = latest(f)) return p->target;
    return std::string(s);   // concrete (or newer-than-the-table) target
}

Profile Registry::describe(std::string_view target) const {
    if (const Profile* p = find(target)) return *p;

    Profile p;
    p.target = std::string(target);
    p.family = inferFamily(target);
    p.mobile = endsWith(target, "_android") || endsWith(target, "_ios");
    p.alt    = target.size() >= 2 && target.back() == 'a' &&
               std::isdigit(static_cast<unsigned char>(target[target.size() - 2]));
    p.os     = "unknown";

    std::string_view fam = familyName(p.family);   // version = digits right after the family prefix
    if (startsWith(target, fam)) {
        std::string_view rest = target.substr(fam.size());
        size_t n = 0;
        while (n < rest.size() && std::isdigit(static_cast<unsigned char>(rest[n]))) ++n;
        p.version = std::string(rest.substr(0, n));
    }
    return p;
}

// ------------------------------------------------------------ switcher ----

Switcher::Switcher(std::string_view initial) {
    const auto& reg = Registry::builtin();
    current_ = reg.describe(reg.resolve(initial));
}

Profile Switcher::current() const {
    std::lock_guard<std::mutex> lock(m_);
    return current_;
}

void Switcher::setStrict(bool strict) {
    std::lock_guard<std::mutex> lock(m_);
    strict_ = strict;
}

Switcher::Id Switcher::addValidator(Validator v) {
    std::lock_guard<std::mutex> lock(m_);
    Id id = nextId_++;
    validators_.emplace_back(id, std::move(v));
    return id;
}

void Switcher::removeValidator(Id id) {
    std::lock_guard<std::mutex> lock(m_);
    for (auto it = validators_.begin(); it != validators_.end(); ++it)
        if (it->first == id) { validators_.erase(it); return; }
}

Switcher::Id Switcher::subscribe(Listener l) {
    std::lock_guard<std::mutex> lock(m_);
    Id id = nextId_++;
    listeners_.emplace_back(id, std::move(l));
    return id;
}

void Switcher::unsubscribe(Id id) {
    std::lock_guard<std::mutex> lock(m_);
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it)
        if (it->first == id) { listeners_.erase(it); return; }
}

bool Switcher::select(std::string_view nameOrAlias, std::string* err) {
    const auto& reg = Registry::builtin();
    Profile next = reg.describe(reg.resolve(nameOrAlias));

    auto fail = [&](const std::string& why) { if (err) *err = why; return false; };

    if (next.target.empty()) return fail("empty profile name");

    // Snapshot under the lock, then run user callbacks WITHOUT it (so a
    // validator or listener may safely call current() or even select()).
    bool strict;
    std::vector<Validator> validators;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (next.target == current_.target) return true;   // already active: no-op
        strict = strict_;
        for (const auto& v : validators_) validators.push_back(v.second);
    }

    if (strict && !reg.find(next.target))
        return fail("'" + next.target + "' is not in the built-in profile table (strict mode)");

    for (const auto& v : validators) {
        std::string why;
        if (!v(next, &why))
            return fail(why.empty() ? "'" + next.target + "' was rejected by a validator" : why);
    }

    Profile prev;
    std::vector<Listener> listeners;
    {
        std::lock_guard<std::mutex> lock(m_);
        prev = current_;
        current_ = next;
        for (const auto& l : listeners_) listeners.push_back(l.second);
    }
    for (const auto& l : listeners) l(prev, next);
    return true;
}

// ---------------------------------------------------------------- json ----

std::string toJson(const Profile& p) {
    auto b = [](bool v) { return v ? "true" : "false"; };
    return "{\"target\":"  + detail::jsonQuote(p.target) +
           ",\"family\":"  + detail::jsonQuote(familyName(p.family)) +
           ",\"version\":" + detail::jsonQuote(p.version) +
           ",\"os\":"      + detail::jsonQuote(p.os) +
           ",\"mobile\":"  + b(p.mobile) +
           ",\"h3\":"      + b(p.h3) +
           ",\"alt\":"     + b(p.alt) + "}";
}

std::string toJson(const std::vector<Profile>& ps) {
    std::string o = "[";
    for (size_t i = 0; i < ps.size(); ++i) {
        if (i) o += ",";
        o += toJson(ps[i]);
    }
    return o + "]";
}

}  // namespace lucyn::profile
