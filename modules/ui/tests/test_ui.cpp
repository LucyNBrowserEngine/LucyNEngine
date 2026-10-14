// Runs under SDL_VIDEODRIVER=dummy (set by CMakeLists.txt's add_test ENVIRONMENT,
// and in SESSION11.md for manual runs) so it works in the sandbox with no
// real display. What it CANNOT verify: anything a human eye would judge —
// an actual window appearing on a real desktop, real mouse/keyboard input,
// real resize dragging. See SESSION11.md's "NOT verified" list.
#include <cstdio>
#include <lucyn/paint/framebuffer.h>
#include <lucyn/ui/window.h>

using namespace lucyn::ui;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    // 1. providingUI() is always true, regardless of anything else.
    CHECK(providingUI() == true);

    // 2. create/destroy, hidden by default, resizes, sets title — all against
    // the dummy driver.
    {
        Window w;
        bool ok = w.create(320, 240, "LucyN test window", /*visible=*/false);
        CHECK(ok);
        if (!ok) std::printf("  (SDL error: %s)\n", w.lastError().c_str());
        CHECK(w.visible() == false);  // Ghostful default: hidden
        CHECK(w.width() == 320 && w.height() == 240);

        w.setTitle("Renamed");  // no crash is the check; dummy driver has no real title bar to read back

        w.setVisible(true);
        CHECK(w.visible() == true);
        w.setVisible(false);
        CHECK(w.visible() == false);
    }

    // 3. present() works whether hidden or visible, with a real Framebuffer
    // (not an all-zero one) — no crash, surface accepted.
    {
        Window w;
        CHECK(w.create(64, 64, "present test", false));
        lucyn::paint::Framebuffer fb;
        fb.init(64, 64, lucyn::paint::packRGBA(10, 20, 30, 255));
        w.present(fb);   // hidden: should still accept the surface without error
        w.setVisible(true);
        w.present(fb);   // visible: should also work under the dummy driver
        CHECK(true);
    }

    // 4. pollEvents() on a freshly created window returns an empty (or small)
    // vector without crashing — the dummy driver generates no real events.
    {
        Window w;
        CHECK(w.create(100, 100, "events test", false));
        auto events = w.pollEvents();
        (void)events;
        CHECK(true);
    }

    // 5. A Window that was never created() is inert: pollEvents empty, no crash.
    {
        Window w;
        auto events = w.pollEvents();
        CHECK(events.empty());
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ui: all tests passed (dummy driver — see file header for what this cannot verify)\n");
    return 0;
}
