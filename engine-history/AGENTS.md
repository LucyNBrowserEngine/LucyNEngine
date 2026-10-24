# Repository Guidelines

## Project Structure & Module Organization

LucyN is a modular C++17 browser engine. The root `CMakeLists.txt` wires modules under `modules/<name>/`; each module typically contains `include/lucyn/<name>/`, `src/`, `tests/`, and sometimes `tools/`, plus its own `CMakeLists.txt`. The executable entry point is under `apps/lucyn/`. `client/` contains the Node.js client, `dashboard/` contains the capture dashboard, and `third_party/` holds vendored dependencies. Treat `build/` and `build-asan/` as generated output directories.

## Build, Test, and Development Commands

- `cmake -S . -B build` configures the full project and enables module tests by default.
- `cmake --build build -j` builds enabled modules and tools.
- `ctest --test-dir build --output-on-failure` runs registered tests and prints failure details.
- For a focused module, use `cmake -S modules/core -B build-core && cmake --build build-core -j && ctest --test-dir build-core --output-on-failure` (replace `core` as needed).
- `node` 18 or newer is required for the client; its package currently defines no build or test scripts.

System dependencies are a C++17 compiler and CMake plus the prebuilt CEF (Chromium) distribution in `third_party/cef/`, which `cmake/FindCEF.cmake` locates and whose runtime files the build copies next to the binaries. The engine is headless and has no windowing dependency of its own; there is no libcurl, libgumbo or QuickJS left. Check a module's README before changing its dependencies or build options.

## Coding Style & Naming Conventions

Match nearby code: C++ uses four-space indentation, descriptive names, and C++17 features. Keep public headers in the module's `include/lucyn/<module>/` tree and implementations in `src/`. Name tests `test_<area>.cpp` and register them with CTest in the module's CMake file. There is no repository-wide formatter configured; keep changes consistent with adjacent files and retain warning flags.

## Testing Guidelines

Add or update focused tests in the owning module's `tests/` directory. Run that module's CTest suite during iteration, then run root `ctest` for changes that affect module wiring or shared behavior. Tests are standalone C++ executables; no coverage threshold is configured. Keep test data temporary and avoid writing into tracked source directories.

## Commit & Pull Request Guidelines

Git history is not available in this checkout, so no existing commit convention could be verified. Use short, imperative commit subjects, optionally scoped by module (for example, `dom: handle empty attributes`). Pull requests should explain the behavior change, list relevant modules and build/test commands run, link related issues when applicable, and include screenshots for dashboard or rendered output changes.

## Security & Configuration Tips

Do not commit credentials, local identity data, or generated build artifacts. Keep capture and internal server listeners restricted to loopback as documented in the project README; changes to network-facing behavior should include security-focused tests.
