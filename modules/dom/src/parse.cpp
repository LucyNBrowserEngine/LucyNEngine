#include <lucyn/dom/dom.h>
#include "internal.h"
#include <gumbo.h>
#include <cctype>

namespace lucyn::dom {

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string tagName(const GumboElement& e) {
    if (e.tag != GUMBO_TAG_UNKNOWN) return gumbo_normalized_tagname(e.tag);
    GumboStringPiece sp = e.original_tag;          // e.g. "<my-widget"
    gumbo_tag_from_original_text(&sp);             // trims to the name
    return lower(std::string(sp.data, sp.length));
}

static void copyChildren(const GumboVector& kids, Node& into) {
    for (unsigned i = 0; i < kids.length; ++i) {
        auto* g = static_cast<GumboNode*>(kids.data[i]);
        auto n = std::make_unique<Node>();
        n->parent = &into;
        n->ownerDocument = into.ownerDocument;
        switch (g->type) {
            case GUMBO_NODE_ELEMENT:
            case GUMBO_NODE_TEMPLATE: {
                n->type = NodeType::Element;
                n->name = tagName(g->v.element);
                const GumboVector& at = g->v.element.attributes;
                for (unsigned a = 0; a < at.length; ++a) {
                    auto* ga = static_cast<GumboAttribute*>(at.data[a]);
                    n->attrs.emplace_back(ga->name, ga->value);
                }
                copyChildren(g->v.element.children, *n);
                break;
            }
            case GUMBO_NODE_TEXT:
            case GUMBO_NODE_CDATA:
            case GUMBO_NODE_WHITESPACE:
                n->type = NodeType::Text;
                n->text = g->v.text.text;
                break;
            case GUMBO_NODE_COMMENT:
                n->type = NodeType::Comment;
                n->text = g->v.text.text;
                break;
            default: continue;   // document nodes never appear as children
        }
        into.children.push_back(std::move(n));
    }
}

std::unique_ptr<Document> parseHtml(std::string_view html) {
    GumboOutput* out = gumbo_parse_with_options(&kGumboDefaultOptions, html.data(), html.size());
    auto doc = std::unique_ptr<Document>(new Document());
    doc->root_ = std::make_unique<Node>();
    doc->root_->type = NodeType::Document;
    doc->root_->ownerDocument = doc.get();
    copyChildren(out->document->v.document.children, *doc->root_);
    gumbo_destroy_output(&kGumboDefaultOptions, out);
    return doc;
}

// ---- fragments (innerHTML) ---------------------------------------------------
// Gumbo's fragment API is not in every packaged version, so we parse the HTML
// inside a tiny wrapper document that puts the parser in the right mode, then
// steal the children of the wrapper element.
std::vector<std::unique_ptr<Node>> parseFragment(const std::string& ctx, std::string_view html) {
    std::string prefix = "<!doctype html><html><body>";
    std::vector<std::string> path = {"html", "body"};
    if (ctx == "table") { prefix += "<table>"; path.push_back("table"); }
    else if (ctx == "thead" || ctx == "tbody" || ctx == "tfoot" || ctx == "colgroup") {
        prefix += "<table><" + ctx + ">"; path.push_back("table"); path.push_back(ctx);
    } else if (ctx == "tr") { prefix += "<table><tbody><tr>"; path = {"html", "body", "table", "tbody", "tr"}; }
    else if (ctx == "select") { prefix += "<select>"; path.push_back("select"); }
    else if (ctx == "optgroup") { prefix += "<select><optgroup>"; path.push_back("select"); path.push_back("optgroup"); }
    else if (ctx == "head") { prefix = "<!doctype html><html><head>"; path = {"html", "head"}; }

    auto tmp = parseHtml(prefix + std::string(html));
    Node* cur = &tmp->root();
    for (auto& step : path) {
        Node* next = nullptr;
        for (auto& c : cur->children) if (c->isElement() && c->name == step) { next = c.get(); break; }
        if (!next) return {};
        cur = next;
    }
    std::vector<std::unique_ptr<Node>> out = std::move(cur->children);
    cur->children.clear();
    for (auto& n : out) n->parent = nullptr;          // still owned by `tmp`'s Document tag; mutate.cpp re-tags
    return out;
}

}  // namespace lucyn::dom
