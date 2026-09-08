#include "b3/Log.hpp"

#include <iostream>
#include <mutex>

namespace b3
{

namespace
{

LogLevel g_MinimumLevel = LogLevel::Info;
std::mutex g_LogMutex;

std::string_view levelPrefix(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Trace:
        return "[TRACE] ";
    case LogLevel::Info:
        return "[INFO]  ";
    case LogLevel::Warning:
        return "[WARN]  ";
    case LogLevel::Error:
        return "[ERROR] ";
    }
    return "[?]     ";
}

} // namespace

void setLogLevel(LogLevel level)
{
    const std::scoped_lock lock(g_LogMutex);
    g_MinimumLevel = level;
}

LogLevel logLevel()
{
    const std::scoped_lock lock(g_LogMutex);
    return g_MinimumLevel;
}

void logMessage(LogLevel level, std::string_view message)
{
    const std::scoped_lock lock(g_LogMutex);
    if (level < g_MinimumLevel)
    {
        return;
    }

    std::ostream& stream = level >= LogLevel::Warning ? std::cerr : std::cout;
    stream << levelPrefix(level) << message << std::endl;
}

} // namespace b3
