#include <cstdio>
#include <filesystem>
#include <fstream>
#include <lucyn/core/paths.h>
#include <lucyn/core/overrides.h>

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

    // Overrides testing:
    CHECK(assetFilename("https://example.com/assets/app.js?v=2#hash") == "example.com__app.js");
    CHECK(assetFilename("https://sub.test.org/lib/bundle.mjs") == "sub.test.org__bundle.mjs");
    CHECK(mimeTypeForPath("app.js") == "application/javascript");
    CHECK(mimeTypeForPath("style.css") == "text/css");
    CHECK(mimeTypeForPath("index.html") == "text/html");
    CHECK(mimeTypeForPath("data.json") == "application/json");

    // Initially no override exists for url
    const std::string testUrl = "https://example.com/assets/app.js";
    CHECK(findOverrideFile(testUrl).empty());

    // Save override
    CHECK(saveOverride(testUrl, "console.log('patched');"));
    std::string found = findOverrideFile(testUrl);
    CHECK(!found.empty());
    CHECK(fs::exists(found));
    {
        std::ifstream f(found);
        std::string s;
        f >> s;
        CHECK(s == "console.log('patched');");
    }

    // List overrides
    auto list = listOverrides();
    CHECK(list.size() == 1);
    CHECK(list[0].name == "example.com__app.js");

    // Clear override
    CHECK(clearOverride(testUrl));
    CHECK(findOverrideFile(testUrl).empty());
    CHECK(listOverrides().empty());

    // Hierarchical override: overrides/example.com/assets/app.js
    fs::path hierDir = fs::path(dataDir("overrides")) / "example.com" / "assets";
    fs::create_directories(hierDir);
    {
        std::ofstream hf(hierDir / "app.js");
        hf << "/* hierarchical */";
    }
    CHECK(!findOverrideFile(testUrl).empty());
    fs::remove_all(fs::path(dataDir("overrides")) / "example.com");

    // Auto-deobf fallback from artifacts/
    fs::path artDir = fs::path(dataDir("artifacts"));
    fs::create_directories(artDir);
    {
        std::ofstream af(artDir / "example.com__app.deobf.js");
        af << "/* deobfuscated artifact */";
    }
    OverrideOptions noAuto;
    noAuto.autoDeobf = false;
    CHECK(findOverrideFile(testUrl, noAuto).empty());
    OverrideOptions withAuto;
    withAuto.autoDeobf = true;
    CHECK(!findOverrideFile(testUrl, withAuto).empty());

    setDataRootForTests("");
    CHECK(fs::path(dataRoot()).filename() == "nothing-data");       // restored
    fs::remove_all(tmp);

    std::printf(failures ? "core: %d FAILED\n" : "core: all passed\n", failures);
    return failures ? 1 : 0;
}
