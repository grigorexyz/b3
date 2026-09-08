#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace b3
{

enum class LogLevel
{
    Trace,
    Info,
    Warning,
    Error,
};

/// Sets the minimum severity that is printed. Defaults to LogLevel::Info.
void setLogLevel(LogLevel level);

LogLevel logLevel();

void logMessage(LogLevel level, std::string_view message);

template <typename... Args>
void logTrace(std::format_string<Args...> format, Args&&... args)
{
    logMessage(LogLevel::Trace, std::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
void logInfo(std::format_string<Args...> format, Args&&... args)
{
    logMessage(LogLevel::Info, std::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
void logWarning(std::format_string<Args...> format, Args&&... args)
{
    logMessage(LogLevel::Warning, std::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
void logError(std::format_string<Args...> format, Args&&... args)
{
    logMessage(LogLevel::Error, std::format(format, std::forward<Args>(args)...));
}

} // namespace b3
