## Summary

<!-- What user problem does this solve, and what does the change do? One or two paragraphs.
     LucyN is a scraping-first browser engine driven over JSON stdio; Chromium (CEF) does the
     browsing. Say what the engine does differently after this PR. See CONTRIBUTING.md. -->

## Modules and surfaces

<!-- Tick every area this PR touches. The engine modules are: -->
- [ ] `apps/lucyn` — the engine binary / CLI flags
- [ ] `modules/cef_host` — the CEF (Chromium) host: init, navigation, eval, cookies, screenshots
- [ ] `modules/controller` — the JSON command loop and command surface
- [ ] `modules/core` · `modules/net` (Moonbug) · `modules/security` (NULLBYTE) · `modules/server` (dashboard)
- [ ] `modules/search` · `modules/logger` · `modules/adblock`
- [ ] `client/` — the `nothing-browser` Node package
- [ ] `scripts/` — install.sh / update_cef.sh
- [ ] `docs/` · README · other documentation
- [ ] Build or CI configuration (CMake, `.github/`)

## Verification

<!-- Be concrete: the command you ran and what it printed. If you could not run something, say why. -->
- [ ] `cmake -S . -B build && cmake --build build -j` — 0 errors, 0 warnings
- [ ] `ctest --test-dir build --output-on-failure` — all suites pass
- [ ] Exercised the changed behaviour end to end (e.g. `visit`/`extract` against a loopback server,
      `./build/bin/lucyn --download <url>`, a Node client run) — describe what you ran
- [ ] If CEF-facing: the change still works from a **copied** binary (the `cli_download` suite covers this)

## Notes for reviewers

<!-- Protocol/API compatibility, security implications, screenshots, linked issue, anything surprising. -->

## AI assistance

<!-- If AI tools helped write this, say so briefly and describe how you reviewed and verified the
     result. Tag the commits themselves as described in .github/COMMIT_CONVENTION.md
     (e.g. `feat: add waitFor timeout [claude]`). -->
