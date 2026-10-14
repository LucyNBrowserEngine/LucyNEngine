// The JavaScript half of the bridge, embedded. It is one function, `(function(n, window){...})`, called once per attach with the private
// native object `n`. Sources live in src/prelude/*.js (so they get syntax highlighting) and CMake turns them into prelude_gen.inc.
#include "impl.h"

namespace lucyn::js::detail {

std::string preludeSource() {
    static const char* const chunks[] = {
#include "prelude_gen.inc"
    };
    std::string s;
    for (const char* c : chunks) s += c;
    return s;
}

}  // namespace lucyn::js::detail
