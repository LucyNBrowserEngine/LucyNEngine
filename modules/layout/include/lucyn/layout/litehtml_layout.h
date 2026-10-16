#pragma once
// lucyn/layout/litehtml_layout.h
// Renders an HTML page via litehtml → lucyn::paint::DisplayList.
// Drop-in alternative to layoutDocument() + paint() for pages with complex CSS,
// background colours, gradients, and images (bug fixes for the "black text on
// white, no colour, no images" problem documented in SESSION16/17).
//
// Usage:
//   LitehtmlRenderer r(viewportW, viewportH);
//   r.loadHtml(html_string, base_url);   // parses + styles
//   r.render();                           // layout pass
//   auto& list  = r.displayList();        // draw commands
//   auto& imgs  = r.imageTable();         // resolved images
//   float docH  = r.documentHeight();    // full page height
#include <lucyn/paint/displaylist.h>
#include <lucyn/paint/software_painter.h>  // ImageTable

#include <memory>
#include <string>

namespace lucyn::layout {

class LucynLitehtmlContainer;  // forward

class LitehtmlRenderer {
public:
    explicit LitehtmlRenderer(int viewportWidth = 800, int viewportHeight = 600);
    ~LitehtmlRenderer();

    // (Re)load HTML. base_url is used to resolve relative CSS/image paths.
    void loadHtml(const std::string& html, const std::string& base_url = "");

    // Run/re-run the layout pass (call after loadHtml, or after a resize).
    void render();

    // Results — valid after render().
    const lucyn::paint::DisplayList& displayList() const;
    lucyn::paint::ImageTable&        imageTable();
    float                            documentHeight() const { return doc_height_; }

    // Resize the viewport and re-render.
    void resize(int w, int h);

private:
    int vp_w_, vp_h_;
    float doc_height_ = 0.f;

    std::unique_ptr<LucynLitehtmlContainer> container_;

    struct Impl;   // pimpl hides litehtml::document shared_ptr from the header
    std::unique_ptr<Impl> impl_;
};

}  // namespace lucyn::layout
