// lucyn/paint/framebuffer.h - a plain pixel buffer SoftwarePainter draws into,
// and that ui::Window (Session 11 Part D) presents, or lucyn-paint / the
// "screenshot" controller command write out as a PNG.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lucyn::paint {

// RGBA8888, row-major, top-left origin (y grows downward, same as DOM/layout).
struct Framebuffer {
    int w = 0, h = 0;
    std::vector<uint32_t> pixels;   // packed 0xRRGGBBAA, see packRGBA/unpackRGBA below

    void init(int width, int height, uint32_t fillRGBA = 0xFFFFFFFFu) {
        w = width;
        h = height;
        pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h), fillRGBA);
    }

    bool inBounds(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }

    void setPixel(int x, int y, uint32_t rgba) {
        if (!inBounds(x, y)) return;
        pixels[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = rgba;
    }

    uint32_t getPixel(int x, int y) const {
        if (!inBounds(x, y)) return 0;
        return pixels[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
    }

    // Writes an 8-bit RGBA PNG. Returns false (and writes nothing) on an empty
    // buffer or an I/O error.
    bool savePNG(const std::string& path) const;

    // Reads any image stb_image understands back into an RGBA8888 Framebuffer
    // (used by the paint test suite's PNG round-trip check, and by
    // ImageTable::load for <img> sources). Returns false on failure; *out is
    // left unchanged on failure.
    static bool loadPNG(const std::string& path, Framebuffer& out);
};

inline uint32_t packRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (static_cast<uint32_t>(r) << 24) | (static_cast<uint32_t>(g) << 16) |
           (static_cast<uint32_t>(b) << 8) | static_cast<uint32_t>(a);
}
inline void unpackRGBA(uint32_t v, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    r = static_cast<uint8_t>((v >> 24) & 0xFF);
    g = static_cast<uint8_t>((v >> 16) & 0xFF);
    b = static_cast<uint8_t>((v >> 8) & 0xFF);
    a = static_cast<uint8_t>(v & 0xFF);
}

}  // namespace lucyn::paint
