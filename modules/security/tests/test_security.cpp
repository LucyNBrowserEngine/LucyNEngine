// test_security.cpp - NULLBYTE: official hash/HMAC vectors, envelopes, tamper detection, key file rules.
// Uses CHECK (not assert) so the checks still run in Release builds.
#include <lucyn/security/nullbyte.h>
#include <lucyn/core/paths.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

using namespace lucyn::security;

static void test_sha256_vectors() {
    CHECK(toHex(sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(toHex(sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(toHex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(toHex(sha256(std::string(1000000, 'a'))) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // lengths around the 55/56/63/64 padding edges must all be distinct and stable
    std::string seen;
    for (size_t n : {54u, 55u, 56u, 57u, 63u, 64u, 65u, 119u, 120u, 128u}) {
        std::string h = toHex(sha256(std::string(n, 'x')));
        CHECK(h.size() == 64);
        CHECK(seen.find(h) == std::string::npos);
        seen += h;
    }
    std::puts("PASS: sha256_vectors");
}

static void test_hmac_rfc4231() {
    CHECK(toHex(hmacSha256(std::string(20, '\x0b'), "Hi There")) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    CHECK(toHex(hmacSha256("Jefe", "what do ya want for nothing?")) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    CHECK(toHex(hmacSha256(std::string(20, '\xaa'), std::string(50, '\xdd'))) == "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
    std::string k4; for (int i = 1; i <= 25; ++i) k4 += char(i);
    CHECK(toHex(hmacSha256(k4, std::string(50, '\xcd'))) == "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
    // keys longer than the 64-byte block get hashed first (the classic HMAC bug)
    CHECK(toHex(hmacSha256(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First")) ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    CHECK(toHex(hmacSha256(std::string(131, '\xaa'),
          "This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm.")) ==
          "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
    std::puts("PASS: hmac_rfc4231");
}

static void test_hex() {
    std::string out;
    CHECK(fromHex("00ff10Ab", &out) && out == std::string("\x00\xff\x10\xab", 4));
    CHECK(!fromHex("abc", &out));
    CHECK(!fromHex("zz", &out));
    CHECK(toHex(std::string_view("\x01\xfe", 2)) == "01fe");
    std::puts("PASS: hex");
}

static void test_sign_verify() {
    Nullbyte nb("0123456789abcdef0123456789abcdef");
    std::string sig = nb.sign("hello");
    CHECK(sig.size() == 64);
    CHECK(nb.verify("hello", sig));
    CHECK(!nb.verify("hellp", sig));
    CHECK(!nb.verify("hello", sig.substr(1)));          // wrong length
    CHECK(!nb.verify("hello", std::string(64, 'g')));   // not hex
    std::string flipped = sig; flipped[10] = flipped[10] == '0' ? '1' : '0';
    CHECK(!nb.verify("hello", flipped));
    std::string upper = sig; for (auto& c : upper) if (c >= 'a' && c <= 'f') c = char(c - 32);
    CHECK(nb.verify("hello", upper));                   // hex case does not matter
    Nullbyte other("fedcba9876543210fedcba9876543210");
    CHECK(!other.verify("hello", sig));                 // different key = different seal
    bool threw = false;
    try { Nullbyte bad(""); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    std::puts("PASS: sign_verify");
}

static void test_envelope() {
    Nullbyte nb("k-k-k-k-k-k-k-k-k-k-k-k-k-k-k-k");
    // payload with every awkward thing: quotes, backslashes, newlines, control chars, unicode, braces in strings
    std::string payload = "{\"url\":\"http://x/?a=\\\"b\\\"\",\"note\":\"line1\nline2\ttab \x01 caf\xc3\xa9 \xf0\x9f\x91\xbb } { ]\"}";
    std::string env = nb.envelope(payload);
    auto o = nb.open(env);
    CHECK(o.parsed && o.ok);
    CHECK(o.payload == payload);                         // exact byte round trip

    // tamper with the payload text inside the envelope
    std::string t = env;
    auto at = t.find("line1");
    CHECK(at != std::string::npos);
    t[at] = 'L';
    auto o2 = nb.open(t);
    CHECK(o2.parsed && !o2.ok);

    // tamper with the signature
    std::string t2 = env;
    auto sp = t2.find("\"sig\":\"") + 7;
    t2[sp] = t2[sp] == 'a' ? 'b' : 'a';
    CHECK(!nb.open(t2).ok);

    // another key cannot open it
    CHECK(!Nullbyte("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz").open(env).ok);

    // field order / whitespace / extra fields do not matter (a re-serialised envelope, e.g. from JS, still verifies)
    std::string sig = nb.sign("abc");
    CHECK(nb.open("{ \"payload\" : \"abc\", \"extra\":{\"a\":[1,2,{\"b\":\"}\"}]}, \"sig\":\"" + sig + "\" }").ok);
    // JS JSON.stringify style escapes: \u00e9 and a surrogate pair for U+1F47B
    std::string uni = "caf\xc3\xa9 \xf0\x9f\x91\xbb";
    std::string usig = nb.sign(uni);
    auto o3 = nb.open("{\"sig\":\"" + usig + "\",\"payload\":\"caf\\u00e9 \\ud83d\\udc7b\"}");
    CHECK(o3.ok && o3.payload == uni);
    // wrong algorithm is refused, not "verified anyway"
    CHECK(!nb.open("{\"alg\":\"MD5\",\"sig\":\"" + sig + "\",\"payload\":\"abc\"}").ok);
    // a future envelope version is refused with a clear error (v:1 is the only known version)
    auto ov = nb.open("{\"v\":2,\"sig\":\"" + sig + "\",\"payload\":\"abc\"}");
    CHECK(!ov.ok && ov.error == "unsupported envelope version");
    // ...and a legacy envelope with no `v` at all still verifies (checked in the re-serialised case above)
    // garbage never crashes and never verifies
    for (const char* bad : {"", "{", "[]", "{\"sig\":1,\"payload\":2}", "{\"sig\":\"00\"}", "{\"payload\":\"abc\"}",
                            "{\"sig\":\"x\",\"payload\":\"\\ud83d\"}", "{\"sig\":\"x\",\"payload\":\"abc", "null"}) {
        auto ob = nb.open(bad);
        CHECK(!ob.ok);
        CHECK(!ob.error.empty());
    }
    std::puts("PASS: envelope");
}

static void test_key_file() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("lucyn_nb_test_" + std::to_string(std::rand()));
    fs::create_directories(dir);
    lucyn::core::setDataRootForTests((dir / "data").string());   // set BEFORE creating: new keys must live under it
    std::string path = (dir / "data" / "sub" / "nullbyte.key").string();

    std::string err;
    std::string k1 = Nullbyte::loadOrCreateKey(path, &err);
    CHECK(k1.size() == 32 && err.empty());
    CHECK(fs::exists(path));
    std::string k2 = Nullbyte::loadOrCreateKey(path, &err);      // second call loads the same key
    CHECK(k1 == k2);
    CHECK(Nullbyte(k1).verify("x", Nullbyte(k2).sign("x")));     // "same person comes back tomorrow"
    CHECK(Nullbyte::generateKey() != Nullbyte::generateKey());

    { std::ofstream f(path, std::ios::trunc); f << "this is not hex\n"; }
    std::string k3 = Nullbyte::loadOrCreateKey(path, &err);
    CHECK(k3.empty() && !err.empty());                            // reported ...
    std::ifstream f(path); std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(content == "this is not hex\n");                        // ... and NOT silently replaced

    // creating a NEW key OUTSIDE the data directory is refused (defence in depth), and nothing is written
    std::string outside = (dir / "outside.key").string();
    std::string e2;
    CHECK(Nullbyte::loadOrCreateKey(outside, &e2).empty());
    CHECK(!e2.empty());
    CHECK(!fs::exists(outside));

    CHECK(Nullbyte::defaultKeyPath() == (dir / "data" / "security" / "nullbyte.key").string());
    lucyn::core::setDataRootForTests("");
    std::error_code ec; fs::remove_all(dir, ec);
    std::puts("PASS: key_file");
}

int main() {
    test_sha256_vectors();
    test_hmac_rfc4231();
    test_hex();
    test_sign_verify();
    test_envelope();
    test_key_file();
    if (g_fail) { std::fprintf(stderr, "%d check(s) FAILED\n", g_fail); return 1; }
    std::puts("security: all passed");
    return 0;
}
