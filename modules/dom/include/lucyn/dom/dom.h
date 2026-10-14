// lucyn/dom/dom.h - HTML -> DOM tree you can read AND change. No JS, no layout, no network.
// Parsing is done by Gumbo (HTML5 spec tree builder); we copy it into our own
// tree so nothing outside this module ever touches Gumbo types.
//
// Ownership (matters for the JS bridge):
//   * Every node belongs to exactly one Document and is always owned by it:
//     either by its parent, or, while detached, by the Document's "detached pool".
//   * Node* handed out stay valid until the Document dies or purgeDetached() is
//     called. Removing a node never frees it, so JS can keep a reference.
//   * Nodes cannot move between Documents (use cloneNode + the target's own factory).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lucyn::dom {

enum class NodeType { Document, Element, Text, Comment };

class Document;

struct Node {
    NodeType    type = NodeType::Document;
    std::string name;     // Element: lowercase tag. Text/Comment: empty
    std::string text;     // Text/Comment: data
    std::vector<std::pair<std::string, std::string>> attrs;   // Element only, source order
    Node*       parent = nullptr;
    Document*   ownerDocument = nullptr;
    std::vector<std::unique_ptr<Node>> children;

    Node() = default;
    ~Node();                                  // iterative: deep trees cannot overflow the stack
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    bool isElement() const { return type == NodeType::Element; }
    bool isText() const    { return type == NodeType::Text; }

    // ---- read ---------------------------------------------------------------
    const std::string* attr(const std::string& n) const;      // nullptr if absent
    bool        hasAttr(const std::string& n) const { return attr(n) != nullptr; }
    std::string id() const;
    bool        hasClass(const std::string& c) const;

    // Concatenated text of this subtree (script/style excluded).
    std::string textContent() const;
    // Back to HTML (not byte-identical to the source: it is the parsed tree).
    std::string outerHTML() const;
    std::string innerHTML() const;

    // Selectors: tag, #id, .class, [attr], [attr=val], compounds (a.b#c[x]),
    // descendant (space) and child (>) combinators, comma lists, '*'.
    std::vector<const Node*> querySelectorAll(const std::string& selector) const;
    const Node*              querySelector(const std::string& selector) const;
    std::vector<Node*>       querySelectorAll(const std::string& selector);
    Node*                    querySelector(const std::string& selector);

    // Navigation (nullptr when there is none).
    const Node* firstChild() const;
    const Node* lastChild() const;
    const Node* nextSibling() const;
    const Node* previousSibling() const;
    Node* firstChild();
    Node* lastChild();
    Node* nextSibling();
    Node* previousSibling();
    bool  contains(const Node* other) const;      // true for `this` itself too

    // ---- change -------------------------------------------------------------
    // All of these return false (and fill *err when given) instead of throwing.
    // Every successful change bumps Document::version() and notifies listeners.
    //
    // appendChild/insertBefore/replaceChild MOVE the child if it already sits
    // somewhere (like the real DOM). Refused: a node into itself or its own
    // descendant, into a text/comment node, a Document node anywhere, or a node
    // that belongs to another Document.
    bool  appendChild(Node* child, std::string* err = nullptr);
    bool  insertBefore(Node* child, Node* ref, std::string* err = nullptr);   // ref == nullptr: append
    bool  replaceChild(Node* newChild, Node* oldChild, std::string* err = nullptr);
    Node* removeChild(Node* child);               // returns child (now detached), or nullptr if not ours
    bool  remove();                               // detach self from its parent

    bool  setAttr(const std::string& name, const std::string& value);   // lowercases name; false if not an element / bad name
    bool  removeAttr(const std::string& name);                          // false if it was not there
    bool  setTextContent(const std::string& text);   // element: replaces all children; text/comment: sets data
    bool  setInnerHTML(std::string_view html);       // element only; parsed in this element's context
    Node* cloneNode(bool deep);                      // detached copy, owned by the same Document
};

class Document {
public:
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    const Node& root() const { return *root_; }
    Node&       root()       { return *root_; }
    const Node* documentElement() const;   // <html>
    const Node* head() const;
    const Node* body() const;
    Node* documentElement();
    Node* head();
    Node* body();
    std::string title() const;             // whitespace-collapsed, "" if none
    const Node* getElementById(const std::string& id) const;
    Node*       getElementById(const std::string& id);
    std::vector<const Node*> getElementsByTagName(const std::string& tag) const;
    std::vector<Node*>       getElementsByTagName(const std::string& tag);
    std::vector<const Node*> querySelectorAll(const std::string& s) const { return std::as_const(*root_).querySelectorAll(s); }
    const Node*              querySelector(const std::string& s) const    { return std::as_const(*root_).querySelector(s); }
    std::vector<Node*>       querySelectorAll(const std::string& s)       { return root_->querySelectorAll(s); }
    Node*                    querySelector(const std::string& s)          { return root_->querySelector(s); }

    // Factories: the new node starts detached (owned by the Document).
    Node* createElement(const std::string& tag);          // nullptr if the tag name is invalid
    Node* createTextNode(std::string text);
    Node* createComment(std::string text);
    // Copy a node (and its subtree when deep) from ANOTHER Document into this one. Detached.
    // nullptr for a Document node.
    Node* importNode(const Node& src, bool deep);

    size_t detachedCount() const { return detached_.size(); }
    // Frees every detached node. Only call when nothing (JS included) still points at them.
    void   purgeDetached();

    // Changes so far. A cheap "has anything moved since I last looked" for wait-for-idle.
    uint64_t version() const { return version_; }

    // Mutation hook (the future MutationObserver sits on this).
    struct Mutation {
        enum class Kind { ChildInserted, ChildRemoved, Attribute, Text } kind;
        Node*       target;     // the parent (child changes) or the node itself (attribute/text)
        Node*       node;       // the child that moved; nullptr for Attribute/Text
        std::string name;       // attribute name for Attribute, else empty
    };
    using MutationFn = std::function<void(const Mutation&)>;
    using ListenerId = unsigned;
    ListenerId onMutation(MutationFn fn);
    void       offMutation(ListenerId id);

private:
    Document() = default;
    friend std::unique_ptr<Document> parseHtml(std::string_view);
    friend struct Node;

    std::unique_ptr<Node> takeOwnership(Node* n);          // pulls n out of its parent or the detached pool
    Node*                 keepDetached(std::unique_ptr<Node> n);
    void                  notify(const Mutation& m);

    std::unique_ptr<Node> root_;
    std::unordered_map<Node*, std::unique_ptr<Node>> detached_;
    uint64_t version_ = 0;
    ListenerId nextListener_ = 1;
    std::vector<std::pair<ListenerId, MutationFn>> listeners_;
};

// Never fails: HTML5 parsing recovers from anything.
std::unique_ptr<Document> parseHtml(std::string_view html);

// Debug view: indented tree, one node per line.
std::string dumpTree(const Node& n, int maxDepth = 64);

// Like outerHTML, but leaves out every node (and its subtree) for which skip() is true.
std::string serialize(const Node& n, const std::function<bool(const Node&)>& skip);

}  // namespace lucyn::dom
