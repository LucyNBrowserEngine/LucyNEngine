// modules/cef_host/src/preview_window.cpp — see preview_window.h for the idea.
//
// Built with X11 when the headers are there (CMake defines LUCYN_HAVE_X11);
// otherwise this file is the no-op version and the engine runs headless.
#include "preview_window.h"

#include <atomic>

#if defined(LUCYN_HAVE_X11)
// System headers FIRST, outside the namespace: Xlib declares extern "C"
// symbols and pulls in libc headers, which must never land inside one of ours.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <vector>
#endif  // LUCYN_HAVE_X11

namespace lucyn::cef_host::preview {

namespace {
std::atomic<bool> g_enabled{false};
}  // namespace

void setEnabled(bool on) { g_enabled.store(on); }
bool enabled() { return g_enabled.load(); }

#if !defined(LUCYN_HAVE_X11)

// No X11 available at build time: everything is a no-op and start() says "no".
bool start(const std::string&, int, int) { return false; }
void present(const void*, int, int) {}
void setTitle(const std::string&) {}
void stop() {}
void setInputHandler(std::function<void(const InputEvent&)>) {}

#else  // LUCYN_HAVE_X11

namespace {

struct Frame {
    std::vector<unsigned char> px;
    int w = 0, h = 0;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::atomic<bool> g_started{false};  // start()/stop() bookkeeping
bool g_ready = false;   // the thread finished its first attempt (guarded by g_mu)
bool g_ok = false;      // ... and it succeeded (guarded by g_mu)
std::function<void(const InputEvent&)> g_input;  // set before start (guarded by g_mu)
int g_buttons = 0;      // mouse buttons currently held (viewer thread only)
Frame g_pending;          // newest frame not yet drawn
bool g_havePending = false;
std::string g_pendingTitle;
std::atomic<bool> g_stop{false};
int g_wake[2] = {-1, -1};  // self-pipe: wakes poll() when work arrives
std::thread g_thread;

// The preview thread's own copy of the last frame, so an Expose can repaint.
Frame g_last;

void wake() {
    if (g_wake[1] >= 0) {
        const char b = 1;
        const ssize_t n = ::write(g_wake[1], &b, 1);
        (void)n;   // EAGAIN just means a wake is already queued
    }
}

// ---- input translation: X11 -> the engine's InputEvent -----------------------
// cef_event_flags_t bits, spelled out so this file stays CEF-free:
// SHIFT 1<<1  CONTROL 1<<2  ALT 1<<3  LEFT 1<<4  MIDDLE 1<<5  RIGHT 1<<6  COMMAND 1<<7
unsigned int stateToModifiers(unsigned int state) {
    unsigned int m = 0;
    if (state & ShiftMask)   m |= 1u << 1;
    if (state & ControlMask) m |= 1u << 2;
    if (state & Mod1Mask)    m |= 1u << 3;
    if (state & Mod4Mask)    m |= 1u << 7;   // Super as the command key
    return m;
}

unsigned int buttonFlag(int button) {
    switch (button) {
        case 1:  return 1u << 4;   // left
        case 2:  return 1u << 5;   // middle
        case 3:  return 1u << 6;   // right
        default: return 0;
    }
}

// X11 button number -> 1/2/3 (0 for anything exotic: we do not forward it).
int buttonNumber(unsigned int button) {
    return (button >= 1 && button <= 3) ? static_cast<int>(button) : 0;
}

// X keysym -> cef windows_key_code (VK_*). 0 = no useful mapping; the CHAR
// event still carries the text, which is what typing actually needs.
unsigned int keySymToVk(KeySym sym) {
    switch (sym) {
        case XK_Return: case XK_KP_Enter:     return 0x0D;
        case XK_BackSpace:                    return 0x08;
        case XK_Tab: case XK_ISO_Left_Tab:    return 0x09;
        case XK_Escape:                       return 0x1B;
        case XK_Home:                         return 0x24;
        case XK_End:                          return 0x23;
        case XK_Prior:                        return 0x21;   // PageUp
        case XK_Next:                         return 0x22;   // PageDown
        case XK_Left:                         return 0x25;
        case XK_Up:                           return 0x26;
        case XK_Right:                        return 0x27;
        case XK_Down:                         return 0x28;
        case XK_Insert:                       return 0x2D;
        case XK_Delete:                       return 0x2E;
        case XK_Shift_L: case XK_Shift_R:     return 0x10;
        case XK_Control_L: case XK_Control_R: return 0x11;
        case XK_Alt_L: case XK_Alt_R:         return 0x12;
        case XK_Super_L:                      return 0x5B;
        case XK_Super_R:                      return 0x5C;
        case XK_Caps_Lock:                    return 0x14;
        case XK_space:                        return 0x20;
        default: break;
    }
    if (sym >= XK_F1 && sym <= XK_F12) return static_cast<unsigned int>(0x70 + (sym - XK_F1));
    if (sym >= XK_a && sym <= XK_z)    return static_cast<unsigned int>('A' + (sym - XK_a));
    if (sym >= XK_0 && sym <= XK_9)    return static_cast<unsigned int>(sym);   // VK_0..VK_9
    // Punctuation -> the Windows OEM virtual keys, as cefclient's GTK keyboard
    // map does. Returning the raw keysym here is WRONG: '.' is 0x2e, which
    // collides with VK_DELETE, and Chromium then drops the CHAR event (type
    // silently fails while keydowns keep arriving). Shifted partners (":?<>~)
    // map to the same physical key; `character` carries the shifted glyph.
    switch (sym) {
        case '-': case '_': return 0xBD;   // VK_OEM_MINUS
        case '=': case '+': return 0xBB;   // VK_OEM_PLUS
        case '[': case '{': return 0xDB;   // VK_OEM_4
        case ']': case '}': return 0xDD;   // VK_OEM_6
        case '\\': case '|': return 0xDC;   // VK_OEM_5
        case ';':  case ':': return 0xBA;   // VK_OEM_1
        case '\'': case '"': return 0xDE;   // VK_OEM_7
        case ',':  case '<': return 0xBC;   // VK_OEM_COMMA
        case '.':  case '>': return 0xBE;   // VK_OEM_PERIOD
        case '/':  case '?': return 0xBF;   // VK_OEM_2
        case '`':  case '~': return 0xC0;   // VK_OEM_3
        default: break;
    }
    if (sym >= 0x20 && sym < 0x7F)     return static_cast<unsigned int>(sym);
    return 0;
}

void fire(const InputEvent& e) {
    std::function<void(const InputEvent&)> h;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        h = g_input;
    }
    if (h) h(e);
}

// Draw one frame. XImage is built per call: Xlib owns nothing here, so the
// buffer is freed by hand (XDestroyImage would free it too if we let it).
void draw(Display* dpy, Window win, int screen, const Frame& f) {
    if (f.px.empty() || f.w <= 0 || f.h <= 0) return;
    unsigned char* data = static_cast<unsigned char*>(std::malloc(f.px.size()));
    if (!data) return;
    std::memcpy(data, f.px.data(), f.px.size());

    XImage* img = XCreateImage(dpy, DefaultVisual(dpy, screen), DefaultDepth(dpy, screen),
                               ZPixmap, 0, reinterpret_cast<char*>(data),
                               f.w, f.h, 32, 0);
    if (!img) {
        std::free(data);
        return;
    }
    XPutImage(dpy, win, DefaultGC(dpy, screen), img, 0, 0, 0, 0, f.w, f.h);
    img->data = nullptr;      // detach before destroying: we free `data` ourselves
    XDestroyImage(img);
    std::free(data);
}

// One thread owns the Display for its whole life (Xlib without XInitThreads is
// single-thread-per-connection, and that is all we need).
void threadMain(int reqW, int reqH, std::string title) {
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_ok = false;
        g_ready = true;
        g_cv.notify_all();
        return;
    }
    const int screen = DefaultScreen(dpy);
    const int w = reqW > 0 ? reqW : 1280;
    const int h = reqH > 0 ? reqH : 800;
    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, w, h, 0,
                                     BlackPixel(dpy, screen), BlackPixel(dpy, screen));
    XStoreName(dpy, win, title.c_str());
    // Everything we forward into the page: motion, buttons, the wheel, keys
    // and focus. Without these masks the window would be a pure viewer.
    XSelectInput(dpy, win, ExposureMask | StructureNotifyMask | ButtonPressMask |
                              ButtonReleaseMask | PointerMotionMask | KeyPressMask |
                              KeyReleaseMask | FocusChangeMask);
    Atom wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wmDelete, 1);
    XMapWindow(dpy, win);
    XFlush(dpy);

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_ok = true;
        g_ready = true;
        g_cv.notify_all();
    }

    const int xfd = ConnectionNumber(dpy);
    while (!g_stop.load()) {
        // Take the newest frame/title (latest wins — a slow window drops frames
        // instead of queueing them behind CEF's paint thread).
        Frame frame;
        bool haveFrame = false, haveTitle = false;
        std::string newTitle;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_havePending) {
                frame = std::move(g_pending);
                g_pending = Frame{};
                g_havePending = false;
                haveFrame = true;
            }
            if (!g_pendingTitle.empty()) {
                newTitle = g_pendingTitle;
                g_pendingTitle.clear();
                haveTitle = true;
            }
        }
        if (haveTitle) XStoreName(dpy, win, newTitle.c_str());
        if (haveFrame) {
            g_last = frame;
            draw(dpy, win, screen, frame);
            XFlush(dpy);
        }

        pollfd pfds[2];
        pfds[0].fd = xfd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = g_wake[0];
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;
        if (::poll(pfds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfds[1].revents) {   // drain the wake pipe
            char buf[64];
            while (::read(g_wake[0], buf, sizeof buf) > 0) {}
        }
        if (g_stop.load()) break;

        if (pfds[0].revents & POLLIN) {
            while (XPending(dpy)) {
                XEvent ev;
                XNextEvent(dpy, &ev);
                if (ev.type == Expose) {
                    // Repaint what we already have over the freshly exposed area.
                    XSetForeground(dpy, DefaultGC(dpy, screen), BlackPixel(dpy, screen));
                    XFillRectangle(dpy, win, DefaultGC(dpy, screen), 0, 0,
                                   (unsigned)w, (unsigned)h);
                    if (!g_last.px.empty()) draw(dpy, win, screen, g_last);
                    XFlush(dpy);
                } else if (ev.type == ButtonPress) {
                    const XButtonEvent& b = ev.xbutton;
                    XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
                    InputEvent ie;
                    if (b.button == 4 || b.button == 5) {
                        // X11 wheel: 4 = up, 5 = down. Positive = wheel up,
                        // the Windows convention CEF's SendMouseWheelEvent uses.
                        ie.kind = InputEvent::Wheel;
                        ie.deltaY = (b.button == 4) ? 120 : -120;
                    } else {
                        const int btn = buttonNumber(b.button);
                        if (!btn) continue;
                        g_buttons |= buttonFlag(btn);
                        ie.kind = InputEvent::Down;
                        ie.button = btn;
                    }
                    ie.x = b.x;
                    ie.y = b.y;
                    ie.modifiers = stateToModifiers(b.state) | g_buttons;
                    fire(ie);
                } else if (ev.type == ButtonRelease) {
                    const XButtonEvent& b = ev.xbutton;
                    if (b.button == 4 || b.button == 5) {
                        // wheel releases are not forwarded (CEF takes the delta
                        // straight from the press)
                    } else {
                        const int btn = buttonNumber(b.button);
                        if (btn) {
                            g_buttons &= ~buttonFlag(btn);
                            InputEvent ie;
                            ie.kind = InputEvent::Up;
                            ie.button = btn;
                            ie.x = b.x;
                            ie.y = b.y;
                            ie.modifiers = stateToModifiers(b.state) | g_buttons;
                            fire(ie);
                        }
                    }
                } else if (ev.type == MotionNotify) {
                    InputEvent ie;
                    ie.kind = InputEvent::Move;
                    ie.x = ev.xmotion.x;
                    ie.y = ev.xmotion.y;
                    ie.modifiers = stateToModifiers(ev.xmotion.state) | g_buttons;
                    fire(ie);
                } else if (ev.type == KeyPress || ev.type == KeyRelease) {
                    XKeyEvent& k = ev.xkey;
                    KeySym sym = NoSymbol;
                    char buf[64];
                    int n = 0;
                    if (ev.type == KeyPress) {
                        XComposeStatus compose{};
                        n = XLookupString(&k, buf, static_cast<int>(sizeof(buf) - 1), &sym, &compose);
                    } else {
                        sym = XLookupKeysym(&k, 0);
                    }
                    InputEvent ie;
                    ie.kind = (ev.type == KeyPress) ? InputEvent::KeyDown : InputEvent::KeyUp;
                    ie.keyCode = keySymToVk(sym);
                    ie.nativeKey = static_cast<int>(k.keycode);
                    ie.modifiers = stateToModifiers(k.state) | g_buttons;
                    if (n > 0) ie.text.assign(buf, static_cast<size_t>(n));
                    fire(ie);
                    if (std::getenv("LUCYN_DEBUG_INPUT"))
                        std::fprintf(stderr,
                                     "[input-dbg] X11 %s keysym=0x%lx vk=0x%x keycode=%u n=%d text=\"%.16s\"\n",
                                     ev.type == KeyPress ? "KeyPress" : "KeyRelease",
                                     static_cast<unsigned long>(sym), ie.keyCode,
                                     static_cast<unsigned int>(ie.nativeKey), n, ie.text.c_str());
                } else if (ev.type == FocusIn || ev.type == FocusOut) {
                    InputEvent ie;
                    ie.kind = InputEvent::Focus;
                    ie.focus = (ev.type == FocusIn);
                    fire(ie);
                    if (std::getenv("LUCYN_DEBUG_INPUT"))
                        std::fprintf(stderr, "[input-dbg] X11 %s\n",
                                     ev.type == FocusIn ? "FocusIn" : "FocusOut");
                } else if (ev.type == ClientMessage &&
                           static_cast<Atom>(ev.xclient.data.l[0]) == wmDelete) {
                    // User closed the window: stop showing frames, but let the
                    // engine carry on (headless from here on).
                    g_stop.store(true);
                }
            }
        }
    }

    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    g_last = Frame{};
}

}  // namespace

void setInputHandler(std::function<void(const InputEvent&)> h) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_input = std::move(h);
}

bool start(const std::string& title, int width, int height) {
    if (!g_enabled.load()) return false;
    std::unique_lock<std::mutex> lk(g_mu);
    if (g_started.load()) return g_ok;
    g_started.store(true);
    g_stop.store(false);
    g_ready = false;
    g_ok = false;
    if (::pipe(g_wake) != 0) {
        g_wake[0] = g_wake[1] = -1;
    } else {
        // BOTH ends non-blocking. The read end matters most: the drain loop
        // (while (read(...) > 0)) would block forever on an empty pipe the
        // moment it caught up — which hung the process on quit.
        for (int fd : {g_wake[0], g_wake[1]}) {
            const int fl = ::fcntl(fd, F_GETFL, 0);
            if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        }
    }
    lk.unlock();
    g_thread = std::thread(threadMain, width, height, title);
    lk.lock();
    g_cv.wait(lk, [] { return g_ready; });
    return g_ok;
}

void present(const void* bgra, int width, int height) {
    if (!g_started.load() || !g_enabled.load() || bgra == nullptr || width <= 0 || height <= 0) return;
    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_ok) return;
        g_pending.px.resize(bytes);
        std::memcpy(g_pending.px.data(), bgra, bytes);
        g_pending.w = width;
        g_pending.h = height;
        g_havePending = true;
    }
    wake();
}

void setTitle(const std::string& title) {
    if (!g_started.load()) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_ok) return;
        g_pendingTitle = title;
    }
    wake();
}

void stop() {
    std::thread t;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_started.load()) return;
        g_started.store(false);
        t = std::move(g_thread);
    }
    g_stop.store(true);
    wake();
    if (t.joinable()) t.join();
    if (g_wake[0] >= 0) { ::close(g_wake[0]); g_wake[0] = -1; }
    if (g_wake[1] >= 0) { ::close(g_wake[1]); g_wake[1] = -1; }
    std::lock_guard<std::mutex> lk(g_mu);
    g_ready = false;
    g_ok = false;
    g_pending = Frame{};
    g_havePending = false;
    g_pendingTitle.clear();
}

#endif  // LUCYN_HAVE_X11

}  // namespace lucyn::cef_host::preview
