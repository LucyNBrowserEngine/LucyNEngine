#include <lucyn/lnm/parser.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace lucyn::lnm {

const char* toString(SwitchMode m) {
    switch (m) {
        case SwitchMode::Replace: return "replace";
        case SwitchMode::Overlay: return "overlay";
        case SwitchMode::Tooltip: return "tooltip";
    }
    return "?";
}
const char* toString(TooltipPosition p) {
    switch (p) {
        case TooltipPosition::None: return "";
        case TooltipPosition::Top: return "top";
        case TooltipPosition::Bottom: return "bottom";
        case TooltipPosition::Left: return "left";
        case TooltipPosition::Right: return "right";
        case TooltipPosition::Cursor: return "cursor";
    }
    return "";
}

bool ParseResult::ok() const {
    for (const auto& d : diagnostics)
        if (d.severity == Diagnostic::Severity::Error) return false;
    return true;
}

const std::vector<std::string>& knownEvents() {
    static const std::vector<std::string> e = {"onclick", "onhover", "ondoubleclick", "onsubmit",
                                               "onfocus", "onblur", "onrightclick"};
    return e;
}

std::string formatDiagnostic(const Diagnostic& d, const std::string& file) {
    std::ostringstream o;
    if (!file.empty()) o << file << ":";
    o << d.loc.line << ":" << d.loc.col << ": "
      << (d.severity == Diagnostic::Severity::Error ? "error" : "warning") << ": " << d.message;
    return o.str();
}

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lowerStr(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string collapseSpaces(const std::string& s) {
    std::string out;
    bool sp = true;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!sp) { out += ' '; sp = true; }
        } else {
            out += c;
            sp = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        return s.substr(1, s.size() - 2);
    return s;
}

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$'; }
bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$'; }

// Splits on top-level commas (outside quotes, (), [], {}).
std::vector<std::string> splitArgs(const std::string& s) {
    std::vector<std::string> parts;
    std::string cur;
    char quote = 0;
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            cur += c;
            if (c == '\\' && i + 1 < s.size()) cur += s[++i];
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
        if (c == '(' || c == '[' || c == '{') ++depth;
        else if ((c == ')' || c == ']' || c == '}') && depth > 0) --depth;
        if (c == ',' && depth == 0) { parts.push_back(trim(cur)); cur.clear(); continue; }
        cur += c;
    }
    parts.push_back(trim(cur));
    return parts;
}

class Parser {
public:
    explicit Parser(const std::string& src) : s_(src) {
        if (s_.compare(0, 3, "\xEF\xBB\xBF") == 0) pos_ = 3;   // skip a UTF-8 BOM
        lineStarts_.push_back(0);
        for (size_t i = 0; i < s_.size(); ++i)
            if (s_[i] == '\n') lineStarts_.push_back(i + 1);
    }

    ParseResult run() {
        while (true) {
            skipTrivia();
            if (eof()) break;
            if (cur() == '}') {
                err(pos_, "unmatched '}'");
                ++pos_;
                continue;
            }
            if (startsGrab()) parseGrab();
            else parseRule();
        }
        return std::move(res_);
    }

private:
    const std::string& s_;
    size_t pos_ = 0;
    std::vector<size_t> lineStarts_;
    ParseResult res_;

    bool eof() const { return pos_ >= s_.size(); }
    char cur() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }
    char at(size_t p) const { return p < s_.size() ? s_[p] : '\0'; }

    SourceLoc locAt(size_t p) const {
        auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), p);
        size_t idx = static_cast<size_t>(it - lineStarts_.begin()) - 1;
        return SourceLoc{static_cast<int>(idx) + 1, static_cast<int>(p - lineStarts_[idx]) + 1};
    }
    void diag(Diagnostic::Severity sev, size_t p, const std::string& msg) {
        res_.diagnostics.push_back(Diagnostic{sev, locAt(p), msg});
    }
    void err(size_t p, const std::string& m) { diag(Diagnostic::Severity::Error, p, m); }
    void warn(size_t p, const std::string& m) { diag(Diagnostic::Severity::Warning, p, m); }

    void skipInlineWs() { while (!eof() && isSpace(cur())) ++pos_; }
    void skipToEol() { while (!eof() && cur() != '\n') ++pos_; }

    // `#` begins a comment only when followed by whitespace, EOL, EOF or another `#`.
    // (The caller guarantees the `#` sits at a boundary: statement start or after whitespace.)
    bool commentAt(size_t p) const {
        if (at(p) != '#') return false;
        char n = at(p + 1);
        return n == '\0' || n == '\n' || n == '#' || isSpace(n);
    }

    void skipTrivia() {
        while (!eof()) {
            char c = cur();
            if (c == '\n' || isSpace(c) || c == ';') { ++pos_; continue; }
            if (commentAt(pos_)) { skipToEol(); continue; }
            break;
        }
    }

    // Skips a `{ ... }` block starting at the `{` at `p`, honouring nesting; returns pos after the `}`.
    size_t skipBlock(size_t p) {
        int depth = 0;
        char quote = 0;
        for (; p < s_.size(); ++p) {
            char c = s_[p];
            if (quote) { if (c == quote || c == '\n') quote = 0; continue; }
            if (c == '"' || c == '\'') { quote = c; continue; }
            if (c == '{') ++depth;
            else if (c == '}') { if (--depth == 0) return p + 1; }
        }
        return s_.size();
    }

    // ---- grab ----------------------------------------------------------------------------------
    bool startsGrab() const {
        size_t p = pos_;
        if (s_.compare(p, 4, "grab") != 0) return false;
        p += 4;
        if (s_.compare(p, 3, ".js") == 0) p += 3;
        while (isSpace(at(p))) ++p;
        return at(p) == '(';
    }

    void parseGrab() {
        size_t start = pos_;
        Grab g;
        g.loc = locAt(start);
        pos_ += 4;
        if (s_.compare(pos_, 3, ".js") == 0) { g.kind = Grab::Kind::Js; pos_ += 3; }
        skipInlineWs();
        ++pos_;  // '('
        size_t q = pos_;
        while (q < s_.size() && s_[q] != ')' && s_[q] != '\n') ++q;
        if (at(q) != ')') {
            err(start, std::string("missing ')' in ") + (g.kind == Grab::Kind::Js ? "grab.js" : "grab") + "(...)");
            pos_ = q;
            return;
        }
        g.path = stripQuotes(trim(s_.substr(pos_, q - pos_)));
        pos_ = q + 1;
        skipInlineWs();
        if (!eof() && cur() != '\n' && cur() != ';' && !commentAt(pos_)) {
            err(pos_, "unexpected text after grab(...)");
            skipToEol();
        }
        if (g.path.empty()) {
            err(start, "grab() needs a file path");
            return;
        }
        res_.program.grabs.push_back(std::move(g));
    }

    // ---- rules ---------------------------------------------------------------------------------
    void parseRule() {
        size_t start = pos_;
        std::string header;
        size_t p = pos_;
        bool found = false;
        while (p < s_.size()) {
            char c = s_[p];
            if (c == '{') { found = true; break; }
            if (c == '}') break;
            if (c == '\n') {
                std::string t = trim(header);
                if (!t.empty() && t.back() == ',') { header += ' '; ++p; continue; }   // `a,\n b {` is one selector list
                break;
            }
            if (c == '#' && (header.empty() || isSpace(header.back())) && commentAt(p)) {
                while (p < s_.size() && s_[p] != '\n') ++p;
                continue;
            }
            header += c;
            ++p;
        }
        if (!found) {
            err(start, "expected '{' after selector '" + collapseSpaces(header) + "'");
            pos_ = std::max(p, start + 1);
            if (cur() != '}') skipToEol();
            return;
        }
        pos_ = p + 1;   // past '{'

        std::string h = collapseSpaces(header);
        std::string event;
        {   // trailing `:on<name>` (lowercase letters only)
            size_t colon = h.rfind(':');
            if (colon != std::string::npos) {
                std::string tail = h.substr(colon + 1);
                if (tail.size() > 2 && tail.compare(0, 2, "on") == 0 &&
                    std::all_of(tail.begin(), tail.end(), [](char c) { return c >= 'a' && c <= 'z'; })) {
                    event = tail;
                    h = trim(h.substr(0, colon));
                }
            }
        }
        if (h.empty()) {
            err(start, event.empty() ? "empty selector before '{'" : "event rule ':" + event + "' needs a selector");
            pos_ = skipBlock(pos_ - 1);
            return;
        }
        if (!event.empty()) checkEvent(event, start);

        Rule rule;
        rule.selector = h;
        rule.event = event;
        rule.loc = locAt(start);
        std::vector<Rule> nested;
        parseBody(rule, nested);
        if (!rule.style.empty() || !rule.actions.empty() || rule.event.size() || nested.empty())
            res_.program.rules.push_back(std::move(rule));
        for (auto& n : nested) res_.program.rules.push_back(std::move(n));
    }

    void checkEvent(const std::string& event, size_t p) {
        const auto& k = knownEvents();
        if (std::find(k.begin(), k.end(), event) == k.end())
            warn(p, "unknown event ':" + event + "' (accepted; the spec lists onclick, onhover, ondoubleclick, onsubmit, onfocus, onblur, onrightclick)");
    }

    void parseBody(Rule& rule, std::vector<Rule>& nested) {
        while (true) {
            skipTrivia();
            if (eof()) {
                err(offsetOf(rule.loc), "missing '}' for rule '" + rule.selector + "'");
                return;
            }
            if (cur() == '}') { ++pos_; return; }
            if (cur() == ':') { parseNestedEvent(rule, nested); continue; }
            parseStatement(rule);
        }
    }

    size_t offsetOf(const SourceLoc& l) const {
        return lineStarts_[static_cast<size_t>(l.line - 1)] + static_cast<size_t>(l.col - 1);
    }

    void parseNestedEvent(Rule& parent, std::vector<Rule>& nested) {
        size_t start = pos_;
        ++pos_;  // ':'
        std::string name;
        while (!eof() && std::isalnum(static_cast<unsigned char>(cur()))) name += cur(), ++pos_;
        skipInlineWs();
        if (cur() != '{') {
            err(start, "expected '{' after ':" + name + "'");
            skipToEol();
            return;
        }
        size_t brace = pos_;
        ++pos_;
        if (!parent.event.empty()) {
            err(start, "event blocks cannot be nested inside an event rule (':" + parent.event + "')");
            pos_ = skipBlock(brace);
            return;
        }
        if (name.size() < 3 || name.compare(0, 2, "on") != 0) {
            err(start, "':" + name + "' is not an event (events look like :onclick)");
            pos_ = skipBlock(brace);
            return;
        }
        checkEvent(name, start);
        Rule r;
        r.selector = parent.selector;
        r.event = name;
        r.loc = locAt(start);
        std::vector<Rule> none;
        parseBody(r, none);
        nested.push_back(std::move(r));
    }

    void parseStatement(Rule& rule) {
        size_t start = pos_;
        size_t q = pos_;
        int depth = 0;
        for (; q < s_.size(); ++q) {
            char c = s_[q];
            if (c == '(') ++depth;
            else if (c == ')') { if (depth > 0) --depth; }
            else if (depth == 0 && (c == ':' || c == '\n' || c == '}' || c == ';' || c == '{')) break;
        }
        char stop = at(q);
        if (q >= s_.size() || stop != ':') {
            std::string k = trim(s_.substr(start, q - start));
            if (stop == '{') {
                err(start, "nested rules are not supported ('" + k + " { ... }'); a block needs a top-level selector, or ':onevent'");
                pos_ = skipBlock(q);
            } else {
                err(start, "expected ':' after '" + k + "'");
                pos_ = q;
            }
            return;
        }
        std::string key = trim(s_.substr(start, q - start));
        pos_ = q + 1;

        // value: to end of line / ';' / '}' (outside quotes and parentheses), or a trailing comment
        size_t vs = pos_, r = pos_;
        char quote = 0;
        int vd = 0;
        for (; r < s_.size(); ++r) {
            char c = s_[r];
            if (quote) {
                if (c == quote) quote = 0;
                else if (c == '\n') break;   // unterminated string: stop at end of line
                continue;
            }
            if (c == '"' || c == '\'') { quote = c; continue; }
            if (c == '(') { ++vd; continue; }
            if (c == ')') { if (vd > 0) --vd; continue; }
            if (vd == 0 && (c == '\n' || c == ';' || c == '}')) break;
            if (vd == 0 && c == '#' && r > vs && isSpace(s_[r - 1]) && commentAt(r)) break;
        }
        std::string value = trim(s_.substr(vs, r - vs));
        pos_ = r;
        if (key.empty()) { err(start, "missing property name before ':'"); return; }
        if (value.empty()) { err(start, "missing value for '" + key + "'"); return; }

        std::string compactKey;
        for (char c : key) if (!isSpace(c)) compactKey += c;

        if (compactKey == "runjs") { parseRunJs(rule, value, start); return; }
        if (compactKey.compare(0, 7, "switch.") == 0) { parseSwitch(rule, compactKey, value, start); return; }

        std::string name = lowerStr(compactKey);
        for (char c : name)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) {
                err(start, "invalid property name '" + key + "'");
                return;
            }
        rule.style.push_back(StyleDecl{name, value, locAt(start)});
    }

    void parseSwitch(Rule& rule, const std::string& key, const std::string& value, size_t start) {
        std::string base = key, arg;
        bool hasParen = false;
        size_t lp = key.find('(');
        if (lp != std::string::npos) {
            hasParen = true;
            if (key.back() != ')') { err(start, "missing ')' in '" + key + "'"); return; }
            base = key.substr(0, lp);
            arg = lowerStr(key.substr(lp + 1, key.size() - lp - 2));
        }
        SwitchAction a;
        a.loc = locAt(start);
        if (base == "switch.replace") a.mode = SwitchMode::Replace;
        else if (base == "switch.overlay") a.mode = SwitchMode::Overlay;
        else if (base == "switch.tooltip") a.mode = SwitchMode::Tooltip;
        else {
            err(start, "unknown switch mode '" + base + "' (expected switch.replace, switch.overlay or switch.tooltip)");
            return;
        }
        if (a.mode == SwitchMode::Tooltip) {
            if (!hasParen || arg.empty()) { err(start, "switch.tooltip needs a position: switch.tooltip(top|bottom|left|right|cursor)"); return; }
            if (arg == "top") a.position = TooltipPosition::Top;
            else if (arg == "bottom") a.position = TooltipPosition::Bottom;
            else if (arg == "left") a.position = TooltipPosition::Left;
            else if (arg == "right") a.position = TooltipPosition::Right;
            else if (arg == "cursor") a.position = TooltipPosition::Cursor;
            else { err(start, "unknown tooltip position '" + arg + "' (expected top, bottom, left, right or cursor)"); return; }
        } else if (hasParen) {
            err(start, base + " takes no position (only switch.tooltip does)");
            return;
        }
        a.target = stripQuotes(trim(value));
        if (a.target.empty()) { err(start, base + " needs a target file"); return; }
        rule.actions.push_back(std::move(a));
    }

    void parseRunJs(Rule& rule, const std::string& value, size_t start) {
        const char* shape = "runjs expects a call like handleLogin() or setTheme(\"dark\")";
        size_t lp = value.find('(');
        if (lp == std::string::npos || value.back() != ')') { err(start, shape); return; }
        std::string fn = trim(value.substr(0, lp));
        bool good = !fn.empty() && isIdentStart(fn[0]);
        for (char c : fn) if (!(isIdentChar(c) || c == '.')) good = false;
        if (!good) { err(start, shape); return; }
        RunJs r;
        r.function = fn;
        r.source = value;
        r.loc = locAt(start);
        std::string inner = trim(value.substr(lp + 1, value.size() - lp - 2));
        if (!inner.empty()) r.args = splitArgs(inner);
        for (const auto& a : r.args)
            if (a.empty()) { err(start, "empty argument in " + value); return; }
        rule.actions.push_back(std::move(r));
    }
};

}  // namespace

ParseResult parse(const std::string& source) {
    Parser p(source);
    return p.run();
}

std::string dump(const Program& p) {
    std::ostringstream o;
    for (const auto& g : p.grabs) o << "grab " << (g.kind == Grab::Kind::Js ? "js" : "html") << " " << g.path << "\n";
    for (const auto& r : p.rules) {
        o << "rule \"" << r.selector << "\" " << (r.event.empty() ? std::string("static") : "on:" + r.event) << "\n";
        for (const auto& d : r.style) o << "  style " << d.name << ": " << d.value << "\n";
        for (const auto& a : r.actions) {
            if (auto* s = std::get_if<SwitchAction>(&a)) {
                o << "  switch." << toString(s->mode);
                if (s->mode == SwitchMode::Tooltip) o << "(" << toString(s->position) << ")";
                o << " -> " << s->target << "\n";
            } else if (auto* j = std::get_if<RunJs>(&a)) {
                o << "  runjs " << j->function << " args=[";
                for (size_t i = 0; i < j->args.size(); ++i) o << (i ? "|" : "") << j->args[i];
                o << "]\n";
            }
        }
    }
    return o.str();
}

}  // namespace lucyn::lnm
