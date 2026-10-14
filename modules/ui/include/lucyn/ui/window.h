// lucyn/ui/window.h - SDL2 window wrapper (SESSION11_PLAN.md Part D).
//
// README + HANDOFF section 3 item 9 set the design: the `ui` module ALWAYS
// reports "I am providing a UI" (providingUI() is a constant true); the
// controller decides whether that UI is actually shown. That is Ghostful
// Mode: a hidden window is still a real SDL window — real display, real
// size, real surface — it just never calls SDL_ShowWindow. Headless checks
// that look for a real window all pass, because the window *is* real.
#pragma once
#include <lucyn/paint/framebuffer.h>

#include <string>
#include <vector>

namespace lucyn::ui {

enum class EventType { Quit, Resize, MouseWheel, MouseClick };

struct Event {
    EventType type = EventType::Quit;
    int   width = 0, height = 0;     // Resize
    float scrollDX = 0, scrollDY = 0;  // MouseWheel
    int   clickX = 0, clickY = 0;    // MouseClick
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
    bool visible() const { return visible_; }

    void setTitle(const std::string& title);
    int  width() const { return w_; }
    int  height() const { return h_; }

    // Copies `fb` onto the window's surface and (only if visible) presents
    // it on screen. Safe, and expected, to call while hidden.
    void present(const lucyn::paint::Framebuffer& fb);

    // Pumps SDL's event queue and returns what happened since the last call.
    // Must run on the main thread (see SESSION11.md's "event loop vs stdin
    // loop" note: the controller pumps this between stdin commands, not on a
    // separate thread — controller.h's "no threads" sentence is kept as-is;
    // the trade-off is that window events are only noticed when the next
    // command line arrives, not instantly). A Window that was never
    // created(), or failed to, returns an empty vector.
    std::vector<Event> pollEvents();

    const std::string& lastError() const { return lastError_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    int w_ = 0, h_ = 0;
    bool visible_ = false;
    bool sdlOwned_ = false;   // true once create() succeeded in acquiring an SDL video refcount
    std::string lastError_;
};

}  // namespace lucyn::ui
