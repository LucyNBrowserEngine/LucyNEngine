// lucyn/shell/scripts.h - the user's injected scripts (20 on one page is fine).
//
// A script is a .js file. An optional header says when and where it runs:
//
//     // ==LucyScript==
//     // @name      hide-banner
//     // @run-at    start            start = before the site's own scripts, end = after the page settled
//     // @match     https://*.example.com/*      (repeatable, default: every page)
//     // @priority  -5               (lower runs first, default 0)
//     // ==/LucyScript==
//
// No header = name from the file name, run-at start, match everything.
// Order on a page: all `start` scripts (priority, then the order they were added), the page,
// then all `end` scripts the same way.
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lucyn::shell {

enum class RunAt { Start, End };
const char* runAtName(RunAt r);

struct UserScript {
    std::string name;
    std::string source;
    RunAt       runAt = RunAt::Start;
    std::vector<std::string> match;     // globs; empty = "*"
    int         priority = 0;
    size_t      order = 0;              // registration order, set by the registry
    std::string path;                   // where it came from ("" if added from a string)
};

// '*' = any run of characters (also '/'), '?' = one character. Case-sensitive. Everything else literal.
bool globMatch(std::string_view pattern, std::string_view text);

class ScriptRegistry {
public:
    // Parses the header and adds the script. On a bad header (unknown @run-at, bad
    // @priority, duplicate name) nothing is added: returns nullopt and fills *err.
    std::optional<UserScript> add(std::string source, const std::string& fallbackName, std::string* err = nullptr,
                                  const std::string& path = "");
    // Adds every *.js in `dir`, sorted by file name. Problems go to *errors, the rest still load.
    // A missing folder is not an error (0 scripts). Returns how many were added.
    size_t loadDir(const std::string& dir, std::vector<std::string>* errors = nullptr);
    bool   remove(const std::string& name);
    void   clear() { scripts_.clear(); nextOrder_ = 0; }
    size_t size() const { return scripts_.size(); }
    const std::vector<UserScript>& list() const { return scripts_; }

    // Scripts that apply to `url` at this moment, in run order.
    std::vector<UserScript> forUrl(const std::string& url, RunAt at) const;

    static std::string defaultDir();    // <running binary>/nothing-data/scripts

private:
    std::vector<UserScript> scripts_;
    size_t nextOrder_ = 0;
};

}  // namespace lucyn::shell
