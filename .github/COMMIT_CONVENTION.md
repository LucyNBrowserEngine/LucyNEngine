# Commit conventions

LucyN uses [Conventional Commits](https://www.conventionalcommits.org/). Every commit message on this
repository starts with a type, a colon and a space, then a short imperative summary:

```
<type>: <imperative summary, lowercase, no trailing period>
```

## Types

| Type | Use it for | Example |
|---|---|---|
| `feat:` | a new user-facing capability — a command, a CLI flag, a script, a client method | `feat: add waitFor timeout` |
| `fix:` | a defect fix, including "make the build work again" | `fix: keep ICU data next to a copied lucyn binary` |
| `docs:` | documentation only — README, `docs/`, comments that describe behaviour | `docs: document the captures reply shape` |
| `chore:` | maintenance that changes no behaviour — dependency bumps, CI, cleanup | `chore: drop the dead profile module from CMake` |
| `perf:` | a performance improvement, ideally with the measurement | `perf: cut eval round-trips in waitFor polling` |

Anything that is not one of these is not worth its own commit here: split the work instead. A commit
that spans types (code **and** docs) takes the type of its main change and mentions the rest in the
body.

## Format

```
<type>: <summary, imperative, <= 72 chars, no trailing period>

<why, not what — wrapped at ~72 chars>

Refs: #123
```

- **Scope is optional** and only used when the change is confined to one area:
  `feat(cef_host): report the final URL after a redirect`.
  Scopes follow the directory names: `lucyn`, `cef_host`, `controller`, `core`, `net`, `security`,
  `server`, `search`, `logger`, `adblock`, `client`, `scripts`, `docs`, `ci`.
- **Breaking changes** get a `!` after the type/scope and an explanation in the body:
  `feat!: drop the profile command from the protocol`.
  Say in the body who breaks and what to do instead — the protocol is public API for the Node client.
- The subject line is what shows up in release notes; write it for a user, not for yourself.
  "fix: cef_host: adjust deadline" tells nobody anything; "fix: stop waitFor giving up at 5 s" does.
- Never commit generated output (`build/`, `nothing-data/`, `third_party/cef/`) or secrets.

## AI-assisted commits

AI-written or AI-assisted commits are welcome and must be **tagged in the subject line** with the tool
that did the work, in square brackets at the end:

```
feat: add waitFor timeout [claude]
fix: keep ICU data next to a copied lucyn binary [gemini]
chore: drop the dead profile module from CMake [copilot]
```

Use your own handle for anything else (`[codex]`, `[cursor]`, …) — the tag names the tool, not the
model version. The tag never replaces the type, and it does not change how the commit is read; it is
there so `git log --grep='\[claude\]'` can find a tool's work when something needs auditing.

Review does not get looser because a tool wrote the patch: an AI-assisted commit carries the same
verification bar as any other (build clean, `ctest` green, behaviour actually exercised), and the PR
should say how it was checked.

## Examples

```
feat: add --check to update_cef.sh

Prints the installed and latest stable CEF builds and what it would
install, without downloading, so CI can poll it cheaply.

chore: rename lucyn_cef_copy_runtime to match the new option

fix: stop the download command leaving .part files behind

The rename dropped the hidden-file prefix, so a failed transfer was
left visible in nothing-data/downloads/.

feat!: remove the profile command from the protocol

CEF sends its own headers, so switching a persona could not do
anything. Callers should drop the profile field from visit and the
profile command itself; info() now reports {url, captures}.
```
