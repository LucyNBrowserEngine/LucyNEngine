// lucyn-paint - tiny CLI to poke modules/paint (and, once LUCYN_MODULE_LAYOUT
// is on, modules/layout too). SESSION11_PLAN.md Part B: "lucyn-paint --demo
// out.png draws a hard-coded display list. Easiest first proof the painter
// works."
//
//   lucyn-paint --demo out.png                 draw a hard-coded DisplayList, no layout/DOM
//   lucyn-paint --file page.html out.png        (Part C, only when LUCYN_PAINT_HAVE_LAYOUT_DOM)
//                                                parse page.html -> dom -> layout -> paint
#include <lucyn/paint/displaylist.h>
#include <lucyn/paint/framebuffer.h>
#include <lucyn/paint/software_painter.h>

#ifdef LUCYN_PAINT_HAVE_LAYOUT_DOM
#include <lucyn/dom/dom.h>
#include <lucyn/layout/layout.h>
#endif

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace lucyn::paint;

namespace {

DisplayList demoList() {
    DisplayList list;
    // A page-shaped sanity check: a header bar, a bordered card, and some text.
    list.push_back(DrawRect{0, 0, 800, 600, Color::white(), true, 0, false, {}});
    list.push_back(DrawRect{0, 0, 800, 60, Color::rgba(30, 30, 40), true, 0, false, {}});
    list.push_back(DrawText{20, 38, "LucyN Engine - Session 11 painter demo", 22, Color::white(), false, {}});

    list.push_back(DrawBorder{40, 100, 400, 200, 2, Color::grey(), false, {}});
    list.push_back(DrawText{60, 140, "layout never draws.", 18, Color::black(), false, {}});
    list.push_back(DrawText{60, 170, "It makes a display list,", 16, Color::black(), false, {}});
    list.push_back(DrawText{60, 195, "and exactly one backend", 16, Color::black(), false, {}});
    list.push_back(DrawText{60, 220, "turns that into pixels.", 16, Color::black(), false, {}});

    // A clipped rect: only the part inside the clip rectangle should show.
    DrawRect clipped{480, 100, 300, 300, Color::rgba(200, 60, 60), true, 0, true, ClipRect{480, 100, 150, 150}};
    list.push_back(clipped);
    list.push_back(DrawText{480, 420, "overflow: hidden works (red square is clipped)", 14, Color::black(), false, {}});

    return list;
}

int runDemo(const std::string& outPath) {
    Framebuffer fb;
    fb.init(800, 600, 0xFFFFFFFFu);
    SoftwarePainter painter(fb);
    painter.draw(demoList());
    if (!fb.savePNG(outPath)) {
        std::cerr << "lucyn-paint: failed to write " << outPath << "\n";
        return 1;
    }
    std::cout << outPath << "\n";
    return 0;
}

#ifdef LUCYN_PAINT_HAVE_LAYOUT_DOM
int runFile(const std::string& htmlPath, const std::string& outPath) {
    std::ifstream in(htmlPath, std::ios::binary);
    if (!in) {
        std::cerr << "lucyn-paint: cannot read " << htmlPath << "\n";
        return 1;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string html = ss.str();

    auto doc = lucyn::dom::parseHtml(html);
    if (!doc) {
        std::cerr << "lucyn-paint: failed to parse " << htmlPath << "\n";
        return 1;
    }

    lucyn::layout::LayoutOptions opts;
    opts.viewportWidth = 800;
    lucyn::layout::Box root = lucyn::layout::layoutDocument(*doc, opts);
    DisplayList list = lucyn::layout::paint(root);

    Framebuffer fb;
    int height = static_cast<int>(root.height) > 0 ? static_cast<int>(root.height) + 40 : 600;
    fb.init(opts.viewportWidth, height, 0xFFFFFFFFu);
    SoftwarePainter painter(fb);
    painter.draw(list);
    if (!fb.savePNG(outPath)) {
        std::cerr << "lucyn-paint: failed to write " << outPath << "\n";
        return 1;
    }
    std::cout << outPath << "\n";
    return 0;
}
#endif

void usage() {
    std::cerr << "usage: lucyn-paint --demo out.png\n";
#ifdef LUCYN_PAINT_HAVE_LAYOUT_DOM
    std::cerr << "       lucyn-paint --file page.html out.png\n";
#endif
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    std::string mode = argv[1];
    if (mode == "--demo" && argc >= 3) {
        return runDemo(argv[2]);
    }
#ifdef LUCYN_PAINT_HAVE_LAYOUT_DOM
    if (mode == "--file" && argc >= 4) {
        return runFile(argv[2], argv[3]);
    }
#endif
    usage();
    return 1;
}
