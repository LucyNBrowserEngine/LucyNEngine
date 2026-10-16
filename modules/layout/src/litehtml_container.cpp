#include "litehtml_container.h"

#include <litehtml/document.h>
#include <litehtml/element.h>
#include <litehtml/borders.h>
#include <litehtml/media_query.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace lucyn::layout {

// ---- helpers ---------------------------------------------------------------

namespace {

std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Very small file reader used for import_css.
std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Join a base dir and a relative URL the simple way.
std::string joinUrl(const std::string& base, const std::string& rel) {
    if (rel.empty()) return base;
    // Absolute: starts with / or has a scheme
    if (rel[0] == '/' || rel.find("://") != std::string::npos) return rel;
    // Relative: append to base directory
    std::string dir = base;
    auto slash = dir.rfind('/');
    if (slash != std::string::npos) dir = dir.substr(0, slash + 1);
    else dir += '/';
    return dir + rel;
}

// Strip "file://" prefix.
std::string stripFileScheme(const std::string& url) {
    if (url.substr(0, 7) == "file://") return url.substr(7);
    return url;
}

}  // namespace

// ---- ctor ------------------------------------------------------------------

LucynLitehtmlContainer::LucynLitehtmlContainer(const std::string& base_url)
    : base_url_(base_url) {
    // Reserve slot 0 as "invalid / null font".
    fonts_.push_back(FontSlot{});
}

// ---- private helpers -------------------------------------------------------

lucyn::paint::Color LucynLitehtmlContainer::toColor(const litehtml::web_color& c) const {
    return lucyn::paint::Color::rgba(c.red, c.green, c.blue, c.alpha);
}

lucyn::paint::ClipRect LucynLitehtmlContainer::toClip(const litehtml::position& p) const {
    return lucyn::paint::ClipRect{float(p.x), float(p.y), float(p.width), float(p.height)};
}

std::string LucynLitehtmlContainer::resolveUrl(const std::string& src,
                                                const std::string& base) const {
    if (src.empty()) return {};
    if (src.find("://") != std::string::npos) return src;
    const std::string& b = base.empty() ? base_url_ : base;
    return joinUrl(b, src);
}

litehtml::position LucynLitehtmlContainer::currentClip() const {
    if (clip_stack_.empty()) return {};
    return clip_stack_.back();
}

// ---- font ------------------------------------------------------------------

litehtml::uint_ptr LucynLitehtmlContainer::create_font(
        const litehtml::font_description& descr,
        const litehtml::document* /*doc*/,
        litehtml::font_metrics* fm) {

    float sz = static_cast<float>(descr.size);
    if (sz <= 0) sz = 16.f;

    float lh   = lucyn::paint::SoftwarePainter::lineHeight(sz);
    // stb_truetype ascent is approximately 0.8 of lineHeight for PTSans
    float asc  = lh * 0.78f;
    float desc = lh - asc;

    FontSlot slot;
    slot.size     = sz;
    slot.ascent   = asc;
    slot.descent  = desc;
    slot.height   = lh;
    slot.x_height = sz * 0.5f;

    if (fm) {
        fm->font_size  = sz;
        fm->height     = slot.height;
        fm->ascent     = slot.ascent;
        fm->descent    = slot.descent;
        fm->x_height   = slot.x_height;
        fm->ch_width   = lucyn::paint::SoftwarePainter::measureTextWidth("0", sz);
        fm->draw_spaces = false;
        fm->sub_shift   = sz * 0.2f;
        fm->super_shift = sz * 0.4f;
    }

    fonts_.push_back(slot);
    return static_cast<litehtml::uint_ptr>(fonts_.size() - 1);
}

void LucynLitehtmlContainer::delete_font(litehtml::uint_ptr /*hFont*/) {
    // Slots stay; IDs are never reused in a single render pass.
}

litehtml::pixel_t LucynLitehtmlContainer::text_width(const char* text,
                                                       litehtml::uint_ptr hFont) {
    float sz = 16.f;
    if (hFont < fonts_.size()) sz = fonts_[hFont].size;
    return lucyn::paint::SoftwarePainter::measureTextWidth(text ? text : "", sz);
}

void LucynLitehtmlContainer::draw_text(litehtml::uint_ptr /*hdc*/, const char* text,
                                        litehtml::uint_ptr hFont,
                                        litehtml::web_color color,
                                        const litehtml::position& pos) {
    if (!text || !*text) return;
    float sz = 16.f;
    float asc = sz * 0.78f;
    if (hFont < fonts_.size()) {
        sz  = fonts_[hFont].size;
        asc = fonts_[hFont].ascent;
    }

    bool hasClip = !clip_stack_.empty();
    lucyn::paint::DrawText dt;
    dt.x       = float(pos.x);
    dt.y       = float(pos.y) + asc;   // baseline = top + ascent
    dt.text    = text;
    dt.size    = sz;
    dt.color   = toColor(color);
    dt.hasClip = hasClip;
    if (hasClip) dt.clip = toClip(clip_stack_.back());
    list_.push_back(std::move(dt));
}

litehtml::pixel_t LucynLitehtmlContainer::pt_to_px(float pt) const {
    return pt * 1.333f;   // 96 dpi: 1pt = 4/3 px
}

litehtml::pixel_t LucynLitehtmlContainer::get_default_font_size() const { return 16; }
const char*        LucynLitehtmlContainer::get_default_font_name() const { return "sans-serif"; }

// ---- backgrounds / fills ---------------------------------------------------

void LucynLitehtmlContainer::draw_solid_fill(litehtml::uint_ptr /*hdc*/,
                                              const litehtml::background_layer& layer,
                                              const litehtml::web_color& color) {
    if (color.alpha == 0) return;
    bool hasClip = !clip_stack_.empty();
    lucyn::paint::DrawRect dr;
    dr.x       = float(layer.border_box.x);
    dr.y       = float(layer.border_box.y);
    dr.w       = float(layer.border_box.width);
    dr.h       = float(layer.border_box.height);
    dr.color   = toColor(color);
    dr.fill    = true;
    dr.hasClip = hasClip;
    if (hasClip) dr.clip = toClip(clip_stack_.back());
    list_.push_back(std::move(dr));
}

void LucynLitehtmlContainer::draw_linear_gradient(litehtml::uint_ptr hdc,
        const litehtml::background_layer& layer,
        const litehtml::background_layer::linear_gradient& gradient) {
    // Approximate: draw with the first stop's color (full CSS gradient needs a
    // scanline blender — out of scope for this integration layer).
    if (!gradient.color_points.empty())
        draw_solid_fill(hdc, layer, gradient.color_points.front().color);
}

void LucynLitehtmlContainer::draw_radial_gradient(litehtml::uint_ptr hdc,
        const litehtml::background_layer& layer,
        const litehtml::background_layer::radial_gradient& gradient) {
    if (!gradient.color_points.empty())
        draw_solid_fill(hdc, layer, gradient.color_points.front().color);
}

void LucynLitehtmlContainer::draw_conic_gradient(litehtml::uint_ptr hdc,
        const litehtml::background_layer& layer,
        const litehtml::background_layer::conic_gradient& gradient) {
    if (!gradient.color_points.empty())
        draw_solid_fill(hdc, layer, gradient.color_points.front().color);
}

// ---- images ----------------------------------------------------------------

void LucynLitehtmlContainer::load_image(const char* src, const char* baseurl,
                                         bool /*redraw_on_ready*/) {
    if (!src || !*src) return;
    std::string url = resolveUrl(src, baseurl ? baseurl : "");
    if (img_cache_.count(url)) return;   // already attempted

    // Try to load from disk (file:// or bare path).
    std::string path = stripFileScheme(url);
    int id = images_.load(path);
    img_cache_[url] = id;
}

void LucynLitehtmlContainer::get_image_size(const char* src, const char* baseurl,
                                             litehtml::size& sz) {
    if (!src || !*src) { sz.width = sz.height = 0; return; }
    std::string url = resolveUrl(src, baseurl ? baseurl : "");

    // Load if not yet cached.
    if (!img_cache_.count(url)) {
        load_image(src, baseurl, false);
    }
    auto it = img_cache_.find(url);
    if (it == img_cache_.end() || it->second < 0) {
        sz.width = sz.height = 0;
        return;
    }
    const lucyn::paint::Framebuffer* fb = images_.get(it->second);
    if (!fb) { sz.width = sz.height = 0; return; }
    sz.width  = fb->w;
    sz.height = fb->h;
}

void LucynLitehtmlContainer::draw_image(litehtml::uint_ptr /*hdc*/,
                                         const litehtml::background_layer& layer,
                                         const std::string& url,
                                         const std::string& base_url) {
    auto it = img_cache_.find(resolveUrl(url, base_url));
    if (it == img_cache_.end() || it->second < 0) return;

    bool hasClip = !clip_stack_.empty();
    lucyn::paint::DrawImage di;
    di.x       = float(layer.border_box.x);
    di.y       = float(layer.border_box.y);
    di.w       = float(layer.border_box.width);
    di.h       = float(layer.border_box.height);
    di.imageId = it->second;
    di.hasClip = hasClip;
    if (hasClip) di.clip = toClip(clip_stack_.back());
    list_.push_back(std::move(di));
}

// ---- borders ---------------------------------------------------------------

void LucynLitehtmlContainer::draw_borders(litehtml::uint_ptr /*hdc*/,
                                           const litehtml::borders& b,
                                           const litehtml::position& draw_pos,
                                           bool /*root*/) {
    bool hasClip = !clip_stack_.empty();
    lucyn::paint::ClipRect clip;
    if (hasClip) clip = toClip(clip_stack_.back());

    float x = float(draw_pos.x), y = float(draw_pos.y);
    float w = float(draw_pos.width), h = float(draw_pos.height);

    // Emit each non-zero side as a thin filled rect.
    auto emitSide = [&](float sx, float sy, float sw, float sh, const litehtml::border& side) {
        if (side.width <= litehtml::pixel_t(0) || side.style == litehtml::border_style_none) return;
        if (side.style == litehtml::border_style_hidden) return;
        lucyn::paint::DrawRect dr;
        dr.x = sx; dr.y = sy; dr.w = sw; dr.h = sh;
        dr.color = toColor(side.color);
        dr.fill  = true;
        dr.hasClip = hasClip;
        dr.clip  = clip;
        list_.push_back(std::move(dr));
    };

    float bw_top    = float(b.top.width);
    float bw_right  = float(b.right.width);
    float bw_bottom = float(b.bottom.width);
    float bw_left   = float(b.left.width);

    emitSide(x,           y,           w,         bw_top,    b.top);
    emitSide(x + w - bw_right, y,     bw_right,   h,         b.right);
    emitSide(x,           y + h - bw_bottom, w,   bw_bottom, b.bottom);
    emitSide(x,           y,           bw_left,   h,         b.left);
}

// ---- list markers ----------------------------------------------------------

void LucynLitehtmlContainer::draw_list_marker(litehtml::uint_ptr /*hdc*/,
                                               const litehtml::list_marker& marker) {
    // Render bullet as a small filled square for simplicity.
    bool hasClip = !clip_stack_.empty();
    lucyn::paint::DrawRect dr;
    float sz = float(marker.pos.height) * 0.35f;
    dr.x = float(marker.pos.x);
    dr.y = float(marker.pos.y) + (float(marker.pos.height) - sz) * 0.5f;
    dr.w = sz; dr.h = sz;
    dr.color = toColor(marker.color);
    dr.fill  = true;
    dr.hasClip = hasClip;
    if (hasClip) dr.clip = toClip(clip_stack_.back());
    list_.push_back(std::move(dr));
}

// ---- clip ------------------------------------------------------------------

void LucynLitehtmlContainer::set_clip(const litehtml::position& pos,
                                       const litehtml::border_radiuses& /*bdr_radius*/) {
    clip_stack_.push_back(pos);
}

void LucynLitehtmlContainer::del_clip() {
    if (!clip_stack_.empty()) clip_stack_.pop_back();
}

// ---- CSS import ------------------------------------------------------------

void LucynLitehtmlContainer::import_css(std::string& text, const std::string& url,
                                         std::string& baseurl) {
    std::string path = stripFileScheme(resolveUrl(url, baseurl));
    text = readFile(path);
    // Update baseurl to the directory of the loaded file.
    auto slash = path.rfind('/');
    baseurl = (slash != std::string::npos) ? path.substr(0, slash + 1) : path;
}

// ---- misc ------------------------------------------------------------------

void LucynLitehtmlContainer::set_caption(const char* caption) {
    if (caption) caption_ = caption;
}

void LucynLitehtmlContainer::set_base_url(const char* base_url) {
    if (base_url) base_url_ = base_url;
}

void LucynLitehtmlContainer::link(const std::shared_ptr<litehtml::document>& /*doc*/,
                                   const litehtml::element::ptr& /*el*/) {}

void LucynLitehtmlContainer::on_anchor_click(const char* /*url*/,
                                              const litehtml::element::ptr& /*el*/) {}

void LucynLitehtmlContainer::on_mouse_event(const litehtml::element::ptr& /*el*/,
                                             litehtml::mouse_event /*event*/) {}

void LucynLitehtmlContainer::set_cursor(const char* /*cursor*/) {}

void LucynLitehtmlContainer::transform_text(std::string& text,
                                             litehtml::text_transform tt) {
    switch (tt) {
    case litehtml::text_transform_uppercase:
        for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        break;
    case litehtml::text_transform_lowercase:
        for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        break;
    case litehtml::text_transform_capitalize:
        if (!text.empty()) text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
        break;
    default: break;
    }
}

void LucynLitehtmlContainer::get_viewport(litehtml::position& viewport) const {
    viewport.x = viewport.y = 0;
    viewport.width  = vp_width_;
    viewport.height = vp_height_;
}

litehtml::element::ptr LucynLitehtmlContainer::create_element(
        const char* /*tag_name*/,
        const litehtml::string_map& /*attributes*/,
        const std::shared_ptr<litehtml::document>& /*doc*/) {
    return nullptr;   // use litehtml's default element factory for everything
}

void LucynLitehtmlContainer::get_media_features(litehtml::media_features& media) const {
    media.type        = litehtml::media_type_screen;
    media.width       = vp_width_;
    media.height      = vp_height_;
    media.device_width  = vp_width_;
    media.device_height = vp_height_;
    media.color       = 8;
    media.monochrome  = 0;
    media.color_index = 256;
    media.resolution  = 96;
}

void LucynLitehtmlContainer::get_language(std::string& language,
                                           std::string& culture) const {
    language = "en";
    culture  = "US";
}

}  // namespace lucyn::layout
