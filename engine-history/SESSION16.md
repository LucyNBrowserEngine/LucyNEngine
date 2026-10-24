# Session 16 - the window stays alive while the engine is busy

## Symptom (from the author's desktop)
The LNM UI rendered fine. After a terminal command such as `{"cmd":"search",...}` the window turned into a
"screenshot of the terminal" (stale/foreign pixels), the painter/UI looked broken, and the search box could not be typed into.

## Root cause
All SDL work (event polling, `SDL_GetWindowSurface`, blits) ran on the same thread that executes controller commands.
`search`/`visit` block on the network for seconds, during which the window got no expose/restore/resize handling, and
`present()` only drew when the controller explicitly called it. Whatever buffer the compositor held was shown. Reproduced
under Xvfb + openbox (stall `visit` with a slow server, unmap/map the window -> solid black).
The painter, layout and LNM were never at fault.

## Changes
- `ui::Window` now owns a private **UI thread**; every SDL call happens there. `present()` just stores the frame; the UI
  thread blits it and **re-blits the last frame on EXPOSED / SHOWN / RESTORED / MAXIMIZED / MOVED / FOCUS / resize**.
  `pollEvents()` drains a queue, so input typed during a slow command is kept. Public API unchanged except the header
  now says so; `width()/height()/visible()` are atomics. Not supported on macOS (Cocoa wants the main thread).
- SDL hints: `MOUSE_FOCUS_CLICKTHROUGH` (the focusing click is no longer swallowed), `IME_SHOW_UI`; `SDL_RaiseWindow` on show;
  text input is restarted on focus gain.
- Controller: typing with nothing focused goes to the first `<input>` (no click needed); numpad Enter submits;
  **Esc on a loaded page returns to the LNM home view** (previously the LNM UI was unreachable after any `visit`/`search`).
- Commands still run on one thread, so QuickJS (which records its stack base) is unaffected.

## Verification (Xvfb + openbox, plus 23/23 ctest with `SDL_VIDEODRIVER=dummy`)
- Stalled-command expose test: before = black window, after = UI stays painted.
- xdotool: type with no click, Backspace, `visit` repaint, Esc -> home, resize all behave.
- `test_ui` gained thread/re-create/churn cases.

## Not verified
A real Wayland/compositor desktop (the author's). Network search itself (no internet in the sandbox; Brave returned 429 on
the author's machine, which is a rate-limit page, not an engine bug - try `--config` with `"default": "duckduckgo"` or another provider).
