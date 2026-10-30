# cmake/FindCEF.cmake — locate the CEF binary distribution and set up everything
# LucyN needs to link against it. Session 19 (CEF migration), step 1.
#
# Modeled on the distribution's own third_party/cef/cmake/FindCEF.cmake: it loads
# the distro's cef_variables.cmake + cef_macros.cmake (compiler flags, file lists,
# COPY macros) and then adds the LucyN-specific bits on top:
#
#   CEF_ROOT                 - distro root, defaults to <repo>/third_party/cef
#   libcef_dll_wrapper       - the C++ wrapper library, built from the distro source
#   lucyn::cef               - INTERFACE target: includes, wrapper + libcef.so + rpath
#   lucyn_cef_copy_runtime() - copies Release/ + Resources/ into <bin>/cef/ — ONE
#                              subfolder beside the binaries instead of a pile of
#                              .so/.pak files loose in the same directory as lucyn
#                              (CEF needs icudtl.dat, *.pak, locales/ and libcef.so
#                              findable; rpath $ORIGIN/cef points the loader there)
#
# Usage from the root CMakeLists.txt:
#   include(cmake/FindCEF.cmake)
#   ...
#   add_subdirectory(apps/lucyn)
#   if(TARGET lucyn AND TARGET lucyn::cef)
#     target_link_libraries(lucyn PRIVATE lucyn::cef)
#   endif()
#   lucyn_cef_copy_runtime()

if(TARGET lucyn_cef)
  return()  # already loaded (find modules may be included twice)
endif()

# ---- locate the distribution -------------------------------------------------------------------
if(NOT CEF_ROOT)
  set(CEF_ROOT "${CMAKE_CURRENT_LIST_DIR}/../third_party/cef")
endif()
get_filename_component(CEF_ROOT "${CEF_ROOT}" ABSOLUTE)

if(NOT EXISTS "${CEF_ROOT}/cmake/cef_variables.cmake" OR
   NOT EXISTS "${CEF_ROOT}/libcef_dll/CMakeLists.txt")
  message(FATAL_ERROR
    "CEF binary distribution not found at: ${CEF_ROOT}\n"
    "Expected an unpacked CEF binary distribution (include/, libcef_dll/, cmake/, "
    "Release/, Resources/). Set CEF_ROOT to point at one.")
endif()
if(NOT IS_DIRECTORY "${CEF_ROOT}/Release")
  message(FATAL_ERROR
    "CEF distribution at ${CEF_ROOT} is incomplete: Release/ is missing.")
endif()

# ---- load the distribution's own CMake machinery ----------------------------------------------
# cef_variables.cmake refuses to run unless these two are set the way its own
# FindCEF.cmake sets them, and it defaults an empty CMAKE_BUILD_TYPE to Release
# (with a warning) — which would silently flip the whole LucyN build to -O2/-DNDEBUG.
# LucyN has always been built with no explicit build type; keep that by pinning
# "None" (same effective flags as empty, but cef_variables leaves it alone).
set(_CEF_ROOT "${CEF_ROOT}")
set(_CEF_ROOT_EXPLICIT 1)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "None")
endif()
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
include("${CEF_ROOT}/cmake/cef_variables.cmake")
include("${CEF_ROOT}/cmake/cef_macros.cmake")

# ---- libcef_dll_wrapper: the C++ API wrapper, compiled from the distro source ------------------
# (This is the "libcef_dll_wrapper" every CEF app links against; it contains the
# cpptoc/ctocpp translators between our C++ classes and Chromium's C API.)
if(NOT TARGET libcef_dll_wrapper)
  add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)
endif()

# ---- libcef itself as an imported target ---------------------------------------------------------
# The distro's own cef_variables.cmake gives us the platform-correct path on Linux
# (Release/libcef.so) and Windows (Release/libcef.lib for linking, libcef.dll at run time).
# macOS is the odd one: that file does NOT set CEF_LIB_RELEASE there at all, so probe the
# layouts CEF has shipped for macOS and fail with an actionable message if none match.
# Since CEF 100-ish the macOS standard distribution has no bare libcef.dylib — libcef IS
# the framework binary:
#   Release/Chromium Embedded Framework.framework/Chromium Embedded Framework
# (verified against cef_binary_156.0.14_*_macosarm64.tar.bz2: Release/ holds nothing but
# that framework). Older flat layouts kept libcef.dylib in Release/ or libcef_dylib/.
if(NOT CEF_LIB_RELEASE)
  foreach(_cef_candidate
      "${CEF_BINARY_DIR_RELEASE}/libcef.dylib"
      "${CEF_BINARY_DIR_RELEASE}/Chromium Embedded Framework.framework/Chromium Embedded Framework"
      "${CEF_BINARY_DIR_RELEASE}/Chromium Embedded Framework.framework/Versions/A/Chromium Embedded Framework"
      "${_CEF_ROOT}/libcef_dylib/libcef.dylib"
      "${_CEF_ROOT}/libcef_dylib/libcef.so")
    if(EXISTS "${_cef_candidate}")
      set(CEF_LIB_RELEASE "${_cef_candidate}")
      break()
    endif()
  endforeach()
endif()
if(NOT CEF_LIB_RELEASE OR NOT EXISTS "${CEF_LIB_RELEASE}")
  message(FATAL_ERROR
    "Could not locate libcef in the CEF distribution at ${CEF_ROOT}.\n"
    "Expected Release/libcef.so (Linux), Release/libcef.lib (Windows) or\n"
    "Release/Chromium Embedded Framework.framework/Chromium Embedded Framework (macOS;\n"
    "bare libcef.dylib only on older distros). Set CEF_LIB_RELEASE to the real path if\n"
    "your distro puts it somewhere else.")
endif()
# The framework, on macOS (empty elsewhere): the whole bundle, dylib + Resources/ inside it.
set(CEF_FRAMEWORK_RELEASE "")
if(APPLE AND IS_DIRECTORY "${CEF_BINARY_DIR_RELEASE}/Chromium Embedded Framework.framework")
  set(CEF_FRAMEWORK_RELEASE "${CEF_BINARY_DIR_RELEASE}/Chromium Embedded Framework.framework")
endif()

if(NOT TARGET lucyn_libcef)
  add_library(lucyn_libcef SHARED IMPORTED GLOBAL)
  set_target_properties(lucyn_libcef PROPERTIES
    IMPORTED_LOCATION "${CEF_LIB_RELEASE}"
  )
endif()

# ---- lucyn::cef: everything a LucyN target needs to compile and link against CEF ---------------
add_library(lucyn_cef INTERFACE)
add_library(lucyn::cef ALIAS lucyn_cef)
# SYSTEM so CEF's own headers never trip LucyN's -Wall -Wextra.
target_include_directories(lucyn_cef SYSTEM INTERFACE "${_CEF_ROOT}")
target_link_libraries(lucyn_cef INTERFACE
  libcef_dll_wrapper
  lucyn_libcef
  ${CEF_STANDARD_LIBS}      # X11 + dl on Linux — same libs the distro's own clients link
)
# Defines every CEF consumer needs (mirrors the distro's CEF_COMPILER_DEFINES for
# Linux minus per-build-type ones; do NOT pull in the full flag list — it carries
# -Werror/-fno-exceptions, which are wrapper-build-only concerns).
# Where icudtl.dat, *.pak and locales/ live in THIS distro — initCef() points CEF here when a
# relocated binary has no runtime files beside it: such a binary loads libcef from the distro's
# Release/ (rpath fallback) but the data files live elsewhere — without them CEF aborts with
# "Invalid file descriptor to ICU data". Linux/Windows keep them in a top-level Resources/;
# on macOS they are inside the framework (the macOS distro has NO top-level Resources/).
if(CEF_FRAMEWORK_RELEASE)
  set(_lucyn_cef_resources "${CEF_FRAMEWORK_RELEASE}/Resources")
else()
  set(_lucyn_cef_resources "${_CEF_ROOT}/Resources")
endif()
target_compile_definitions(lucyn_cef INTERFACE
  __STDC_CONSTANT_MACROS
  __STDC_FORMAT_MACROS
  _FILE_OFFSET_BITS=64
  LUCYN_CEF_RESOURCES_DIR="${_lucyn_cef_resources}"
  # macOS only: absolute path of the distro's Chromium Embedded Framework.framework,
  # used by initCef() as the framework_dir_path when nothing sits beside the binary.
  LUCYN_CEF_FRAMEWORK_DIR="${CEF_FRAMEWORK_RELEASE}"
)
# The library is copied next to the binaries (see lucyn_cef_copy_runtime below), so the
# binary must find it relative to itself, wherever it is run from.
if(WIN32)
  # MSVC has no rpath concept: Windows looks for a DLL beside the .exe —
  # lucyn_cef_copy_runtime() puts libcef.dll in the cef/ subfolder and main.cpp
  # adds that folder to PATH for child processes.
elseif(APPLE)
  # $ORIGIN is an ELF-ism; Mach-O resolves siblings through @loader_path.
  target_link_options(lucyn_cef INTERFACE "-Wl,-rpath,@loader_path/cef")
else()
  target_link_options(lucyn_cef INTERFACE "-Wl,-rpath,$ORIGIN/cef")
endif()

file(STRINGS "${_CEF_ROOT}/include/cef_version.h" _cef_version_line REGEX "^#define CEF_VERSION \"")
string(REGEX REPLACE "^.*CEF_VERSION \"([^\"]+)\".*$" "\\1" _cef_version "${_cef_version_line}")
message(STATUS "Found CEF ${_cef_version} at ${_CEF_ROOT}")

# ---- runtime files -----------------------------------------------------------------------------
# Copy the distro's runtime (Release/* binaries + Resources/* packs + locales)
# next to the built executables. Done at configure time with file(COPY): it keeps
# timestamps and skips files that are already current, so it costs nothing on
# reconfigure. (POST_BUILD would be the other option, but add_custom_command
# TARGET form only works inside the directory that creates the target, and this
# helper is called from the root after apps/lucyn exists.)
function(lucyn_cef_copy_runtime)
  set(_dst "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
  if(NOT _dst)
    set(_dst "${CMAKE_BINARY_DIR}/bin")
  endif()
  # Everything CEF needs goes into ONE subfolder — <bin>/cef/ — so the binary's own
  # directory stays clean: no libcef.so, no *.pak, no locales/ next to `lucyn`.
  set(_cef_dir "${_dst}/cef")
  file(MAKE_DIRECTORY "${_cef_dir}")
  # macOS: CEF_BINARY_FILES is empty for the mac distro (cef_variables.cmake only fills it
  # in for Linux and Windows), so copy the whole framework instead — dylib, Libraries/ and
  # Resources/ (icudtl.dat, *.pak, *.lproj) in one go. CEF resolves the framework through
  # the @loader_path/cef rpath set above.
  if(APPLE AND CEF_FRAMEWORK_RELEASE AND IS_DIRECTORY "${CEF_FRAMEWORK_RELEASE}")
    file(COPY "${CEF_FRAMEWORK_RELEASE}" DESTINATION "${_cef_dir}")
  else()
    foreach(_f IN LISTS CEF_BINARY_FILES)
      if(EXISTS "${CEF_BINARY_DIR_RELEASE}/${_f}")
        file(COPY "${CEF_BINARY_DIR_RELEASE}/${_f}" DESTINATION "${_cef_dir}")
      endif()
    endforeach()
    foreach(_f IN LISTS CEF_RESOURCE_FILES)
      if(EXISTS "${CEF_RESOURCE_DIR}/${_f}")
        file(COPY "${CEF_RESOURCE_DIR}/${_f}" DESTINATION "${_cef_dir}")
      endif()
    endforeach()
  endif()
  # Version stamp: initCef() reads this and refuses to run against a libcef from a
  # DIFFERENT CEF build than the one this binary was compiled with — a mismatch is
  # exactly what shows up as "stack smashing detected" at run time.
  file(WRITE "${_cef_dir}/cef-runtime-version.txt"
       "${_cef_version}\nbuilt_from=${_CEF_ROOT}\n")
endfunction()
