#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace b3
{

/// A single external process invocation, stored as an argument vector so that
/// no shell quoting is ever required. Uses the pImpl pattern to keep the
/// process handling details out of the public interface.
class Command
{
public:
    Command();
    explicit Command(std::vector<std::string> arguments);
    ~Command();

    Command(const Command& other);
    Command& operator=(const Command& other);
    Command(Command&& other) noexcept;
    Command& operator=(Command&& other) noexcept;

    /// Appends a single argument.
    Command& append(std::string argument);

    /// Appends any number of arguments convertible to std::string.
    template <typename... Args>
    Command& appendAll(Args&&... arguments)
    {
        (append(std::string(std::forward<Args>(arguments))), ...);
        return *this;
    }

    [[nodiscard]] const std::vector<std::string>& arguments() const;

    [[nodiscard]] bool empty() const;

    /// Human readable rendering of the command, used for logging.
    [[nodiscard]] std::string render() const;

    /// Runs the command synchronously and returns its exit code. A negative
    /// value means the process could not be started or was terminated.
    [[nodiscard]] int run() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace b3
