// lucyn-shell: open a page inside the shell, run (or honestly skip) the user's scripts, save before/after.
//   lucyn-shell <url> [-i target] [--script file.js]... [--no-save] [--no-user-scripts]
//   lucyn-shell --file page.html [...]          (no network: treat a local file as the site)
//   lucyn-shell --init                          (write shell.html, inject.js, hydrator.js, scripts/example.js.disabled)
//   lucyn-shell --list-scripts                  (what is in nothing-data/scripts and when each runs)
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <filesystem>
#include <lucyn/core/paths.h>
#include <lucyn/shell/assets.h>
#include <lucyn/shell/hydrator.h>
#include <lucyn/shell/url.h>
#ifdef LUCYN_SHELL_HAVE_JS
#include <lucyn/js/quickjs_host.h>
#include <lucyn/profile/identity.h>
#include <lucyn/profile/profile.h>
#endif
#ifdef LUCYN_SHELL_HAVE_NET
#include <lucyn/net/http_client.h>
#endif

using namespace lucyn;
namespace fs = std::filesystem;

static void usage() {
    std::puts("usage: lucyn-shell <url> [-i target] [--script file.js]... [--no-save] [--no-user-scripts] [--console]\n"
              "       lucyn-shell --file page.html [...]\n"
              "       lucyn-shell --init | --list-scripts\n"
#ifndef LUCYN_SHELL_HAVE_NET
              "(built without the net module: <url> is unavailable, use --file)\n"
#endif
              "Data lives in nothing-data/ next to this program.");
}

static bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

int main(int argc, char** argv) {
    std::string url, file, target = "chrome";
    std::vector<std::string> extra;
    bool save = true, userScripts = true, doInit = false, doList = false, showConsole = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--init") doInit = true;
        else if (a == "--list-scripts") doList = true;
        else if (a == "--no-save") save = false;
        else if (a == "--no-user-scripts") userScripts = false;
        else if (a == "--console") showConsole = true;
        else if (a == "--file" && i + 1 < argc) file = argv[++i];
        else if (a == "-i" && i + 1 < argc) target = argv[++i];
        else if (a == "--script" && i + 1 < argc) extra.push_back(argv[++i]);
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 1; }
        else url = a;
    }

    if (doInit) {
        std::string err;
        if (!shell::ensureShellFiles(&err)) { std::fprintf(stderr, "init failed: %s\n", err.c_str()); return 2; }
        std::printf("ready: %s\n", core::dataRoot().c_str());
        return 0;
    }

    shell::ScriptRegistry scripts;
    std::vector<std::string> errs;
    if (userScripts) scripts.loadDir(shell::ScriptRegistry::defaultDir(), &errs);
    for (auto& path : extra) {
        std::string src;
        if (!readFile(path, src)) { errs.push_back(path + ": cannot read"); continue; }
        std::string e;
        if (!scripts.add(std::move(src), fs::path(path).stem().string(), &e, path)) errs.push_back(e);
    }
    for (auto& e : errs) std::fprintf(stderr, "script problem: %s\n", e.c_str());

    if (doList) {
        std::printf("%zu script(s) in %s\n", scripts.size(), shell::ScriptRegistry::defaultDir().c_str());
        for (auto& s : scripts.list()) {
            std::string m;
            for (auto& g : s.match) m += (m.empty() ? "" : ", ") + g;
            std::printf("  [%-5s] prio %-3d %-24s match: %s\n", shell::runAtName(s.runAt), s.priority, s.name.c_str(), m.empty() ? "*" : m.c_str());
        }
        return errs.empty() ? 0 : 1;
    }

    shell::Fetcher fetcher;
#if defined(LUCYN_SHELL_HAVE_NET) || defined(LUCYN_SHELL_HAVE_JS)
    profile::Switcher sw(target);
#endif
#ifdef LUCYN_SHELL_HAVE_NET
    net::HttpClient client(sw);
#endif
    if (!file.empty()) {
        std::error_code ec;
        std::string abs = fs::absolute(file, ec).string();
        url = "file://" + abs;
        fetcher = [file, url](const std::string&) {
            shell::FetchResult r;
            r.ok = readFile(file, r.body);
            r.status = r.ok ? 200 : 0;
            r.finalUrl = url;
            if (!r.ok) r.error = "cannot open " + file;
            return r;
        };
    } else if (!url.empty()) {
#ifdef LUCYN_SHELL_HAVE_NET
        fetcher = [&client](const std::string& u) {
            shell::FetchResult r;
            auto resp = client.get(u);
            r.ok = resp.ok();
            r.status = resp.status;
            r.body = std::move(resp.body);
            r.finalUrl = resp.effectiveUrl;
            r.error = resp.error;
            return r;
        };
#else
        std::fputs("built without the net module: use --file\n", stderr);
        return 2;
#endif
    } else { usage(); return 1; }

#ifdef LUCYN_SHELL_HAVE_JS
    // The page believes it is whoever the identity manager says it is, re-read on every attach.
    profile::IdentityManager idm(sw, profile::IdentityStore(profile::IdentityStore::defaultDir()));
    js::HostOptions hopts;
    hopts.persona = [&idm]() -> std::optional<js::Persona> {
        auto id = idm.identity();
        auto su = idm.surface();
        if (!id || !su) return std::nullopt;
        return js::Persona{*su, id->device};
    };
    if (showConsole) hopts.onConsole = [](const std::string& lv, const std::string& line) { std::fprintf(stderr, "console.%s: %s\n", lv.c_str(), line.c_str()); };
#ifdef LUCYN_SHELL_HAVE_NET
    hopts.httpGet = [&client](const std::string& url) -> js::HttpGetResult {
        auto r = client.get(url);
        js::HttpGetResult out;
        out.status      = r.status;
        out.body        = std::move(r.body);
        out.headers     = std::move(r.headers);
        out.effectiveUrl = r.effectiveUrl;
        out.error       = r.error;
        return out;
    };
#endif
    js::QuickJsHost host(hopts);
#else
    shell::NullHost host;     // built without the js module: scripts are matched but never run
#endif
    shell::Hydrator hy(fetcher, scripts, host);
    hy.options.saveSnapshots = save;
    auto r = hy.hydrate(url);
    if (!r.ok) { std::fprintf(stderr, "failed: %s\n", r.error.c_str()); return 2; }

    std::printf("url:       %s  (status %ld)\n", r.url.c_str(), r.status);
    std::printf("title:     %s\n", r.title.c_str());
    std::printf("favicon:   %s\n", r.faviconUrl.empty() ? "(none)" : r.faviconUrl.c_str());
    std::printf("js engine: %s\n", r.jsAvailable ? "yes (QuickJS)" : "no: this build has no js module, so scripts are matched but not run and before == after");
    std::printf("scripts:   %zu matched\n", r.scripts.size());
    for (auto& s : r.scripts) {
        const char* st = s.status == shell::ScriptReport::Status::Ran ? "ran" : s.status == shell::ScriptReport::Status::Failed ? "FAILED" : "skipped";
        std::printf("  [%-5s] %-24s %s%s%s\n", shell::runAtName(s.runAt), s.name.c_str(), st, s.detail.empty() ? "" : ": ", s.detail.c_str());
    }
    for (auto& w : r.warnings) std::printf("warning:   %s\n", w.c_str());
#ifdef LUCYN_SHELL_HAVE_JS
    for (auto& n : host.notes()) std::printf("note:      %s\n", n.c_str());
    for (auto& e : host.pageErrors()) std::printf("page error: %s\n", e.c_str());
#endif
    if (!r.snapshotDir.empty()) std::printf("snapshots: %s\n", r.snapshotDir.c_str());
    std::printf("diff (before -> after):\n%s", dom::formatDiff(r.diff).c_str());
    return 0;
}
