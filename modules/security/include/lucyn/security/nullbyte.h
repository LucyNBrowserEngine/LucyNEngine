// lucyn/security/nullbyte.h - NULLBYTE: LucyN's integrity seal for captured traffic.
//
// HMAC-SHA256 over the exact bytes of a capture. It answers one question: "was this
// captured cleanly, and has it been touched since?" It does NOT protect against a
// compromised engine process (the key lives in that process): a seal, not a sandbox.
//
// Wire format (the "envelope"), one JSON object:
//   {"v":1,"alg":"HMAC-SHA256","sig":"<64 hex>","payload":"<the signed bytes, as a JSON string>"}
// `payload` is a JSON *string* holding the exact signed text, so verification never has to
// re-serialise anything: unescape, HMAC, compare.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace lucyn::security {

using Digest = std::array<std::uint8_t, 32>;

Digest      sha256(std::string_view data);
Digest      hmacSha256(std::string_view key, std::string_view data);
std::string toHex(const Digest& d);
std::string toHex(std::string_view bytes);
// Decodes hex ("ab01..."); returns false on odd length or a non-hex character.
bool        fromHex(std::string_view hex, std::string* out);

class Nullbyte {
public:
    // `key` is raw key bytes. An empty key is refused (throws std::invalid_argument):
    // an empty HMAC key would "sign" with a constant everybody knows.
    explicit Nullbyte(std::string key);

    static constexpr const char* kAlgorithm = "HMAC-SHA256";

    // <dataRoot>/security/nullbyte.key
    static std::string defaultKeyPath();
    // 32 fresh random bytes from the OS.
    static std::string generateKey();
    // Reads the hex key stored at `path`, or creates it (owner-only permissions where the OS
    // supports that). A key file that exists but is unreadable or corrupt is REPORTED and never
    // silently replaced: returns "" and sets *err. (Replacing it would orphan every earlier signature.)
    static std::string loadOrCreateKey(const std::string& path, std::string* err = nullptr);

    // Lowercase hex HMAC-SHA256.
    std::string sign(std::string_view data) const;
    // Constant-time compare. False for a wrong-length or non-hex signature.
    bool        verify(std::string_view data, std::string_view sigHex) const;

    // sign() + wrap into the envelope above.
    std::string envelope(std::string_view payload) const;

    struct Opened {
        bool        ok = false;      // envelope parsed AND signature valid
        std::string payload;         // the signed bytes (only meaningful if parsed)
        bool        parsed = false;  // envelope was well formed
        std::string error;           // why not ok
    };
    // Parses an envelope and verifies it. Never trusts anything in it except the math.
    Opened open(std::string_view envelopeJson) const;

private:
    std::string key_;
};

}  // namespace lucyn::security
