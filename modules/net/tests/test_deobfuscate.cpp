// test_deobfuscate.cpp — deobfuscation unit tests (no network, no execution).
// Each case states the guarantee: plain code survives, known obfuscation
// patterns decode, and anything NOT recognized with confidence is left alone
// (wrong strings would be worse than none).
#include <lucyn/net/deobfuscate.h>

#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

using lucyn::net::DeobfStats;
using lucyn::net::deobfuscate;

// A plain script must come back semantically intact (re-indented at most).
static void test_plain_code_survives() {
    DeobfStats st;
    const std::string out = deobfuscate("function add(a,b){return a+b;}var x=add(1,2);", &st);
    CHECK(out.find("function add(a,b) {") != std::string::npos);
    CHECK(out.find("return a+b;") != std::string::npos);
    CHECK(out.find("var x=add(1,2);") != std::string::npos);
    CHECK(st.arraysFound == 0);
    CHECK(!st.lookedObfuscated);
    CHECK(st.stringsDecoded == 0);
    CHECK(!out.empty() && out.back() == '\n');
    std::puts("PASS: plain_code_survives");
}

// \x escapes decoded, constant concat folded, constant atob inlined.
static void test_escapes_concat_atob() {
    DeobfStats st;
    const std::string out =
        deobfuscate("var s='\\x68\\x69';var t=\"a\"+\"b\";var u=atob(\"d29ybGQ=\");", &st);
    CHECK(out.find("\"hi\"") != std::string::npos);        // '\x68\x69' -> "hi"
    CHECK(out.find("\"ab\"") != std::string::npos);        // "a"+"b" -> "ab"
    CHECK(out.find("\"world\"") != std::string::npos);     // atob("d29ybGQ=") -> "world"
    CHECK(out.find("\\x68") == std::string::npos);
    CHECK(st.escapesDecoded == 2);
    CHECK(st.concatsFolded == 1);
    CHECK(st.base64Decoded == 1);
    CHECK(st.lookedObfuscated);
    std::puts("PASS: escapes_concat_atob");
}

// Classic string array + push/shift rotation IIFE: indices resolve AFTER rotation.
static void test_rotated_string_array() {
    DeobfStats st;
    const std::string out = deobfuscate(
        "var a=['c','a','b'];"
        "(function(x,y){while(--y)x.push(x.shift());})(a,2);"
        "console.log(a[0]);",
        &st);
    CHECK(st.arraysFound == 1);
    CHECK(st.stringsDecoded == 1);
    CHECK(out.find("console.log(\"a\");") != std::string::npos);  // rotated once -> 'a'
    CHECK(st.lookedObfuscated);
    std::puts("PASS: rotated_string_array");
}

// The one-expression wrapper function around an index read: w(1,0) -> m[1].
static void test_wrapper_function() {
    DeobfStats st;
    const std::string out = deobfuscate(
        "var m=['x','y'];function w(i,k){return m[i-k];}console.log(w(1,0));",
        &st);
    CHECK(st.arraysFound == 1);
    CHECK(st.stringsDecoded == 1);
    CHECK(out.find("console.log(\"y\");") != std::string::npos);
    std::puts("PASS: wrapper_function");
}

// Aliased array: `var b = a;` then b[1] still resolves.
static void test_alias() {
    DeobfStats st;
    const std::string out = deobfuscate(
        "var a=['x','y'];var b=a;console.log(b[1]);", &st);
    CHECK(out.find("console.log(\"y\");") != std::string::npos);
    std::puts("PASS: alias");
}

// Non-constant index: NOT decodable with confidence -> nothing is rewritten.
static void test_variable_index_bails() {
    DeobfStats st;
    const std::string out = deobfuscate("var a=['x','y'];var i=0;console.log(a[i]);", &st);
    CHECK(st.stringsDecoded == 0);
    CHECK(out.find("a[i]") != std::string::npos);
    std::puts("PASS: variable_index_bails");
}

// Any mutation of the array disqualifies it entirely.
static void test_mutation_bails() {
    DeobfStats st;
    const std::string out = deobfuscate("var a=['x'];a[0]='z';console.log(a[0]);", &st);
    CHECK(st.stringsDecoded == 0);
    CHECK(out.find("a[0]") != std::string::npos);
    std::puts("PASS: mutation_bails");
}

// A rotation loop shape we do NOT recognize (post-decrement) must disable the
// array rather than decode it against the wrong order.
static void test_unrecognized_rotation_bails() {
    DeobfStats st;
    const std::string out = deobfuscate(
        "var a=['x','y'];"
        "(function(p,q){while(q--)p.push(p.shift());})(a,3);"
        "console.log(a[0]);",
        &st);
    CHECK(st.stringsDecoded == 0);
    CHECK(st.arraysFound == 1);
    CHECK(st.lookedObfuscated);
    CHECK(out.find("a[0]") != std::string::npos);
    std::puts("PASS: unrecognized_rotation_bails");
}

// Templates, regex literals and comments move as single units.
static void test_templates_regex_comments() {
    DeobfStats st;
    const std::string out = deobfuscate(
        "var t=`a${1+2}b`;\n// keep me\nvar r=/a+b/gi;/* block */var s='x';", &st);
    CHECK(out.find("`a${1+2}b`") != std::string::npos);
    CHECK(out.find("// keep me") != std::string::npos);
    CHECK(out.find("/a+b/gi") != std::string::npos);
    CHECK(out.find("/* block */") != std::string::npos);
    std::puts("PASS: templates_regex_comments");
}

int main() {
    test_plain_code_survives();
    test_escapes_concat_atob();
    test_rotated_string_array();
    test_wrapper_function();
    test_alias();
    test_variable_index_bails();
    test_mutation_bails();
    test_unrecognized_rotation_bails();
    test_templates_regex_comments();
    if (g_fail) {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::puts("ALL deobfuscate tests passed");
    return 0;
}
