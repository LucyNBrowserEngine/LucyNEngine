#pragma once
// litehtml_container.h — LucyN's implementation of litehtml::document_container.
// Bridges litehtml's draw callbacks → lucyn::paint::DisplayList + ImageTable.
// Also handles synchronous image loading from disk (file:// or pre-fetched paths).
#include <litehtml/document_container.h>
#include <lucyn/paint/displaylist.h>
#include <lucyn/paint/software_painter.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace lucyn::layout {

// A font "handle" is just an index into our font_sizes_ table.
// We only have one typeface (PTSans), so we track size, ascent, descent.
struct FontSlot {
    float size    = 16.f;
    float ascent  = 0.f;
    float descent = 0.f;
    float height  = 0.f;
    float x_height = 0.f;
};

class LucynLitehtmlContainer : public litehtml::document_container {
public:
    // `base_url` is the directory of the page (used to resolve relative image paths).
    explicit LucynLitehtmlContainer(const std::string& base_url = "");

    // After rendering call these to collect results.
    lucyn::paint::DisplayList&       displayList()       { return list_; }
    const lucyn::paint::DisplayList& displayList() const { return list_; }
    lucyn::paint::ImageTable&        imageTable()        { return images_; }

    // Clip stack management (used by set_clip / del_clip)
    bool     hasClip() const { return !clip_stack_.empty(); }
    litehtml::position currentClip() const;

    // ---- document_container pure virtuals ---------------------------------
    litehtml::uint_ptr create_font(const litehtml::font_description& descr,
                                   const litehtml::document* doc,
                                   litehtml::font_metrics* fm) override;
    void               delete_font(litehtml::uint_ptr hFont) override;
    litehtml::pixel_t  text_width(const char* text, litehtml::uint_ptr hFont) override;
    void               draw_text(litehtml::uint_ptr hdc, const char* text,
                                  litehtml::uint_ptr hFont, litehtml::web_color color,
                                  const litehtml::position& pos) override;
    litehtml::pixel_t  pt_to_px(float pt) const override;
    litehtml::pixel_t  get_default_font_size() const override;
    const char*        get_default_font_name() const override;
    void               draw_list_marker(litehtml::uint_ptr hdc,
                                         const litehtml::list_marker& marker) override;
    void               load_image(const char* src, const char* baseurl,
                                   bool redraw_on_ready) override;
    void               get_image_size(const char* src, const char* baseurl,
                                       litehtml::size& sz) override;
    void draw_image(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                    const std::string& url, const std::string& base_url) override;
    void draw_solid_fill(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                          const litehtml::web_color& color) override;
    void draw_linear_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                               const litehtml::background_layer::linear_gradient& gradient) override;
    void draw_radial_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                               const litehtml::background_layer::radial_gradient& gradient) override;
    void draw_conic_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                              const litehtml::background_layer::conic_gradient& gradient) override;
    void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders& borders,
                       const litehtml::position& draw_pos, bool root) override;
    void set_caption(const char* caption) override;
    void set_base_url(const char* base_url) override;
    void link(const std::shared_ptr<litehtml::document>& doc,
               const litehtml::element::ptr& el) override;
    void on_anchor_click(const char* url, const litehtml::element::ptr& el) override;
    void on_mouse_event(const litehtml::element::ptr& el,
                         litehtml::mouse_event event) override;
    void set_cursor(const char* cursor) override;
    void transform_text(std::string& text, litehtml::text_transform tt) override;
    void import_css(std::string& text, const std::string& url,
                     std::string& baseurl) override;
    void set_clip(const litehtml::position& pos,
                   const litehtml::border_radiuses& bdr_radius) override;
    void del_clip() override;
    void get_viewport(litehtml::position& viewport) const override;
    litehtml::element::ptr create_element(const char* tag_name,
                                            const litehtml::string_map& attributes,
                                            const std::shared_ptr<litehtml::document>& doc) override;
    void get_media_features(litehtml::media_features& media) const override;
    void get_language(std::string& language, std::string& culture) const override;

private:
    lucyn::paint::Color toColor(const litehtml::web_color& c) const;
    lucyn::paint::ClipRect toClip(const litehtml::position& p) const;
    std::string resolveUrl(const std::string& src, const std::string& base) const;

    std::string base_url_;
    std::string caption_;

    // font slots: index → FontSlot  (index == hFont cast to size_t)
    std::vector<FontSlot> fonts_;

    lucyn::paint::DisplayList list_;
    lucyn::paint::ImageTable  images_;

    // src → ImageTable id (-1 = failed)
    std::unordered_map<std::string, int> img_cache_;

    // clip stack
    std::vector<litehtml::position> clip_stack_;

    // viewport
    int vp_width_  = 800;
    int vp_height_ = 600;
};

}  // namespace lucyn::layout
