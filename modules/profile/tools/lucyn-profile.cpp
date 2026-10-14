// lucyn-profile: poke the profile module from a terminal. No network, no libcurl.
//
//   lucyn-profile list [--family chrome] [--json]
//   lucyn-profile info <target|alias> [--json]
//   lucyn-profile resolve <alias>
//   lucyn-profile switch [--strict] <name> [<name> ...]
//   lucyn-profile identity <target|alias> [--seed N] [--region R] [--timezone TZ] [--json]
//   lucyn-profile identity <target|alias> [--dir D] [--name N] [--regenerate] [--region R]
//       (--name/--regenerate without --dir store in <this program>/nothing-data/identities)
//   lucyn-profile --version
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <lucyn/profile/identity.h>

using namespace lucyn::profile;

static void printRow(const Profile& p) {
    std::printf("%-18s %-8s v%-7s %-15s%s%s%s\n", p.target.c_str(), familyName(p.family),
                p.version.c_str(), p.os.c_str(), p.mobile ? " mobile" : "",
                p.h3 ? " h3" : "", p.alt ? " alt" : "");
}

static int usage() {
    std::puts("usage:\n"
              "  lucyn-profile list [--family chrome|edge|safari|firefox|tor] [--json]\n"
              "  lucyn-profile info <target|alias> [--json]\n"
              "  lucyn-profile resolve <alias>\n"
              "  lucyn-profile switch [--strict] <name> [<name> ...]\n"
              "  lucyn-profile identity <target|alias> [--seed N] [--region R] [--timezone TZ] [--json]\n"
              "  lucyn-profile identity <target|alias> [--dir D] [--name N] [--regenerate]   (no --dir: nothing-data/identities)\n"
              "  lucyn-profile --version\n"
              "regions: us-east us-west uk de fr jp kr in br au");
    return 1;
}

static int identityCommand(const Profile& persona, const std::string& dir, const std::string& name,
                           bool haveSeed, uint64_t seed, bool regenerate, const std::string& region,
                           const std::string& timezone, bool json) {
    IdentityOptions o;
    if (!region.empty())   o.region = region;
    if (!timezone.empty()) o.timezone = timezone;

    std::string err, source;
    std::optional<Identity> id;
    if (!dir.empty()) {
        IdentityStore store(dir);
        bool created = false;
        id = regenerate ? store.regenerate(name, persona, o, &err) : store.loadOrCreate(name, persona, o, &err, &created);
        if (id) source = (regenerate ? "regenerated: " : created ? "created: " : "loaded from disk: ") +
                         store.pathFor(name, id->os);
    } else {
        uint64_t s = haveSeed ? seed : newSeed();
        id = generateIdentity(persona, name, s, o, &err);
        source = "generated, not saved (seed " + std::to_string(s) + ")";
    }
    if (!id) { std::printf("cannot build an identity: %s\n", err.c_str()); return 3; }
    if (json) { std::fputs(toJson(*id).c_str(), stdout); return 0; }

    const Surface s = surfaceFor(persona, *id);
    const Device& d = id->device;
    std::string langs;
    for (size_t i = 0; i < d.languages.size(); ++i) langs += (i ? "," : "") + d.languages[i];

    std::printf("persona      %s  (%s %s, %s)\n", persona.target.c_str(), familyName(persona.family),
                persona.version.c_str(), persona.os.c_str());
    std::printf("identity     %s\n             %s\n", id->id.c_str(), source.c_str());
    std::printf("device       %s | %d cores | %d GB | %dx%d @%gx | %s\n", osClassName(id->os), d.cpuCores,
                d.memoryGb, d.screenW, d.screenH, d.pixelRatio, d.gpuName.c_str());
    std::printf("region       %s | %s | %s\n", id->region.c_str(), d.timezone.c_str(), langs.c_str());
    std::printf("user-agent   %s\n", s.userAgent.c_str());
    std::printf("platform     %s   vendor \"%s\"\n", s.platform.c_str(), s.vendor.c_str());
    std::printf("sec-ch-ua    %s\n", s.secChUa.empty() ? "(none: Safari and Firefox send no client hints)" : s.secChUa.c_str());
    std::printf("webgl        %s / %s\n", s.webglVendor.c_str(), s.webglRenderer.c_str());
    std::printf("accept-lang  %s\n", s.acceptLanguage.c_str());
    std::printf("seeds        canvas %.4f  audio %.4f  webgl %.4f  font %.4f\n", d.canvasSeed, d.audioSeed, d.webglSeed, d.fontSeed);

    auto issues = checkConsistency(persona, *id);
    if (issues.empty()) { std::puts("check        consistent"); return 0; }
    std::printf("check        %zu problem(s):\n", issues.size());
    for (const auto& i : issues) std::printf("             - %s\n", i.c_str());
    return 4;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const Registry& reg = Registry::builtin();
    std::string cmd = argv[1];

    bool json = false, strict = false, regenerate = false, haveSeed = false, nameGiven = false;
    uint64_t seed = 0;
    std::string family, region, timezone, dir, name = "default";
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) {
        const bool more = i + 1 < argc;
        if      (!std::strcmp(argv[i], "--json"))       json = true;
        else if (!std::strcmp(argv[i], "--strict"))     strict = true;
        else if (!std::strcmp(argv[i], "--regenerate")) regenerate = true;
        else if (!std::strcmp(argv[i], "--family")   && more) family = argv[++i];
        else if (!std::strcmp(argv[i], "--region")   && more) region = argv[++i];
        else if (!std::strcmp(argv[i], "--timezone") && more) timezone = argv[++i];
        else if (!std::strcmp(argv[i], "--dir")      && more) dir = argv[++i];
        else if (!std::strcmp(argv[i], "--name")     && more) { name = argv[++i]; nameGiven = true; }
        else if (!std::strcmp(argv[i], "--seed")     && more) { seed = std::strtoull(argv[++i], nullptr, 10); haveSeed = true; }
        else args.push_back(argv[i]);
    }

    if (cmd == "--version") { std::printf("lucyn-profile module %s\n", version()); return 0; }

    if (cmd == "list") {
        std::vector<Profile> rows;
        for (const auto& p : reg.all())
            if (family.empty() || p.family == parseFamily(family)) rows.push_back(p);
        if (json) std::puts(toJson(rows).c_str());
        else for (const auto& p : rows) printRow(p);
        return rows.empty() ? 2 : 0;
    }

    if (cmd == "info" && args.size() == 1) {
        std::string target = reg.resolve(args[0]);
        Profile p = reg.describe(target);
        bool known = reg.find(target) != nullptr;
        if (json) { std::puts(toJson(p).c_str()); return 0; }
        printRow(p);
        if (!known) std::puts("(not in the built-in table: details above are guessed from the name)");
        return 0;
    }

    if (cmd == "resolve" && args.size() == 1) {
        std::puts(reg.resolve(args[0]).c_str());
        return 0;
    }

    if (cmd == "identity" && args.size() == 1)
        // Saving without --dir goes to nothing-data/identities (next to this program), never anywhere else.
        if (dir.empty() && (nameGiven || regenerate)) dir = IdentityStore::defaultDir();
        return identityCommand(reg.describe(reg.resolve(args[0])), dir, name, haveSeed, seed, regenerate, region, timezone, json);

    if (cmd == "switch" && !args.empty()) {
        Switcher sw("chrome");
        sw.setStrict(strict);
        sw.subscribe([](const Profile& from, const Profile& to) {
            std::printf("  event: %s -> %s\n", from.target.c_str(), to.target.c_str());
        });
        std::printf("start: %s\n", sw.current().target.c_str());
        int rc = 0;
        for (const auto& n : args) {
            std::string err;
            std::printf("select(\"%s\")\n", n.c_str());
            if (!sw.select(n, &err)) { std::printf("  refused: %s\n", err.c_str()); rc = 3; }
        }
        std::printf("end:   %s\n", sw.current().target.c_str());
        return rc;
    }

    return usage();
}
