#include <lucyn/dom/dom.h>
#include <algorithm>
#include <cctype>
#include <functional>
#include <sstream>

namespace lucyn::dom {

// ---- Node basics --------------------------------------------------------
const std::string* Node::attr(const std::string& n) const {
    for (auto& a : attrs) if (a.first == n) return &a.second;
    return nullptr;
}
std::string Node::id() const { auto* v = attr("id"); return v ? *v : std::string(); }

bool Node::hasClass(const std::string& c) const {
    auto* v = attr("class");
    if (!v) return false;
    std::istringstream ss(*v);
    std::string w;
    while (ss >> w) if (w == c) return true;
    return false;
}

static void collectText(const Node& n, std::string& out) {
    if (n.type == NodeType::Text) { out += n.text; return; }
    if (n.type == NodeType::Element && (n.name == "script" || n.name == "style")) return;
    for (auto& c : n.children) collectText(*c, out);
}
std::string Node::textContent() const { std::string s; collectText(*this, s); return s; }

static bool isVoid(const std::string& t) {
    static const char* v[] = {"area","base","br","col","embed","hr","img","input","link","meta","param","source","track","wbr"};
    for (auto* x : v) if (t == x) return true;
    return false;
}
static void escapeInto(std::string& o, const std::string& s, bool attr) {
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': if (attr) o += "&quot;"; else o += c; break;
            default: o += c;
        }
    }
}
using SkipFn = std::function<bool(const Node&)>;

static void ser(const Node& n, std::string& o, bool rawText, const SkipFn* skip) {
    if (skip && *skip && n.type != NodeType::Document && (*skip)(n)) return;
    switch (n.type) {
        case NodeType::Text:    if (rawText) o += n.text; else escapeInto(o, n.text, false); break;
        case NodeType::Comment: o += "<!--" + n.text + "-->"; break;
        case NodeType::Document: for (auto& c : n.children) ser(*c, o, false, skip); break;
        case NodeType::Element: {
            o += "<" + n.name;
            for (auto& a : n.attrs) { o += " " + a.first + "=\""; escapeInto(o, a.second, true); o += "\""; }
            o += ">";
            if (isVoid(n.name)) break;
            bool raw = n.name == "script" || n.name == "style";
            for (auto& c : n.children) ser(*c, o, raw, skip);
            o += "</" + n.name + ">";
        }
    }
}
std::string Node::outerHTML() const { std::string s; ser(*this, s, false, nullptr); return s; }
std::string Node::innerHTML() const {
    std::string s;
    bool raw = isElement() && (name == "script" || name == "style");
    for (auto& c : children) ser(*c, s, raw, nullptr);
    return s;
}
std::string serialize(const Node& n, const std::function<bool(const Node&)>& skip) {
    std::string s;
    ser(n, s, false, &skip);
    return s;
}

Node::~Node() {
    // Free the subtree with a loop instead of recursive unique_ptr destructors.
    std::vector<std::unique_ptr<Node>> pending;
    for (auto& c : children) pending.push_back(std::move(c));
    children.clear();
    while (!pending.empty()) {
        std::unique_ptr<Node> n = std::move(pending.back());
        pending.pop_back();
        for (auto& c : n->children) pending.push_back(std::move(c));
        n->children.clear();
    }
}

// ---- Selectors ----------------------------------------------------------
namespace {
struct AttrSel { std::string name, value; bool hasValue = false; };
struct Compound {
    std::string tag;                       // "" or "*" = any
    std::string id;
    std::vector<std::string> classes;
    std::vector<AttrSel> attrs;
    bool childOfPrev = false;              // true if joined to previous by '>'
};
using Chain = std::vector<Compound>;

std::string lc(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool identCh(char c) { return std::isalnum((unsigned char)c) || c == '-' || c == '_'; }

bool parseChain(const std::string& src, Chain& out) {
    size_t i = 0, n = src.size();
    bool pendingChild = false;
    auto skipWs = [&] { while (i < n && std::isspace((unsigned char)src[i])) ++i; };
    skipWs();
    while (i < n) {
        Compound c;
        c.childOfPrev = pendingChild;
        pendingChild = false;
        size_t start = i;
        if (src[i] == '*') { c.tag = "*"; ++i; }
        else if (identCh(src[i])) { size_t s = i; while (i < n && identCh(src[i])) ++i; c.tag = lc(src.substr(s, i - s)); }
        while (i < n && (src[i] == '#' || src[i] == '.' || src[i] == '[')) {
            if (src[i] == '#' || src[i] == '.') {
                char k = src[i++]; size_t s = i;
                while (i < n && identCh(src[i])) ++i;
                if (s == i) return false;
                if (k == '#') c.id = src.substr(s, i - s);
                else c.classes.push_back(src.substr(s, i - s));
            } else {
                size_t e = src.find(']', i);
                if (e == std::string::npos) return false;
                std::string body = src.substr(i + 1, e - i - 1);
                AttrSel a;
                size_t eq = body.find('=');
                if (eq == std::string::npos) a.name = lc(body);
                else {
                    a.name = lc(body.substr(0, eq)); a.hasValue = true;
                    a.value = body.substr(eq + 1);
                    if (a.value.size() >= 2 && (a.value.front() == '"' || a.value.front() == '\'') && a.value.back() == a.value.front())
                        a.value = a.value.substr(1, a.value.size() - 2);
                }
                if (a.name.empty()) return false;
                c.attrs.push_back(a);
                i = e + 1;
            }
        }
        if (i == start) return false;       // unsupported character
        out.push_back(c);
        size_t before = i;
        skipWs();
        if (i < n && src[i] == '>') { pendingChild = true; ++i; skipWs(); }
        else if (i == before && i < n) return false;
    }
    return !out.empty() && !pendingChild;
}

bool matchCompound(const Node& e, const Compound& c) {
    if (!e.isElement()) return false;
    if (!c.tag.empty() && c.tag != "*" && e.name != c.tag) return false;
    if (!c.id.empty() && e.id() != c.id) return false;
    for (auto& k : c.classes) if (!e.hasClass(k)) return false;
    for (auto& a : c.attrs) {
        auto* v = e.attr(a.name);
        if (!v || (a.hasValue && *v != a.value)) return false;
    }
    return true;
}

// Does `e` match chain[0..idx], with chain[idx] applying to e? Right-to-left.
bool matchChain(const Node& e, const Chain& ch, size_t idx) {
    if (!matchCompound(e, ch[idx])) return false;
    if (idx == 0) return true;
    if (ch[idx].childOfPrev)
        return e.parent && matchChain(*e.parent, ch, idx - 1);
    for (const Node* p = e.parent; p; p = p->parent)
        if (matchChain(*p, ch, idx - 1)) return true;
    return false;
}

void walk(const Node& n, const std::vector<Chain>& chains, std::vector<const Node*>& out) {
    for (auto& c : n.children) {
        if (c->isElement())
            for (auto& ch : chains)
                if (matchChain(*c, ch, ch.size() - 1)) { out.push_back(c.get()); break; }
        walk(*c, chains, out);
    }
}
}  // namespace

std::vector<const Node*> Node::querySelectorAll(const std::string& selector) const {
    std::vector<Chain> chains;
    size_t pos = 0;
    while (pos <= selector.size()) {
        size_t comma = selector.find(',', pos);
        std::string part = selector.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        Chain ch;
        if (!parseChain(part, ch)) return {};          // invalid selector -> nothing
        chains.push_back(std::move(ch));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    std::vector<const Node*> out;
    walk(*this, chains, out);
    return out;
}
const Node* Node::querySelector(const std::string& s) const {
    auto v = querySelectorAll(s);
    return v.empty() ? nullptr : v.front();
}

template <class T> static std::vector<Node*> mut(const std::vector<const T*>& v) {
    std::vector<Node*> out; out.reserve(v.size());
    for (auto* p : v) out.push_back(const_cast<Node*>(p));
    return out;
}
std::vector<Node*> Node::querySelectorAll(const std::string& s) { return mut(std::as_const(*this).querySelectorAll(s)); }
Node* Node::querySelector(const std::string& s) { return const_cast<Node*>(std::as_const(*this).querySelector(s)); }

// ---- Document -----------------------------------------------------------
static const Node* childNamed(const Node* p, const char* name) {
    if (!p) return nullptr;
    for (auto& c : p->children) if (c->isElement() && c->name == name) return c.get();
    return nullptr;
}
const Node* Document::documentElement() const { return childNamed(root_.get(), "html"); }
const Node* Document::head() const { return childNamed(documentElement(), "head"); }
const Node* Document::body() const { return childNamed(documentElement(), "body"); }

std::string Document::title() const {
    const Node* t = root_->querySelector("title");
    if (!t) return "";
    std::string raw = t->textContent(), out;
    bool sp = false;
    for (char c : raw) {
        if (std::isspace((unsigned char)c)) sp = !out.empty();
        else { if (sp) out += ' '; sp = false; out += c; }
    }
    return out;
}
const Node* Document::getElementById(const std::string& id) const {
    const Node* found = nullptr;
    std::vector<const Node*> stack{root_.get()};
    while (!stack.empty() && !found) {
        const Node* n = stack.back(); stack.pop_back();
        if (n->isElement() && n->id() == id) { found = n; break; }
        for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) stack.push_back(it->get());
    }
    return found;
}
std::vector<const Node*> Document::getElementsByTagName(const std::string& tag) const {
    return std::as_const(*root_).querySelectorAll(tag.empty() ? "*" : tag);
}

Node* Document::documentElement() { return const_cast<Node*>(std::as_const(*this).documentElement()); }
Node* Document::head() { return const_cast<Node*>(std::as_const(*this).head()); }
Node* Document::body() { return const_cast<Node*>(std::as_const(*this).body()); }
Node* Document::getElementById(const std::string& id) { return const_cast<Node*>(std::as_const(*this).getElementById(id)); }
std::vector<Node*> Document::getElementsByTagName(const std::string& tag) { return mut(std::as_const(*this).getElementsByTagName(tag)); }

// ---- Debug dump ---------------------------------------------------------
static void dump(const Node& n, int depth, int maxDepth, std::string& o) {
    if (depth > maxDepth) return;
    std::string pad(depth * 2, ' ');
    switch (n.type) {
        case NodeType::Document: o += "#document\n"; break;
        case NodeType::Comment:  o += pad + "<!-- ... -->\n"; break;
        case NodeType::Text: {
            std::string t;
            for (char c : n.text) if (!std::isspace((unsigned char)c) || (!t.empty() && t.back() != ' ')) t += std::isspace((unsigned char)c) ? ' ' : c;
            if (t.empty() || t == " ") return;
            if (t.size() > 60) t = t.substr(0, 57) + "...";
            o += pad + "\"" + t + "\"\n"; break;
        }
        case NodeType::Element:
            o += pad + "<" + n.name;
            for (auto& a : n.attrs) o += " " + a.first + (a.second.empty() ? "" : "=" + a.second.substr(0, 40));
            o += ">\n"; break;
    }
    for (auto& c : n.children) dump(*c, depth + 1, maxDepth, o);
}
std::string dumpTree(const Node& n, int maxDepth) { std::string o; dump(n, 0, maxDepth, o); return o; }

}  // namespace lucyn::dom
