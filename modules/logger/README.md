# lucyn::logger

Dependency-free, thread-safe engine diagnostics. The module writes one
formatted line to `stderr`, uses ANSI colors only for terminals, and honors
`NO_COLOR`. It does not write files or terminate the process on `fatal()`.

```cpp
#include <lucyn/logger/logger.h>

lucyn::logger::Logger log; // DEBUG by default; WARN with NDEBUG
log.info("Engine initialized");
log.success("Profile loaded");
log.warn("Optional asset was not found");
log.error("Request failed");
log.build("Embedding resources");
```

Thresholds are `debug`, `info`, `warning`, `error`, `fatal`, and `off`.
`success` and `build` are informational; `fatal` is emitted at the
fatal threshold but the caller remains responsible for shutdown. Use
`defaultLogger()` for process-wide logging or construct a `Logger` when a
component needs its own threshold.

Build standalone with `cmake -S modules/logger -B build-logger` and
`cmake --build build-logger`. The root build includes the module by default;
disable it with `-DLUCYN_MODULE_LOGGER=OFF`.

This module provides the C++ engine logger only. Page `console` output is kept
separate and configurable (`lucyn --console`).
