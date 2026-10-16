#include <lucyn/layout/litehtml_layout.h>
#include "litehtml_container.h"

#include <litehtml/document.h>

namespace lucyn::layout {

// ---- pimpl (hides litehtml::document shared_ptr) ---------------------------

struct LitehtmlRenderer::Impl {
    std::shared_ptr<litehtml::document> doc;
};

// ---- ctor / dtor -----------------------------------------------------------

LitehtmlRenderer::LitehtmlRenderer(int viewportWidth, int viewportHeight)
    : vp_w_(viewportWidth), vp_h_(viewportHeight),
      container_(std::make_unique<LucynLitehtmlContainer>()),
      impl_(std::make_unique<Impl>()) {}

LitehtmlRenderer::~LitehtmlRenderer() = default;

// ---- loadHtml --------------------------------------------------------------

void LitehtmlRenderer::loadHtml(const std::string& html, const std::string& base_url) {
    container_ = std::make_unique<LucynLitehtmlContainer>(base_url);
    impl_->doc = litehtml::document::createFromString(html.c_str(), container_.get());
}

// ---- render ----------------------------------------------------------------

void LitehtmlRenderer::render() {
    if (!impl_->doc) return;

    // Clear previous draw list (keep image table — images are already loaded).
    container_->displayList().clear();

    // Layout pass.
    impl_->doc->render(vp_w_);
    doc_height_ = static_cast<float>(impl_->doc->height());

    // Draw pass — litehtml calls back into our container_ draw methods.
    litehtml::position clip{0, 0, vp_w_, vp_h_};
    impl_->doc->draw(reinterpret_cast<litehtml::uint_ptr>(container_.get()), 0, 0, &clip);
}

// ---- accessors -------------------------------------------------------------

const lucyn::paint::DisplayList& LitehtmlRenderer::displayList() const {
    return container_->displayList();
}

lucyn::paint::ImageTable& LitehtmlRenderer::imageTable() {
    return container_->imageTable();
}

void LitehtmlRenderer::resize(int w, int h) {
    vp_w_ = w; vp_h_ = h;
    render();
}

}  // namespace lucyn::layout
