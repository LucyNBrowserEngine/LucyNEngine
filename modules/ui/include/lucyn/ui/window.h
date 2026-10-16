// lucyn/ui/window.h - SDL2 window wrapper (SESSION11_PLAN.md Part D).
//
// README + HANDOFF section 3 item 9 set the design: the `ui` module ALWAYS
// reports "I am providing a UI" (providingUI() is a constant true); the
// controller decides whether that UI is actually shown. That is Ghostful
// Mode: a hidden window is still a real SDL window — real display, real
// size, real surface — it just never calls SDL_ShowWindow. Headless checks
// that look for a real window all pass, because the window *is* real.
//
// THREAD MODEL (Session 16). Every SDL call happens on one private UI thread
// owned by the Window. The caller's thread (the controller) never touches SDL:
//   * present() only stores the frame; the UI thread blits it, and re-blits the
//     SAME frame on every expose / restore / focus / resize event. That is why
//     the window no longer turns black (or shows the terminal behind it) while
//     the engine is blocked in a slow network command.
//   * pollEvents() drains a queue the UI thread fills, so it is cheap, never
//     blocks, and may be called from any one thread.
//   * create()/setVisible()/setTitle() hand work to the UI thread and wait.
// Not supported on macOS, where Cocoa insists on the process main thread.
#pragma once
#include <lucyn/paint/framebuffer.h>

#include <atomic>
#include <string>
#include <vector>

namespace lucyn::ui {

enum class EventType { Quit, Resize, MouseWheel, MouseClick, TextInput, KeyDown };

struct Event {
    EventType type = EventType::Quit;
    int   width = 0, height = 0;     // Resize
    float scrollDX = 0, scrollDY = 0;  // MouseWheel
    int   clickX = 0, clickY = 0;    // MouseClick
    int   key = 0;                   // KeyDown: SDL_Keycode value
    bool  ctrl = false;              // KeyDown: Ctrl modifier
    std::string text;                // TextInput: UTF-8 text
};

// Always true. Present only to make the design rule above checkable in code
// (a caller can assert it rather than just reading about it).
bool providingUI();

class Window {
public:
    Window();
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Creates an SDL window titled `title`, `w` x `h`. The window is always
    // created (SDL_WINDOW_HIDDEN); `visible` only controls whether it is
    // shown right away — see setVisible(). Returns false on an SDL failure
    // (lastError() has SDL_GetError()'s message); common cause: no display
    // and SDL_VIDEODRIVER isn't set to "dummy".
    bool create(int w, int h, const std::string& title, bool visible = false);

    // Ghostful Mode's switch. false (the CLI default — existing scripts and
    // the suites that ran before this module existed must behave exactly as
    // before) keeps the window real but un-shown: present() still renders
    // into it, so a later setVisible(true) shows real content immediately,
    // not a blank window.
    void setVisible(bool visible);
    bool visible() const { return visible_.load(); }

    void setTitle(const std::string& title);
    int  width() const { return w_; }
    int  height() const { return h_; }

    // Stores `fb` as the current frame. The UI thread copies it onto the
    // window's surface (and, only if visible, onto the screen) within a few
    // milliseconds, and keeps re-drawing it whenever the OS asks. Safe, and
    // expected, to call while hidden. Returns immediately.
    void present(const lucyn::paint::Framebuffer& fb);

    // Returns the input events that happened since the last call (the UI
    // thread collects them even while the controller is busy running a
    // command, so nothing typed during a slow page load is lost). Repainting
    // for expose/restore/focus is handled by the UI thread itself and is not
    // reported here; Resize still is, so the owner can re-layout.
    // A Window that was never created(), or failed to, returns an empty vector.
    std::vector<Event> pollEvents();

    const std::string& lastError() const { return lastError_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::atomic<int> w_{0}, h_{0};
    std::atomic<bool> visible_{false};
    std::string lastError_;
};

}  // namespace lucyn::ui
