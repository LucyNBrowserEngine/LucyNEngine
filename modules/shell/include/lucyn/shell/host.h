// lucyn/shell/host.h - THE SEAM WHERE THE JS MODULE PLUGS IN (next session).
//
// The shell module knows nothing about QuickJS. It talks to a ScriptHost. Today the only
// host is NullHost ("no JS engine": scripts are matched and reported as skipped). The js module will
// implement ScriptHost with QuickJS and the DOM bridge, then:
//
//   1. attach()          create the context for this page; bind `document`/`window` to `shell`
//   2. runUserScript()   once per matching script, `start` ones before the site is mounted
//   3. runPageScripts()  the site's own <script> elements, in document order
//   4. settle()          pump timers/fetches until idle or timeout
//   5. runUserScript()   the `end` ones
//   6. detach()
//
// NativeHooks are the C++ functions the shell offers to injected scripts (snapshot, log, page url).
// The js module walks names() and exposes each one under a single namespace that ONLY injected
// scripts can see. RULE (lesson from Nothing Browser): never a global the page can find, never
// a property that shows up in `for..in`, never a console line.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <lucyn/dom/dom.h>
#include <lucyn/shell/scripts.h>

namespace lucyn::shell {

struct HookResult {
    bool        ok = true;
    std::string value;      // result on success, message on failure
};
using HookFn = std::function<HookResult(const std::vector<std::string>& args)>;

class NativeHooks {
public:
    // false if the name is taken or empty. Names look like "snapshot.take".
    bool add(const std::string& name, HookFn fn);
    void remove(const std::string& name) { hooks_.erase(name); }
    bool has(const std::string& name) const { return hooks_.count(name) != 0; }
    std::vector<std::string> names() const;                 // sorted
    // Never throws: a hook that throws comes back as {false, what()}. Unknown name: {false, ...}.
    HookResult call(const std::string& name, const std::vector<std::string>& args) const;
private:
    std::map<std::string, HookFn> hooks_;
};

struct ScriptOutcome {
    bool        ok = true;
    std::string error;
};

class ScriptHost {
public:
    virtual ~ScriptHost() = default;
    // False = there is no JS engine behind this host; the Hydrator will not call anything else.
    virtual bool available() const = 0;
    virtual bool attach(dom::Document& shell, const std::string& pageUrl, NativeHooks& hooks, std::string* err) = 0;
    virtual ScriptOutcome runUserScript(const UserScript& script) = 0;
    virtual ScriptOutcome runPageScripts() = 0;
    // True if the page went idle, false if the timeout hit first.
    virtual bool settle(int timeoutMs) = 0;
    virtual void detach() = 0;
};

// "No JS yet." Honest and harmless.
class NullHost : public ScriptHost {
public:
    bool available() const override { return false; }
    bool attach(dom::Document&, const std::string&, NativeHooks&, std::string* err) override {
        if (err) *err = "no JS engine";
        return false;
    }
    ScriptOutcome runUserScript(const UserScript&) override { return {false, "no JS engine"}; }
    ScriptOutcome runPageScripts() override { return {false, "no JS engine"}; }
    bool settle(int) override { return true; }
    void detach() override {}
};

}  // namespace lucyn::shell
