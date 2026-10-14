#include <lucyn/profile/identity.h>
#include <lucyn/core/paths.h>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace lucyn::profile {

static std::string nowIso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// ------------------------------------------------------------------- store ----

IdentityStore::IdentityStore(std::string dir) : dir_(std::move(dir)) {}

std::string IdentityStore::defaultDir() {
    // Always <running binary's folder>/nothing-data/identities (see lucyn/core/paths.h).
    return core::dataDir("identities");
}

bool IdentityStore::validName(std::string_view n) {
    if (n.empty() || n.size() > 64) return false;
    for (char c : n) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;     // no dots, no slashes: a name can never walk out of the directory
    }
    return true;
}

std::string IdentityStore::pathFor(std::string_view name, OsClass os) const {
    return (fs::path(dir_) / (std::string(name) + "." + osClassName(os) + ".identity.json")).string();
}

IdentityStore::Status IdentityStore::load(std::string_view name, OsClass os, Identity& out, std::string* err) const {
    auto bad = [&](const std::string& m) { if (err) *err = m; return Status::Corrupt; };
    if (!validName(name)) return bad("invalid profile name '" + std::string(name) + "'");

    const std::string path = pathFor(name, os);
    std::error_code ec;
    if (!fs::exists(path, ec)) return Status::NotFound;

    std::ifstream f(path, std::ios::binary);
    if (!f) return bad(path + ": cannot be read");
    std::stringstream ss;
    ss << f.rdbuf();

    std::string perr;
    auto id = identityFromJson(ss.str(), &perr);
    if (!id) return bad(path + ": " + perr + " (move the file away to start a fresh identity)");
    if (id->profile != name || id->os != os)
        return bad(path + ": the file says it belongs to '" + id->profile + "' / " + osClassName(id->os));
    out = *id;
    return Status::Ok;
}

bool IdentityStore::save(const Identity& id, std::string* err) const {
    auto bad = [&](const std::string& m) { if (err) *err = m; return false; };
    if (!validName(id.profile)) return bad("invalid profile name '" + id.profile + "'");

    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) return bad("cannot create " + dir_ + ": " + ec.message());

    // Write a temp file, then rename over the real one. A crash mid-write can
    // never leave a half-written identity behind (that would silently turn
    // "the same person" into a new one).
    const std::string path = pathFor(id.profile, id.os);
    const std::string tmp  = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return bad("cannot write " + tmp);
        f << toJson(id);
        f.flush();
        if (!f) return bad("write failed for " + tmp);
    }
    fs::rename(tmp, path, ec);
    if (ec) { fs::remove(tmp, ec); return bad("cannot replace " + path); }
    return true;
}

std::optional<Identity> IdentityStore::loadOrCreate(std::string_view name, const Profile& persona,
                                                    const IdentityOptions& opts, std::string* err,
                                                    bool* created) const {
    if (created) *created = false;
    const OsClass os = osClassFor(persona);

    Identity existing;
    switch (load(name, os, existing, err)) {
        case Status::Ok:      return existing;
        case Status::Corrupt: return std::nullopt;       // err already filled in
        case Status::NotFound: break;
    }

    IdentityOptions o = opts;
    o.createdAt = nowIso();
    auto id = generateIdentity(persona, name, newSeed(), o, err);
    if (!id) return std::nullopt;
    if (!save(*id, err)) return std::nullopt;
    if (created) *created = true;
    return id;
}

std::optional<Identity> IdentityStore::regenerate(std::string_view name, const Profile& persona,
                                                  const IdentityOptions& opts, std::string* err) const {
    IdentityOptions o = opts;
    o.createdAt = nowIso();
    auto id = generateIdentity(persona, name, newSeed(), o, err);
    if (!id) return std::nullopt;
    if (!save(*id, err)) return std::nullopt;
    return id;
}

// ----------------------------------------------------------------- manager ----

IdentityManager::IdentityManager(Switcher& sw, IdentityStore store, std::string profileName, IdentityOptions opts)
    : sw_(sw), store_(std::move(store)), name_(std::move(profileName)), opts_(std::move(opts)) {
    reload(sw_.current());

    validator_ = sw_.addValidator([](const Profile& next, std::string* err) {
        if (next.family == Family::Tor) {
            *err = "no identity can be generated for '" + next.target + "' yet";
            return false;
        }
        if (osClassFor(next) == OsClass::Unknown) {
            *err = "cannot tell which OS '" + next.target + "' runs on";
            return false;
        }
        return true;
    });
    listener_ = sw_.subscribe([this](const Profile&, const Profile& now) { reload(now); });
}

IdentityManager::~IdentityManager() {
    sw_.unsubscribe(listener_);
    sw_.removeValidator(validator_);
}

void IdentityManager::reload(const Profile& persona) {
    std::string err;
    auto id = store_.loadOrCreate(name_, persona, opts_, &err);

    std::lock_guard<std::mutex> lock(m_);
    if (!id) {
        id_.reset();
        surface_.reset();
        err_ = err;
        return;
    }
    surface_ = surfaceFor(persona, *id, opts_);
    id_      = std::move(id);
    err_.clear();
}

std::optional<Identity> IdentityManager::identity() const { std::lock_guard<std::mutex> l(m_); return id_; }
std::optional<Surface>  IdentityManager::surface()  const { std::lock_guard<std::mutex> l(m_); return surface_; }
std::string             IdentityManager::lastError() const { std::lock_guard<std::mutex> l(m_); return err_; }

}  // namespace lucyn::profile
