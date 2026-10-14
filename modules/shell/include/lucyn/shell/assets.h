// lucyn/shell/assets.h - the files that make up the shell, and where they live.
//
//     nothing-data/shell/shell.html     the page every site is opened in
//     nothing-data/shell/inject.js      (JS module fills this in) runs the user's scripts by @run-at
//     nothing-data/shell/hydrator.js    (JS module fills this in) loads the site into the shell
//     nothing-data/scripts/             the user's own scripts
#pragma once
#include <string>

namespace lucyn::shell {

const char* defaultShellHtml();
const char* defaultInjectJs();
const char* defaultHydratorJs();
const char* exampleScript();

// shell.html from nothing-data/shell if the user has one, else the built-in.
std::string loadShellHtml();

// Writes shell.html, inject.js, hydrator.js and scripts/example.js.disabled into nothing-data
// when they are missing. Never overwrites a file. Returns false (and fills *err) if a write fails.
bool ensureShellFiles(std::string* err = nullptr);

}  // namespace lucyn::shell
