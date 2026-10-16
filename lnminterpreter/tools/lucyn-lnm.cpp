// lucyn-lnm: poke at LNM from a terminal.
//   lucyn-lnm ast    app.lnm                      parse and print the AST (+ diagnostics)
//   lucyn-lnm check  app.lnm                      parse, load every grabbed file, report problems; exit 1 on any error
//   lucyn-lnm render app.lnm out.png [--width N] [--height N] [--do "<selector>" <event>]...
//        loads the app, fires each --do event in order (e.g. --do "#settings-btn" onclick), then
//        lays the result out and paints it to a PNG (no SDL, no display needed).
#include <lucyn/lnm/app.h>
#include <lucyn/paint/framebuffer.h>
#include <lucyn/paint/software_painter.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace lucyn::lnm;

static bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream o;
    o << f.rdbuf();
    out = o.str();
    return true;
}
static std::string dirOf(const std::string& p) {
    auto s = p.find_last_of('/');
    return s == std::string::npos ? "." : p.substr(0, s);
}
static int usage() {
    std::fprintf(stderr, "usage: lucyn-lnm ast|check <file.lnm>\n       lucyn-lnm render <file.lnm> <out.png> [--width N] [--height N] [--do <selector> <event>]...\n");
    return 2;
}
static void printDiags(const std::vector<Diagnostic>& ds, const std::string& file) {
    for (const auto& d : ds) std::fprintf(stderr, "%s\n", formatDiagnostic(d, file).c_str());
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1], file = argv[2];
    std::string src;
    if (!readFile(file, src)) {
        std::fprintf(stderr, "lucyn-lnm: cannot read %s\n", file.c_str());
        return 2;
    }

    if (cmd == "ast") {
        ParseResult r = parse(src);
        std::fputs(dump(r.program).c_str(), stdout);
        printDiags(r.diagnostics, file);
        return r.ok() ? 0 : 1;
    }

    App app(diskReader(dirOf(file)));
    if (cmd == "check") {
        bool ok = app.load(src);
        printDiags(app.diagnostics(), file);
        int errs = 0;
        for (auto& d : app.diagnostics()) errs += d.severity == Diagnostic::Severity::Error;
        std::printf("%s: %s (%zu rules, %zu grabs, %d error(s))\n", file.c_str(), ok && !errs ? "ok" : "FAILED",
                    app.program().rules.size(), app.program().grabs.size(), errs);
        return ok && !errs ? 0 : 1;
    }

    if (cmd == "render") {
        if (argc < 4) return usage();
        const std::string out = argv[3];
        ViewOptions vo;
        std::vector<std::pair<std::string, std::string>> actions;
        for (int i = 4; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--width" && i + 1 < argc) vo.viewportWidth = static_cast<float>(std::atof(argv[++i]));
            else if (a == "--height" && i + 1 < argc) vo.viewportHeight = static_cast<float>(std::atof(argv[++i]));
            else if (a == "--do" && i + 2 < argc) { actions.emplace_back(argv[i + 1], argv[i + 2]); i += 2; }
            else return usage();
        }
        app.setViewport(vo);
        if (!app.load(src)) {
            printDiags(app.diagnostics(), file);
            return 1;
        }
        for (const auto& a : actions) {
            lucyn::dom::Node* el = app.document()->querySelector(a.first);
            if (!el) { std::fprintf(stderr, "--do: no element matches %s\n", a.first.c_str()); return 1; }
            int n = app.trigger(el, a.second);
            std::fprintf(stderr, "%s %s -> %d rule(s) fired\n", a.first.c_str(), a.second.c_str(), n);
        }
        printDiags(app.diagnostics(), file);
        lucyn::layout::Box root = app.layoutView();
        auto list = lucyn::layout::paint(root);
        int w = static_cast<int>(vo.viewportWidth);
        int h = std::max(static_cast<int>(vo.viewportHeight), static_cast<int>(root.height) + 20);
        lucyn::paint::Framebuffer fb;
        fb.init(w, h, lucyn::paint::packRGBA(255, 255, 255, 255));
        lucyn::paint::SoftwarePainter painter(fb);
        painter.draw(list);
        if (!fb.savePNG(out)) {
            std::fprintf(stderr, "lucyn-lnm: cannot write %s\n", out.c_str());
            return 1;
        }
        std::printf("wrote %s (%dx%d)\n", out.c_str(), w, h);
        return 0;
    }
    return usage();
}
