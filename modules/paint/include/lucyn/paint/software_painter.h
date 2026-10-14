// lucyn/paint/software_painter.h - the CPU-only Painter backend.
// No SDL anywhere in this module, so it builds and tests on any machine
// (ui::Window, Session 11 Part D, is what shows a Framebuffer on screen).
// Text: stb_truetype over the single embedded font (third_party/fonts, see
// its VENDORED.txt). Images: stb_image to decode, stb_image_write for PNG out.
#pragma once
#include <lucyn/paint/displaylist.h>
#include <lucyn/paint/framebuffer.h>
#include <lucyn/paint/painter.h>

#include <string>
#include <vector>

namespace lucyn::paint {

// Tiny image table: load a PNG/JPEG/etc (anything stb_image decodes) off
// disk, get back an id >= 0, hand that id to DrawImage. A failed load
// returns -1 and leaves the table unchanged, so a missing file is a draw
// no-op (a blank box), never a crash.
class ImageTable {
public:
    int load(const std::string& path);
    const Framebuffer* get(int id) const;

private:
    std::vector<Framebuffer> images_;
};

class SoftwarePainter : public Painter {
public:
    // Draws into `target` (resized by the caller before drawing; SoftwarePainter
    // never resizes it). `images`, if given, resolves DrawImage::imageId; null
    // means every DrawImage draws nothing (fine for text-only pages/tests).
    explicit SoftwarePainter(Framebuffer& target, const ImageTable* images = nullptr);

    void draw(const DisplayList& list) override;

    // Measures how wide `text` would render at `size` with the embedded font,
    // without drawing anything. layout (Part C) uses this for line wrapping.
    static float measureTextWidth(const std::string& text, float size);
    // Recommended line height (ascent+descent+linegap) for `size`, from the
    // embedded font's own metrics (README: "text line-height from the font
    // metrics").
    static float lineHeight(float size);

private:
    Framebuffer& fb_;
    const ImageTable* images_;

    void drawRect(const DrawRect& r);
    void drawBorder(const DrawBorder& b);
    void drawText(const DrawText& t);
    void drawImage(const DrawImage& img);
};

}  // namespace lucyn::paint
