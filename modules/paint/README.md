# lucyn::paint

The display list and the one backend (so far) that turns it into pixels.
README's rule: **layout never draws.** `modules/layout` builds a `DisplayList`
(a dumb vector of "draw this here" commands); this module is the only place
that list becomes actual pixels.

## Build

```bash
cmake -S modules/paint -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

Needs nothing but a C++17 compiler — no SDL, no GPU. `ui::Window` (Session 11
Part D) is what later shows a `Framebuffer` on screen; this module only ever
writes into memory or a PNG file.

## What's here

| File | What |
|---|---|
| `include/lucyn/paint/displaylist.h` | `DrawRect`, `DrawBorder`, `DrawText`, `DrawImage`, `DisplayList` |
| `include/lucyn/paint/painter.h` | the `Painter` interface (`draw(const DisplayList&)`) |
| `include/lucyn/paint/framebuffer.h` | `Framebuffer` (RGBA8888 pixel buffer), PNG save/load |
| `include/lucyn/paint/software_painter.h` | `SoftwarePainter`, `ImageTable`, text measurement |
| `src/software_painter.cpp` | the only translation unit with `STB_*_IMPLEMENTATION` defined |
| `src/font_data.{h,cpp}` | the embedded font bytes (generated, see below) |
| `tools/lucyn-paint.cpp` | `--demo out.png` (always) / `--file page.html out.png` (needs `dom`+`layout`) |

## Decisions (Session 11, documented per the plan)

- **Colour and fill/stroke.** `DrawRect` carries an `rgba` `Color` and a
  `fill` flag; `strokeWidth` only matters when `fill == false`. `DrawBorder`
  is the four-sided-outline convenience layout actually emits for element
  borders. Defaults: black text on white, 1px grey borders — "looks like
  sh*t but renders," per the README promise.
- **Clipping.** Every draw item carries an optional `ClipRect` (`hasClip`
  gates it). `overflow: hidden` in `layout` becomes a clip rect on the
  children's draw items, not a separate push/pop stack — simplest thing
  that works for a single level; nested overflow clips are intersected by
  `layout`, not by this module (it only ever sees one rect per item).
- **Text.** One embedded font (`third_party/fonts/PTSans-Regular.ttf`, OFL
  licensed — see that folder's `VENDORED.txt` for why PT Sans and not
  DejaVu). Rasterized per-glyph with `stb_truetype`, ASCII only this
  session (non-ASCII bytes are skipped, not mojibake'd or crashed on — see
  README's "Text is basic" Known Limitation). No font search on the host:
  the embedded font is the only font, ever, by design (a host font list is
  itself a fingerprinting surface).
- **Images.** `ImageTable::load(path)` decodes with `stb_image` (PNG/JPEG/BMP
  compiled in; see the `STBI_ONLY_*` defines) and returns an id, or -1 on
  failure. A `DrawImage` with no matching id draws nothing — a blank box,
  never a crash.

## Tests (`paint` suite)

Known-pixel rect placement, out-of-bounds rects (every edge) under
ASan/UBSan, text bounding-box sanity + empty/huge string, clip-rect
behaviour, a PNG round trip (write -> `stb_image` read-back -> compare), and
a golden-hash tripwire (see the note printed by that test — it is a
regression check, not an independent correctness proof).

**ASan/UBSan**, as the plan asked:
```bash
cmake -S modules/paint -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure
```

## Looking at output

`lucyn-paint --demo out.png` draws a hard-coded `DisplayList` (header bar,
bordered card, wrapped-looking text block, a clipped red square) — the
easiest first proof the painter works, and a PNG the AI (or anyone) can
actually look at with an image viewer. `--file page.html out.png` exists
only when this module was built as part of the root project with
`LUCYN_MODULE_LAYOUT` on (it links `lucyn::dom` + `lucyn::layout`).
