#include <lucyn/core/paths.h>
#include <filesystem>
#include <mutex>
#include <vector>

#if defined(_WIN32)
  #include <windows.h>
#elif defined(__APPLE__)
  #include <mach-o/dyld.h>
#else
  #include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lucyn::core {

namespace {
std::mutex  g_m;
std::string g_override;

std::string findExeDir() {
    std::error_code ec;
#if defined(_WIN32)
    std::vector<wchar_t> buf(32768);
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    if (n > 0 && n < buf.size()) return fs::path(std::wstring(buf.data(), n)).parent_path().string();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1);
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
        fs::path p = fs::canonical(buf.data(), ec);
        if (!ec) return p.parent_path().string();
    }
#else
    std::vector<char> buf(4096);
    ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n > 0) {
        fs::path p = fs::path(std::string(buf.data(), (size_t)n));
        return p.parent_path().string();
    }
#endif
    fs::path cwd = fs::current_path(ec);
    return ec ? std::string(".") : cwd.string();
}
}  // namespace

std::string exeDir() {
    static const std::string dir = findExeDir();
    return dir;
}

std::string dataRoot() {
    {
        std::lock_guard<std::mutex> l(g_m);
        if (!g_override.empty()) return g_override;
    }
    return (fs::path(exeDir()) / "nothing-data").string();
}

std::string dataDir(std::string_view sub) {
    fs::path rel{std::string(sub)};
    if (rel.empty() || rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) return "";
    for (auto& part : rel)
        if (part == ".." || part == ".") return "";
    fs::path p = fs::path(dataRoot()) / rel;
    std::error_code ec;
    fs::create_directories(p, ec);
    return p.string();
}

void setDataRootForTests(const std::string& path) {
    std::lock_guard<std::mutex> l(g_m);
    g_override = path;
}

}  // namespace lucyn::core
