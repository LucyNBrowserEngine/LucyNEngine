// Changing the tree. Every node is always owned by exactly one place: its parent's
// `children`, or the Document's detached pool. Moving = take ownership out of the old
// place, put it in the new one. Pointers never dangle while the Document lives.
#include <lucyn/dom/dom.h>
#include <algorithm>
#include <cctype>
#include "internal.h"

namespace lucyn::dom {

namespace {
bool fail(std::string* err, const char* msg) { if (err) *err = msg; return false; }

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

bool validTag(const std::string& t) {
    if (t.empty() || t.size() > 128) return false;
    if (!std::isalpha((unsigned char)t[0])) return false;
    for (char c : t) if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ':' || c == '.')) return false;
    return true;
}
bool validAttrName(const std::string& n) {
    if (n.empty() || n.size() > 256) return false;
    for (char c : n) {
        if (std::isspace((unsigned char)c) || c == '"' || c == '\'' || c == '>' || c == '/' || c == '=' || c == '<' || (unsigned char)c < 0x20)
            return false;
    }
    return true;
}

// Visit every node of a subtree without recursion.
template <class F> void eachNode(Node* top, F&& f) {
    std::vector<Node*> st{top};
    while (!st.empty()) {
        Node* n = st.back(); st.pop_back();
        f(n);
        for (auto& c : n->children) st.push_back(c.get());
    }
}

bool rawTextElement(const std::string& n) { return n == "script" || n == "style" || n == "textarea" || n == "title"; }
}  // namespace

// ---- Document internals ------------------------------------------------------
Document::~Document() = default;

std::unique_ptr<Node> Document::takeOwnership(Node* n) {
    if (n->parent) {
        auto& kids = n->parent->children;
        for (size_t i = 0; i < kids.size(); ++i) {
            if (kids[i].get() == n) {
                std::unique_ptr<Node> out = std::move(kids[i]);
                kids.erase(kids.begin() + (std::ptrdiff_t)i);
                out->parent = nullptr;
                return out;
            }
        }
        return nullptr;
    }
    auto it = detached_.find(n);
    if (it == detached_.end()) return nullptr;
    std::unique_ptr<Node> out = std::move(it->second);
    detached_.erase(it);
    return out;
}

Node* Document::keepDetached(std::unique_ptr<Node> n) {
    Node* raw = n.get();
    raw->parent = nullptr;
    detached_[raw] = std::move(n);
    return raw;
}

void Document::notify(const Mutation& m) {
    ++version_;
    // Copy: a listener may add/remove listeners (or mutate again) while we iterate.
    auto copy = listeners_;
    for (auto& l : copy) l.second(m);
}

Document::ListenerId Document::onMutation(MutationFn fn) {
    ListenerId id = nextListener_++;
    listeners_.emplace_back(id, std::move(fn));
    return id;
}
void Document::offMutation(ListenerId id) {
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(),
                                    [&](auto& p) { return p.first == id; }), listeners_.end());
}

void Document::purgeDetached() { detached_.clear(); }

Node* Document::createElement(const std::string& tag) {
    std::string t = lower(tag);
    if (!validTag(t)) return nullptr;
    auto n = std::make_unique<Node>();
    n->type = NodeType::Element;
    n->name = std::move(t);
    n->ownerDocument = this;
    return keepDetached(std::move(n));
}
Node* Document::createTextNode(std::string text) {
    auto n = std::make_unique<Node>();
    n->type = NodeType::Text;
    n->text = std::move(text);
    n->ownerDocument = this;
    return keepDetached(std::move(n));
}
Node* Document::createComment(std::string text) {
    auto n = std::make_unique<Node>();
    n->type = NodeType::Comment;
    n->text = std::move(text);
    n->ownerDocument = this;
    return keepDetached(std::move(n));
}

// ---- Node: navigation --------------------------------------------------------
static size_t indexInParent(const Node* n) {
    if (!n->parent) return (size_t)-1;
    auto& k = n->parent->children;
    for (size_t i = 0; i < k.size(); ++i) if (k[i].get() == n) return i;
    return (size_t)-1;
}
const Node* Node::firstChild() const { return children.empty() ? nullptr : children.front().get(); }
const Node* Node::lastChild() const  { return children.empty() ? nullptr : children.back().get(); }
const Node* Node::nextSibling() const {
    size_t i = indexInParent(this);
    return (i == (size_t)-1 || i + 1 >= parent->children.size()) ? nullptr : parent->children[i + 1].get();
}
const Node* Node::previousSibling() const {
    size_t i = indexInParent(this);
    return (i == (size_t)-1 || i == 0) ? nullptr : parent->children[i - 1].get();
}
Node* Node::firstChild()      { return const_cast<Node*>(std::as_const(*this).firstChild()); }
Node* Node::lastChild()       { return const_cast<Node*>(std::as_const(*this).lastChild()); }
Node* Node::nextSibling()     { return const_cast<Node*>(std::as_const(*this).nextSibling()); }
Node* Node::previousSibling() { return const_cast<Node*>(std::as_const(*this).previousSibling()); }

bool Node::contains(const Node* other) const {
    for (const Node* p = other; p; p = p->parent) if (p == this) return true;
    return false;
}

// ---- Node: tree changes ------------------------------------------------------
bool Node::insertBefore(Node* child, Node* ref, std::string* err) {
    if (!child) return fail(err, "no child given");
    if (type != NodeType::Element && type != NodeType::Document) return fail(err, "this node cannot have children");
    if (child->type == NodeType::Document) return fail(err, "a Document cannot be a child");
    if (!ownerDocument || child->ownerDocument != ownerDocument) return fail(err, "node belongs to another document");
    if (child->contains(this)) return fail(err, "cannot insert a node into itself or its own descendant");
    if (ref && ref->parent != this) return fail(err, "reference node is not a child of this node");
    if (ref == child) return true;                       // already exactly where it is asked to be

    Document* doc = ownerDocument;
    Node* oldParent = child->parent;
    std::unique_ptr<Node> owned = doc->takeOwnership(child);
    if (!owned) return fail(err, "node is not owned by its document");   // cannot happen unless the tree was hand-edited
    if (oldParent) doc->notify({Document::Mutation::Kind::ChildRemoved, oldParent, child, ""});

    size_t pos = children.size();
    if (ref) pos = indexInParent(ref);
    owned->parent = this;
    children.insert(children.begin() + (std::ptrdiff_t)pos, std::move(owned));
    doc->notify({Document::Mutation::Kind::ChildInserted, this, child, ""});
    return true;
}

bool Node::appendChild(Node* child, std::string* err) { return insertBefore(child, nullptr, err); }

Node* Node::removeChild(Node* child) {
    if (!child || child->parent != this || !ownerDocument) return nullptr;
    Document* doc = ownerDocument;
    std::unique_ptr<Node> owned = doc->takeOwnership(child);
    if (!owned) return nullptr;
    doc->keepDetached(std::move(owned));
    doc->notify({Document::Mutation::Kind::ChildRemoved, this, child, ""});
    return child;
}

bool Node::remove() {
    if (!parent) return false;
    return parent->removeChild(this) != nullptr;
}

bool Node::replaceChild(Node* newChild, Node* oldChild, std::string* err) {
    if (!newChild || !oldChild) return fail(err, "missing node");
    if (oldChild->parent != this) return fail(err, "node to replace is not a child of this node");
    if (newChild == oldChild) return true;
    // Put the new node right before the old one, then drop the old one.
    if (!insertBefore(newChild, oldChild, err)) return false;
    removeChild(oldChild);
    return true;
}

// ---- Node: attributes and text -----------------------------------------------
bool Node::setAttr(const std::string& name, const std::string& value) {
    if (type != NodeType::Element) return false;
    std::string n = lower(name);
    if (!validAttrName(n)) return false;
    bool found = false;
    for (auto& a : attrs) if (a.first == n) { a.second = value; found = true; break; }
    if (!found) attrs.emplace_back(n, value);
    if (ownerDocument) ownerDocument->notify({Document::Mutation::Kind::Attribute, this, nullptr, n});
    return true;
}

bool Node::removeAttr(const std::string& name) {
    if (type != NodeType::Element) return false;
    std::string n = lower(name);
    for (size_t i = 0; i < attrs.size(); ++i) {
        if (attrs[i].first == n) {
            attrs.erase(attrs.begin() + (std::ptrdiff_t)i);
            if (ownerDocument) ownerDocument->notify({Document::Mutation::Kind::Attribute, this, nullptr, n});
            return true;
        }
    }
    return false;
}

static void detachAllChildren(Node& n) {
    // Copy the pointers first: removeChild edits n.children while we go.
    std::vector<Node*> kids;
    for (auto& c : n.children) kids.push_back(c.get());
    for (Node* k : kids) n.removeChild(k);
}

bool Node::setTextContent(const std::string& t) {
    if (type == NodeType::Text || type == NodeType::Comment) {
        text = t;
        if (ownerDocument) ownerDocument->notify({Document::Mutation::Kind::Text, this, nullptr, ""});
        return true;
    }
    if (type != NodeType::Element || !ownerDocument) return false;
    detachAllChildren(*this);
    if (!t.empty()) appendChild(ownerDocument->createTextNode(t));
    return true;
}

bool Node::setInnerHTML(std::string_view html) {
    if (type != NodeType::Element || !ownerDocument) return false;
    Document* doc = ownerDocument;

    std::vector<Node*> fresh;
    if (rawTextElement(name)) {
        // Content of <script>/<style>/<textarea>/<title> is raw text, never markup.
        detachAllChildren(*this);
        if (!html.empty()) appendChild(doc->createTextNode(std::string(html)));
        return true;
    }

    std::vector<std::unique_ptr<Node>> parsed = parseFragment(name, html);
    detachAllChildren(*this);
    for (auto& p : parsed) {
        Node* raw = p.get();
        eachNode(raw, [&](Node* n) { n->ownerDocument = doc; });   // re-tag the whole subtree
        raw->parent = nullptr;
        fresh.push_back(doc->keepDetached(std::move(p)));
    }
    for (Node* f : fresh) appendChild(f);
    return true;
}

Node* Document::importNode(const Node& src, bool deep) {
    if (src.type == NodeType::Document) return nullptr;
    auto copyOne = [&](const Node* from) {
        auto n = std::make_unique<Node>();
        n->type = from->type; n->name = from->name; n->text = from->text; n->attrs = from->attrs;
        n->ownerDocument = this;
        return n;
    };
    // Iterative deep copy: (source, destination) pairs.
    std::unique_ptr<Node> top = copyOne(&src);
    Node* topRaw = top.get();
    if (deep) {
        std::vector<std::pair<const Node*, Node*>> st{{&src, topRaw}};
        while (!st.empty()) {
            auto [from, dst] = st.back(); st.pop_back();
            for (auto& c : from->children) {
                auto n = copyOne(c.get());
                n->parent = dst;
                Node* raw = n.get();
                dst->children.push_back(std::move(n));
                st.push_back({c.get(), raw});
            }
        }
    }
    return keepDetached(std::move(top));
}

Node* Node::cloneNode(bool deep) {
    if (!ownerDocument) return nullptr;
    return ownerDocument->importNode(*this, deep);
}

}  // namespace lucyn::dom
