// tinybank - minimal logging facility
#include "tinybank/log.h"

#include <iostream>

namespace tinybank {

namespace {
LogLevel g_level = LogLevel::Info;

const char* levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}
}  // namespace

void set_log_level(LogLevel level) { g_level = level; }

LogLevel log_level() { return g_level; }

void log_message(LogLevel level, const std::string& message) {
    if (static_cast<int>(level) < static_cast<int>(g_level)) return;
    std::ostream& os = (level == LogLevel::Error) ? std::cerr : std::cout;
    os << "[" << levelName(level) << "] " << message << "\n";
}

void log_debug(const std::string& message) { log_message(LogLevel::Debug, message); }
void log_info(const std::string& message) { log_message(LogLevel::Info, message); }
void log_warn(const std::string& message) { log_message(LogLevel::Warn, message); }
void log_error(const std::string& message) { log_message(LogLevel::Error, message); }

}  // namespace tinybank
