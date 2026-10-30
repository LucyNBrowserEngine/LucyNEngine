// modules/cef_host/src/preview_window.h — the headful preview window (internal).
//
// LucyN still renders OFF-SCREEN (Chromium runs with ozone-platform=headless and
// paints into OnPaint), so screenshots, eval and every other command keep working
// exactly as they do headless. The preview window is just a viewer: it takes the
// frames Chromium already produced and shows them in a real X11 window, so a
// human who runs `./lucyn` sees the page the script is driving.
//
// One thread owns the Display and the event loop; present() only copies the
// newest frame into a slot, so a slow window can never block CEF's UI thread.
// With no display (or no X11 compiled in) every call is a cheap no-op and the
// engine simply runs headless.
#pragma once

#include <functional>
#include <string>

namespace lucyn::cef_host::preview {

// Master switch, set by the app before initCef(): `lucyn` turns it on unless
// --headless was given, libraries and tests leave it off.
void setEnabled(bool on);
bool enabled();

// ---- input -----------------------------------------------------------------
// The window is a viewer around an OFF-SCREEN browser: without forwarding,
// clicks and keystrokes captured here would go nowhere. Every X11 input event
// becomes one InputEvent, delivered on the viewer thread; the receiver (the
// engine) posts it onto CEF's UI thread.
struct InputEvent {
    enum Kind { Move, Down, Up, Wheel, KeyDown, KeyUp, Focus } kind = Move;
    double x = 0, y = 0;             // view pixels (Move/Down/Up/Wheel)
    int button = 0;                  // Down/Up: 1 = left, 2 = middle, 3 = right
    int deltaY = 0;                  // Wheel: +120 = wheel up, -120 = wheel down
    unsigned int modifiers = 0;      // cef_event_flags_t bits
    bool focus = false;              // Focus only
    // Key events:
    unsigned int keyCode = 0;        // cef windows_key_code (VK_*), 0 = unmapped
    int nativeKey = 0;               // the X11 keycode
    std::string text;                // UTF-8 characters this keystroke produced
};

// Set before start(); called for every input event on the viewer thread.
// Empty handler (the default) = input is dropped.
void setInputHandler(std::function<void(const InputEvent&)> h);

// Open the window (title + viewport size). Returns false when there is nothing
// to draw on — no $DISPLAY, no X11 support — which is not an error: the engine
// just stays headless. Idempotent.
bool start(const std::string& title, int width, int height);

// Hand over Chromium's newest BGRA frame. Cheap: one memcpy, no X11 call here.
void present(const void* bgra, int width, int height);

// Update the title bar (page title changed). Also cheap and lock-bounded.
void setTitle(const std::string& title);

// Close the window and join its thread. Safe to call more than once, safe to
// call when start() never succeeded.
void stop();

}  // namespace lucyn::cef_host::preview
