#include <cstdio>
#include <filesystem>
#include <lucyn/core/paths.h>

namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    using namespace lucyn::core;

    // Default: nothing-data sits right next to the running binary.
    CHECK(!exeDir().empty());
    CHECK(fs::path(dataRoot()).filename() == "nothing-data");
    CHECK(fs::path(dataRoot()).parent_path() == fs::path(exeDir()));

    // Everything below happens in a temp folder, never next to the test binary.
    fs::path tmp = fs::temp_directory_path() / "lucyn_core_test";
    fs::remove_all(tmp);
    setDataRootForTests((tmp / "nothing-data").string());

    CHECK(dataRoot() == (tmp / "nothing-data").string());
    CHECK(!fs::exists(tmp / "nothing-data"));                       // not created until asked

    std::string ids = dataDir("identities");
    CHECK(fs::is_directory(ids));
    CHECK(fs::path(ids) == tmp / "nothing-data" / "identities");
    CHECK(fs::is_directory(fs::path(dataDir("snapshots/example.com"))));   // nesting works

    // A folder name can never walk out of nothing-data.
    CHECK(dataDir("../evil").empty());
    CHECK(dataDir("a/../../evil").empty());
    CHECK(dataDir("/etc").empty());
    CHECK(dataDir("").empty());
    CHECK(!fs::exists(tmp / "evil"));

    setDataRootForTests("");
    CHECK(fs::path(dataRoot()).filename() == "nothing-data");       // restored
    fs::remove_all(tmp);

    std::printf(failures ? "core: %d FAILED\n" : "core: all passed\n", failures);
    return failures ? 1 : 0;
}
