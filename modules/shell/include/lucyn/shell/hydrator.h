// lucyn/shell/hydrator.h - open a site INSIDE the shell.
//
//   shell.html  = <head> inject.js + hydrator.js </head> <body></body>      (the app's own page)
//   hydrate(url) = fetch the site, run `start` scripts, mount the site into the shell,
//                  run the site's scripts, wait for idle, run `end` scripts, snapshot before/after.
//
// "Mounting" merges the site into the shell's own <html>/<head>/<body> rather than into a <div>:
//   * site <head> children (title, favicon <link>, <meta>, styles, scripts) are hoisted into the shell <head>
//   * site <html> and <body> attributes are copied onto the shell's
//   * site <body> children are appended to the shell <body>
// so document.title, document.body, `body.dark` selectors and favicon lookups behave like a real page.
// Shell-owned nodes (the ones the shell.html had) are tracked by pointer, not by attribute,
// so nothing inside the page marks them. They are left out of the "after" snapshot.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <lucyn/dom/diff.h>
#include <lucyn/dom/dom.h>
#include <lucyn/shell/host.h>
#include <lucyn/shell/scripts.h>

namespace lucyn::shell {

struct FetchResult {
    bool        ok = false;
    long        status = 0;
    std::string body;
    std::string finalUrl;     // after redirects; empty = same as requested
    std::string error;
};
using Fetcher = std::function<FetchResult(const std::string& url)>;

struct ScriptReport {
    enum class Status { Ran, Failed, Skipped };
    std::string name;
    RunAt       runAt = RunAt::Start;
    Status      status = Status::Skipped;
    std::string detail;       // error text or skip reason
};

struct HydrateResult;

// Optional callbacks at each stage. Set the ones you care about before hydrate().
struct PhaseHooks {
    std::function<void(const std::string& url)>        onFetchStart;
    std::function<void(const FetchResult&)>            onFetched;
    std::function<void(dom::Document& shell)>          onStartScripts;   // start scripts are about to run (site NOT mounted)
    std::function<void(dom::Document& shell)>          onMounted;        // site is in, its scripts have not run
    std::function<void(dom::Document& shell)>          onSettled;        // page scripts done and idle (or timed out)
    std::function<void(dom::Document& shell)>          onEndScripts;     // end scripts are about to run
    std::function<void(HydrateResult&)>               onDone;
};

struct HydrateOptions {
    bool saveSnapshots = true;    // write before/after/diff under nothing-data/snapshots
    int  settleTimeoutMs = 5000;
    // If true, do NOT call host.detach() at the end of hydrate().
    // Caller is responsible for calling detach() later (e.g. before the host or document is destroyed).
    // This is required for eval() / waitFor() to work after visit(): the JS context must stay alive.
    bool keepAlive = false;
};

struct HydrateResult {
    bool        ok = false;
    std::string error;                     // set when !ok
    std::string url;                       // final URL
    long        status = 0;
    std::string title;
    std::string faviconUrl;
    bool        jsAvailable = false;       // false = scripts could not run (see `scripts`)
    bool        settled = true;
    std::vector<ScriptReport> scripts;
    std::vector<std::string>  warnings;
    std::vector<std::string>  log;         // lines scripts sent through the "log" hook
    std::string beforeHtml;                // exactly what the server sent
    std::string afterHtml;                 // the live page (shell scaffolding left out) once everything ran
    dom::HtmlDiff diff;                    // before -> after
    std::string snapshotDir;               // "" when not saved
    std::unique_ptr<dom::Document> document;   // the live shell holding the site
};

class Hydrator {
public:
    Hydrator(Fetcher fetch, ScriptRegistry& scripts, ScriptHost& host);

    PhaseHooks     hooks;
    HydrateOptions options;

    // Add your own hooks (before hydrate()). The standard ones are installed automatically:
    //   page.url, log, snapshot.take, snapshot.get, snapshot.labels, snapshot.diff
    NativeHooks& native() { return native_; }

    // Default: nothing-data/shell/shell.html if it exists, else the built-in one.
    void setShellHtml(std::string html) { shellHtml_ = std::move(html); haveShell_ = true; }

    // Never throws. Failures come back in the result.
    HydrateResult hydrate(const std::string& url);

private:
    Fetcher         fetch_;
    ScriptRegistry& scripts_;
    ScriptHost&     host_;
    NativeHooks     native_;
    std::string     shellHtml_;
    bool            haveShell_ = false;
};

// Exposed for tests and for tools: merge `site` into `shell` as described above. Returns false
// (and fills *err) if the shell has no <html>/<head>/<body>.
bool mountSite(dom::Document& shell, const dom::Document& site, std::string* err = nullptr);

}  // namespace lucyn::shell
