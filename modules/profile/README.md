# lucyn::profile

"Who is LucyN pretending to be right now?" Only depends on `lucyn::core` (it pulls that in itself), builds and tests on its own.

```
cmake -S modules/profile -B build && cmake --build build && ctest --test-dir build
./build/lucyn-profile list
./build/lucyn-profile switch safari firefox chrome131_android
```

## API (`#include <lucyn/profile/profile.h>`, namespace `lucyn::profile`)

| Piece | What it does |
|---|---|
| `Registry::builtin()` | Read-only table of every curl-impersonate browser. `find`, `latest`, `resolve("chrome")`, `describe`, `byFamily`. |
| `Switcher` | Holds the **active** profile. `select(name)`, `current()`, `addValidator`, `subscribe`, `setStrict`. Thread-safe. |
| `toJson(...)` | JSON for the controller's stdio channel and for tooling. |
| `generateIdentity` / `Identity` | A believable **device** (cores, screen, GPU, timezone, language, noise seeds). Deterministic from a seed. |
| `surfaceFor(persona, identity)` | What the browser *reports*: UA, platform, vendor, `sec-ch-ua`, `userAgentData`, WebGL strings, which JS APIs exist. Derived, never stored. |
| `checkConsistency(persona, identity)` | The README's "all details agree" rule as code. Empty list = consistent. |
| `IdentityStore` | Saves one device per (profile name, OS class) as JSON, atomically. Corrupt files are reported, never silently replaced. |
| `IdentityManager` | Follows a `Switcher` and always holds the identity + surface of the current persona. |

```cpp
Switcher sw("chrome");                       // shared by every module
sw.subscribe([](auto& from, auto& to) { /* net rebuilds, ui repaints, js re-fakes navigator */ });
sw.addValidator([](auto& p, std::string* err) { /* veto: "my library can't do that" */ return true; });

sw.select("safari184");                      // concrete target
sw.select("firefox");                        // alias -> newest desktop Firefox
sw.select("chrome999");                      // newer than the table: allowed (lenient) unless setStrict(true)
```

Rules: a failed `select` leaves the old profile untouched. Selecting the active profile is a silent no-op.
Validators run before the switch, listeners after, both on the caller's thread and never under the internal lock.

## Adding a browser
One row in `Registry::Registry()` in `src/profile.cpp`, oldest to newest within its family.

## Identities: persona vs device

```
persona (from the Profile)  ->  UA, sec-ch-ua, platform, vendor, WebGL style, which APIs exist   [derived, never stored]
device  (generated, stored) ->  cores, memory, screen, GPU, timezone, languages, noise seeds        [per profile name + OS]
```

```cpp
Switcher sw("chrome116");
IdentityManager who(sw, IdentityStore(IdentityStore::defaultDir()), "default");

who.surface()->userAgent;     // what navigator.userAgent AND the HTTP header must say
sw.select("safari184");       // manager follows: loads/creates this profile's Mac, surface becomes Safari's
sw.select("chrome110");       // back on the SAME Windows machine as before
sw.select("tor145");          // refused: no identity can be built for it yet
```

```
lucyn-profile identity safari184 --seed 7                 # generate and print, nothing saved
lucyn-profile identity chrome116 --name work              # create or reload a stored person in nothing-data/identities
lucyn-profile identity chrome116 --regenerate             # roll a new person (same place)
lucyn-profile identity chrome116 --dir ./ids --name work  # explicit folder, for experiments
lucyn-profile identity chrome131_android --region de      # timezone + languages for a German exit IP
```

Regions (`us-east us-west uk de fr jp kr in br au`) set timezone and languages. Pick the one that matches
your proxy's exit IP, or pass `timezone` / `languages` yourself.

### Know the limits
- The persona's OS comes from curl-impersonate's own headers (e.g. `chrome146` claims macOS), so the device follows the persona, not the host machine.
- The GPU / screen tables are realistic but hand-written. Mobile tables are coarse. Verify against real traces before relying on them.
- Chrome's `sec-ch-ua` brand list is computed from the major version and is tested against known real headers for Chrome 107 to 136. For newer versions, compare with the header in `curl_chrome<N>`.
- Targets older than `chrome101` carry full version numbers in their real UA; we produce the modern reduced form.
- `tor145` is refused: Tor Browser fixes its own values.

Where identities live: `IdentityStore::defaultDir()` is **always** `<running binary>/nothing-data/identities` (see `modules/core`).
