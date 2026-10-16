#include <lucyn/ui/window.h>

#include <SDL.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

namespace lucyn::ui {

bool providingUI() { return true; }

// All SDL state lives here and is touched ONLY by the UI thread, except the
// members marked "shared", which are guarded by `mu`.
struct Window::Impl {
    std::thread thread;
    SDL_Window* window = nullptr;          // UI thread only
    bool sdlUp = false;                    // UI thread only

    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::function<void()>> tasks;   // shared: work for the UI thread
    bool stop = false;                          // shared
    std::vector<Event> events;                  // shared: input waiting for pollEvents()
    lucyn::paint::Framebuffer frame;            // shared: the frame to (re)draw
    bool haveFrame = false;                     // shared
    bool dirty = false;                         // shared: frame changed since the last blit
    std::atomic<bool> visible{false};           // mirrors Window::visible_ for the UI thread

    // ---- UI-thread helpers -------------------------------------------------
    void blit() {
        if (!window) return;
        std::lock_guard<std::mutex> lock(mu);
        dirty = false;
        if (!haveFrame || frame.w <= 0 || frame.h <= 0) return;
        // fb.pixels is packed (r<<24)|(g<<16)|(b<<8)|a; in little-endian memory
        // that is byte order A,B,G,R, which is exactly SDL_PIXELFORMAT_ABGR32's
        // guarantee (unlike *_RGBA32, which is endianness-normalised to R,G,B,A).
        SDL_Surface* src = SDL_CreateRGBSurfaceWithFormatFrom(
            frame.pixels.data(), frame.w, frame.h, 32, frame.w * 4, SDL_PIXELFORMAT_ABGR32);
        if (!src) return;
        // Ask for the surface on EVERY blit: SDL invalidates the old one when the
        // window is resized/restored, and drawing into a stale one is what leaves
        // old desktop contents in the window.
        if (SDL_Surface* surf = SDL_GetWindowSurface(window)) {
            SDL_BlitScaled(src, nullptr, surf, nullptr);
            // Ghostful Mode: when hidden the surface is still filled, so a later
            // show has real content; only a visible window is pushed to the screen.
            if (visible.load()) SDL_UpdateWindowSurface(window);
        }
        SDL_FreeSurface(src);   // "From" an external buffer: does not free the pixels
    }

    void pump(Window* owner) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            Event ev;
            switch (e.type) {
                case SDL_QUIT:
                    ev.type = EventType::Quit;
                    push(std::move(ev));
                    break;
                case SDL_WINDOWEVENT:
                    switch (e.window.event) {
                        case SDL_WINDOWEVENT_RESIZED:
                        case SDL_WINDOWEVENT_SIZE_CHANGED:
                            owner->w_.store(e.window.data1);
                            owner->h_.store(e.window.data2);
                            ev.type = EventType::Resize;
                            ev.width = e.window.data1;
                            ev.height = e.window.data2;
                            push(std::move(ev));
                            blit();   // show the last frame (scaled) right away, until the owner repaints
                            break;
                        case SDL_WINDOWEVENT_EXPOSED:
                        case SDL_WINDOWEVENT_SHOWN:
                        case SDL_WINDOWEVENT_RESTORED:
                        case SDL_WINDOWEVENT_MAXIMIZED:
                        case SDL_WINDOWEVENT_MOVED:
                            blit();
                            break;
                        case SDL_WINDOWEVENT_FOCUS_GAINED:
                        case SDL_WINDOWEVENT_ENTER:
                            SDL_StartTextInput();   // some IMEs/compositors drop it while unfocused
                            blit();
                            break;
                        default:
                            break;
                    }
                    break;
                case SDL_MOUSEWHEEL:
                    ev.type = EventType::MouseWheel;
                    ev.scrollDX = static_cast<float>(e.wheel.x);
                    ev.scrollDY = static_cast<float>(e.wheel.y);
                    push(std::move(ev));
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    ev.type = EventType::MouseClick;
                    ev.clickX = e.button.x;
                    ev.clickY = e.button.y;
                    push(std::move(ev));
                    break;
                case SDL_TEXTINPUT:
                    ev.type = EventType::TextInput;
                    ev.text = e.text.text;
                    push(std::move(ev));
                    break;
                case SDL_KEYDOWN:
                    ev.type = EventType::KeyDown;
                    ev.key = static_cast<int>(e.key.keysym.sym);
                    ev.ctrl = (e.key.keysym.mod & KMOD_CTRL) != 0;
                    push(std::move(ev));
                    break;
                default:
                    break;
            }
        }
    }

    void push(Event&& ev) {
        std::lock_guard<std::mutex> lock(mu);
        events.push_back(std::move(ev));
    }

    void loop(Window* owner) {
        for (;;) {
            // 1) work handed over by the owner thread (create / show / hide / title)
            for (;;) {
                std::function<void()> task;
                {
                    std::lock_guard<std::mutex> lock(mu);
                    if (tasks.empty()) break;
                    task = std::move(tasks.front());
                    tasks.pop_front();
                }
                task();
            }
            {
                std::lock_guard<std::mutex> lock(mu);
                if (stop) break;
            }
            // 2) OS events (expose/resize/input). Wake at least every 8 ms so new
            //    tasks and new frames are picked up quickly.
            SDL_Event first;
            if (window && SDL_WaitEventTimeout(&first, 8)) {
                SDL_PushEvent(&first);   // put it back; pump() drains the whole queue uniformly
            } else if (!window) {
                std::unique_lock<std::mutex> lock(mu);
                cv.wait_for(lock, std::chrono::milliseconds(8), [&] { return stop || !tasks.empty(); });
            }
            if (window) pump(owner);
            // 3) a frame arrived from present()
            bool needBlit;
            { std::lock_guard<std::mutex> lock(mu); needBlit = dirty; }
            if (needBlit) blit();
        }
        // Tear down on the same thread that created everything.
        if (window) { SDL_StopTextInput(); SDL_DestroyWindow(window); window = nullptr; }
        if (sdlUp) { SDL_QuitSubSystem(SDL_INIT_VIDEO); sdlUp = false; }
    }

    // Run `fn` on the UI thread and wait for its result.
    template <class R>
    R call(std::function<R()> fn) {
        auto job = std::make_shared<std::packaged_task<R()>>(std::move(fn));
        std::future<R> fut = job->get_future();
        {
            std::lock_guard<std::mutex> lock(mu);
            tasks.push_back([job] { (*job)(); });
        }
        cv.notify_one();
        return fut.get();
    }
};

Window::Window() : impl_(new Impl()) {}

Window::~Window() {
    if (!impl_) return;
    if (impl_->thread.joinable()) {
        { std::lock_guard<std::mutex> lock(impl_->mu); impl_->stop = true; }
        impl_->cv.notify_one();
        impl_->thread.join();
    }
    delete impl_;
}

bool Window::create(int w, int h, const std::string& title, bool visible) {
    if (!impl_->thread.joinable()) {
        // A click that merely focuses the window must still reach the app, or the
        // first click on the search box is swallowed and typing "does nothing".
        SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
        SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
        impl_->thread = std::thread([this] { impl_->loop(this); });
    }
    Impl* impl = impl_;
    std::string err = impl->call<std::string>([impl, w, h, title]() -> std::string {
        if (!impl->sdlUp) {
            if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return std::string("!") + SDL_GetError();
            impl->sdlUp = true;
        }
        if (impl->window) { SDL_DestroyWindow(impl->window); impl->window = nullptr; }
        // Always created hidden first; see Ghostful Mode.
        impl->window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h,
                                        SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
        if (!impl->window) return std::string("!") + SDL_GetError();
        SDL_StartTextInput();
        impl->visible.store(false);
        return {};
    });
    if (!err.empty()) { lastError_ = err.substr(1); return false; }
    w_.store(w);
    h_.store(h);
    visible_.store(false);
    if (visible) setVisible(true);
    return true;
}

void Window::setVisible(bool visible) {
    visible_.store(visible);
    if (!impl_->thread.joinable()) return;
    Impl* impl = impl_;
    impl->call<int>([impl, visible]() -> int {
        impl->visible.store(visible);
        if (!impl->window) return 0;
        if (visible) {
            SDL_ShowWindow(impl->window);
            SDL_RaiseWindow(impl->window);   // ask for keyboard focus so typing works without a first click
            SDL_StartTextInput();
            impl->blit();                    // paint before the first screen update, never an empty window
        } else {
            SDL_HideWindow(impl->window);
        }
        return 0;
    });
}

void Window::setTitle(const std::string& title) {
    if (!impl_->thread.joinable()) return;
    Impl* impl = impl_;
    impl->call<int>([impl, title]() -> int {
        if (impl->window) SDL_SetWindowTitle(impl->window, title.c_str());
        return 0;
    });
}

void Window::present(const lucyn::paint::Framebuffer& fb) {
    if (!impl_->thread.joinable() || fb.w <= 0 || fb.h <= 0) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->frame = fb;
        impl_->haveFrame = true;
        impl_->dirty = true;
    }
    impl_->cv.notify_one();
}

std::vector<Event> Window::pollEvents() {
    std::vector<Event> out;
    if (!impl_->thread.joinable()) return out;
    std::lock_guard<std::mutex> lock(impl_->mu);
    out.swap(impl_->events);
    return out;
}

}  // namespace lucyn::ui
