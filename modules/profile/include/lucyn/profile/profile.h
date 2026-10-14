// lucyn/profile/profile.h - public API of the LucyN profile module.
//
// This module answers one question: "who is LucyN pretending to be right now?"
// It has NO dependencies (no libcurl, no SDL, no JS engine), so it builds and
// tests on its own. Other modules plug into it:
//
//   net        subscribes, so it rebuilds its curl handle on every switch
//   ui         can display the active profile
//   controller calls select() when a client says {"cmd":"profile","name":"safari184"}
//   js         reads the same Profile to fake navigator.* consistently
//
// Everyone shares ONE Switcher, so TLS and JS can never disagree.
#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lucyn::profile {

const char* version();   // module version, e.g. "0.1.0"

// ---------------------------------------------------------------- data ----

enum class Family { Chrome, Edge, Safari, Firefox, Tor, Unknown };

const char* familyName(Family f);                 // Family::Safari -> "safari"
Family      parseFamily(std::string_view name);   // "safari" -> Family::Safari, else Unknown

struct Profile {
    std::string target;   // exact name handed to curl_easy_impersonate, e.g. "safari184"
    Family      family = Family::Unknown;
    std::string version;  // "124", "18.4", "26.0.1"
    std::string os;       // "macOS Sonoma", "iOS 18.4", "Android 14"
    bool        mobile = false;
    bool        h3     = false;   // ships HTTP/3 fingerprints
    bool        alt    = false;   // "a" variant: seen via A/B testing, not an official release
};

// ------------------------------------------------------------ registry ----
// Read-only table of every browser curl-impersonate ships.

class Registry {
public:
    static const Registry& builtin();

    const std::vector<Profile>& all() const { return profiles_; }
    std::vector<const Profile*> byFamily(Family f) const;

    // Exact lookup in the built-in table. nullptr if unknown.
    const Profile* find(std::string_view target) const;

    // Newest official, non-mobile profile of a family ("chrome" family -> chrome146).
    const Profile* latest(Family f) const;

    // "chrome" / "edge" / "safari" / "firefox" / "tor" -> concrete target name.
    // Anything else passes through untouched, so targets newer than this table
    // still work (the network layer has the final say).
    std::string resolve(std::string_view nameOrAlias) const;

    // Always returns something: the table row if known, otherwise a best-effort
    // guess built from the name (family from prefix, mobile from suffix).
    Profile describe(std::string_view target) const;

private:
    Registry();
    std::vector<Profile> profiles_;
};

// ------------------------------------------------------------ switcher ----
// Holds the ACTIVE profile and tells everyone when it changes.
// Thread-safe. Validators and listeners run on the thread that called select().

class Switcher {
public:
    using Id        = int;
    // Veto power: return false (and optionally fill *err) to refuse a switch.
    // `err` is never null. Example: net asks libcurl "do you accept this target?".
    using Validator = std::function<bool(const Profile& next, std::string* err)>;
    // Called AFTER a successful switch.
    using Listener  = std::function<void(const Profile& previous, const Profile& current)>;

    explicit Switcher(std::string_view initial = "chrome");
    Switcher(const Switcher&) = delete;
    Switcher& operator=(const Switcher&) = delete;

    Profile current() const;   // by value, so it is safe across threads

    // Switch to a concrete target or alias. On success the listeners fire and
    // true is returned. On failure the old profile stays active and *err
    // (if given) says why. Selecting the already-active target is a no-op.
    bool select(std::string_view nameOrAlias, std::string* err = nullptr);

    // Strict mode: only targets present in the built-in table are accepted.
    // Default is lenient, so new browsers work before the table is updated.
    void setStrict(bool strict);

    Id   addValidator(Validator v);
    void removeValidator(Id id);
    Id   subscribe(Listener l);
    void unsubscribe(Id id);

private:
    mutable std::mutex m_;
    Profile current_;
    bool    strict_  = false;
    Id      nextId_  = 1;
    std::vector<std::pair<Id, Validator>> validators_;
    std::vector<std::pair<Id, Listener>>  listeners_;
};

// ---------------------------------------------------------------- json ----
// For the controller's newline-delimited JSON channel and for tooling.

std::string toJson(const Profile& p);
std::string toJson(const std::vector<Profile>& ps);   // JSON array

}  // namespace lucyn::profile
