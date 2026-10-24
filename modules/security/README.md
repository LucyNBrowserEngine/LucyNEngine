# security - NULLBYTE

HMAC-SHA256 integrity seal for captured traffic. `lucyn::security`, depends only on `core`.

**What it proves:** a capture has not been altered since it was sealed, by anyone who does not have the key.
**What it does not prove:** anything about a compromised engine process (the key lives in that process). A seal, not a sandbox.

```cpp
security::Nullbyte nb(security::Nullbyte::loadOrCreateKey(security::Nullbyte::defaultKeyPath()));
std::string sig = nb.sign(data);              // 64 lowercase hex chars
bool ok         = nb.verify(data, sig);       // constant time
std::string env = nb.envelope(json);          // {"v":1,"alg":"HMAC-SHA256","sig":"..","payload":"<json as a string>"}
auto opened     = nb.open(env);               // parse + verify; opened.ok / opened.payload / opened.error
```

* SHA-256 and HMAC are implemented here (no OpenSSL) and tested against the FIPS 180-4 and RFC 4231 vectors, including keys longer than the block size.
* The envelope carries the signed text as a JSON *string*, so verifying never re-serialises anything (field order, whitespace, `\uXXXX` escapes all verify). Anything but the math is distrusted: wrong algorithm, malformed JSON, lone surrogates all fail closed.
* The key lives in `<nothing-data>/security/nullbyte.key` (hex, owner-only permissions where the OS has them), created on first use, so a seal made today verifies tomorrow. A **corrupt key file is reported and never replaced** (replacing it would orphan every earlier seal); callers then fall back to a throwaway key and say so.
* Key bytes come from the OS CSPRNG: `BCryptGenRandom` (Windows), `getrandom(2)` then `/dev/urandom` (POSIX). If no OS entropy source works it **throws** rather than fall back to predictable entropy — a guessable HMAC key is worse than no key. A **new** key file is only ever created **under the data directory** (defence in depth against a caller-controlled path).
* The envelope's `v` field is checked: only `v:1` is accepted (a future version is refused with "unsupported envelope version"); an envelope with no `v` still verifies, so hand-written/legacy seals keep working.
* An empty key throws: an empty HMAC key would "sign" with a constant everyone knows.

A libFuzzer harness for the parser/sign path lives in `tests/fuzz_nullbyte.cpp` (build with `-DLUCYN_BUILD_FUZZERS=ON` under Clang).

Tests: `ctest -R security`. Standalone: `cmake -S modules/security -B b`.
