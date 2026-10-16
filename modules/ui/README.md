# lucyn::ui

SDL2 window wrapper. Ghostful Mode lives here: `providingUI()` is always
`true` — the window is always real — and `Window::setVisible()` is the only
thing that changes between "a window nobody can see" and "a window on
screen." See README.md's "Ghostful Mode" section and `SESSION11.md` for the
full design note.

## Build

```bash
cmake -S modules/ui -B build && cmake --build build -j
SDL_VIDEODRIVER=dummy ctest --test-dir build --output-on-failure
```

**Optional dependency.** If SDL2 (`libsdl2-dev`) isn't found, this module's
CMakeLists prints a `STATUS` message and `return()`s — nothing else in the
tree is affected; `paint`, `layout`, and the `screenshot` controller command
all still build and work with no display at all. Only the `window` controller
command and real on-screen rendering need this module.

## Decisions made this session (see SESSION11.md for the longer version)

- **Event loop vs stdin loop.** With a live window and real stdin, a short-lived
  reader thread waits for the next JSON line while the controller's main thread
  pumps SDL events. This keeps SDL calls on the main thread and lets LNM inputs
  accept mouse focus and keyboard text while the CLI waits. Injected streams
  remain synchronous. Resize events repaint the current view at the new size.
- **Pixel format.** `Framebuffer`'s packed `uint32_t` (`(r<<24)|(g<<16)|(b<<8)|a`)
  is, in little-endian memory, byte order A,B,G,R — `SDL_PIXELFORMAT_ABGR32`,
  not `SDL_PIXELFORMAT_RGBA32` (that alias is endianness-normalised to
  R,G,B,A and would come out with channels swapped). Get this wrong and
  nothing crashes — the window just shows blue skies as orange. Written
  down here on purpose.
- **Tests run under `SDL_VIDEODRIVER=dummy`** (`set_tests_properties(... ENVIRONMENT ...)`
  in CMakeLists.txt, so `ctest` alone is enough — no manual env var). They
  check creation, hide/show flags, resizing, `present()` not crashing, and
  `pollEvents()` not crashing. **What they cannot verify:** an actual window
  appearing on a real desktop or real mouse/keyboard input landing correctly.
  The LNM demo provides a search field to check that manually.
