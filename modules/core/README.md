# lucyn::core

Where LucyN keeps its data. No dependencies.

**Rule: by default and always, everything is stored in `nothing-data/` next to the running binary.** Nothing goes to `~/.config` or `%APPDATA%`. Delete the folder and the browser forgets everything.

```
<folder of the running binary>/nothing-data/
    identities/    stored devices            (lucyn::profile)
    snapshots/     <site>/<time>/before.html after.html diff.txt   (lucyn::shell)
    scripts/       user scripts to inject    (lucyn::shell)
    shell/         shell.html inject.js hydrator.js   (lucyn::shell)
    downloads/     files saved by `lucyn --download` / the `download` command   (lucyn::controller)
```

```cpp
#include <lucyn/core/paths.h>
core::exeDir();                 // folder of the running binary (symlinks resolved)
core::dataRoot();               // <exeDir>/nothing-data   (not created yet)
core::dataDir("snapshots/x");   // created if missing; "../x" or "/etc" returns "" and creates nothing
```

With the root build every tool lands in `build/bin/`, so they all share `build/bin/nothing-data`. A standalone module build keeps its own.
`setDataRootForTests()` exists for tests only.
