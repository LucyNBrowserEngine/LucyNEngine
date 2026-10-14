# Contributing to LucyN

LucyN is a collection of modules assembled into an engine. Contributions should name the module they change and preserve the module's ability to build and evolve independently.

## Choose the owning module

Start at `modules/<name>/`. Public C++ headers belong in `include/lucyn/<name>/`, implementation in `src/`, tests in `tests/`, and small module tools in `tools/`. The app entry point is `apps/lucyn/`; the Node.js API is in `client/`; vendored dependencies belong in `third_party/`.

If a change spans modules, identify each affected module and explain the dependency or data flow. Keep app-specific policy in the controller or app layer instead of adding an unnecessary dependency to a lower-level module. For example, a request filter can live in `modules/adblock`, while `modules/controller` chooses its rules and `modules/net` applies the configured decision at the request boundary.

## Build and test

Configure and build the repository with:

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For focused work, build a module independently where supported, for example:

```sh
cmake -S modules/dom -B build-dom
cmake --build build-dom -j
ctest --test-dir build-dom --output-on-failure
```

Add tests with the owning module and follow its existing `test_<area>.cpp` and CTest conventions. If you do not run a relevant test or build, say so and explain why. Do not claim a build proves runtime behavior; include integration evidence for changes that cross module boundaries.

## Pull requests

Use a focused title and describe:

1. The user problem or reproducible failure.
2. Why the change belongs in the named module or modules.
3. The behavior or API change and any compatibility impact.
4. Build/test commands run and their results.
5. Screenshots, logs, or a small reproducer when they help review.

Link a related issue when one exists. Keep unrelated cleanup out of feature PRs. AI-assisted work is welcome; follow [LLM.md](LLM.md), especially its requirement to explain the real reason for the change and verify generated code before submitting it.

## Code and repository care

Match nearby C++17 style and keep public interfaces small. Do not commit build output, local identity data, credentials, or private browsing captures. Preserve loopback-only constraints on internal capture/server listeners. Avoid editing vendored code unless the change specifically requires it, and describe the upstream source when you do.

By submitting a contribution, you confirm you have the right to submit it and agree that your LucyN-authored contribution may be distributed under the repository's MIT license. Do not submit third-party material unless its license permits inclusion and its required notices are preserved.
