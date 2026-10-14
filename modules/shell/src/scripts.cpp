#include <lucyn/shell/scripts.h>
#include <lucyn/core/paths.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace lucyn::shell {

const char* runAtName(RunAt r) { return r == RunAt::Start ? "start" : "end"; }

bool globMatch(std::string_view p, std::string_view t) {
    // Iterative with one backtrack point: linear-ish, no recursion, no blow-up on "*a*a*a*b".
    size_t pi = 0, ti = 0, star = std::string_view::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == t[ti])) { ++pi; ++ti; }
        else if (pi < p.size() && p[pi] == '*') { star = pi++; mark = ti; }
        else if (star != std::string_view::npos) { pi = star + 1; ti = ++mark; }
        else return false;
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool parseInt(const std::string& s, int* out) {
    if (s.empty() || s.size() > 9) return false;
    size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i == s.size()) return false;
    for (size_t k = i; k < s.size(); ++k) if (!std::isdigit((unsigned char)s[k])) return false;
    *out = std::atoi(s.c_str());
    return true;
}

// Reads the optional header. Returns false (with *err) if the header is present but wrong.
bool parseHeader(const std::string& src, UserScript& s, std::string* err) {
    std::istringstream in(src);
    std::string line;
    bool first = true, inBlock = false;
    while (std::getline(in, line)) {
        if (first) {                                   // a UTF-8 BOM must not hide the header
            first = false;
            if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
                line.erase(0, 3);
        }
        std::string t = trim(line);
        if (!inBlock) {
            if (t.empty()) continue;
            if (t.rfind("//", 0) != 0) return true;    // real code started: no header
            if (trim(t.substr(2)) == "==LucyScript==") inBlock = true;
            continue;
        }
        if (t.rfind("//", 0) != 0) { *err = "header is missing // ==/LucyScript=="; return false; }
        std::string body = trim(t.substr(2));
        if (body == "==/LucyScript==") return true;
        if (body.empty() || body[0] != '@') continue;
        size_t sp = body.find_first_of(" \t");
        std::string key = body.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
        std::string val = sp == std::string::npos ? "" : trim(body.substr(sp));
        if (key == "name") {
            if (val.empty()) { *err = "@name needs a value"; return false; }
            s.name = val;
        } else if (key == "run-at") {
            if (val == "start") s.runAt = RunAt::Start;
            else if (val == "end") s.runAt = RunAt::End;
            else { *err = "@run-at must be 'start' or 'end', got '" + val + "'"; return false; }
        } else if (key == "match") {
            if (val.empty()) { *err = "@match needs a pattern"; return false; }
            s.match.push_back(val);
        } else if (key == "priority") {
            if (!parseInt(val, &s.priority)) { *err = "@priority must be a whole number, got '" + val + "'"; return false; }
        }                                              // unknown keys are ignored (room to grow)
    }
    if (inBlock) { *err = "header is missing // ==/LucyScript=="; return false; }
    return true;
}
}  // namespace

std::optional<UserScript> ScriptRegistry::add(std::string source, const std::string& fallbackName, std::string* err,
                                              const std::string& path) {
    auto bad = [&](const std::string& m) -> std::optional<UserScript> {
        if (err) *err = (fallbackName.empty() ? std::string("script") : fallbackName) + ": " + m;
        return std::nullopt;
    };
    UserScript s;
    s.name = fallbackName;
    std::string herr;
    if (!parseHeader(source, s, &herr)) return bad(herr);
    if (s.name.empty()) return bad("script has no name");
    if (s.name.size() > 128) return bad("name is longer than 128 characters");
    for (char c : s.name) if ((unsigned char)c < 0x20) return bad("name contains a control character");
    for (auto& e : scripts_) if (e.name == s.name) return bad("a script named '" + s.name + "' already exists");
    s.source = std::move(source);
    s.path = path;
    s.order = nextOrder_++;
    scripts_.push_back(s);
    return s;
}

size_t ScriptRegistry::loadDir(const std::string& dir, std::vector<std::string>* errors) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ".js") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    size_t added = 0;
    for (auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in) { if (errors) errors->push_back(f.filename().string() + ": cannot read"); continue; }
        std::string src((std::istreambuf_iterator<char>(in)), {});
        std::string err;
        if (add(std::move(src), f.stem().string(), &err, f.string())) ++added;
        else if (errors) errors->push_back(err);
    }
    return added;
}

bool ScriptRegistry::remove(const std::string& name) {
    auto it = std::remove_if(scripts_.begin(), scripts_.end(), [&](auto& s) { return s.name == name; });
    bool any = it != scripts_.end();
    scripts_.erase(it, scripts_.end());
    return any;
}

std::vector<UserScript> ScriptRegistry::forUrl(const std::string& url, RunAt at) const {
    std::vector<UserScript> out;
    for (auto& s : scripts_) {
        if (s.runAt != at) continue;
        bool ok = s.match.empty();
        for (auto& m : s.match) if (globMatch(m, url)) { ok = true; break; }
        if (ok) out.push_back(s);
    }
    std::stable_sort(out.begin(), out.end(), [](const UserScript& a, const UserScript& b) {
        return a.priority != b.priority ? a.priority < b.priority : a.order < b.order;
    });
    return out;
}

std::string ScriptRegistry::defaultDir() { return core::dataDir("scripts"); }

}  // namespace lucyn::shell
