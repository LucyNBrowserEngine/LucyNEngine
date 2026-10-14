// Internal to modules/paint. The actual bytes live in gen/font_gen.inc,
// generated at configure time by embed_font.cmake from
// third_party/fonts/PTSans-Regular.ttf (see that folder's VENDORED.txt),
// and compiled once into font_data.cpp. Nothing outside this module should
// need these symbols directly (use SoftwarePainter's text APIs instead).
#pragma once

extern const unsigned char kLucynPaintFont[];
extern const unsigned int  kLucynPaintFont_len;
