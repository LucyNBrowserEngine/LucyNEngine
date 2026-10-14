#include <cstdio>
#include <lucyn/dom/dom.h>

using namespace lucyn::dom;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    // Tag soup still gets a full html/head/body skeleton.
    auto d = parseHtml("<title>  Hello \n  World </title><p>one<p>two");
    CHECK(d->documentElement() && d->head() && d->body());
    CHECK(d->title() == "Hello World");
    CHECK(d->getElementsByTagName("p").size() == 2);       // implied </p>

    auto g = parseHtml(R"(<!doctype html><html><body>
      <div id="main" class="a b"><ul><li class="x"><a href="/1">one</a></li><li><a href="/2" data-k="v">two</a></li></ul></div>
      <a href="/outside">out</a><my-widget foo=bar>w</my-widget>
      <script>if (a < b && c) {}</script><style>p{}</style></body></html>)");
    CHECK(g->getElementById("main") != nullptr);
    CHECK(g->getElementById("nope") == nullptr);
    CHECK(g->getElementById("main")->hasClass("b"));
    CHECK(g->querySelectorAll("a").size() == 3);
    CHECK(g->querySelectorAll("#main a").size() == 2);        // descendant
    CHECK(g->querySelectorAll("div > a").size() == 0);        // child, not descendant
    CHECK(g->querySelectorAll("ul > li > a").size() == 2);
    CHECK(g->querySelectorAll("li.x a").size() == 1);
    CHECK(g->querySelectorAll("a[data-k=v]").size() == 1);
    CHECK(g->querySelectorAll("a[href]").size() == 3);
    CHECK(g->querySelectorAll("li.x, #main").size() == 2);    // comma list
    CHECK(g->querySelectorAll("[[bad").empty());              // invalid -> empty, no crash
    CHECK(*g->querySelector("li.x a")->attr("href") == "/1");
    CHECK(g->querySelector("my-widget") != nullptr);          // unknown tags survive, lowercased

    // textContent skips script/style; serializer escapes text but not script.
    CHECK(g->body()->textContent().find("a < b") == std::string::npos);
    CHECK(g->querySelector("script")->outerHTML() == "<script>if (a < b && c) {}</script>");
    auto e = parseHtml("<p title='a\"b'>x &amp; y &lt; z</p>");
    CHECK(e->querySelector("p")->outerHTML() == "<p title=\"a&quot;b\">x &amp; y &lt; z</p>");
    CHECK(e->querySelector("p")->textContent() == "x & y < z");

    // Round trip: parse(serialize(tree)) gives the same serialization.
    std::string once = g->root().outerHTML();
    CHECK(parseHtml(once)->root().outerHTML() == once);

    // Garbage in, no crash out.
    parseHtml("");
    parseHtml("<<<<>>>></div></div><a href=");
    std::string deep; for (int i = 0; i < 5000; ++i) deep += "<div>";
    parseHtml(deep);

    std::printf(failures ? "dom: %d FAILED\n" : "dom: all passed\n", failures);
    return failures ? 1 : 0;
}
