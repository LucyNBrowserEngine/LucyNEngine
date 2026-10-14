// lucyn-dom: parse HTML and poke at the tree.
//   lucyn-dom [--title|--tree|--text|--html] [-q selector] [-a attr] <file|->
//   lucyn-dom --diff <before.html> <after.html>      (new/gone tags and elements)
//   lucyn-dom ... --url <url> [-i target]     (only when built with the net module)
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <lucyn/dom/diff.h>
#include <lucyn/dom/dom.h>
#ifdef LUCYN_DOM_HAVE_NET
#include <lucyn/net/http_client.h>
#endif

using namespace lucyn;

static void usage() {
    std::puts("usage: lucyn-dom [--title|--tree|--text|--html] [-q selector] [-a attr] <file|->\n"
              "       lucyn-dom --diff <before.html> <after.html>\n"
#ifdef LUCYN_DOM_HAVE_NET
              "       lucyn-dom [...] --url <url> [-i target]\n"
#endif
              "default: print title + node count. -q lists matches (outerHTML, or -a attribute value).");
}

static bool slurp(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--diff") {
        std::string a, b;
        if (!slurp(argv[2], a)) { std::fprintf(stderr, "cannot open %s\n", argv[2]); return 2; }
        if (!slurp(argv[3], b)) { std::fprintf(stderr, "cannot open %s\n", argv[3]); return 2; }
        std::fputs(dom::formatDiff(dom::diffHtml(a, b)).c_str(), stdout);
        return 0;
    }
    std::string mode = "summary", sel, attr, src, url, target = "chrome";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--title" || a == "--tree" || a == "--text" || a == "--html") mode = a.substr(2);
        else if (a == "-q" && i + 1 < argc) sel = argv[++i];
        else if (a == "-a" && i + 1 < argc) attr = argv[++i];
        else if (a == "--url" && i + 1 < argc) url = argv[++i];
        else if (a == "-i" && i + 1 < argc) target = argv[++i];
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else src = a;
    }
    std::string html;
    if (!url.empty()) {
#ifdef LUCYN_DOM_HAVE_NET
        profile::Switcher sw(target);
        net::HttpClient client(sw);
        auto r = client.get(url);
        if (!r.ok()) { std::fprintf(stderr, "fetch failed: %s\n", r.error.c_str()); return 2; }
        html = std::move(r.body);
#else
        std::fputs("built without the net module: --url unavailable\n", stderr); return 2;
#endif
    } else if (src.empty()) { usage(); return 1; }
    else if (src == "-") { html.assign(std::istreambuf_iterator<char>(std::cin), {}); }
    else {
        std::ifstream f(src, std::ios::binary);
        if (!f) { std::fprintf(stderr, "cannot open %s\n", src.c_str()); return 2; }
        html.assign(std::istreambuf_iterator<char>(f), {});
    }

    auto doc = dom::parseHtml(html);
    if (!sel.empty()) {
        for (auto* n : doc->querySelectorAll(sel)) {
            if (!attr.empty()) { if (auto* v = n->attr(attr)) std::printf("%s\n", v->c_str()); }
            else std::printf("%s\n", n->outerHTML().c_str());
        }
        return 0;
    }
    if (mode == "title") std::printf("%s\n", doc->title().c_str());
    else if (mode == "tree") std::fputs(dom::dumpTree(doc->root()).c_str(), stdout);
    else if (mode == "text") std::printf("%s\n", (doc->body() ? doc->body()->textContent() : doc->root().textContent()).c_str());
    else if (mode == "html") std::printf("%s\n", doc->root().outerHTML().c_str());
    else std::printf("title: %s\nnodes: %zu elements\n", doc->title().c_str(), doc->getElementsByTagName("*").size());
    return 0;
}
