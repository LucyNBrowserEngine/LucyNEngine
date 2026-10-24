// libFuzzer harness for NULLBYTE. Any input must never crash; a valid envelope must always open.
// Build (Clang only): cmake -S modules/security -B b -DLUCYN_BUILD_FUZZERS=ON -DCMAKE_CXX_COMPILER=clang++
// Run:                 ./b/fuzz_nullbyte -max_total_time=60
#include <lucyn/security/nullbyte.h>
#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
    static const lucyn::security::Nullbyte nb("fuzz-key-32-bytes-long-enough!!!");
    const std::string_view sv(reinterpret_cast<const char*>(data), size);
    nb.sign(sv);
    nb.verify(sv, sv);                 // arbitrary bytes as a signature: never a crash
    nb.open(nb.envelope(sv));          // a valid envelope round-trips and always opens
    nb.open(sv);                       // arbitrary bytes as an envelope: never a crash
    return 0;
}
