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
#   lucyn_cef_copy_runtime() - copies Release/ + Resources/ next to the built binaries
#                              (CEF needs icudtl.dat, *.pak, locales/ and libcef.so
#                              next to the executable or it dies with a cryptic
#                              locale error — SESSION19_CEF_MIGRATION.md "Known hard parts")
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
if(NOT EXISTS "${CEF_ROOT}/Release/libcef.so")
  message(FATAL_ERROR
    "CEF distribution at ${CEF_ROOT} is incomplete: Release/libcef.so is missing.")
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

# ---- libcef.so as an imported target -----------------------------------------------------------
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
target_compile_definitions(lucyn_cef INTERFACE
  __STDC_CONSTANT_MACROS
  __STDC_FORMAT_MACROS
  _FILE_OFFSET_BITS=64
  # Distro Resources/ (icudtl.dat, *.pak, locales/). initCef() points CEF here when a
  # relocated binary has no runtime files beside it: such a binary loads libcef.so from
  # the distro's Release/ (INSTALL_RPATH fallback) but icudtl.dat lives in Resources/
  # — without this CEF aborts with "Invalid file descriptor to ICU data".
  LUCYN_CEF_RESOURCES_DIR="${_CEF_ROOT}/Resources"
)
# libcef.so is copied next to the binaries (see lucyn_cef_copy_runtime below), so
# the binary must find it relative to itself, wherever it is run from.
target_link_options(lucyn_cef INTERFACE "-Wl,-rpath,$ORIGIN")

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
  file(MAKE_DIRECTORY "${_dst}")
  foreach(_f IN LISTS CEF_BINARY_FILES)
    if(EXISTS "${CEF_BINARY_DIR_RELEASE}/${_f}")
      file(COPY "${CEF_BINARY_DIR_RELEASE}/${_f}" DESTINATION "${_dst}")
    endif()
  endforeach()
  foreach(_f IN LISTS CEF_RESOURCE_FILES)
    if(EXISTS "${CEF_RESOURCE_DIR}/${_f}")
      file(COPY "${CEF_RESOURCE_DIR}/${_f}" DESTINATION "${_dst}")
    endif()
  endforeach()
endfunction()
