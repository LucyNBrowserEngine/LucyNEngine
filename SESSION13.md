# Session 13 — engine startup loads and displays its LNM app

## What changed

- `lucyn --config <file>` now reads the runtime JSON setting `lnm.entry`. The entry path is resolved relative to the config file; files referenced by the LNM entry are resolved relative to the entry's directory.
- The executable passes the resolved entry to `ControllerOptions`. When the controller starts, it reads the `.lnm` source, creates `lnm::App` with the existing disk reader, and reports load diagnostics as controller note events.
- When an LNM app loads, the controller creates the existing SDL window and paints the App's current DOM through `layout::layoutView()`, `layout::paint()`, and `paint::SoftwarePainter`. The frame is presented when the window is created and after `window` commands. The window remains hidden by default; pass `--show` to show it immediately.
- The root build links the controller to `lucyn::lnm` when the LNM module is enabled. The engine API spec documents the executable config shape and path rules.

Example `engine.json`:

```json
{
  "lnm": {
    "entry": "./ui/shell.lnm"
  }
}
```

Run it visibly with `build/bin/lucyn --config engine.json --show`. Omitting `lnm.entry` keeps the existing engine startup behavior.

## Scope and known gaps

The LNM runtime is now loaded and painted by the engine, but this session did not add mouse delivery/hit-testing or execute `runjs`. Those are the next candidate steps and require the author's confirmation before starting. LNM continues to use its own style declarations; no `.css` parser work was added.

The current runtime config loader reads `lnm.entry` only. The broader C++ config API remains the embedding surface described in `LUCYN_ENGINE_API_SPEC.md`; the JSON config does not claim to implement the other spec fields yet.

## Verification

- Before edits: `cmake -S . -B build`, `cmake --build build -j`, and `SDL_VIDEODRIVER=dummy ctest --test-dir build --output-on-failure` all succeeded; 23/23 existing suites passed.
- After edits: `cmake --build build -j` succeeded. The test suite was not rerun after the changes. SDL window content was not visually inspected in this environment.
