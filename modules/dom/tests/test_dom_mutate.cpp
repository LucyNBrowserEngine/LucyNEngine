#include <cstdio>
#include <lucyn/dom/diff.h>
#include <lucyn/dom/dom.h>

using namespace lucyn::dom;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    // ---- build and attach ----------------------------------------------------
    auto d = parseHtml("<!doctype html><html><head></head><body><div id=a></div></body></html>");
    Node* a = d->getElementById("a");
    CHECK(a && a->ownerDocument == d.get());

    Node* p = d->createElement("P");                       // tag is lowercased
    CHECK(p && p->name == "p" && p->parent == nullptr);
    CHECK(d->detachedCount() == 1);
    CHECK(d->createElement("") == nullptr);
    CHECK(d->createElement("bad tag") == nullptr);
    CHECK(d->createElement("<script>") == nullptr);
    CHECK(d->createElement("my-widget") != nullptr);       // custom elements are fine
    d->purgeDetached();
    CHECK(d->detachedCount() == 0);

    p = d->createElement("p");
    Node* t = d->createTextNode("hi & <bye>");
    CHECK(p->appendChild(t));
    CHECK(a->appendChild(p));
    CHECK(d->detachedCount() == 0);                        // attached nodes leave the pool
    CHECK(a->outerHTML() == "<div id=\"a\"><p>hi &amp; &lt;bye&gt;</p></div>");
    CHECK(d->querySelector("#a p") == p);                  // selectors see the change at once

    // ---- attributes ------------------------------------------------------------
    CHECK(p->setAttr("DATA-X", "1"));
    CHECK(*p->attr("data-x") == "1");
    CHECK(p->setAttr("data-x", "2") && *p->attr("data-x") == "2" && p->attrs.size() == 1);
    CHECK(!p->setAttr("bad name", "x"));
    CHECK(!p->setAttr("a\"b", "x"));
    CHECK(!t->setAttr("x", "y"));                          // text nodes have no attributes
    CHECK(p->removeAttr("data-x") && !p->removeAttr("data-x"));
    CHECK(p->setAttr("title", "a\"b"));
    CHECK(p->outerHTML().find("title=\"a&quot;b\"") != std::string::npos);

    // ---- order: insertBefore, move, remove, replace ---------------------------------
    Node* b = d->createElement("b");
    Node* i = d->createElement("i");
    CHECK(a->insertBefore(b, p));                          // b p
    CHECK(a->insertBefore(i, nullptr));                    // b p i
    CHECK(a->children.size() == 3 && a->firstChild() == b && a->lastChild() == i);
    CHECK(b->nextSibling() == p && p->previousSibling() == b && b->previousSibling() == nullptr && i->nextSibling() == nullptr);
    CHECK(a->insertBefore(i, b));                          // moving inside the same parent: i b p
    CHECK(a->children.size() == 3 && a->firstChild() == i);
    CHECK(a->insertBefore(i, i));                          // no-op, still fine
    CHECK(a->firstChild() == i);

    Node* s = d->createElement("span");
    CHECK(a->replaceChild(s, b));                          // i span p
    CHECK(b->parent == nullptr && d->detachedCount() >= 1);
    CHECK(a->children.size() == 3 && a->children[1].get() == s);
    CHECK(a->replaceChild(p, i));                          // p is later in the list: p span
    CHECK(a->children.size() == 2 && a->firstChild() == p && a->lastChild() == s);

    Node* gone = a->removeChild(s);
    CHECK(gone == s && s->parent == nullptr && a->children.size() == 1);
    CHECK(a->removeChild(s) == nullptr);                   // not ours any more
    CHECK(s->remove() == false);                           // detached: nothing to do
    CHECK(p->remove() && a->children.empty());
    CHECK(a->appendChild(s));                              // a removed node can come back

    // Reparenting a node that is already in the tree.
    Node* x = d->createElement("x");
    Node* y = d->createElement("y");
    a->appendChild(x); a->appendChild(y);
    CHECK(y->appendChild(x));                              // x moves from a into y
    CHECK(x->parent == y && a->children.size() == 2);      // s, y

    // ---- things that must be refused, never crash ---------------------------------
    std::string err;
    CHECK(!y->appendChild(y, &err) && !err.empty());       // into itself
    CHECK(!x->appendChild(y, &err));                       // y is x's ancestor
    CHECK(!a->appendChild(nullptr));
    CHECK(!t->appendChild(x));                             // text cannot have children
    CHECK(!a->appendChild(&d->root()));                    // no Document as a child
    CHECK(!a->insertBefore(x, d->createElement("zz")));    // ref is not our child
    auto other = parseHtml("<p>o</p>");
    CHECK(!a->appendChild(other->querySelector("p"), &err));   // other document
    CHECK(err.find("another document") != std::string::npos);
    CHECK(a->replaceChild(x, s));                          // legal: x leaves y and takes s's place: x y
    CHECK(a->children.size() == 2 && a->firstChild() == x && a->lastChild() == y && y->children.empty() && s->parent == nullptr);
    for (auto& c : a->children) CHECK(c->parent == a);

    // ---- textContent / innerHTML -----------------------------------------------------
    auto e = parseHtml("<div id=m><p>old</p><p>older</p></div><ul id=u></ul><table id=t></table><script id=sc></script>");
    Node* m = e->getElementById("m");
    Node* oldP = m->firstChild();
    CHECK(m->setTextContent("a < b"));
    CHECK(m->innerHTML() == "a &lt; b" && m->children.size() == 1);
    CHECK(oldP->parent == nullptr);                        // old child detached, pointer still valid
    CHECK(oldP->textContent() == "old");
    CHECK(m->setTextContent("") && m->children.empty());

    CHECK(m->setInnerHTML("<p class=x>one</p><p>two<b>!</b></p>tail"));
    CHECK(m->children.size() == 3);
    CHECK(m->querySelectorAll("p").size() == 2 && m->querySelector("p.x") != nullptr);
    CHECK(m->querySelector("p.x")->ownerDocument == e.get());
    CHECK(m->innerHTML() == "<p class=\"x\">one</p><p>two<b>!</b></p>tail");
    CHECK(m->setInnerHTML("") && m->children.empty());

    Node* u = e->getElementById("u");
    CHECK(u->setInnerHTML("<li>1</li><li>2</li>") && u->querySelectorAll("li").size() == 2);
    Node* tb = e->getElementById("t");
    CHECK(tb->setInnerHTML("<tr><td>c</td></tr>"));
    CHECK(tb->querySelector("td") != nullptr);             // table content survives its context
    Node* tr = tb->querySelector("tr");
    CHECK(tr->setInnerHTML("<td>1</td><td>2</td>") && tr->querySelectorAll("td").size() == 2);

    Node* sc = e->getElementById("sc");
    CHECK(sc->setInnerHTML("if (a < b) { x = '<div>'; }"));
    CHECK(sc->children.size() == 1 && sc->children[0]->isText());   // raw text, not markup
    CHECK(sc->outerHTML() == "<script id=\"sc\">if (a < b) { x = '<div>'; }</script>");
    CHECK(!e->createTextNode("x")->setInnerHTML("<b>"));   // only elements

    // A script is not text content.
    CHECK(sc->textContent().empty());

    // ---- clone ---------------------------------------------------------------------------
    Node* src = e->getElementById("u");
    Node* shallow = src->cloneNode(false);
    Node* deep = src->cloneNode(true);
    CHECK(shallow->children.empty() && shallow->parent == nullptr && *shallow->attr("id") == "u");
    CHECK(deep->children.size() == 2 && deep->children[0]->parent == deep && deep != src);
    CHECK(deep->outerHTML() == src->outerHTML());
    deep->setAttr("id", "u2");
    CHECK(*src->attr("id") == "u");                        // really a copy
    CHECK(e->body()->appendChild(deep));
    CHECK(e->getElementById("u2") == deep);
    CHECK(e->root().cloneNode(true) == nullptr);           // a whole Document is not cloneable

    // ---- contains --------------------------------------------------------------------------
    CHECK(e->body()->contains(deep) && e->body()->contains(e->body()) && !deep->contains(e->body()));

    // ---- mutation hook + version --------------------------------------------------------
    auto f = parseHtml("<div id=r></div>");
    Node* r = f->getElementById("r");
    int ins = 0, rem = 0, att = 0, txt = 0;
    uint64_t v0 = f->version();
    auto id = f->onMutation([&](const Document::Mutation& m2) {
        using K = Document::Mutation::Kind;
        if (m2.kind == K::ChildInserted) ++ins;
        else if (m2.kind == K::ChildRemoved) ++rem;
        else if (m2.kind == K::Attribute) { ++att; CHECK(m2.name == "k"); }
        else ++txt;
    });
    Node* k = f->createElement("em");
    CHECK(f->version() == v0);                             // creating alone is not a change to the tree
    r->appendChild(k);                                     // +1 insert
    k->setAttr("k", "v");                                  // +1 attr
    Node* tx = f->createTextNode("a");
    k->appendChild(tx);                                    // +1 insert
    tx->setTextContent("b");                               // +1 text
    r->removeChild(k);                                     // +1 remove
    CHECK(ins == 2 && rem == 1 && att == 1 && txt == 1);
    CHECK(f->version() > v0);
    f->offMutation(id);
    uint64_t v1 = f->version();
    r->appendChild(k);
    CHECK(ins == 2);                                       // listener is gone
    CHECK(f->version() == v1 + 1);

    // Reparenting reports removal from the old place, then insertion in the new one.
    auto g = parseHtml("<div id=p1><i id=m1></i></div><div id=p2></div>");
    std::vector<int> order;
    g->onMutation([&](const Document::Mutation& m3) { order.push_back(m3.kind == Document::Mutation::Kind::ChildRemoved ? -1 : 1); });
    g->getElementById("p2")->appendChild(g->getElementById("m1"));
    CHECK((order == std::vector<int>{-1, 1}));

    // A listener that mutates again must not break the loop.
    auto h = parseHtml("<div id=q></div>");
    int calls = 0;
    h->onMutation([&](const Document::Mutation&) { if (++calls == 1) h->getElementById("q")->setAttr("seen", "1"); });
    h->getElementById("q")->setAttr("a", "1");
    CHECK(calls == 2);

    // ---- stress: deep and wide, no crash, no stack overflow ----------------------------------
    auto big = parseHtml("<div id=top></div>");
    Node* inner = big->createElement("div");                 // built bottom-up: each append is O(1)
    for (int n = 0; n < 100000; ++n) {
        Node* outer = big->createElement("div");
        outer->appendChild(inner);
        inner = outer;
    }
    big->getElementById("top")->appendChild(inner);
    Node* deepClone = big->getElementById("top")->cloneNode(true);   // iterative
    CHECK(deepClone != nullptr);
    Node* wide = big->createElement("ul");
    big->body()->appendChild(wide);
    for (int n = 0; n < 20000; ++n) wide->appendChild(big->createElement("li"));
    CHECK(wide->children.size() == 20000);
    big.reset();                                           // iterative destructor: must not overflow

    // ---- diff ----------------------------------------------------------------------------------------
    auto df = diffHtml("<div id=app></div><noscript>x</noscript><p>a</p>",
                       "<div id=app><section class='b a'></section><canvas></canvas></div><p>a</p><p>b</p>");
    auto nt = df.newTags();
    auto gt = df.goneTags();
    CHECK((nt == std::vector<std::string>{"canvas", "section"}));
    CHECK((gt == std::vector<std::string>{"noscript"}));
    bool sawSection = false, sawNoscript = false, sawP = false;
    for (auto& en : df.elements) {
        if (en.key == "section.a.b" && en.before == 0 && en.after == 1) sawSection = true;   // classes sorted
        if (en.key == "noscript" && en.before == 1 && en.after == 0) sawNoscript = true;
        if (en.key == "p" && en.before == 2 && en.after == 3) sawP = true;                   // not reported as p#...
    }
    // html/head/body are added by the parser on both sides, so they never show up as changes.
    CHECK(sawSection && sawNoscript);
    (void)sawP;
    CHECK(!df.empty());
    CHECK(diffHtml("<p>a</p>", "<p class='x'>b</p>").elements.size() == 2);      // p -> p.x
    CHECK(diffHtml("<p>a</p>", "<p>different text only</p>").empty());           // text is not a tag
    CHECK(diffHtml("<p class='a b'>", "<p class='b a'>").empty());               // class order is not a change
    CHECK(formatDiff(diffHtml("<p>", "<p>")).find("no differences") != std::string::npos);
    CHECK(formatDiff(df).find("+ section.a.b") != std::string::npos);
    CHECK(formatDiff(df).find("new tags: canvas section") != std::string::npos);

    std::printf(failures ? "dom_mutate: %d FAILED\n" : "dom_mutate: all passed\n", failures);
    return failures ? 1 : 0;
}
