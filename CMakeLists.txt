# LucyN root: it only wires modules together. Each module lives in modules/<name>/,
# has its own CMakeLists.txt, and also builds standalone:
#     cmake -S modules/net -B build-net
cmake_minimum_required(VERSION 3.16)
# C is enabled because CEF's cmake machinery (cef_variables.cmake) runs
# CHECK_C_COMPILER_FLAG probes when it is included.
project(LucyN VERSION 0.1.0 LANGUAGES C CXX)

option(LUCYN_BUILD_TESTS "Build module tests" ON)
option(LUCYN_BUILD_TOOLS "Build module command-line tools" ON)

# One switch per module. Turn off what you don't need (e.g. no libcurl on this box).
option(LUCYN_MODULE_CORE    "Core module (nothing-data paths)"      ON)
option(LUCYN_MODULE_LOGGER "Logger module (engine diagnostics)" ON)
option(LUCYN_MODULE_NET     "Network module (Moonbug capture wiretap; Chromium/CEF is the HTTP stack now)" ON)
option(LUCYN_MODULE_SECURITY "Security module (NULLBYTE: HMAC-SHA256 seal for captured traffic)" ON)
option(LUCYN_MODULE_SERVER   "Server module (TinyElysia on vendored Mongoose + Moonbug capture dashboard)" ON)
option(LUCYN_MODULE_SEARCH  "Search provider registry (free web search URL templates)" ON)

# All tools land in build/bin, so they share ONE nothing-data folder (build/bin/nothing-data).
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
# Multi-config generators (Visual Studio on Windows) would otherwise append a per-config
# subfolder (build/bin/Release), which would separate lucyn.exe from the CEF runtime files
# lucyn_cef_copy_runtime() copies into build/bin — and break the release packaging step,
# which copies build/bin/lucyn.exe. Pin every configuration to the same flat folder.
foreach(_lucyn_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_lucyn_cfg})
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_lucyn_cfg} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
  endif()
endforeach()

enable_testing()

# Session 19: CEF is the new engine (Chromium does the HTTP, JS, DOM and rendering).
# The binary distribution is unpacked at third_party/cef/; cmake/FindCEF.cmake
# loads the distro's own CMake machinery, builds libcef_dll_wrapper and defines
# the lucyn::cef interface target. Nothing consumes CEF yet beyond linking it
# into the lucyn binary (main.cpp's CefExecuteProcess subprocess check); no old
# module is removed until the CEF build is green.
option(LUCYN_WITH_CEF "Link the CEF engine (Chromium Embedded Framework) into the lucyn binary" ON)
if(LUCYN_WITH_CEF)
  include(cmake/FindCEF.cmake)
endif()

if(LUCYN_MODULE_CORE)
  add_subdirectory(modules/core)       # first: everything else uses it
endif()

if(LUCYN_MODULE_SEARCH)
  add_subdirectory(modules/search)
endif()

if(LUCYN_MODULE_LOGGER)
  add_subdirectory(modules/logger)
endif()
add_subdirectory(modules/adblock)

if(LUCYN_MODULE_SECURITY AND LUCYN_MODULE_CORE)
  add_subdirectory(modules/security)  # NULLBYTE; only needs core
endif()
if(LUCYN_MODULE_NET)
  add_subdirectory(modules/net)       # Moonbug only (self-contained, no deps)
endif()

# M5: TinyElysia + capture dashboard. Needs net (Moonbug) and security (NULLBYTE); Mongoose is vendored (GPL-2.0 or commercial).
if(LUCYN_MODULE_SERVER AND LUCYN_MODULE_NET AND LUCYN_MODULE_SECURITY)
  add_subdirectory(modules/server)
endif()

# Session 19: the old engine modules (dom, shell, js, layout, paint) are gone — CEF is the
# engine now (see engine-history/SESSION19_CEF_MIGRATION.md "What DIES").
# Session 19 steps 2-4: the CEF host module (initCef / shutdownCef / createInstance).
# Needs lucyn::cef (from cmake/FindCEF.cmake, included near the top) and lucyn::core
# (added near the top). Guarded by both switches; the module's own CMakeLists is a
# no-op if LUCYN_WITH_CEF is OFF. lucyn_cef_copy_runtime() is called here too so the
# module's smoke tool + ctest find libcef.so/icudtl.dat without depending on the app.
option(LUCYN_MODULE_CEF_HOST "CEF host module (initCef/createInstance over CEF)" ON)
if(LUCYN_MODULE_CEF_HOST AND LUCYN_WITH_CEF)
  add_subdirectory(modules/cef_host)
  lucyn_cef_copy_runtime()
endif()

# M4: controller module (JSON command loop) + lucyn app binary (the front door).
option(LUCYN_MODULE_CONTROLLER "Controller module (M4 JSON command loop over stdio)" ON)
if(LUCYN_MODULE_CONTROLLER
   AND LUCYN_MODULE_CORE
   AND LUCYN_MODULE_NET
   AND LUCYN_MODULE_SECURITY
   AND LUCYN_MODULE_SERVER)
  add_subdirectory(modules/controller)
  add_subdirectory(apps/lucyn)

  # Session 19: CEF is the engine — the controller's screenshot (and every other
  # engine command) runs through Chromium, so the old layout/paint wiring is gone.

  # Session 19 step 1: CEF is linked into the lucyn binary (root wires it, same
  # "both targets must exist" rule as the surviving module wiring above). CEF 156
  # headers require C++20, so the app target bumps its standard; the static
  # modules stay on their own C++17. lucyn_cef_copy_runtime() drops libcef.so,
  # icudtl.dat, *.pak and locales/ next to bin/ — CEF looks for them next to
  # the executable at startup.
  if(LUCYN_WITH_CEF AND TARGET lucyn AND TARGET lucyn::cef)
    # lucyn::cef_host too: main.cpp calls executeCefProcess/initCef/shutdownCef
    # and the Controller drives a BrowserInstance (Session 19 steps 5-6).
    target_link_libraries(lucyn PRIVATE lucyn::cef lucyn::cef_host)
    set_target_properties(lucyn PROPERTIES CXX_STANDARD 20)
    # Deterministic rpath, $ORIGIN FIRST:
    #   $ORIGIN (= build/bin)  -> where lucyn_cef_copy_runtime put libcef.so +
    #                             icudtl.dat/*.pak/locales, so CEF resolves its
    #                             runtime files next to the binary (it looks beside
    #                             wherever libcef.so loaded from). For a normal
    #                             run from build/bin this always wins -> ICU works.
    #   build/bin (fallback)   -> the COMPLETE runtime (libcef.so + icudtl.dat side
    #                             by side), used when the binary is copied elsewhere
    #                             (the cli_download test copies lucyn into a temp
    #                             dir). ICU data must sit next to the loaded
    #                             libcef.so, so pointing here instead of cef/Release
    #                             keeps a relocated binary from dying with the
    #                             cryptic "Invalid file descriptor to ICU data".
    #   cef/Release (last)     -> lets the binary at least LOAD if build/bin itself
    #                             is gone.
    # Set explicitly so we never depend on link order; for a build/bin run $ORIGIN
    # always wins over both fallbacks. macOS has no $ORIGIN (ELF-ism) — there the
    # sibling directory is @loader_path, and the "framework" entry is the distro's
    # Chromium Embedded Framework.framework (libcef IS that framework's binary).
    if(APPLE)
      set(_lucyn_rpath "@loader_path;${CMAKE_RUNTIME_OUTPUT_DIRECTORY};${CEF_BINARY_DIR_RELEASE}")
    else()
      set(_lucyn_rpath "$ORIGIN;${CMAKE_RUNTIME_OUTPUT_DIRECTORY};${CEF_BINARY_DIR_RELEASE}")
    endif()
    set_target_properties(lucyn PROPERTIES
      INSTALL_RPATH "${_lucyn_rpath}"
      BUILD_WITH_INSTALL_RPATH TRUE)
    lucyn_cef_copy_runtime()
  endif()
endif()
