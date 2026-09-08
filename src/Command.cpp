#include "b3/Command.hpp"

#include "b3/Log.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace b3
{

struct Command::Impl
{
    std::vector<std::string> m_Arguments;
};

namespace
{

/// Quotes an argument for logging only, never for execution.
std::string renderArgument(const std::string& argument)
{
    const bool needsQuotes = argument.empty()
        || argument.find_first_of(" \t\n\"'\\$&|;<>()") != std::string::npos;
    if (!needsQuotes)
    {
        return argument;
    }

    std::string quoted;
    quoted.reserve(argument.size() + 2);
    quoted.push_back('"');
    for (const char character : argument)
    {
        if (character == '"' || character == '\\')
        {
            quoted.push_back('\\');
        }
        quoted.push_back(character);
    }
    quoted.push_back('"');
    return quoted;
}

} // namespace

Command::Command()
    : m_Impl(std::make_unique<Impl>())
{
}

Command::Command(std::vector<std::string> arguments)
    : m_Impl(std::make_unique<Impl>(Impl{std::move(arguments)}))
{
}

Command::~Command() = default;

Command::Command(const Command& other)
    : m_Impl(std::make_unique<Impl>(*other.m_Impl))
{
}

Command& Command::operator=(const Command& other)
{
    if (this != &other)
    {
        *m_Impl = *other.m_Impl;
    }
    return *this;
}

Command::Command(Command&& other) noexcept = default;

Command& Command::operator=(Command&& other) noexcept = default;

Command& Command::append(std::string argument)
{
    m_Impl->m_Arguments.push_back(std::move(argument));
    return *this;
}

const std::vector<std::string>& Command::arguments() const
{
    return m_Impl->m_Arguments;
}

bool Command::empty() const
{
    return m_Impl->m_Arguments.empty();
}

std::string Command::render() const
{
    std::string rendered;
    for (const std::string& argument : m_Impl->m_Arguments)
    {
        if (!rendered.empty())
        {
            rendered.push_back(' ');
        }
        rendered += renderArgument(argument);
    }
    return rendered;
}

int Command::run() const
{
    const std::vector<std::string>& arguments = m_Impl->m_Arguments;
    if (arguments.empty())
    {
        logError("refusing to run an empty command");
        return -1;
    }

    std::vector<char*> rawArguments;
    rawArguments.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments)
    {
        rawArguments.push_back(const_cast<char*>(argument.c_str()));
    }
    rawArguments.push_back(nullptr);

#if defined(_WIN32)
    const intptr_t status = _spawnvp(_P_WAIT, rawArguments[0], rawArguments.data());
    if (status < 0)
    {
        logError("cannot run '{}'", arguments.front());
        return -1;
    }
    return static_cast<int>(status);
#else
    const pid_t childPid = ::fork();
    if (childPid < 0)
    {
        logError("cannot fork for '{}'", arguments.front());
        return -1;
    }

    if (childPid == 0)
    {
        ::execvp(rawArguments[0], rawArguments.data());
        ::_exit(127);
    }

    int status = 0;
    while (::waitpid(childPid, &status, 0) < 0)
    {
        if (errno != EINTR)
        {
            logError("cannot wait for '{}'", arguments.front());
            return -1;
        }
    }

    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status))
    {
        logError("'{}' was terminated by signal {}", arguments.front(), WTERMSIG(status));
    }
    return -1;
#endif
}

} // namespace b3
