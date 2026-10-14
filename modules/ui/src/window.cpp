#include <lucyn/ui/window.h>

#include <SDL.h>

#include <atomic>

namespace lucyn::ui {

bool providingUI() { return true; }

namespace {
std::atomic<int> g_sdlInitCount{0};
}

struct Window::Impl {
    SDL_Window* window = nullptr;
};

Window::Window() : impl_(new Impl()) {}

Window::~Window() {
    if (impl_) {
        if (impl_->window) SDL_DestroyWindow(impl_->window);
        delete impl_;
    }
    if (sdlOwned_ && g_sdlInitCount.fetch_sub(1) == 1) {
        // Last Window alive: safe to tear SDL video down. (Single-window
        // engine in practice — one browser window — so refcounting is mostly
        // a formality, but it keeps repeated create()/destroy() in tests honest.)
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
}

bool Window::create(int w, int h, const std::string& title, bool visible) {
    if (!sdlOwned_) {
        if (g_sdlInitCount.fetch_add(1) == 0) {
            if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
                lastError_ = SDL_GetError();
                g_sdlInitCount.fetch_sub(1);
                return false;
            }
        }
        sdlOwned_ = true;
    }
    if (impl_->window) {
        SDL_DestroyWindow(impl_->window);
        impl_->window = nullptr;
    }
    Uint32 flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE;  // always created hidden first; see Ghostful Mode
    impl_->window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, flags);
    if (!impl_->window) {
        lastError_ = SDL_GetError();
        return false;
    }
    w_ = w;
    h_ = h;
    visible_ = false;
    if (visible) setVisible(true);
    return true;
}

void Window::setVisible(bool visible) {
    if (!impl_->window) { visible_ = visible; return; }
    visible_ = visible;
    if (visible_) SDL_ShowWindow(impl_->window);
    else SDL_HideWindow(impl_->window);
}

void Window::setTitle(const std::string& title) {
    if (impl_->window) SDL_SetWindowTitle(impl_->window, title.c_str());
}

void Window::present(const lucyn::paint::Framebuffer& fb) {
    if (!impl_->window || fb.w <= 0 || fb.h <= 0) return;
    // fb.pixels is packed (r<<24)|(g<<16)|(b<<8)|a; in little-endian memory
    // that is byte order A,B,G,R, which is exactly SDL_PIXELFORMAT_ABGR32's
    // guarantee (unlike *_RGBA32, which is endianness-normalised to R,G,B,A
    // — the wrong one for this layout). See framebuffer.h's packRGBA.
    SDL_Surface* src = SDL_CreateRGBSurfaceWithFormatFrom(
        const_cast<uint32_t*>(fb.pixels.data()), fb.w, fb.h, 32, fb.w * 4, SDL_PIXELFORMAT_ABGR32);
    if (!src) { lastError_ = SDL_GetError(); return; }

    SDL_Surface* winSurface = SDL_GetWindowSurface(impl_->window);
    if (winSurface) {
        SDL_BlitScaled(src, nullptr, winSurface, nullptr);
        if (visible_) SDL_UpdateWindowSurface(impl_->window);
        // When hidden, the surface is still updated above (blit happened) so
        // a later setVisible(true) shows real content, not a blank window —
        // Ghostful Mode's whole point.
    }
    SDL_FreeSurface(src);  // does not free fb.pixels: created "From" an external buffer
}

std::vector<Event> Window::pollEvents() {
    std::vector<Event> out;
    if (!impl_->window) return out;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                out.push_back(Event{EventType::Quit});
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_RESIZED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    w_ = e.window.data1;
                    h_ = e.window.data2;
                    Event ev{EventType::Resize};
                    ev.width = w_;
                    ev.height = h_;
                    out.push_back(ev);
                }
                break;
            case SDL_MOUSEWHEEL: {
                Event ev{EventType::MouseWheel};
                ev.scrollDX = static_cast<float>(e.wheel.x);
                ev.scrollDY = static_cast<float>(e.wheel.y);
                out.push_back(ev);
                break;
            }
            case SDL_MOUSEBUTTONDOWN: {
                Event ev{EventType::MouseClick};
                ev.clickX = e.button.x;
                ev.clickY = e.button.y;
                out.push_back(ev);
                break;
            }
            default:
                break;
        }
    }
    return out;
}

}  // namespace lucyn::ui
