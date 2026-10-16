# LucyN root: it only wires modules together. Each module lives in modules/<name>/,
# has its own CMakeLists.txt, and also builds standalone:
#     cmake -S modules/profile -B build-profile
cmake_minimum_required(VERSION 3.16)
project(LucyN VERSION 0.1.0 LANGUAGES CXX)

option(LUCYN_BUILD_TESTS "Build module tests" ON)
option(LUCYN_BUILD_TOOLS "Build module command-line tools" ON)

# One switch per module. Turn off what you don't need (e.g. no libcurl on this box).
option(LUCYN_MODULE_CORE    "Core module (nothing-data paths)"      ON)
option(LUCYN_MODULE_LOGGER "Logger module (engine diagnostics)" ON)
option(LUCYN_MODULE_PROFILE "Profile module (identity + switching)" ON)
option(LUCYN_MODULE_NET     "Network module (libcurl wrapper)"      ON)
option(LUCYN_MODULE_DOM     "DOM module (HTML -> tree, needs libgumbo-dev)" ON)
option(LUCYN_MODULE_SHELL   "Shell module (open a site inside the shell, inject scripts, snapshots)" ON)
option(LUCYN_MODULE_SECURITY "Security module (NULLBYTE: HMAC-SHA256 seal for captured traffic)" ON)
option(LUCYN_MODULE_SERVER   "Server module (TinyElysia on vendored Mongoose + Moonbug capture dashboard)" ON)
option(LUCYN_MODULE_JS      "JS module (QuickJS + DOM bridge: the real script host for the shell)" ON)
option(LUCYN_MODULE_PAINT   "Paint module (display list + SoftwarePainter; stb_truetype/stb_image, no SDL)" ON)
option(LUCYN_MODULE_LAYOUT  "Layout module (block/inline box tree -> display list; needs dom + paint)" ON)
option(LUCYN_MODULE_LNM     "LNM interpreter (lnminterpreter/: parser + styled-DOM compiler + view switching; needs dom + paint + layout)" ON)
option(LUCYN_MODULE_SEARCH  "Search provider registry (free web search URL templates)" ON)
option(LUCYN_MODULE_UI      "UI module (SDL2 window + Ghostful Mode; builds only if SDL2 is found)" ON)
option(LUCYN_LITEHTML       "Vendor litehtml for full CSS colour/image rendering (SESSION17+)" ON)

# All tools land in build/bin, so they share ONE nothing-data folder (build/bin/nothing-data).
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

enable_testing()

# Vendor litehtml + bundled gumbo FIRST — dom module falls back to lucyn::gumbo
# if libgumbo-dev is not installed on the host.
if(LUCYN_LITEHTML AND LUCYN_MODULE_LAYOUT AND LUCYN_MODULE_DOM AND LUCYN_MODULE_PAINT)
  if(NOT TARGET litehtml)
    add_subdirectory(third_party/litehtml)
  endif()
endif()

if(LUCYN_MODULE_CORE)
  add_subdirectory(modules/core)       # first: profile and shell use it
endif()

if(LUCYN_MODULE_SEARCH)
  add_subdirectory(modules/search)
endif()

if(LUCYN_MODULE_LOGGER)
  add_subdirectory(modules/logger)
endif()
add_subdirectory(modules/adblock)

if(LUCYN_MODULE_PROFILE)
  add_subdirectory(modules/profile)
endif()
if(LUCYN_MODULE_SECURITY AND LUCYN_MODULE_CORE)
  add_subdirectory(modules/security)  # NULLBYTE; only needs core
endif()
if(LUCYN_MODULE_NET)
  add_subdirectory(modules/net)       # needs profile; pulls it in itself if you switched it off
endif()

# M5: TinyElysia + capture dashboard. Needs net (Moonbug) and security (NULLBYTE); Mongoose is vendored (GPL-2.0 or commercial).
if(LUCYN_MODULE_SERVER AND LUCYN_MODULE_NET AND LUCYN_MODULE_SECURITY)
  add_subdirectory(modules/server)
endif()

if(LUCYN_MODULE_DOM)
  add_subdirectory(modules/dom)       # after net, so lucyn-dom gets --url when net is on
endif()

if(LUCYN_MODULE_SHELL AND LUCYN_MODULE_DOM)
  add_subdirectory(modules/shell)     # after dom (needs it) and net (lucyn-shell gets URL fetching when net is on)
endif()

if(LUCYN_MODULE_JS AND LUCYN_MODULE_SHELL AND LUCYN_MODULE_PROFILE)
  add_subdirectory(modules/js)        # after shell (implements its ScriptHost) and profile (navigator/screen come from the persona)
  # js depends on shell, so shell cannot link js itself: the real host is plugged into lucyn-shell here, at the root.
  if(TARGET lucyn-shell)
    target_link_libraries(lucyn-shell PRIVATE lucyn::js)
    target_compile_definitions(lucyn-shell PRIVATE LUCYN_SHELL_HAVE_JS=1)
  endif()
endif()

# M6: paint (display list + SoftwarePainter, no SDL) -> layout (box tree, needs
# dom + paint) -> ui (SDL2 window + Ghostful Mode, needs paint; optional: builds
# only if SDL2 is found). Session 11, see SESSION11.md.
if(LUCYN_MODULE_PAINT)
  add_subdirectory(modules/paint)
endif()
if(LUCYN_MODULE_LAYOUT AND LUCYN_MODULE_DOM AND LUCYN_MODULE_PAINT)
  add_subdirectory(modules/layout)
  # paint's CMakeLists.txt runs before lucyn::layout exists, so it cannot wire
  # lucyn-paint's --file flag itself (same reason js is wired into lucyn-shell
  # below, not inside shell's own CMakeLists.txt). Done here instead, now that
  # both targets exist. modules/layout/CMakeLists.txt does the same thing for
  # its own standalone build.
  if(TARGET lucyn-paint AND TARGET lucyn::dom)
    target_link_libraries(lucyn-paint PRIVATE lucyn::dom lucyn::layout)
    target_compile_definitions(lucyn-paint PRIVATE LUCYN_PAINT_HAVE_LAYOUT_DOM=1)
  endif()
endif()
# Session 12: LNM. Lives in lnminterpreter/ (next to modules/, the author's choice); it is shaped like a module
# (include/lucyn/lnm, src, tools, tests, standalone CMakeLists) so moving it to modules/lnm/ is a rename.
if(LUCYN_MODULE_LNM AND TARGET lucyn::layout)
  add_subdirectory(lnminterpreter)
endif()
if(LUCYN_MODULE_UI AND LUCYN_MODULE_PAINT)
  add_subdirectory(modules/ui)   # silently skips itself (message + return()) if SDL2 is missing
endif()

# M4: controller module (JSON command loop) + lucyn app binary (the front door).
option(LUCYN_MODULE_CONTROLLER "Controller module (M4 JSON command loop over stdio)" ON)
if(LUCYN_MODULE_CONTROLLER
   AND LUCYN_MODULE_CORE
   AND LUCYN_MODULE_PROFILE
   AND LUCYN_MODULE_NET
   AND LUCYN_MODULE_DOM
   AND LUCYN_MODULE_SHELL
   AND LUCYN_MODULE_JS
   AND LUCYN_MODULE_SECURITY
   AND LUCYN_MODULE_SERVER)
  add_subdirectory(modules/controller)
  add_subdirectory(apps/lucyn)

  # screenshot (needs dom + layout + paint) and window (needs ui) are wired
  # into the controller library here, same "both targets must already exist"
  # reason as the js-into-shell wiring above (controller.cpp is compiled
  # inside the lucyn_controller static library, not the lucyn executable, so
  # that's the target that needs the link + the define). Each is
  # independently optional: a controller built without LUCYN_MODULE_LAYOUT/
  # PAINT simply doesn't get "screenshot"; without ui, "window" still exists
  # as a command but replies ok:false with a clear message (see
  # modules/controller/src/controller.cpp).
  if(TARGET lucyn_controller AND TARGET lucyn::layout AND TARGET lucyn::paint)
    target_link_libraries(lucyn_controller PRIVATE lucyn::layout lucyn::paint)
    target_compile_definitions(lucyn_controller PRIVATE LUCYN_CONTROLLER_HAVE_PAINT=1)
  endif()
  if(TARGET lucyn_controller AND TARGET lucyn::ui)
    target_link_libraries(lucyn_controller PRIVATE lucyn::ui)
    target_compile_definitions(lucyn_controller PRIVATE LUCYN_CONTROLLER_HAVE_UI=1)
  endif()
  if(TARGET lucyn_controller AND TARGET lucyn::lnm)
    target_link_libraries(lucyn_controller PRIVATE lucyn::lnm)
    target_compile_definitions(lucyn_controller PRIVATE LUCYN_CONTROLLER_HAVE_LNM=1)
  endif()
endif()
