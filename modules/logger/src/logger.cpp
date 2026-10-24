#include <lucyn/logger/logger.h>

#include <cstdlib>
#include <iostream>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace lucyn::logger {
namespace {

struct Label {
    const char* name;
    const char* emoji;
    const char* color;
    Threshold minimum;
};

Label labelFor(Level level) {
    switch (level) {
        case Level::debug:   return {"DEBUG", "🐞", "\033[90m", Threshold::debug};
        case Level::info:    return {"INFO", "ℹ️", "\033[36m", Threshold::info};
        case Level::success: return {"SUCCESS", "✅", "\033[32m", Threshold::info};
        case Level::warning: return {"WARN", "⚠️", "\033[33m", Threshold::warning};
        case Level::error:   return {"ERROR", "❌", "\033[31m", Threshold::error};
        case Level::fatal:   return {"FATAL", "💀", "\033[91m", Threshold::fatal};
        case Level::build:   return {"BUILD", "🔧", "\033[34m", Threshold::info};
    }
    return {"INFO", "ℹ️", "\033[36m", Threshold::info};
}

int rank(Threshold threshold) {
    switch (threshold) {
        case Threshold::debug: return 0;
        case Threshold::info: return 1;
        case Threshold::warning: return 2;
        case Threshold::error: return 3;
        case Threshold::fatal: return 4;
        case Threshold::off: return 5;
    }
    return 5;
}

bool colorEnabled() {
    if (std::getenv("NO_COLOR")) return false;
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

}  // namespace

Logger::Logger(Threshold threshold) : threshold_(threshold) {}

void Logger::setThreshold(Threshold threshold) {
    std::lock_guard<std::mutex> lock(mutex_);
    threshold_ = threshold;
}

Threshold Logger::threshold() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return threshold_;
}

bool Logger::enabled(Level level) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rank(labelFor(level).minimum) >= rank(threshold_);
}

void Logger::log(Level level, const std::string& message) {
    const Label label = labelFor(level);
    const bool color = colorEnabled();
    std::lock_guard<std::mutex> lock(mutex_);
    if (rank(label.minimum) < rank(threshold_)) return;
    if (color) std::cerr << label.color;
    std::cerr << '[' << label.name << ']';
    if (color) std::cerr << "\033[0m";
    std::cerr << ' ' << label.emoji << ' ' << message << '\n';
}

Logger& defaultLogger() {
    static Logger logger;
    return logger;
}

}  // namespace lucyn::logger
