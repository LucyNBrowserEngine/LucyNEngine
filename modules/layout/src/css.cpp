#include <lucyn/layout/css.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace lucyn::layout {
namespace {

std::string lowerStr(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Removes /* ... */ comments. Quoted strings are respected.
std::string stripComments(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    char quote = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (quote) {
            out += c;
            if (c == '\\' && i + 1 < in.size()) { out += in[++i]; }
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; out += c; continue; }
        if (c == '/' && i + 1 < in.size() && in[i + 1] == '*') {
            size_t end = in.find("*/", i + 2);
            if (end == std::string::npos) break;
            i = end + 1;
            out += ' ';
            continue;
        }
        out += c;
    }
    return out;
}

// -- selector parsing --------------------------------------------------

// Parses one compound selector (no combinators) from `s` (already trimmed).
// Drops anything we don't understand (pseudo-elements, unsupported
// pseudo-classes) rather than failing the whole selector — that matches what
// browsers do for unknown pseudo-classes.
SimpleSelector parseCompound(std::string s) {
    SimpleSelector out;
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == '*') { out.universal = true; ++i; continue; }
        if (c == '#') {
            ++i;
            std::string id;
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '_'))
                id += s[i++];
            out.id = id;
            continue;
        }
        if (c == '.') {
            ++i;
            std::string cls;
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '_'))
                cls += s[i++];
            if (!cls.empty()) out.classes.push_back(cls);
            continue;
        }
        if (c == '[') {
            ++i;
            std::string body;
            while (i < s.size() && s[i] != ']') body += s[i++];
            if (i < s.size()) ++i;            // consume ']'
            // body is `name` or `name=value` or `name="value"`
            std::string name, value;
            bool hasValue = false;
            size_t eq = body.find('=');
            if (eq != std::string::npos) {
                name = trim(body.substr(0, eq));
                value = trim(body.substr(eq + 1));
                hasValue = true;
                // strip surrounding quotes from value
                if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                                         (value.front() == '\'' && value.back() == '\'')))
                    value = value.substr(1, value.size() - 2);
            } else {
                name = trim(body);
            }
            if (!name.empty()) out.attrs.emplace_back(lowerStr(name), hasValue ? value : std::string());
            continue;
        }
        if (c == ':') {
            // pseudo-class / pseudo-element. Consume to next non-identifier
            // char or matching ')'. We mostly drop these (kept selector
            // matches as if they weren't there). One exception: :not(...)
            // is parsed recursively into extra compound requirements; for
            // simplicity we currently drop its contents too — a real :not
            // would need a sub-selector and inverted match, not worth it
            // for VitePress's main stylesheets.
            ++i;
            // :: pseudo-element (consume extra ':')
            if (i < s.size() && s[i] == ':') ++i;
            std::string name;
            while (i < s.size() && (std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '-'))
                name += s[i++];
            // Consume an arg list `(...)` if present
            if (i < s.size() && s[i] == '(') {
                int depth = 0;
                while (i < s.size()) {
                    if (s[i] == '(') ++depth;
                    else if (s[i] == ')') { --depth; if (depth == 0) { ++i; break; } }
                    ++i;
                }
            }
            // Special: :root matches the html element; we represent it as
            // tag="html" so the matcher Just Works.
            if (lowerStr(name) == "root") { out.tag = "html"; }
            continue;
        }
        // tag name
        if (std::isalpha(static_cast<unsigned char>(c))) {
            std::string tag;
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '_'))
                tag += s[i++];
            out.tag = lowerStr(tag);
            continue;
        }
        // Unknown character: skip one to avoid infinite loop
        ++i;
    }
    return out;
}

// Splits a selector into compound pieces by whitespace combinators, treating
// `>`, `+`, `~` as whitespace (we don't model child/sibling combinators —
// they degrade to descendant, which is a strict superset of matches).
CompoundChain parseSelector(std::string s) {
    CompoundChain chain;
    // Replace > + ~ with space, then split on whitespace.
    for (auto& c : s) if (c == '>' || c == '+' || c == '~') c = ' ';
    std::istringstream in(s);
    std::string part;
    while (in >> part) {
        // We may have lost the space between `>a` and `b`; re-tokenize by
        // scanning for the next compound. parseCompound handles a single
        // compound string (no whitespace).
        SimpleSelector sel = parseCompound(part);
        // Drop an empty compound (e.g. from a leading `>`).
        if (sel.tag.empty() && sel.id.empty() && sel.classes.empty() && !sel.universal && sel.attrs.empty())
            continue;
        chain.push_back(std::move(sel));
    }
    return chain;
}

void computeSpecificity(const CompoundChain& chain, int& a, int& b, int& c) {
    a = b = c = 0;
    for (auto& s : chain) {
        if (!s.id.empty()) ++a;
        b += static_cast<int>(s.classes.size()) + static_cast<int>(s.attrs.size());
        if (!s.tag.empty()) ++c;
    }
}

// -- CSS rule parsing --------------------------------------------------

// Parses the body of a rule (between `{` and the matching `}`), respecting
// nested braces (e.g. for `@media { ... }`). Returns the body and the index
// just after the closing brace. Braces inside strings are respected.
std::string consumeBlock(const std::string& s, size_t& i) {
    std::string out;
    if (i >= s.size() || s[i] != '{') return out;
    ++i;
    int depth = 1;
    char quote = 0;
    while (i < s.size() && depth > 0) {
        char c = s[i];
        if (quote) {
            out += c;
            if (c == '\\' && i + 1 < s.size()) { out += s[++i]; }
            else if (c == quote) quote = 0;
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; out += c; ++i; continue; }
        if (c == '{') { ++depth; out += c; ++i; continue; }
        if (c == '}') { --depth; if (depth > 0) out += c; ++i; continue; }
        out += c;
        ++i;
    }
    return out;
}

// Is the given selector a custom-property declaration on :root (or .dark)?
// Returns: 0 = no, 1 = :root (light), 2 = .dark (dark).
int rootKind(const std::string& selText) {
    std::string s = trim(lowerStr(selText));
    if (s.empty()) return 0;
    // Strip pseudo-classes (`:root:where(:lang(zh))` etc.) — anything after ':'.
    size_t colon = s.find(':');
    if (colon != std::string::npos) s = s.substr(0, colon);
    s = trim(s);
    // Comma-separated; any token that is ":root" / ":root.dark" / "html" /
    // "html.dark" counts. We pick the most specific: if any token mentions
    // .dark, this rule's custom properties go to darkVars.
    bool anyRoot = false, anyDark = false;
    std::string cur;
    auto flush = [&]() {
        std::string t = trim(cur);
        cur.clear();
        if (t == "html" || t == "*") anyRoot = true;
        else if (t == "html.dark" || t == ".dark" || t == "html.dark,") anyDark = true;
        else if (t == "*") anyRoot = true;
    };
    for (char c : s) {
        if (c == ',') { flush(); continue; }
        cur += c;
    }
    flush();
    if (anyDark) return 2;
    if (anyRoot) return 1;
    return 0;
}

// Parse declarations from `body`, splitting on top-level `;`. Quoted
// strings and parentheses are respected (already done by style.h's parser,
// but here we also need to recognise custom properties `--name: value`).
std::vector<Declaration> parseRuleDeclarations(const std::string& body) {
    // style.h's parseDeclarations already handles comments, quotes, parens.
    // It does NOT keep `--foo: bar` declarations (they aren't in the known
    // property table — they're just dropped). We re-implement the splitter
    // here to keep them: the Stylesheet caller will fish out --vars before
    // passing the rest to styleFromDeclarations.
    std::vector<Declaration> out;
    std::string cur;
    char quote = 0;
    int depth = 0;
    for (size_t i = 0; i < body.size(); ++i) {
        char c = body[i];
        if (quote) {
            cur += c;
            if (c == '\\' && i + 1 < body.size()) { cur += body[++i]; }
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
        if (c == '(') { ++depth; cur += c; continue; }
        if (c == ')' && depth > 0) { --depth; cur += c; continue; }
        if (c == ';' && depth == 0) {
            std::string d = trim(cur);
            cur.clear();
            if (d.empty()) continue;
            auto colon = d.find(':');
            if (colon == std::string::npos) continue;
            Declaration decl;
            decl.name = trim(lowerStr(d.substr(0, colon)));
            std::string v = trim(d.substr(colon + 1));
            // Strip trailing `!important` (case/space-insensitive).
            size_t bang = v.rfind('!');
            if (bang != std::string::npos && lowerStr(trim(v.substr(bang + 1))) == "important") {
                decl.important = true;
                v = trim(v.substr(0, bang));
            }
            decl.value = v;
            out.push_back(std::move(decl));
            continue;
        }
        cur += c;
    }
    std::string d = trim(cur);
    if (!d.empty()) {
        auto colon = d.find(':');
        if (colon != std::string::npos) {
            Declaration decl;
            decl.name = trim(lowerStr(d.substr(0, colon)));
            std::string v = trim(d.substr(colon + 1));
            size_t bang = v.rfind('!');
            if (bang != std::string::npos && lowerStr(trim(v.substr(bang + 1))) == "important") {
                decl.important = true;
                v = trim(v.substr(0, bang));
            }
            decl.value = v;
            out.push_back(std::move(decl));
        }
    }
    return out;
}

}  // namespace

std::vector<CompoundChain> parseSelectorList(const std::string& text) {
    std::vector<CompoundChain> out;
    // Split on top-level commas (outside parens and brackets).
    std::string cur;
    int pdepth = 0, bdepth = 0;
    char quote = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quote) { cur += c; if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
        if (c == '(') { ++pdepth; cur += c; continue; }
        if (c == ')') { if (pdepth > 0) --pdepth; cur += c; continue; }
        if (c == '[') { ++bdepth; cur += c; continue; }
        if (c == ']') { if (bdepth > 0) --bdepth; cur += c; continue; }
        if (c == ',' && pdepth == 0 && bdepth == 0) {
            std::string s = trim(cur);
            cur.clear();
            if (!s.empty()) {
                auto chain = parseSelector(s);
                if (!chain.empty()) out.push_back(std::move(chain));
            }
            continue;
        }
        cur += c;
    }
    std::string s = trim(cur);
    if (!s.empty()) {
        auto chain = parseSelector(s);
        if (!chain.empty()) out.push_back(std::move(chain));
    }
    return out;
}

void Stylesheet::add(const std::string& cssText) {
    std::string src = stripComments(cssText);
    size_t i = 0;
    while (i < src.size()) {
        // Skip whitespace.
        while (i < src.size() && std::isspace(static_cast<unsigned char>(src[i]))) ++i;
        if (i >= src.size()) break;
        // At-rules. We skip @font-face / @keyframes / @import / @charset /
        // @namespace entirely; @media / @supports are unwrapped (their block
        // is parsed as if it were at the top level — we ignore the media
        // condition, which is what a "no media queries" layout has to do).
        if (src[i] == '@') {
            // Read the at-keyword.
            std::string atName;
            ++i;
            while (i < src.size() && (std::isalpha(static_cast<unsigned char>(src[i])) || src[i] == '-'))
                atName += static_cast<char>(std::tolower(static_cast<unsigned char>(src[i++])));
            atName = lowerStr(atName);
            // Read up to either '{' or ';' (the prelude).
            std::string prelude;
            while (i < src.size() && src[i] != '{' && src[i] != ';') {
                prelude += src[i++];
            }
            if (i >= src.size()) break;
            if (src[i] == ';') { ++i; continue; }    // simple at-rule (e.g. @import)
            // src[i] == '{': consume the block.
            std::string body = consumeBlock(src, i);
            if (atName == "media" || atName == "supports") {
                // Recursively parse the block as a stylesheet fragment.
                add(body);
            }
            // else: skip (@font-face, @keyframes, @page, etc.).
            continue;
        }
        // Regular rule: selector { decls }
        std::string selText;
        while (i < src.size() && src[i] != '{') selText += src[i++];
        if (i >= src.size()) break;
        std::string body = consumeBlock(src, i);
        std::string trimmedSel = trim(selText);
        if (trimmedSel.empty()) continue;

        // Custom-property block? If the selector is `:root` or `.dark`,
        // collect `--foo: bar` declarations into the right var table.
        int root = rootKind(trimmedSel);
        auto decls = parseRuleDeclarations(body);
        if (root != 0) {
            auto& target = (root == 2) ? darkVars : vars;
            if (root == 2) hasDarkRules = true;
            for (auto& d : decls) {
                if (d.name.size() > 2 && d.name[0] == '-' && d.name[1] == '-') {
                    target[d.name] = d.value;
                }
                // Other declarations on :root (e.g. `font-optical-sizing:auto`)
                // are also kept as real rules, below, so layout can use them.
            }
        }
        // Always also try to register as a regular rule: `:root` often
        // carries real declarations too (font-family, font-size, etc.).
        std::vector<CompoundChain> chains = parseSelectorList(trimmedSel);
        for (auto& chain : chains) {
            CssRule rule;
            rule.selector = chain;
            computeSpecificity(chain, rule.specA, rule.specB, rule.specC);
            rule.decls = decls;
            rules.push_back(std::move(rule));
        }
    }
}

// -- matching ----------------------------------------------------------

namespace {

bool classListContains(const lucyn::dom::Node& el, const std::string& cls) {
    const std::string* c = el.attr("class");
    if (!c) return false;
    std::istringstream in(*c);
    std::string tok;
    while (in >> tok) if (lowerStr(tok) == cls) return true;
    return false;
}

bool compoundMatches(const SimpleSelector& sel, const lucyn::dom::Node& el) {
    if (!el.isElement()) return false;
    if (!sel.tag.empty() && lowerStr(el.name) != sel.tag) return false;
    for (auto& cls : sel.classes) if (!classListContains(el, cls)) return false;
    if (!sel.id.empty()) {
        const std::string* id = el.attr("id");
        if (!id || *id != sel.id) return false;
    }
    for (auto& [name, value] : sel.attrs) {
        const std::string* v = el.attr(name.c_str());
        if (!v) return false;
        if (!value.empty() && *v != value) return false;
    }
    return true;
}

}  // namespace

bool matches(const CompoundChain& chain, const lucyn::dom::Node& el) {
    if (chain.empty()) return false;
    // Walk from the last compound backwards through ancestors. The last
    // compound must match `el`; each earlier compound must match some
    // ancestor (descendant combinator).
    size_t k = chain.size() - 1;
    if (!compoundMatches(chain[k], el)) return false;
    const lucyn::dom::Node* cur = el.parent;
    while (k > 0 && cur) {
        if (compoundMatches(chain[k - 1], *cur)) {
            --k;
            cur = cur->parent;
            continue;
        }
        cur = cur->parent;
    }
    return k == 0;
}

// -- var resolution ----------------------------------------------------

std::string resolveVars(const std::string& value, const Stylesheet& sheet, bool darkActive) {
    // Recursively resolve `var(--name)` and `var(--name, fallback)`.
    std::string out;
    out.reserve(value.size());
    size_t i = 0;
    int depth = 0;
    while (i < value.size()) {
        char c = value[i];
        // Look for `var(`.
        if (c == 'v' && value.compare(i, 4, "var(") == 0) {
            // Find matching ')'.
            size_t j = i + 4;
            int d = 1;
            while (j < value.size() && d > 0) {
                if (value[j] == '(') ++d;
                else if (value[j] == ')') --d;
                if (d > 0) ++j;
            }
            if (j >= value.size()) { out += value.substr(i); break; }
            std::string inside = trim(value.substr(i + 4, j - (i + 4)));
            // Split on first ','.
            std::string name, fallback;
            size_t comma = inside.find(',');
            bool hasFallback = false;
            if (comma != std::string::npos) {
                name = trim(inside.substr(0, comma));
                fallback = trim(inside.substr(comma + 1));
                hasFallback = true;
            } else {
                name = inside;
            }
            const std::string* v = nullptr;
            if (darkActive) {
                auto it = sheet.darkVars.find(name);
                if (it != sheet.darkVars.end()) v = &it->second;
            }
            if (!v) {
                auto it = sheet.vars.find(name);
                if (it != sheet.vars.end()) v = &it->second;
            }
            if (v && !v->empty()) {
                out += resolveVars(*v, sheet, darkActive);
            } else if (hasFallback) {
                out += resolveVars(fallback, sheet, darkActive);
            } else {
                // Leave the var() in place — layout will ignore the declaration.
                out += value.substr(i, j - i + 1);
            }
            i = j + 1;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

// -- styleForElement --------------------------------------------------

namespace {

bool elementIsDark(const lucyn::dom::Node& el) {
    // We look at the <html> element's class list for "dark".
    const lucyn::dom::Node* root = &el;
    while (root->parent) root = root->parent;
    if (!root->isElement()) return false;
    return classListContains(*root, "dark");
}

// Cascade sort key for a matched rule (with !important broken out):
// important beats non-important, then by (specA, specB, specC), then by source order.
struct SortKey {
    bool important;
    int a, b, c;
    size_t order;  // index in sheet->rules
    bool operator<(const SortKey& o) const {
        if (important != o.important) return important < o.important;
        if (a != o.a) return a < o.a;
        if (b != o.b) return b < o.b;
        if (c != o.c) return c < o.c;
        return order < o.order;
    }
};

}  // namespace

Style styleForElement(const lucyn::dom::Node& el, const Stylesheet* sheet) {
    Style st;
    if (!el.isElement()) return st;

    std::vector<Declaration> decls;
    // UA rule: [hidden] -> display:none. Author rules can override.
    if (el.hasAttr("hidden")) decls.push_back(Declaration{"display", "none", false});

    if (sheet) {
        bool dark = sheet->hasDarkRules && elementIsDark(el);
        // Match rules, collect (rule, source order).
        struct Match { const CssRule* rule; size_t order; };
        std::vector<Match> matchedRules;
        for (size_t i = 0; i < sheet->rules.size(); ++i) {
            const auto& r = sheet->rules[i];
            if (lucyn::layout::matches(r.selector, el)) matchedRules.push_back({&r, i});
        }
        // Sort matches by (specificity, source order). Later wins, so apply
        // low-specificity / earlier rules first.
        std::sort(matchedRules.begin(), matchedRules.end(), [](const Match& x, const Match& y) {
            const CssRule& a = *x.rule; const CssRule& b = *y.rule;
            if (a.specA != b.specA) return a.specA < b.specA;
            if (a.specB != b.specB) return a.specB < b.specB;
            if (a.specC != b.specC) return a.specC < b.specC;
            return x.order < y.order;
        });
        // Two passes: non-important first, then important. Within each pass,
        // higher specificity / later source order overwrites earlier ones.
        for (auto& m : matchedRules) {
            for (auto& d : m.rule->decls) {
                if (d.important) continue;
                if (d.name.size() > 2 && d.name[0] == '-' && d.name[1] == '-') continue;
                Declaration copy = d;
                copy.value = resolveVars(copy.value, *sheet, dark);
                decls.push_back(copy);
            }
        }
        for (auto& m : matchedRules) {
            for (auto& d : m.rule->decls) {
                if (!d.important) continue;
                if (d.name.size() > 2 && d.name[0] == '-' && d.name[1] == '-') continue;
                Declaration copy = d;
                copy.value = resolveVars(copy.value, *sheet, dark);
                decls.push_back(copy);
            }
        }
    }

    // Inline `style=""` overrides everything.
    if (const std::string* s = el.attr("style")) {
        auto parsed = parseDeclarations(*s);
        decls.insert(decls.end(), parsed.begin(), parsed.end());
    }

    return styleFromDeclarations(decls);
}

}  // namespace lucyn::layout
