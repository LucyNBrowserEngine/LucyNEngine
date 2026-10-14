// lucyn/core/paths.h - where LucyN keeps its data.
//
// RULE: by default and always, everything LucyN stores lives in a folder called
// `nothing-data` next to the running binary ("the root of the running script").
//
//     <folder of the running binary>/nothing-data/
//         identities/   one stored device per (profile, OS)        (profile module)
//         snapshots/    before/after pages, one folder per run      (shell module)
//         scripts/      user scripts that get injected              (shell module)
//         shell/        shell.html, inject.js, hydrator.js          (shell module)
//         downloads/    files saved by `lucyn --download` / `download`   (controller)
//
// Nothing is ever written to ~/.config or %APPDATA%. Delete the folder and the
// browser forgets everything.
#pragma once
#include <string>
#include <string_view>

namespace lucyn::core {

// Folder that holds the running executable (symlinks resolved). Falls back to
// the current directory only if the OS will not tell us.
std::string exeDir();

// <exeDir>/nothing-data. Not created until you ask for a subfolder.
std::string dataRoot();

// <dataRoot>/<sub>, created if missing. `sub` may contain '/' for nesting but
// never ".." or an absolute path: such a request returns "" (and creates nothing).
// If the folder cannot be created the path is still returned, so the caller's
// own write reports the real error.
std::string dataDir(std::string_view sub);

// TEST ONLY: point dataRoot() somewhere else (a temp folder). Empty = restore.
void setDataRootForTests(const std::string& path);

}  // namespace lucyn::core
