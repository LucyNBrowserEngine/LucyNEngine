// The native half of the DOM bridge: Node/Element/Text/Comment/Document over lucyn::dom.
// Everything that is easier and just as correct in JS (classList, style, dataset, events, ParentNode mixins) lives in prelude.cpp.
#include "impl.h"
#include <algorithm>

namespace lucyn::js::detail {
namespace {

using dom::Node;
using dom::NodeType;

#define GET   JSContext* c, [[maybe_unused]] JSValueConst t
#define SET   JSContext* c, [[maybe_unused]] JSValueConst t, JSValueConst v
#define ARGS  JSContext* c, [[maybe_unused]] JSValueConst t, [[maybe_unused]] int argc, [[maybe_unused]] JSValueConst* argv
#define THIS_NODE(var) Node* var = static_cast<Node*>(JS_GetOpaque2(c, t, S(c)->nodeClass)); if (!var) return JS_EXCEPTION
#define ARG_STR(var, i) std::string var; if ((i) < argc) { if (!toStr(c, argv[i], var)) return JS_EXCEPTION; } else var = "undefined"

Node* nodeArg(State* s, JSValueConst v) { return static_cast<Node*>(JS_GetOpaque(v, s->nodeClass)); }

JSValue nodeList(State& s, const std::vector<Node*>& v) {
    JSValue a = JS_NewArray(s.ctx);
    for (size_t i = 0; i < v.size(); ++i) JS_SetPropertyUint32(s.ctx, a, uint32_t(i), wrapNode(s, v[i]));
    return a;
}

const char* domNameFor(const std::string& err) {
    return (err.find("not a child") != std::string::npos || err.find("not found") != std::string::npos) ? "NotFoundError" : "HierarchyRequestError";
}

Node* topOf(Node* n) { while (n->parent) n = n->parent; return n; }

std::string textOf(const Node* n) {
    if (n->type == NodeType::Text || n->type == NodeType::Comment) return n->text;
    std::string out;
    std::vector<const Node*> st;
    for (size_t i = n->children.size(); i-- > 0;) st.push_back(n->children[i].get());
    while (!st.empty()) {
        const Node* x = st.back(); st.pop_back();
        if (x->type == NodeType::Text) out += x->text;
        else if (x->isElement()) for (size_t i = x->children.size(); i-- > 0;) st.push_back(x->children[i].get());
    }
    return out;
}

Node* firstElem(Node* n) { for (auto& k : n->children) if (k->isElement()) return k.get(); return nullptr; }
Node* lastElem(Node* n)  { for (size_t i = n->children.size(); i-- > 0;) if (n->children[i]->isElement()) return n->children[i].get(); return nullptr; }

// ---- Node ----------------------------------------------------------------------------------------------------------------------
JSValue g_nodeType(GET) { THIS_NODE(n);
    switch (n->type) { case NodeType::Element: return JS_NewInt32(c, 1); case NodeType::Text: return JS_NewInt32(c, 3);
                       case NodeType::Comment: return JS_NewInt32(c, 8); default: return JS_NewInt32(c, 9); } }
JSValue g_nodeName(GET) { THIS_NODE(n);
    switch (n->type) { case NodeType::Element: return jstr(c, upperAscii(n->name)); case NodeType::Text: return JS_NewString(c, "#text");
                       case NodeType::Comment: return JS_NewString(c, "#comment"); default: return JS_NewString(c, "#document"); } }
JSValue g_nodeValue(GET) { THIS_NODE(n); return (n->type == NodeType::Text || n->type == NodeType::Comment) ? jstr(c, n->text) : JS_NULL; }
JSValue s_nodeValue(SET) { THIS_NODE(n); std::string x; if (!toStr(c, v, x)) return JS_EXCEPTION;
    if (n->type == NodeType::Text || n->type == NodeType::Comment) { n->setTextContent(x); }
    return JS_UNDEFINED; }
JSValue g_parentNode(GET) { THIS_NODE(n); return wrapNode(*S(c), n->parent); }
JSValue g_parentElement(GET) { THIS_NODE(n); return (n->parent && n->parent->isElement()) ? wrapNode(*S(c), n->parent) : JS_NULL; }
JSValue g_childNodes(GET) { THIS_NODE(n); std::vector<Node*> v; for (auto& k : n->children) v.push_back(k.get()); return nodeList(*S(c), v); }
JSValue g_firstChild(GET) { THIS_NODE(n); return wrapNode(*S(c), n->firstChild()); }
JSValue g_lastChild(GET) { THIS_NODE(n); return wrapNode(*S(c), n->lastChild()); }
JSValue g_nextSibling(GET) { THIS_NODE(n); return wrapNode(*S(c), n->nextSibling()); }
JSValue g_prevSibling(GET) { THIS_NODE(n); return wrapNode(*S(c), n->previousSibling()); }
JSValue g_ownerDocument(GET) { THIS_NODE(n); State* s = S(c);
    return n->type == NodeType::Document ? JS_NULL : wrapNode(*s, &s->doc->root()); }
JSValue g_isConnected(GET) { THIS_NODE(n); return JS_NewBool(c, isConnected(*S(c), n)); }
JSValue g_textContent(GET) { THIS_NODE(n); return n->type == NodeType::Document ? JS_NULL : jstr(c, textOf(n)); }
JSValue s_textContent(SET) { THIS_NODE(n); std::string x;
    if (!JS_IsNull(v) && !toStr(c, v, x)) return JS_EXCEPTION;          // null clears, like a browser
    if (n->type != NodeType::Document) { n->setTextContent(x); }
    return JS_UNDEFINED; }

JSValue m_appendChild(ARGS) { THIS_NODE(n); State* s = S(c);
    Node* ch = argc ? nodeArg(s, argv[0]) : nullptr;
    if (!ch) return JS_ThrowTypeError(c, "Failed to execute 'appendChild' on 'Node': parameter 1 is not of type 'Node'.");
    std::string err; if (!n->appendChild(ch, &err)) return throwDom(c, domNameFor(err), err);
    return JS_DupValue(c, argv[0]); }
JSValue m_insertBefore(ARGS) { THIS_NODE(n); State* s = S(c);
    Node* ch = argc ? nodeArg(s, argv[0]) : nullptr;
    if (!ch) return JS_ThrowTypeError(c, "Failed to execute 'insertBefore' on 'Node': parameter 1 is not of type 'Node'.");
    Node* ref = nullptr;
    if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
        ref = nodeArg(s, argv[1]);
        if (!ref) return JS_ThrowTypeError(c, "Failed to execute 'insertBefore' on 'Node': parameter 2 is not of type 'Node'.");
    }
    std::string err; if (!n->insertBefore(ch, ref, &err)) return throwDom(c, domNameFor(err), err);
    return JS_DupValue(c, argv[0]); }
JSValue m_removeChild(ARGS) { THIS_NODE(n); State* s = S(c);
    Node* ch = argc ? nodeArg(s, argv[0]) : nullptr;
    if (!ch) return JS_ThrowTypeError(c, "Failed to execute 'removeChild' on 'Node': parameter 1 is not of type 'Node'.");
    if (!n->removeChild(ch)) return throwDom(c, "NotFoundError", "Failed to execute 'removeChild' on 'Node': The node to be removed is not a child of this node.");
    return JS_DupValue(c, argv[0]); }
JSValue m_replaceChild(ARGS) { THIS_NODE(n); State* s = S(c);
    Node* nc = argc > 0 ? nodeArg(s, argv[0]) : nullptr; Node* oc = argc > 1 ? nodeArg(s, argv[1]) : nullptr;
    if (!nc || !oc) return JS_ThrowTypeError(c, "Failed to execute 'replaceChild' on 'Node': 2 arguments of type 'Node' required.");
    std::string err; if (!n->replaceChild(nc, oc, &err)) return throwDom(c, domNameFor(err), err);
    return JS_DupValue(c, argv[1]); }
JSValue m_cloneNode(ARGS) { THIS_NODE(n);
    if (n->type == NodeType::Document) return throwDom(c, "NotSupportedError", "Cloning a document is not supported.");
    bool deep = argc > 0 && JS_ToBool(c, argv[0]) > 0;
    return wrapNode(*S(c), n->cloneNode(deep)); }
JSValue m_contains(ARGS) { THIS_NODE(n); Node* o = argc ? nodeArg(S(c), argv[0]) : nullptr; return JS_NewBool(c, o && n->contains(o)); }
JSValue m_hasChildNodes(ARGS) { THIS_NODE(n); return JS_NewBool(c, !n->children.empty()); }

// ---- ParentNode (Element + Document) ---------------------------------------------------------------------------------------------
JSValue g_children(GET) { THIS_NODE(n); std::vector<Node*> v; for (auto& k : n->children) if (k->isElement()) v.push_back(k.get()); return nodeList(*S(c), v); }
JSValue g_childElementCount(GET) { THIS_NODE(n); int k = 0; for (auto& x : n->children) if (x->isElement()) ++k; return JS_NewInt32(c, k); }
JSValue g_firstElementChild(GET) { THIS_NODE(n); return wrapNode(*S(c), firstElem(n)); }
JSValue g_lastElementChild(GET) { THIS_NODE(n); return wrapNode(*S(c), lastElem(n)); }
JSValue m_querySelector(ARGS) { THIS_NODE(n); ARG_STR(sel, 0); return wrapNode(*S(c), n->querySelector(sel)); }
JSValue m_querySelectorAll(ARGS) { THIS_NODE(n); ARG_STR(sel, 0); return nodeList(*S(c), n->querySelectorAll(sel)); }
JSValue m_getElementsByTagName(ARGS) { THIS_NODE(n); ARG_STR(tag, 0);
    tag = lowerAscii(tag);
    for (char ch : tag) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '*' || ch == '_' || ch == ':')) return JS_NewArray(c);
    }
    return nodeList(*S(c), n->querySelectorAll(tag)); }

// ---- Element -------------------------------------------------------------------------------------------------------------------
JSValue g_tagName(GET) { THIS_NODE(n); return jstr(c, upperAscii(n->name)); }
JSValue g_localName(GET) { THIS_NODE(n); return jstr(c, n->name); }
JSValue g_id(GET) { THIS_NODE(n); const std::string* a = n->attr("id"); return jstr(c, a ? *a : std::string()); }
JSValue s_id(SET) { THIS_NODE(n); std::string x; if (!toStr(c, v, x)) return JS_EXCEPTION; n->setAttr("id", x); return JS_UNDEFINED; }
JSValue g_className(GET) { THIS_NODE(n); const std::string* a = n->attr("class"); return jstr(c, a ? *a : std::string()); }
JSValue s_className(SET) { THIS_NODE(n); std::string x; if (!toStr(c, v, x)) return JS_EXCEPTION; n->setAttr("class", x); return JS_UNDEFINED; }
JSValue g_innerHTML(GET) { THIS_NODE(n); return jstr(c, n->innerHTML()); }
JSValue s_innerHTML(SET) { THIS_NODE(n); std::string x; if (JS_IsNull(v)) x.clear(); else if (!toStr(c, v, x)) return JS_EXCEPTION;
    n->setInnerHTML(x); return JS_UNDEFINED; }
JSValue g_outerHTML(GET) { THIS_NODE(n); return jstr(c, n->outerHTML()); }
JSValue g_nextElem(GET) { THIS_NODE(n); Node* x = n->nextSibling(); while (x && !x->isElement()) x = x->nextSibling(); return wrapNode(*S(c), x); }
JSValue g_prevElem(GET) { THIS_NODE(n); Node* x = n->previousSibling(); while (x && !x->isElement()) x = x->previousSibling(); return wrapNode(*S(c), x); }
JSValue g_attributes(GET) { THIS_NODE(n); JSValue a = JS_NewArray(c); uint32_t i = 0;
    for (auto& kv : n->attrs) { JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "name", jstr(c, kv.first)); JS_SetPropertyStr(c, o, "value", jstr(c, kv.second)); JS_SetPropertyUint32(c, a, i++, o); }
    return a; }

JSValue m_getAttribute(ARGS) { THIS_NODE(n); ARG_STR(name, 0); const std::string* a = n->attr(lowerAscii(name)); return a ? jstr(c, *a) : JS_NULL; }
JSValue m_setAttribute(ARGS) { THIS_NODE(n); ARG_STR(name, 0); ARG_STR(val, 1);
    if (!n->setAttr(name, val)) return throwDom(c, "InvalidCharacterError", "Failed to execute 'setAttribute' on 'Element': '" + name + "' is not a valid attribute name.");
    return JS_UNDEFINED; }
JSValue m_removeAttribute(ARGS) { THIS_NODE(n); ARG_STR(name, 0); n->removeAttr(lowerAscii(name)); return JS_UNDEFINED; }
JSValue m_hasAttribute(ARGS) { THIS_NODE(n); ARG_STR(name, 0); return JS_NewBool(c, n->hasAttr(lowerAscii(name))); }
JSValue m_hasAttributes(ARGS) { THIS_NODE(n); return JS_NewBool(c, !n->attrs.empty()); }
JSValue m_getAttributeNames(ARGS) { THIS_NODE(n); JSValue a = JS_NewArray(c); uint32_t i = 0;
    for (auto& kv : n->attrs) { JS_SetPropertyUint32(c, a, i++, jstr(c, kv.first)); }
    return a; }

// Does `el` match `sel`? querySelectorAll never matches its own root, so a detached root is tested via a shallow clone under a scratch parent.
bool matchesSel(State& s, Node* el, const std::string& sel) {
    Node* top = topOf(el);
    if (top != el) { for (Node* m : top->querySelectorAll(sel)) if (m == el) return true; return false; }
    if (!el->isElement()) return false;
    Node* scratch = s.doc->createElement("div");
    Node* copy = el->cloneNode(false);
    if (!scratch || !copy) return false;
    scratch->appendChild(copy);
    bool ok = false;
    for (Node* m : scratch->querySelectorAll(sel)) if (m == copy) ok = true;
    return ok;
}
JSValue m_matches(ARGS) { THIS_NODE(n); ARG_STR(sel, 0); return JS_NewBool(c, matchesSel(*S(c), n, sel)); }
JSValue m_closest(ARGS) { THIS_NODE(n); ARG_STR(sel, 0); State& s = *S(c);
    Node* top = topOf(n);
    std::unordered_set<const Node*> hits;
    for (Node* m : top->querySelectorAll(sel)) hits.insert(m);
    for (Node* x = n; x; x = x->parent) {
        if (!x->isElement()) continue;
        if (hits.count(x) || (x == top && matchesSel(s, x, sel))) return wrapNode(s, x);
    }
    return JS_NULL; }

// Parse html in the context of `ctxEl` into a detached holder whose children are the new nodes.
Node* parseFragment(State& s, const Node* ctxEl, const std::string& html) {
    Node* holder = s.doc->createElement(ctxEl && ctxEl->isElement() ? ctxEl->name : "div");
    if (!holder) holder = s.doc->createElement("div");
    holder->setInnerHTML(html);
    return holder;
}
void moveChildren(Node* holder, Node* parent, Node* ref) {
    while (Node* ch = holder->firstChild()) { if (!parent->insertBefore(ch, ref, nullptr)) break; }
}
JSValue m_insertAdjacentHTML(ARGS) { THIS_NODE(n); State& s = *S(c); ARG_STR(pos, 0); ARG_STR(html, 1);
    pos = lowerAscii(pos);
    if (pos == "afterbegin" || pos == "beforeend") {
        Node* h = parseFragment(s, n, html);
        moveChildren(h, n, pos == "afterbegin" ? n->firstChild() : nullptr);
    } else if (pos == "beforebegin" || pos == "afterend") {
        Node* p = n->parent;
        if (!p || p->type == NodeType::Document) return throwDom(c, "NoModificationAllowedError", "The element has no parent element to insert into.");
        Node* ref = pos == "beforebegin" ? n : n->nextSibling();
        Node* h = parseFragment(s, p, html);
        moveChildren(h, p, ref);
    } else return throwDom(c, "SyntaxError", "The value provided ('" + pos + "') is not one of 'beforebegin', 'afterbegin', 'beforeend', or 'afterend'.");
    return JS_UNDEFINED; }
JSValue s_outerHTML(SET) { THIS_NODE(n); State& s = *S(c); std::string html; if (!toStr(c, v, html)) return JS_EXCEPTION;
    Node* p = n->parent;
    if (!p) return throwDom(c, "NoModificationAllowedError", "This element has no parent node.");
    if (p->type == NodeType::Document) return throwDom(c, "NoModificationAllowedError", "Modifications are not allowed for this document.");
    Node* h = parseFragment(s, p, html);
    moveChildren(h, p, n);
    p->removeChild(n);
    return JS_UNDEFINED; }

// ---- Text / Comment ------------------------------------------------------------------------------------------------------------
JSValue g_length(GET) { THIS_NODE(n); return JS_NewInt64(c, int64_t(n->text.size())); }

// ---- Document ------------------------------------------------------------------------------------------------------------------
JSValue g_documentElement(GET) { THIS_NODE(n); (void)n; return wrapNode(*S(c), S(c)->doc->documentElement()); }
JSValue g_head(GET) { THIS_NODE(n); (void)n; return wrapNode(*S(c), S(c)->doc->head()); }
JSValue g_body(GET) { THIS_NODE(n); (void)n; return wrapNode(*S(c), S(c)->doc->body()); }
JSValue g_title(GET) { THIS_NODE(n); (void)n; return jstr(c, S(c)->doc->title()); }
JSValue s_title(SET) { THIS_NODE(n); (void)n; State& s = *S(c); std::string x; if (!toStr(c, v, x)) return JS_EXCEPTION;
    Node* head = s.doc->head(); if (!head) return JS_UNDEFINED;
    Node* ti = head->querySelector("title");
    if (!ti) { ti = s.doc->createElement("title"); if (!ti) return JS_UNDEFINED; head->appendChild(ti); }
    ti->setTextContent(x); return JS_UNDEFINED; }
JSValue g_readyState(GET) { THIS_NODE(n); (void)n; return jstr(c, S(c)->readyState); }
JSValue g_currentScript(GET) { THIS_NODE(n); (void)n; State& s = *S(c); return wrapNode(s, const_cast<Node*>(s.currentScript)); }
JSValue g_defaultView(GET) { THIS_NODE(n); (void)n; return JS_GetGlobalObject(c); }

JSValue m_createElement(ARGS) { THIS_NODE(n); (void)n; State& s = *S(c); ARG_STR(tag, 0);
    Node* e = s.doc->createElement(lowerAscii(tag));
    if (!e) return throwDom(c, "InvalidCharacterError", "Failed to execute 'createElement' on 'Document': The tag name provided ('" + tag + "') is not a valid name.");
    if (e->name == "script") s.createdScripts.insert(e);
    return wrapNode(s, e); }
JSValue m_createElementNS(ARGS) { THIS_NODE(n); (void)n; State& s = *S(c); ARG_STR(tag, 1);
    Node* e = s.doc->createElement(tag);
    if (!e) e = s.doc->createElement(lowerAscii(tag));
    if (!e) return throwDom(c, "InvalidCharacterError", "The tag name provided ('" + tag + "') is not a valid name.");
    return wrapNode(s, e); }
JSValue m_createTextNode(ARGS) { THIS_NODE(n); (void)n; ARG_STR(x, 0); return wrapNode(*S(c), S(c)->doc->createTextNode(x)); }
JSValue m_createComment(ARGS) { THIS_NODE(n); (void)n; ARG_STR(x, 0); return wrapNode(*S(c), S(c)->doc->createComment(x)); }
JSValue m_getElementById(ARGS) { THIS_NODE(n); (void)n; ARG_STR(id, 0); return wrapNode(*S(c), S(c)->doc->getElementById(id)); }
JSValue m_importNode(ARGS) { THIS_NODE(n); (void)n; State& s = *S(c); Node* src = argc ? nodeArg(&s, argv[0]) : nullptr;
    if (!src) return JS_ThrowTypeError(c, "Failed to execute 'importNode' on 'Document': parameter 1 is not of type 'Node'.");
    bool deep = argc > 1 && JS_ToBool(c, argv[1]) > 0;
    return wrapNode(s, s.doc->importNode(*src, deep)); }

// ---- registration helpers ------------------------------------------------------------------------------------------------------
using Getter = JSValue (*)(JSContext*, JSValueConst);
// QuickJS stores getter/setter/magic functions in one union and calls them by their declared kind; this is the same cast its own macros do.
template <class F> JSCFunction* asCFunc(F f) { return reinterpret_cast<JSCFunction*>(reinterpret_cast<void (*)()>(f)); }
using Setter = JSValue (*)(JSContext*, JSValueConst, JSValueConst);

void method(JSContext* c, JSValueConst obj, const char* name, JSCFunction* fn, int len) {
    JS_DefinePropertyValueStr(c, obj, name, JS_NewCFunction2(c, fn, name, len, JS_CFUNC_generic, 0), JS_PROP_C_W_E);
}
void accessor(JSContext* c, JSValueConst obj, const char* name, Getter g, Setter st = nullptr) {
    JSAtom a = JS_NewAtom(c, name);
    JSValue gf = JS_NewCFunction2(c, asCFunc(g), (std::string("get ") + name).c_str(), 0, JS_CFUNC_getter, 0);
    JSValue sf = st ? JS_NewCFunction2(c, asCFunc(st), (std::string("set ") + name).c_str(), 1, JS_CFUNC_setter, 0) : JS_UNDEFINED;
    JS_DefinePropertyGetSet(c, obj, a, gf, sf, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(c, a);
}

JSValue illegalCtor(JSContext* c, JSValueConst, int, JSValueConst*) { return JS_ThrowTypeError(c, "Illegal constructor"); }

void parentNodeMembers(JSContext* c, JSValueConst p) {
    accessor(c, p, "children", g_children);
    accessor(c, p, "childElementCount", g_childElementCount);
    accessor(c, p, "firstElementChild", g_firstElementChild);
    accessor(c, p, "lastElementChild", g_lastElementChild);
    method(c, p, "querySelector", m_querySelector, 1);
    method(c, p, "querySelectorAll", m_querySelectorAll, 1);
    method(c, p, "getElementsByTagName", m_getElementsByTagName, 1);
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------------------------------

bool isConnected(const State& s, const dom::Node* n) {
    while (n->parent) n = n->parent;
    return n == &s.doc->root();
}

JSValue makeCtor(State& s, const char* name, JSValueConst proto) {
    JSValue ctor = JS_NewCFunction2(s.ctx, illegalCtor, name, 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(s.ctx, ctor, proto);
    return ctor;
}

JSValue wrapNode(State& s, dom::Node* n) {
    if (!n) return JS_NULL;
    auto it = s.wrappers.find(n);
    if (it != s.wrappers.end()) return JS_DupValue(s.ctx, it->second);
    JSValueConst proto = s.protoNode;
    switch (n->type) {
        case NodeType::Text: proto = s.protoText; break;
        case NodeType::Comment: proto = s.protoComment; break;
        case NodeType::Document: proto = s.protoDocument; break;
        case NodeType::Element: {
            auto pc = s.tagProtoCache.find(n->name);
            if (pc == s.tagProtoCache.end()) {
                JSValue p = JS_IsObject(s.tagProtos) ? JS_GetPropertyStr(s.ctx, s.tagProtos, n->name.c_str()) : JS_UNDEFINED;
                if (!JS_IsObject(p)) { JS_FreeValue(s.ctx, p); p = JS_DupValue(s.ctx, JS_IsObject(s.protoHtmlElement) ? s.protoHtmlElement : s.protoElement); }
                pc = s.tagProtoCache.emplace(n->name, p).first;
            }
            proto = pc->second;
            break;
        }
    }
    JSValue o = JS_NewObjectProtoClass(s.ctx, proto, s.nodeClass);
    if (JS_IsException(o)) return o;
    JS_SetOpaque(o, n);
    s.wrappers.emplace(n, JS_DupValue(s.ctx, o));
    return o;
}

bool installDom(State& s) {
    JSContext* c = s.ctx;
    JS_NewClassID(s.rt, &s.nodeClass);
    JSClassDef def = {};
    def.class_name = "Node";                                   // no finalizer: a wrapper never owns its Node
    if (JS_NewClass(s.rt, s.nodeClass, &def) < 0) return false;

    s.protoNode = JS_NewObject(c);
    s.protoElement = JS_NewObjectProto(c, s.protoNode);
    s.protoText = JS_NewObjectProto(c, s.protoNode);
    s.protoComment = JS_NewObjectProto(c, s.protoNode);
    s.protoDocument = JS_NewObjectProto(c, s.protoNode);

    JSValueConst P = s.protoNode;
    accessor(c, P, "nodeType", g_nodeType);       accessor(c, P, "nodeName", g_nodeName);
    accessor(c, P, "nodeValue", g_nodeValue, s_nodeValue);
    accessor(c, P, "parentNode", g_parentNode);   accessor(c, P, "parentElement", g_parentElement);
    accessor(c, P, "childNodes", g_childNodes);   accessor(c, P, "firstChild", g_firstChild);
    accessor(c, P, "lastChild", g_lastChild);     accessor(c, P, "nextSibling", g_nextSibling);
    accessor(c, P, "previousSibling", g_prevSibling);
    accessor(c, P, "ownerDocument", g_ownerDocument);
    accessor(c, P, "isConnected", g_isConnected);
    accessor(c, P, "textContent", g_textContent, s_textContent);
    method(c, P, "appendChild", m_appendChild, 1);   method(c, P, "insertBefore", m_insertBefore, 2);
    method(c, P, "removeChild", m_removeChild, 1);   method(c, P, "replaceChild", m_replaceChild, 2);
    method(c, P, "cloneNode", m_cloneNode, 0);       method(c, P, "contains", m_contains, 1);
    method(c, P, "hasChildNodes", m_hasChildNodes, 0);

    JSValueConst E = s.protoElement;
    parentNodeMembers(c, E);
    accessor(c, E, "tagName", g_tagName);         accessor(c, E, "localName", g_localName);
    accessor(c, E, "id", g_id, s_id);             accessor(c, E, "className", g_className, s_className);
    accessor(c, E, "innerHTML", g_innerHTML, s_innerHTML);
    accessor(c, E, "outerHTML", g_outerHTML, s_outerHTML);
    accessor(c, E, "nextElementSibling", g_nextElem);   accessor(c, E, "previousElementSibling", g_prevElem);
    accessor(c, E, "attributes", g_attributes);
    method(c, E, "getAttribute", m_getAttribute, 1);   method(c, E, "setAttribute", m_setAttribute, 2);
    method(c, E, "removeAttribute", m_removeAttribute, 1);   method(c, E, "hasAttribute", m_hasAttribute, 1);
    method(c, E, "hasAttributes", m_hasAttributes, 0);   method(c, E, "getAttributeNames", m_getAttributeNames, 0);
    method(c, E, "matches", m_matches, 1);   method(c, E, "closest", m_closest, 1);
    method(c, E, "insertAdjacentHTML", m_insertAdjacentHTML, 2);

    for (JSValueConst T : {JSValueConst(s.protoText), JSValueConst(s.protoComment)}) {
        accessor(c, T, "data", g_nodeValue, s_nodeValue);
        accessor(c, T, "length", g_length);
    }

    JSValueConst D = s.protoDocument;
    parentNodeMembers(c, D);
    accessor(c, D, "documentElement", g_documentElement);  accessor(c, D, "head", g_head);  accessor(c, D, "body", g_body);
    accessor(c, D, "title", g_title, s_title);
    accessor(c, D, "readyState", g_readyState);  accessor(c, D, "currentScript", g_currentScript);
    accessor(c, D, "defaultView", g_defaultView);
    method(c, D, "createElement", m_createElement, 1);   method(c, D, "createElementNS", m_createElementNS, 2);
    method(c, D, "createTextNode", m_createTextNode, 1); method(c, D, "createComment", m_createComment, 1);
    method(c, D, "getElementById", m_getElementById, 1); method(c, D, "importNode", m_importNode, 2);

    // Constructors: all "Illegal constructor", like a browser (the prelude publishes them on window).
    for (auto [nm, pr] : {std::pair<const char*, JSValueConst>{"Node", s.protoNode}, {"Element", s.protoElement}, {"Text", s.protoText},
                          {"Comment", s.protoComment}, {"Document", s.protoDocument}})
        JS_FreeValue(c, makeCtor(s, nm, pr));                 // the proto keeps the constructor alive via .constructor
    return true;
}

}  // namespace lucyn::js::detail
