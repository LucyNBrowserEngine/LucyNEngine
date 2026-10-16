#include <lucyn/lnm/compile.h>
#include <lucyn/layout/style.h>

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace lucyn::lnm {
namespace {

void warn(std::vector<Diagnostic>* diags, const SourceLoc& loc, const std::string& msg) {
    if (diags) diags->push_back(Diagnostic{Diagnostic::Severity::Warning, loc, msg});
}

bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '\\'; }

}  // namespace

std::vector<CssDecl> translateStyle(const StyleDecl& d, std::vector<Diagnostic>* diags) {
    const std::string& n = d.name;
    const std::string& v = d.value;
    if (n == "color" || n == "textcolor") {
        if (!lucyn::layout::parseColor(v)) warn(diags, d.loc, "'" + v + "' is not a colour LNM understands (use #rgb, #rrggbb, rgb(), rgba(), a name, or transparent)");
        return {{n == "color" ? "background-color" : "color", v}};
    }
    if (n == "radius") return {{"border-radius", v}};
    if (n == "scale") return {{"transform", "scale(" + v + ")"}};
    if (n == "shadow") return {{"box-shadow", v}};
    if (n == "transition") return {{"transition", "all " + v}};
    if (n == "padding" || n == "margin" || n == "border" || n == "opacity") return {{n, v}};
    warn(diags, d.loc, "'" + n + "' is not an LNM property (LNM_SPEC.md section 2); passed to layout as CSS");
    return {{n, v}};
}

std::vector<std::string> splitSelectorList(const std::string& list) {
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    char quote = 0;
    for (char c : list) {
        if (quote) { cur += c; if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
        if (c == '[' || c == '(') ++depth;
        else if ((c == ']' || c == ')') && depth > 0) --depth;
        if (c == ',' && depth == 0) { parts.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    parts.push_back(cur);
    for (auto& p : parts) {
        size_t a = p.find_first_not_of(" \t\n\r"), b = p.find_last_not_of(" \t\n\r");
        p = a == std::string::npos ? "" : p.substr(a, b - a + 1);
    }
    parts.erase(std::remove(parts.begin(), parts.end(), std::string()), parts.end());
    return parts;
}

int specificity(const std::string& sel) {
    int a = 0, b = 0, c = 0;
    size_t i = 0, n = sel.size();
    auto skipIdent = [&]() { while (i < n && identChar(sel[i])) ++i; };
    while (i < n) {
        char ch = sel[i];
        if (ch == '#') { ++a; ++i; skipIdent(); }
        else if (ch == '.') { ++b; ++i; skipIdent(); }
        else if (ch == '[') { ++b; while (i < n && sel[i] != ']') ++i; if (i < n) ++i; }
        else if (ch == ':') {
            ++b;
            while (i < n && sel[i] == ':') ++i;
            skipIdent();
            if (i < n && sel[i] == '(') { int d = 0; while (i < n) { if (sel[i] == '(') ++d; else if (sel[i] == ')' && --d == 0) { ++i; break; } ++i; } }
        }
        else if (std::isalpha(static_cast<unsigned char>(ch))) { ++c; skipIdent(); }
        else ++i;   // '*', combinators, spaces
    }
    return std::min(a, 99) * 10000 + std::min(b, 99) * 100 + std::min(c, 99);
}

void appendInlineStyle(lucyn::dom::Node& el, const std::vector<CssDecl>& decls) {
    if (decls.empty()) return;
    std::string s;
    if (const std::string* old = el.attr("style")) {
        s = *old;
        while (!s.empty() && (s.back() == ' ' || s.back() == ';')) s.pop_back();
    }
    for (const auto& d : decls) {
        if (!s.empty()) s += "; ";
        s += d.first + ": " + d.second;
    }
    el.setAttr("style", s);
}

bool matchesSelector(lucyn::dom::Document& doc, const std::string& list, const lucyn::dom::Node* el) {
    for (const auto& part : splitSelectorList(list))
        for (const lucyn::dom::Node* n : std::as_const(doc).querySelectorAll(part))
            if (n == el) return true;
    return false;
}

void applyStaticRules(lucyn::dom::Document& doc, const Program& program, std::vector<Diagnostic>* diags,
                      const lucyn::dom::Node* within) {
    struct Entry { int spec; CssDecl css; };
    std::unordered_map<lucyn::dom::Node*, std::vector<Entry>> perNode;
    std::vector<lucyn::dom::Node*> order;   // first-seen order, so output is deterministic

    for (const Rule& rule : program.rules) {
        if (!rule.event.empty()) continue;
        std::vector<CssDecl> css;
        for (const auto& d : rule.style)
            for (auto& c : translateStyle(d, diags)) css.push_back(std::move(c));
        if (css.empty()) continue;
        for (const auto& part : splitSelectorList(rule.selector)) {
            int spec = specificity(part);
            for (lucyn::dom::Node* node : doc.querySelectorAll(part)) {
                if (within && !within->contains(node)) continue;
                auto it = perNode.find(node);
                if (it == perNode.end()) { order.push_back(node); it = perNode.emplace(node, std::vector<Entry>{}).first; }
                for (const auto& c : css) it->second.push_back(Entry{spec, c});
            }
        }
    }
    for (lucyn::dom::Node* node : order) {
        auto& entries = perNode[node];
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& x, const Entry& y) { return x.spec < y.spec; });
        std::string s;
        for (const auto& e : entries) {
            if (!s.empty()) s += "; ";
            s += e.css.first + ": " + e.css.second;
        }
        if (const std::string* old = node->attr("style"); old && !old->empty()) s += "; " + *old;   // the element's own style wins
        node->setAttr("style", s);
    }
}

}  // namespace lucyn::lnm
