// tinybank - minimal logging facility
// Layer: infrastructure (no dependencies of its own)
#pragma once

#include <string>

namespace tinybank {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void set_log_level(LogLevel level);
LogLevel log_level();
void log_message(LogLevel level, const std::string& message);

void log_debug(const std::string& message);
void log_info(const std::string& message);
void log_warn(const std::string& message);
void log_error(const std::string& message);

}  // namespace tinybank
