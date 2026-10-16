#include <lucyn/shell/hydrator.h>
#include <lucyn/shell/assets.h>
#include <lucyn/shell/snapshot.h>
#include <lucyn/shell/url.h>
#include <cstring>
#include <exception>
#include <unordered_set>

namespace lucyn::shell {

namespace {
// Copy attributes straight from a parsed node (no name validation: the parser already accepted them).
void copyAttrs(const dom::Node& from, dom::Node& to) {
    for (auto& a : from.attrs) {
        bool found = false;
        for (auto& b : to.attrs) if (b.first == a.first) { b.second = a.second; found = true; break; }
        if (!found) to.attrs.push_back(a);
    }
}

std::string joinArgs(const std::vector<std::string>& args) {
    std::string s;
    for (size_t i = 0; i < args.size(); ++i) { if (i) s += ' '; s += args[i]; }
    return s;
}

// With scripting enabled a browser reads <noscript> content as raw text and never renders it. gumbo has no scripting flag, so it
// parses that content as live markup instead (and a <noscript> in <head> that holds text even spills it into <body>). That is how
// "JavaScript is required to ..." ended up painted on pages whose scripts DID run. This removes the content of every <noscript>
// before parsing, keeping an empty <noscript></noscript>. Comments and script/style/textarea/title bodies are skipped over, so
// a "<noscript>" inside a JS string is never touched. A <noscript> with no closing tag is left alone.
std::string stripNoscript(const std::string& html, bool* changed) {
    auto lc = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; };
    auto at = [&](size_t pos, const char* lit) {
        const size_t n = std::strlen(lit);
        if (pos + n > html.size()) return false;
        for (size_t i = 0; i < n; ++i) if (lc(html[pos + i]) != lit[i]) return false;
        return true;
    };
    auto find = [&](size_t from, const char* lit) -> size_t {
        const size_t n = std::strlen(lit);
        for (size_t i = from; i + n <= html.size(); ++i) if (at(i, lit)) return i;
        return std::string::npos;
    };
    auto tagEnds = [&](size_t pos, size_t nameLen) {          // the char after "<name" must end the name
        const size_t k = pos + nameLen;
        if (k >= html.size()) return true;
        const char c = html[k];
        return c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
    };
    static const char* const kRaw[] = {"script", "style", "textarea", "title"};

    std::string out;
    out.reserve(html.size());
    size_t i = 0;
    while (i < html.size()) {
        const size_t lt = html.find('<', i);
        if (lt == std::string::npos) { out.append(html, i, std::string::npos); break; }
        out.append(html, i, lt - i);
        i = lt;
        if (at(i, "<!--")) {
            size_t e = html.find("-->", i + 4);
            e = e == std::string::npos ? html.size() : e + 3;
            out.append(html, i, e - i);
            i = e;
            continue;
        }
        bool handled = false;
        for (const char* raw : kRaw) {
            const size_t nl = std::strlen(raw);
            if (at(i + 1, raw) && tagEnds(i + 1, nl)) {
                const std::string closer = std::string("</") + raw;
                size_t c = find(i + 1 + nl, closer.c_str());
                size_t e = html.size();
                if (c != std::string::npos) { size_t gt = html.find('>', c); e = gt == std::string::npos ? html.size() : gt + 1; }
                out.append(html, i, e - i);
                i = e;
                handled = true;
                break;
            }
        }
        if (handled) continue;
        if (at(i + 1, "noscript") && tagEnds(i + 1, 8)) {
            const size_t c = find(i + 9, "</noscript");
            if (c != std::string::npos) {
                const size_t gt = html.find('>', c);
                if (gt != std::string::npos) {
                    out += "<noscript></noscript>";
                    i = gt + 1;
                    if (changed) *changed = true;
                    continue;
                }
            }
        }
        out += '<';
        ++i;
    }
    return out;
}

const char* kStandardHooks[] = {"page.url", "log", "snapshot.take", "snapshot.get", "snapshot.labels", "snapshot.diff"};
}  // namespace

bool mountSite(dom::Document& shell, const dom::Document& site, std::string* err) {
    dom::Node* html = shell.documentElement();
    dom::Node* head = shell.head();
    dom::Node* body = shell.body();
    if (!html || !head || !body) { if (err) *err = "shell page has no <html>/<head>/<body>"; return false; }

    if (const dom::Node* sHtml = site.documentElement()) copyAttrs(*sHtml, *html);

    if (const dom::Node* sHead = site.head()) {
        // One <title>, and it is the site's.
        if (sHead->querySelector("title"))
            for (dom::Node* t : head->querySelectorAll("title")) t->remove();
        for (auto& c : sHead->children) head->appendChild(shell.importNode(*c, true));
    }
    if (const dom::Node* sBody = site.body()) {
        copyAttrs(*sBody, *body);
        for (auto& c : sBody->children) body->appendChild(shell.importNode(*c, true));
    }
    return true;
}

Hydrator::Hydrator(Fetcher fetch, ScriptRegistry& scripts, ScriptHost& host)
    : fetch_(std::move(fetch)), scripts_(scripts), host_(host) {}

HydrateResult Hydrator::hydrate(const std::string& url) {
    HydrateResult res;
    res.url = url;
    bool attached = false;
    // Declared BEFORE the Cleanup guard on purpose: locals are destroyed in reverse order, so the guard runs host_.detach() while
    // the Document is still alive. (Declared inside the try, the Document died first on every error path.)
    std::unique_ptr<dom::Document> shell;
    struct Cleanup {
        Hydrator* h; bool* attached;
        ~Cleanup() {
            for (auto* n : kStandardHooks) h->native_.remove(n);   // they point into this run's stack
            if (*attached && !h->options.keepAlive) h->host_.detach();
        }
    } cleanup{this, &attached};

    try {
        if (url.empty()) { res.error = "no URL given"; return res; }
        if (!fetch_) { res.error = "no fetcher configured"; return res; }

        // 1. fetch -----------------------------------------------------------------------------
        if (hooks.onFetchStart) hooks.onFetchStart(url);
        FetchResult fr = fetch_(url);
        if (hooks.onFetched) hooks.onFetched(fr);
        if (!fr.ok) { res.error = fr.error.empty() ? "fetch failed" : fr.error; return res; }
        res.status = fr.status;
        res.url = fr.finalUrl.empty() ? url : fr.finalUrl;
        if (fr.status >= 400) res.warnings.push_back("HTTP " + std::to_string(fr.status));
        if (fr.body.empty()) res.warnings.push_back("empty response body");

        // A page that is about to get a JS engine is parsed the way a scripting browser parses it (no <noscript> content).
        bool noscriptStripped = false;
        std::unique_ptr<dom::Document> site = host_.available()
            ? dom::parseHtml(stripNoscript(fr.body, &noscriptStripped))
            : dom::parseHtml(fr.body);
        res.beforeHtml = fr.body;

        Snapshots snaps(options.saveSnapshots, res.url);
        std::string serr;
        if (!snaps.take("before", fr.body, &serr)) res.warnings.push_back("snapshot: " + serr);

        // 2. shell page ------------------------------------------------------------------------
        std::string shellHtml = haveShell_ ? shellHtml_ : loadShellHtml();
        shell = dom::parseHtml(shellHtml);
        if (!shell->documentElement() || !shell->head() || !shell->body()) {   // parser makes these; belt and braces
            res.error = "shell page is unusable";
            return res;
        }
        // Remember the shell's own nodes by pointer (never by an attribute the page could see).
        std::unordered_set<const dom::Node*> shellOwned;
        for (const dom::Node* part : {static_cast<const dom::Node*>(shell->head()), static_cast<const dom::Node*>(shell->body())})
            for (auto& c : part->children) {
                std::vector<const dom::Node*> st{c.get()};
                while (!st.empty()) {
                    const dom::Node* n = st.back(); st.pop_back();
                    shellOwned.insert(n);
                    for (auto& k : n->children) st.push_back(k.get());
                }
            }
        auto liveHtml = [&]() {
            return dom::serialize(shell->root(), [&](const dom::Node& n) { return shellOwned.count(&n) != 0; });
        };

        // 3. standard hooks --------------------------------------------------------------------
        const std::string pageUrl = res.url;
        for (auto* n : kStandardHooks) native_.remove(n);
        native_.add("page.url", [pageUrl](const std::vector<std::string>&) { return HookResult{true, pageUrl}; });
        native_.add("log", [&res](const std::vector<std::string>& a) {
            if (res.log.size() < 1000) { std::string l = joinArgs(a); if (l.size() > 4096) l.resize(4096); res.log.push_back(l); }
            return HookResult{true, ""};
        });
        native_.add("snapshot.take", [&](const std::vector<std::string>& a) {
            if (a.empty()) return HookResult{false, "snapshot.take needs a label"};
            if (a[0] == "before" || a[0] == "after") return HookResult{false, "'before' and 'after' are reserved"};
            std::string e;
            if (!snaps.take(a[0], liveHtml(), &e)) return HookResult{false, e};
            return HookResult{true, "ok"};
        });
        native_.add("snapshot.get", [&](const std::vector<std::string>& a) {
            if (a.empty()) return HookResult{false, "snapshot.get needs a label"};
            const std::string* h = snaps.get(a[0]);
            if (!h) return HookResult{false, "no snapshot named '" + a[0] + "'"};
            return HookResult{true, *h};
        });
        native_.add("snapshot.labels", [&](const std::vector<std::string>&) {
            std::string s;
            for (auto& l : snaps.labels()) { if (!s.empty()) s += '\n'; s += l; }
            return HookResult{true, s};
        });
        native_.add("snapshot.diff", [&](const std::vector<std::string>& a) {
            if (a.size() < 2) return HookResult{false, "snapshot.diff needs two labels"};
            auto d = snaps.diff(a[0], a[1]);
            if (!d) return HookResult{false, "unknown snapshot label"};
            return HookResult{true, dom::formatDiff(*d)};
        });

        // 4. host ---------------------------------------------------------------------------
        std::string attachErr;
        if (host_.available()) {
            attached = host_.attach(*shell, res.url, native_, &attachErr);
            if (!attached) res.warnings.push_back("JS host could not attach: " + attachErr);
        }
        res.jsAvailable = attached;
        // The engine was there on paper but could not attach: the page has no scripting, so its <noscript> content is live.
        if (!attached && noscriptStripped) site = dom::parseHtml(fr.body);

        auto runScripts = [&](RunAt at) {
            for (auto& s : scripts_.forUrl(res.url, at)) {
                ScriptReport r;
                r.name = s.name; r.runAt = at;
                if (!attached) {
                    r.status = ScriptReport::Status::Skipped;
                    r.detail = host_.available() ? "JS host could not attach" : "no JS engine";
                } else {
                    ScriptOutcome o = host_.runUserScript(s);
                    r.status = o.ok ? ScriptReport::Status::Ran : ScriptReport::Status::Failed;
                    r.detail = o.error;
                }
                res.scripts.push_back(std::move(r));      // a failing script never stops the others
            }
        };

        // 5. start scripts -> mount -> page scripts -> idle -> end scripts ---------------------
        if (hooks.onStartScripts) hooks.onStartScripts(*shell);
        runScripts(RunAt::Start);

        std::string merr;
        if (!mountSite(*shell, *site, &merr)) { res.error = merr; return res; }
        if (hooks.onMounted) hooks.onMounted(*shell);

        if (attached) {
            ScriptOutcome po = host_.runPageScripts();
            if (!po.ok) res.warnings.push_back("page scripts: " + po.error);
            res.settled = host_.settle(options.settleTimeoutMs);
            if (!res.settled) res.warnings.push_back("page did not go idle within " + std::to_string(options.settleTimeoutMs) + " ms");
        }
        if (hooks.onSettled) hooks.onSettled(*shell);

        if (hooks.onEndScripts) hooks.onEndScripts(*shell);
        runScripts(RunAt::End);

        // 6. after + diff -----------------------------------------------------------------------
        res.afterHtml = liveHtml();
        if (!snaps.take("after", res.afterHtml, &serr)) res.warnings.push_back("snapshot: " + serr);
        res.diff = dom::diffHtml(res.beforeHtml, res.afterHtml);
        if (!snaps.saveNote("diff.txt", dom::formatDiff(res.diff), &serr)) res.warnings.push_back("snapshot: " + serr);
        res.snapshotDir = snaps.runDir();

        res.title = shell->title();
        res.faviconUrl = faviconUrl(*shell, res.url);
        res.document = std::move(shell);
        res.ok = true;
        if (hooks.onDone) hooks.onDone(res);
        return res;
    } catch (const std::exception& e) {
        res.ok = false;
        res.error = std::string("internal error: ") + e.what();
        return res;
    } catch (...) {
        res.ok = false;
        res.error = "internal error";
        return res;
    }
}

}  // namespace lucyn::shell
