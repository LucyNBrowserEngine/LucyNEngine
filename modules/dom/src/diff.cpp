#include <lucyn/dom/diff.h>
#include <lucyn/dom/dom.h>
#include <algorithm>
#include <map>
#include <sstream>

namespace lucyn::dom {

namespace {
struct Counts {
    std::map<std::string, int> tags, sigs;
    int elements = 0;
};

// div#app.big.card  (classes sorted so class order never counts as a change)
std::string signature(const Node& e) {
    std::string s = e.name;
    std::string id = e.id();
    if (!id.empty()) s += "#" + id;
    if (auto* c = e.attr("class")) {
        std::istringstream in(*c);
        std::vector<std::string> cls;
        std::string w;
        while (in >> w) cls.push_back(w);
        std::sort(cls.begin(), cls.end());
        cls.erase(std::unique(cls.begin(), cls.end()), cls.end());
        for (auto& k : cls) s += "." + k;
    }
    return s;
}

Counts count(std::string_view html) {
    Counts c;
    auto doc = parseHtml(html);
    std::vector<const Node*> st{&doc->root()};
    while (!st.empty()) {
        const Node* n = st.back(); st.pop_back();
        if (n->isElement()) {
            ++c.tags[n->name];
            ++c.sigs[signature(*n)];
            ++c.elements;
        }
        for (auto& k : n->children) st.push_back(k.get());
    }
    return c;
}

std::vector<DiffEntry> changed(const std::map<std::string, int>& a, const std::map<std::string, int>& b) {
    std::map<std::string, DiffEntry> all;
    for (auto& [k, v] : a) { all[k].key = k; all[k].before = v; }
    for (auto& [k, v] : b) { all[k].key = k; all[k].after = v; }
    std::vector<DiffEntry> out;
    for (auto& [k, e] : all) if (e.before != e.after) out.push_back(e);
    return out;   // map order = sorted by key
}
}  // namespace

HtmlDiff diffHtml(std::string_view beforeHtml, std::string_view afterHtml) {
    Counts a = count(beforeHtml), b = count(afterHtml);
    HtmlDiff d;
    d.tags = changed(a.tags, b.tags);
    d.elements = changed(a.sigs, b.sigs);
    d.beforeElements = a.elements;
    d.afterElements = b.elements;
    return d;
}

std::vector<std::string> HtmlDiff::newTags() const {
    std::vector<std::string> v;
    for (auto& e : tags) if (e.before == 0 && e.after > 0) v.push_back(e.key);
    return v;
}
std::vector<std::string> HtmlDiff::goneTags() const {
    std::vector<std::string> v;
    for (auto& e : tags) if (e.before > 0 && e.after == 0) v.push_back(e.key);
    return v;
}

std::string formatDiff(const HtmlDiff& d) {
    std::ostringstream o;
    o << "elements: " << d.beforeElements << " -> " << d.afterElements << "\n";
    if (d.empty()) { o << "no differences\n"; return o.str(); }
    auto line = [&](const DiffEntry& e) {
        o << "  " << (e.after > e.before ? '+' : '-') << ' ' << e.key << "  (" << e.before << " -> " << e.after << ")\n";
    };
    auto nt = d.newTags();
    if (!nt.empty()) { o << "new tags:"; for (auto& t : nt) o << ' ' << t; o << "\n"; }
    auto gt = d.goneTags();
    if (!gt.empty()) { o << "gone tags:"; for (auto& t : gt) o << ' ' << t; o << "\n"; }
    o << "tag counts:\n";
    for (auto& e : d.tags) line(e);
    o << "elements:\n";
    for (auto& e : d.elements) line(e);
    return o.str();
}

}  // namespace lucyn::dom
