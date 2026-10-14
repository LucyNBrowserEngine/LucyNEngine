// software_painter.cpp - the one and only translation unit that compiles the
// stb implementations (single-header libs: exactly one STB_*_IMPLEMENTATION
// define site, same habit the plan asked for).
#include <lucyn/paint/software_painter.h>

#include "font_data.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace lucyn::paint {

// ---- the one embedded font, loaded once --------------------------------------------------
namespace {

struct FontHandle {
    stbtt_fontinfo info{};
    bool ok = false;
    FontHandle() {
        ok = stbtt_InitFont(&info, kLucynPaintFont, stbtt_GetFontOffsetForIndex(kLucynPaintFont, 0)) != 0;
    }
};

const FontHandle& font() {
    static FontHandle f;  // thread-safe init (C++11 magic statics); loaded once, reused forever
    return f;
}

float scaleFor(float pixelSize) {
    return stbtt_ScaleForPixelHeight(&font().info, pixelSize);
}

// Straight alpha-over blend of a src colour onto a dst pixel. srcAlphaOverride,
// if >= 0, replaces the src colour's own alpha (used for antialiased glyph coverage).
uint32_t blendOver(uint32_t dstPacked, Color src, int srcAlphaOverride = -1) {
    uint8_t dr, dg, db, da;
    unpackRGBA(dstPacked, dr, dg, db, da);
    int sa = srcAlphaOverride >= 0 ? srcAlphaOverride : src.a;
    if (sa <= 0) return dstPacked;
    if (sa >= 255) return packRGBA(src.r, src.g, src.b, 255);
    float a = sa / 255.0f;
    uint8_t r = static_cast<uint8_t>(std::lround(src.r * a + dr * (1 - a)));
    uint8_t g = static_cast<uint8_t>(std::lround(src.g * a + dg * (1 - a)));
    uint8_t b = static_cast<uint8_t>(std::lround(src.b * a + db * (1 - a)));
    uint8_t outA = static_cast<uint8_t>(std::lround(sa + da * (1 - a)));
    return packRGBA(r, g, b, outA);
}

struct Rect { int x0, y0, x1, y1; };  // half-open [x0,x1) x [y0,y1)

Rect clampRect(float x, float y, float w, float h, bool hasClip, const ClipRect& clip, int fbW, int fbH) {
    float rx0 = x, ry0 = y, rx1 = x + w, ry1 = y + h;
    if (hasClip) {
        rx0 = std::max(rx0, clip.x);
        ry0 = std::max(ry0, clip.y);
        rx1 = std::min(rx1, clip.x + clip.w);
        ry1 = std::min(ry1, clip.y + clip.h);
    }
    rx0 = std::max(rx0, 0.0f);
    ry0 = std::max(ry0, 0.0f);
    rx1 = std::min(rx1, static_cast<float>(fbW));
    ry1 = std::min(ry1, static_cast<float>(fbH));
    return Rect{static_cast<int>(std::floor(rx0)), static_cast<int>(std::floor(ry0)),
                static_cast<int>(std::ceil(rx1)), static_cast<int>(std::ceil(ry1))};
}

}  // namespace

// ---- Framebuffer PNG I/O -----------------------------------------------------------------

bool Framebuffer::savePNG(const std::string& path) const {
    if (w <= 0 || h <= 0 || pixels.empty()) return false;
    // stb_image_write wants interleaved bytes, our storage is packed RRGGBBAA uint32_t.
    std::vector<unsigned char> bytes(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (size_t i = 0; i < pixels.size(); ++i) {
        uint8_t r, g, b, a;
        unpackRGBA(pixels[i], r, g, b, a);
        bytes[i * 4 + 0] = r;
        bytes[i * 4 + 1] = g;
        bytes[i * 4 + 2] = b;
        bytes[i * 4 + 3] = a;
    }
    int ok = stbi_write_png(path.c_str(), w, h, 4, bytes.data(), w * 4);
    return ok != 0;
}

bool Framebuffer::loadPNG(const std::string& path, Framebuffer& out) {
    int iw = 0, ih = 0, channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &iw, &ih, &channels, 4);  // force RGBA
    if (!data) return false;
    out.w = iw;
    out.h = ih;
    out.pixels.assign(static_cast<size_t>(iw) * static_cast<size_t>(ih), 0);
    for (int i = 0; i < iw * ih; ++i) {
        const unsigned char* p = data + static_cast<size_t>(i) * 4;
        out.pixels[static_cast<size_t>(i)] = packRGBA(p[0], p[1], p[2], p[3]);
    }
    stbi_image_free(data);
    return true;
}

// ---- ImageTable ---------------------------------------------------------------------------

int ImageTable::load(const std::string& path) {
    Framebuffer fb;
    if (!Framebuffer::loadPNG(path, fb)) return -1;
    images_.push_back(std::move(fb));
    return static_cast<int>(images_.size()) - 1;
}

const Framebuffer* ImageTable::get(int id) const {
    if (id < 0 || static_cast<size_t>(id) >= images_.size()) return nullptr;
    return &images_[static_cast<size_t>(id)];
}

// ---- SoftwarePainter ------------------------------------------------------------------------

SoftwarePainter::SoftwarePainter(Framebuffer& target, const ImageTable* images)
    : fb_(target), images_(images) {}

void SoftwarePainter::draw(const DisplayList& list) {
    for (const auto& item : list) {
        if (auto* r = std::get_if<DrawRect>(&item)) {
            drawRect(*r);
        } else if (auto* b = std::get_if<DrawBorder>(&item)) {
            drawBorder(*b);
        } else if (auto* t = std::get_if<DrawText>(&item)) {
            drawText(*t);
        } else if (auto* im = std::get_if<DrawImage>(&item)) {
            drawImage(*im);
        }
    }
}

void SoftwarePainter::drawRect(const DrawRect& r) {
    if (r.w <= 0 || r.h <= 0) return;
    if (r.fill) {
        Rect clamped = clampRect(r.x, r.y, r.w, r.h, r.hasClip, r.clip, fb_.w, fb_.h);
        for (int y = clamped.y0; y < clamped.y1; ++y)
            for (int x = clamped.x0; x < clamped.x1; ++x)
                fb_.setPixel(x, y, blendOver(fb_.getPixel(x, y), r.color));
    } else {
        // Stroke: four filled strips (a simple, honest "outline"), each
        // independently clamped/clipped.
        float sw = std::max(1.0f, r.strokeWidth);
        DrawRect top{r.x, r.y, r.w, sw, r.color, true, 0, r.hasClip, r.clip};
        DrawRect bottom{r.x, r.y + r.h - sw, r.w, sw, r.color, true, 0, r.hasClip, r.clip};
        DrawRect left{r.x, r.y, sw, r.h, r.color, true, 0, r.hasClip, r.clip};
        DrawRect right{r.x + r.w - sw, r.y, sw, r.h, r.color, true, 0, r.hasClip, r.clip};
        drawRect(top);
        drawRect(bottom);
        drawRect(left);
        drawRect(right);
    }
}

void SoftwarePainter::drawBorder(const DrawBorder& b) {
    if (b.w <= 0 || b.h <= 0 || b.width <= 0) return;
    DrawRect asStroke{b.x, b.y, b.w, b.h, b.color, false, b.width, b.hasClip, b.clip};
    drawRect(asStroke);
}

void SoftwarePainter::drawText(const DrawText& t) {
    if (t.text.empty() || !font().ok || t.size <= 0) return;
    float scale = scaleFor(t.size);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font().info, &ascent, &descent, &lineGap);
    float penX = t.x;
    float baselineY = t.y;

    for (size_t i = 0; i < t.text.size();) {
        unsigned char c = static_cast<unsigned char>(t.text[i]);
        // Only ASCII is rasterized (M9-era honest limit, same spirit as the
        // "Text is basic" entry in README's Known Limitations); non-ASCII
        // bytes advance by one and draw nothing rather than crash or mojibake.
        if (c >= 0x80) { ++i; continue; }
        if (c == '\n') {
            baselineY += t.size + lineGap * scale;
            penX = t.x;
            ++i;
            continue;
        }
        int advanceWidth, leftBearing;
        stbtt_GetCodepointHMetrics(&font().info, c, &advanceWidth, &leftBearing);

        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&font().info, c, scale, scale, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            std::vector<unsigned char> bmp(static_cast<size_t>(gw) * static_cast<size_t>(gh));
            stbtt_MakeCodepointBitmap(&font().info, bmp.data(), gw, gh, gw, scale, scale, c);
            int originX = static_cast<int>(std::floor(penX)) + x0;
            int originY = static_cast<int>(std::floor(baselineY)) + y0;
            Rect clip = clampRect(static_cast<float>(originX), static_cast<float>(originY),
                                   static_cast<float>(gw), static_cast<float>(gh), t.hasClip, t.clip, fb_.w, fb_.h);
            for (int y = clip.y0; y < clip.y1; ++y) {
                for (int x = clip.x0; x < clip.x1; ++x) {
                    int bx = x - originX, by = y - originY;
                    unsigned char cov = bmp[static_cast<size_t>(by) * static_cast<size_t>(gw) + static_cast<size_t>(bx)];
                    if (cov == 0) continue;
                    fb_.setPixel(x, y, blendOver(fb_.getPixel(x, y), t.color, cov));
                }
            }
        }
        penX += advanceWidth * scale;
        if (i + 1 < t.text.size()) {
            int kern = stbtt_GetCodepointKernAdvance(&font().info, c, static_cast<unsigned char>(t.text[i + 1]));
            penX += kern * scale;
        }
        ++i;
    }
}

void SoftwarePainter::drawImage(const DrawImage& img) {
    if (!images_ || img.w <= 0 || img.h <= 0) return;
    const Framebuffer* src = images_->get(img.imageId);
    if (!src || src->w <= 0 || src->h <= 0) return;  // missing/broken image: blank box, no crash

    Rect clamped = clampRect(img.x, img.y, img.w, img.h, img.hasClip, img.clip, fb_.w, fb_.h);
    float sx = static_cast<float>(src->w) / img.w;
    float sy = static_cast<float>(src->h) / img.h;
    for (int y = clamped.y0; y < clamped.y1; ++y) {
        for (int x = clamped.x0; x < clamped.x1; ++x) {
            int srcX = std::clamp(static_cast<int>((x - img.x) * sx), 0, src->w - 1);
            int srcY = std::clamp(static_cast<int>((y - img.y) * sy), 0, src->h - 1);
            uint32_t sp = src->getPixel(srcX, srcY);
            uint8_t r, g, b, a;
            unpackRGBA(sp, r, g, b, a);
            fb_.setPixel(x, y, blendOver(fb_.getPixel(x, y), Color{r, g, b, a}));
        }
    }
}

float SoftwarePainter::measureTextWidth(const std::string& text, float size) {
    if (!font().ok || text.empty() || size <= 0) return 0.0f;
    float scale = scaleFor(size);
    float width = 0.0f;
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0x80 || c == '\n') continue;
        int advanceWidth, leftBearing;
        stbtt_GetCodepointHMetrics(&font().info, c, &advanceWidth, &leftBearing);
        width += advanceWidth * scale;
        if (i + 1 < text.size()) {
            int kern = stbtt_GetCodepointKernAdvance(&font().info, c, static_cast<unsigned char>(text[i + 1]));
            width += kern * scale;
        }
    }
    return width;
}

float SoftwarePainter::lineHeight(float size) {
    if (!font().ok || size <= 0) return size * 1.2f;  // plausible fallback, never used once the font loads
    float scale = scaleFor(size);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font().info, &ascent, &descent, &lineGap);
    return (ascent - descent + lineGap) * scale;
}

}  // namespace lucyn::paint
