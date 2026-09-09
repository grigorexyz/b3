/// b3 - a small build system in the spirit of make and nob, in a single C++23
/// translation unit.
///
/// Bootstrap it once with
///
///     c++ -std=c++23 -o b3 b3.cpp
///
/// and afterwards just run ./b3, which recompiles and re-executes itself
/// whenever this file changed.
///
/// Conventions: member variables use the m_PascalCase prefix, instances and
/// functions use camelCase, types use PascalCase. Every fixed string in this
/// file is a constexpr std::string_view so that nothing is allocated or copied
/// for it at run time.

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace b3
{

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

enum class LogLevel
{
    Trace,
    Info,
    Warning,
    Error,
};

inline constexpr std::string_view kTracePrefix = "[TRACE] ";
inline constexpr std::string_view kInfoPrefix = "[INFO]  ";
inline constexpr std::string_view kWarningPrefix = "[WARN]  ";
inline constexpr std::string_view kErrorPrefix = "[ERROR] ";

[[nodiscard]] constexpr std::string_view levelPrefix(LogLevel level) noexcept
{
    switch (level)
    {
    case LogLevel::Trace:
        return kTracePrefix;
    case LogLevel::Info:
        return kInfoPrefix;
    case LogLevel::Warning:
        return kWarningPrefix;
    case LogLevel::Error:
        return kErrorPrefix;
    }
    return kInfoPrefix;
}

namespace detail
{

inline LogLevel g_MinimumLevel = LogLevel::Info;
inline std::mutex g_LogMutex;

} // namespace detail

/// Sets the minimum severity that is printed. Defaults to LogLevel::Info.
inline void setLogLevel(LogLevel level)
{
    const std::scoped_lock lock(detail::g_LogMutex);
    detail::g_MinimumLevel = level;
}

[[nodiscard]] inline LogLevel logLevel()
{
    const std::scoped_lock lock(detail::g_LogMutex);
    return detail::g_MinimumLevel;
}

inline void logMessage(LogLevel level, std::string_view message)
{
    const std::scoped_lock lock(detail::g_LogMutex);
    if (level < detail::g_MinimumLevel)
    {
        return;
    }

    // Flushing keeps the ordering of our own output and the output of the
    // child processes we spawn intact.
    std::ostream& stream = level >= LogLevel::Warning ? std::cerr : std::cout;
    stream << levelPrefix(level) << message << std::endl;
}

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

// ---------------------------------------------------------------------------
// File system helpers
// ---------------------------------------------------------------------------

namespace fs
{

using Path = std::filesystem::path;
using FileTime = std::filesystem::file_time_type;

inline constexpr std::string_view kSourceExtension = ".cpp";

inline constexpr std::string_view kPathEnvironmentVariable = "PATH";

#if defined(_WIN32)
inline constexpr char kPathSeparator = ';';
#else
inline constexpr char kPathSeparator = ':';
#endif

inline constexpr std::filesystem::perms kExecutablePermissions =
    std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec
    | std::filesystem::perms::others_exec;

/// Returns the last write time of a path, or std::nullopt when it does not exist.
[[nodiscard]] inline std::optional<FileTime> modificationTime(const Path& path)
{
    std::error_code errorCode;
    const FileTime time = std::filesystem::last_write_time(path, errorCode);
    if (errorCode)
    {
        return std::nullopt;
    }
    return time;
}

[[nodiscard]] inline bool exists(const Path& path)
{
    std::error_code errorCode;
    return std::filesystem::exists(path, errorCode);
}

/// Creates a directory and all of its missing parents. Returns false on failure.
inline bool makeDirectories(const Path& path)
{
    if (path.empty())
    {
        return true;
    }

    std::error_code errorCode;
    std::filesystem::create_directories(path, errorCode);
    if (errorCode)
    {
        logError("cannot create directory '{}': {}", path.string(), errorCode.message());
        return false;
    }
    return true;
}

/// Writes a text file, creating its parent directory when needed.
inline bool writeFile(const Path& path, std::string_view content)
{
    if (path.has_parent_path() && !makeDirectories(path.parent_path()))
    {
        return false;
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        logError("cannot write '{}'", path.string());
        return false;
    }

    stream << content;
    return stream.good();
}

/// Reads a whole file as bytes, or std::nullopt when it cannot be opened.
[[nodiscard]] inline std::optional<std::string> readFile(const Path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

/// Looks a program up the way a shell would: names that already contain a
/// separator are only checked for existence, every other name is searched for
/// in the directories of the PATH environment variable.
[[nodiscard]] inline std::optional<Path> findExecutable(std::string_view name)
{
    const auto isExecutableFile = [](const Path& candidate)
    {
        std::error_code errorCode;
        if (!std::filesystem::is_regular_file(candidate, errorCode))
        {
            return false;
        }
        const std::filesystem::perms permissions =
            std::filesystem::status(candidate, errorCode).permissions();
        if (errorCode)
        {
            return false;
        }
        return (permissions & kExecutablePermissions) != std::filesystem::perms::none;
    };

    if (name.empty())
    {
        return std::nullopt;
    }

    const Path requested{name};
    if (requested.has_parent_path())
    {
        return isExecutableFile(requested) ? std::optional<Path>(requested) : std::nullopt;
    }

    const char* pathVariable = std::getenv(kPathEnvironmentVariable.data());
    if (pathVariable == nullptr)
    {
        return std::nullopt;
    }

    std::string_view remaining(pathVariable);
    while (!remaining.empty())
    {
        const std::size_t separator = remaining.find(kPathSeparator);
        const std::string_view directory = remaining.substr(0, separator);
        remaining = separator == std::string_view::npos ? std::string_view()
                                                        : remaining.substr(separator + 1);
        if (directory.empty())
        {
            continue;
        }

        Path candidate = Path(directory) / name;
        if (isExecutableFile(candidate))
        {
            return candidate;
        }
    }

    return std::nullopt;
}

/// True when an output is missing or when any input is newer than the oldest
/// output. A target without outputs is always considered out of date.
[[nodiscard]] inline bool isOutOfDate(std::span<const Path> outputs, std::span<const Path> inputs)
{
    if (outputs.empty())
    {
        return true;
    }

    std::optional<FileTime> oldestOutput;
    for (const Path& output : outputs)
    {
        const std::optional<FileTime> time = modificationTime(output);
        if (!time.has_value())
        {
            logTrace("output '{}' is missing", output.string());
            return true;
        }
        if (!oldestOutput.has_value() || *time < *oldestOutput)
        {
            oldestOutput = time;
        }
    }

    for (const Path& input : inputs)
    {
        const std::optional<FileTime> time = modificationTime(input);
        if (!time.has_value())
        {
            logTrace("input '{}' is missing", input.string());
            return true;
        }
        if (*time > *oldestOutput)
        {
            logTrace("input '{}' is newer than the outputs", input.string());
            return true;
        }
    }

    return false;
}

/// Lists the files of a directory, non recursive, filtered by extension.
/// Extensions include the leading dot; an empty span accepts every file.
[[nodiscard]] inline std::vector<Path> listFiles(const Path& directory,
                                                 std::span<const std::string_view> extensions)
{
    std::vector<Path> files;

    std::error_code errorCode;
    const std::filesystem::directory_iterator end;
    std::filesystem::directory_iterator iterator(directory, errorCode);
    if (errorCode)
    {
        logWarning("cannot list '{}': {}", directory.string(), errorCode.message());
        return files;
    }

    for (; iterator != end; iterator.increment(errorCode))
    {
        if (errorCode)
        {
            logWarning("cannot list '{}': {}", directory.string(), errorCode.message());
            break;
        }
        if (!iterator->is_regular_file())
        {
            continue;
        }

        const Path& path = iterator->path();
        const bool matches = extensions.empty()
            || std::ranges::any_of(extensions,
                                   [&path](std::string_view extension)
                                   { return path.extension().string() == extension; });
        if (matches)
        {
            files.push_back(path);
        }
    }

    std::ranges::sort(files);
    return files;
}

} // namespace fs

// ---------------------------------------------------------------------------
// Command
// ---------------------------------------------------------------------------

inline constexpr std::string_view kQuotingCharacters = " \t\n\"'\\$&|;<>()";

/// A single external process invocation, stored as an argument vector so that
/// no shell quoting is ever required. The process handling state lives behind
/// a pImpl so that the public interface stays free of platform headers.
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

    /// Appends any number of arguments convertible to std::string, which makes
    /// constexpr std::string_view constants usable directly.
    template <typename... Args>
    Command& appendAll(Args&&... arguments)
    {
        (append(std::string(std::forward<Args>(arguments))), ...);
        return *this;
    }

    [[nodiscard]] const std::vector<std::string>& arguments() const;

    [[nodiscard]] bool empty() const;

    /// Human readable rendering of the command, used for logging only.
    [[nodiscard]] std::string render() const;

    /// Runs the command synchronously and returns its exit code. A negative
    /// value means the process could not be started or was terminated.
    [[nodiscard]] int run() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

struct Command::Impl
{
    std::vector<std::string> m_Arguments;
};

namespace detail
{

/// Quotes an argument for logging only, never for execution.
[[nodiscard]] inline std::string renderArgument(std::string_view argument)
{
    const bool needsQuotes =
        argument.empty() || argument.find_first_of(kQuotingCharacters) != std::string_view::npos;
    if (!needsQuotes)
    {
        return std::string(argument);
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

} // namespace detail

inline Command::Command()
    : m_Impl(std::make_unique<Impl>())
{
}

inline Command::Command(std::vector<std::string> arguments)
    : m_Impl(std::make_unique<Impl>(Impl{std::move(arguments)}))
{
}

inline Command::~Command() = default;

inline Command::Command(const Command& other)
    : m_Impl(std::make_unique<Impl>(*other.m_Impl))
{
}

inline Command& Command::operator=(const Command& other)
{
    if (this != &other)
    {
        *m_Impl = *other.m_Impl;
    }
    return *this;
}

inline Command::Command(Command&& other) noexcept = default;

inline Command& Command::operator=(Command&& other) noexcept = default;

inline Command& Command::append(std::string argument)
{
    m_Impl->m_Arguments.push_back(std::move(argument));
    return *this;
}

inline const std::vector<std::string>& Command::arguments() const
{
    return m_Impl->m_Arguments;
}

inline bool Command::empty() const
{
    return m_Impl->m_Arguments.empty();
}

inline std::string Command::render() const
{
    std::string rendered;
    for (const std::string& argument : m_Impl->m_Arguments)
    {
        if (!rendered.empty())
        {
            rendered.push_back(' ');
        }
        rendered += detail::renderArgument(argument);
    }
    return rendered;
}

inline int Command::run() const
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

// ---------------------------------------------------------------------------
// Target
// ---------------------------------------------------------------------------

/// A make style rule: a named node that produces output files from input files
/// and from other targets by running a list of commands.
class Target
{
public:
    explicit Target(std::string name)
        : m_Name(std::move(name))
    {
    }

    [[nodiscard]] const std::string& name() const { return m_Name; }

    /// A target without outputs is phony and therefore always runs, like a
    /// .PHONY target in make.
    [[nodiscard]] bool isPhony() const { return m_Outputs.empty(); }

    Target& output(fs::Path path)
    {
        m_Outputs.push_back(std::move(path));
        return *this;
    }

    Target& input(fs::Path path)
    {
        m_Inputs.push_back(std::move(path));
        return *this;
    }

    Target& dependsOn(std::string targetName)
    {
        m_Dependencies.push_back(std::move(targetName));
        return *this;
    }

    Target& command(Command command)
    {
        m_Commands.push_back(std::move(command));
        return *this;
    }

    [[nodiscard]] const std::vector<fs::Path>& outputs() const { return m_Outputs; }
    [[nodiscard]] const std::vector<fs::Path>& inputs() const { return m_Inputs; }
    [[nodiscard]] const std::vector<std::string>& dependencies() const { return m_Dependencies; }
    [[nodiscard]] const std::vector<Command>& commands() const { return m_Commands; }

private:
    std::string m_Name;
    std::vector<fs::Path> m_Outputs;
    std::vector<fs::Path> m_Inputs;
    std::vector<std::string> m_Dependencies;
    std::vector<Command> m_Commands;
};

// ---------------------------------------------------------------------------
// The WebAssembly target
// ---------------------------------------------------------------------------

/// b3 targets WebAssembly with a plain clang++ and nothing else. A stock LLVM
/// with the WebAssembly backend and wasm-ld, which every recent clang++ install
/// already ships with, is the whole toolchain: no SDK to install, no sysroot to
/// activate, no separate compiler driver.
///
/// The modules it produces are freestanding: no libc, no start symbol, and
/// every symbol the host should see has to be exported by name.
inline constexpr std::string_view kWasmCompiler = "clang++";
inline constexpr std::string_view kWasmCompilerEnvironmentVariable = "WASM_CXX";

inline constexpr std::string_view kWasmModuleExtension = ".wasm";

/// The first four bytes of every WebAssembly module, "\0asm".
inline constexpr std::string_view kWasmMagic{"\0asm", 4};

/// `--no-entry` drops the requirement for a `_start` symbol and
/// `--allow-undefined` lets the module import host functions it does not
/// define itself.
inline constexpr std::string_view kWasmTargetFlag = "--target=wasm32-unknown-unknown";
inline constexpr std::string_view kWasmFlags[] = {
    "-nostdlib", "-fno-exceptions", "-fno-rtti", "-Wl,--no-entry", "-Wl,--allow-undefined",
};
inline constexpr std::string_view kWasmExportPrefix = "-Wl,--export=";

/// The compiler to invoke, honouring WASM_CXX for cross compilers that are not
/// simply called clang++.
[[nodiscard]] inline std::string wasmCompilerExecutable()
{
    if (const char* fromEnvironment = std::getenv(kWasmCompilerEnvironmentVariable.data());
        fromEnvironment != nullptr && *fromEnvironment != '\0')
    {
        return fromEnvironment;
    }
    return std::string(kWasmCompiler);
}

[[nodiscard]] inline bool isWasmCompilerAvailable()
{
    return fs::findExecutable(wasmCompilerExecutable()).has_value();
}

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

/// Outcome of building a single target.
enum class BuildStatus
{
    UpToDate,
    Rebuilt,
    Failed,
};

inline constexpr std::string_view kDryRunShortFlag = "-n";
inline constexpr std::string_view kDryRunLongFlag = "--dry-run";
inline constexpr std::string_view kAlwaysMakeShortFlag = "-B";
inline constexpr std::string_view kAlwaysMakeLongFlag = "--always-make";
inline constexpr std::string_view kVerboseShortFlag = "-v";
inline constexpr std::string_view kVerboseLongFlag = "--verbose";
inline constexpr std::string_view kQuietShortFlag = "-q";
inline constexpr std::string_view kQuietLongFlag = "--quiet";
inline constexpr std::string_view kHelpShortFlag = "-h";
inline constexpr std::string_view kHelpLongFlag = "--help";

inline constexpr std::string_view kUsage =
    "usage: b3 [-n|--dry-run] [-B|--always-make] [-v|--verbose] [-q|--quiet] "
    "[--self-test] [--example <name>] [target...]";

/// Owns the target graph and executes it. The graph storage and the traversal
/// bookkeeping live behind a pImpl so that new scheduling strategies can be
/// added without touching the public interface.
class Builder
{
public:
    Builder();
    ~Builder();

    Builder(const Builder&) = delete;
    Builder& operator=(const Builder&) = delete;
    Builder(Builder&& other) noexcept;
    Builder& operator=(Builder&& other) noexcept;

    /// Registers a target. An existing target with the same name is replaced.
    Target& addTarget(Target target);

    [[nodiscard]] bool hasTarget(std::string_view name) const;
    [[nodiscard]] const Target* findTarget(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> targetNames() const;

    /// When set, commands are only printed and never executed (make -n).
    void setDryRun(bool dryRun);
    [[nodiscard]] bool dryRun() const;

    /// When set, every target is rebuilt regardless of file timestamps (make -B).
    void setAlwaysMake(bool alwaysMake);
    [[nodiscard]] bool alwaysMake() const;

    /// Builds the named target and everything it depends on, depth first.
    /// Cycles and unknown dependencies are reported as BuildStatus::Failed.
    [[nodiscard]] BuildStatus build(std::string_view name);

    /// Convenience entry point for build scripts: parses the usual make style
    /// flags plus target names and builds them in order, returning an exit code.
    [[nodiscard]] int runCommandLine(int argc, char* argv[], std::string_view defaultTarget);

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

namespace detail
{

enum class VisitState
{
    Unvisited,
    InProgress,
    Done,
};

} // namespace detail

struct Builder::Impl
{
    std::map<std::string, Target, std::less<>> m_Targets;
    std::map<std::string, detail::VisitState, std::less<>> m_States;
    std::map<std::string, BuildStatus, std::less<>> m_Results;
    bool m_DryRun = false;
    bool m_AlwaysMake = false;

    [[nodiscard]] BuildStatus buildTarget(std::string_view name);
    [[nodiscard]] bool runCommands(const Target& target);
};

inline bool Builder::Impl::runCommands(const Target& target)
{
    if (!m_DryRun)
    {
        for (const fs::Path& output : target.outputs())
        {
            if (output.has_parent_path() && !fs::makeDirectories(output.parent_path()))
            {
                return false;
            }
        }
    }

    for (const Command& command : target.commands())
    {
        logInfo("{}", command.render());
        if (m_DryRun)
        {
            continue;
        }

        const int exitCode = command.run();
        if (exitCode != 0)
        {
            logError("target '{}' failed with exit code {}", target.name(), exitCode);
            return false;
        }
    }

    return true;
}

inline BuildStatus Builder::Impl::buildTarget(std::string_view name)
{
    if (const auto result = m_Results.find(name); result != m_Results.end())
    {
        return result->second;
    }

    const auto entry = m_Targets.find(name);
    if (entry == m_Targets.end())
    {
        logError("unknown target '{}'", name);
        return BuildStatus::Failed;
    }

    const std::string targetName(name);
    if (m_States[targetName] == detail::VisitState::InProgress)
    {
        logError("dependency cycle detected at target '{}'", targetName);
        m_Results[targetName] = BuildStatus::Failed;
        return BuildStatus::Failed;
    }
    m_States[targetName] = detail::VisitState::InProgress;

    const Target& target = entry->second;

    bool dependencyRebuilt = false;
    std::vector<fs::Path> inputs = target.inputs();

    for (const std::string& dependency : target.dependencies())
    {
        const BuildStatus status = buildTarget(dependency);
        if (status == BuildStatus::Failed)
        {
            m_States[targetName] = detail::VisitState::Done;
            m_Results[targetName] = BuildStatus::Failed;
            return BuildStatus::Failed;
        }
        if (status == BuildStatus::Rebuilt)
        {
            dependencyRebuilt = true;
        }

        if (const auto dependencyEntry = m_Targets.find(dependency);
            dependencyEntry != m_Targets.end())
        {
            const std::vector<fs::Path>& dependencyOutputs = dependencyEntry->second.outputs();
            inputs.insert(inputs.end(), dependencyOutputs.begin(), dependencyOutputs.end());
        }
    }

    m_States[targetName] = detail::VisitState::Done;

    const bool outOfDate = m_AlwaysMake || dependencyRebuilt || target.isPhony()
        || fs::isOutOfDate(target.outputs(), inputs);
    if (!outOfDate)
    {
        logTrace("target '{}' is up to date", targetName);
        m_Results[targetName] = BuildStatus::UpToDate;
        return BuildStatus::UpToDate;
    }

    const BuildStatus status = runCommands(target) ? BuildStatus::Rebuilt : BuildStatus::Failed;
    m_Results[targetName] = status;
    return status;
}

inline Builder::Builder()
    : m_Impl(std::make_unique<Impl>())
{
}

inline Builder::~Builder() = default;

inline Builder::Builder(Builder&& other) noexcept = default;

inline Builder& Builder::operator=(Builder&& other) noexcept = default;

inline Target& Builder::addTarget(Target target)
{
    const std::string name = target.name();
    m_Impl->m_Results.clear();
    const auto [entry, inserted] = m_Impl->m_Targets.insert_or_assign(name, std::move(target));
    if (!inserted)
    {
        logWarning("target '{}' was redefined", name);
    }
    return entry->second;
}

inline bool Builder::hasTarget(std::string_view name) const
{
    return m_Impl->m_Targets.contains(name);
}

inline const Target* Builder::findTarget(std::string_view name) const
{
    const auto entry = m_Impl->m_Targets.find(name);
    return entry == m_Impl->m_Targets.end() ? nullptr : &entry->second;
}

inline std::vector<std::string> Builder::targetNames() const
{
    std::vector<std::string> names;
    names.reserve(m_Impl->m_Targets.size());
    for (const auto& [name, target] : m_Impl->m_Targets)
    {
        names.push_back(name);
    }
    return names;
}

inline void Builder::setDryRun(bool dryRun)
{
    m_Impl->m_DryRun = dryRun;
}

inline bool Builder::dryRun() const
{
    return m_Impl->m_DryRun;
}

inline void Builder::setAlwaysMake(bool alwaysMake)
{
    m_Impl->m_AlwaysMake = alwaysMake;
}

inline bool Builder::alwaysMake() const
{
    return m_Impl->m_AlwaysMake;
}

inline BuildStatus Builder::build(std::string_view name)
{
    m_Impl->m_States.clear();
    m_Impl->m_Results.clear();
    return m_Impl->buildTarget(name);
}

inline int Builder::runCommandLine(int argc, char* argv[], std::string_view defaultTarget)
{
    std::vector<std::string> requestedTargets;

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument == kDryRunShortFlag || argument == kDryRunLongFlag)
        {
            setDryRun(true);
        }
        else if (argument == kAlwaysMakeShortFlag || argument == kAlwaysMakeLongFlag)
        {
            setAlwaysMake(true);
        }
        else if (argument == kVerboseShortFlag || argument == kVerboseLongFlag)
        {
            setLogLevel(LogLevel::Trace);
        }
        else if (argument == kQuietShortFlag || argument == kQuietLongFlag)
        {
            setLogLevel(LogLevel::Warning);
        }
        else if (argument == kHelpShortFlag || argument == kHelpLongFlag)
        {
            logMessage(LogLevel::Error, kUsage);
            logMessage(LogLevel::Error, "targets:");
            for (const std::string& name : targetNames())
            {
                logMessage(LogLevel::Error, "  " + name);
            }
            return 0;
        }
        else if (argument.starts_with('-'))
        {
            logError("unknown option '{}'", argument);
            return 2;
        }
        else
        {
            requestedTargets.emplace_back(argument);
        }
    }

    if (requestedTargets.empty())
    {
        requestedTargets.emplace_back(defaultTarget);
    }

    for (const std::string& name : requestedTargets)
    {
        const BuildStatus status = build(name);
        if (status == BuildStatus::Failed)
        {
            return 1;
        }
        if (status == BuildStatus::UpToDate)
        {
            logInfo("'{}' is up to date", name);
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Self rebuilding, nob style
// ---------------------------------------------------------------------------

inline constexpr std::string_view kCompilerEnvironmentVariable = "CXX";
inline constexpr std::string_view kDefaultCompiler = "c++";
inline constexpr std::string_view kStandardFlag = "-std=c++23";
inline constexpr std::string_view kOptimizeFlag = "-O2";
inline constexpr std::string_view kOutputFlag = "-o";
inline constexpr std::string_view kCompileOnlyFlag = "-c";
inline constexpr std::string_view kWarningFlags[] = {"-Wall", "-Wextra"};
inline constexpr std::string_view kBackupSuffix = ".old";

[[nodiscard]] inline std::string compilerExecutable()
{
    if (const char* fromEnvironment = std::getenv(kCompilerEnvironmentVariable.data());
        fromEnvironment != nullptr && *fromEnvironment != '\0')
    {
        return fromEnvironment;
    }
    return std::string(kDefaultCompiler);
}

/// When one of the sources of the build script is newer than the running
/// executable, the script is recompiled and re-executed with the original
/// arguments. On success this function does not return.
inline void rebuildYourself(int argc,
                            char* argv[],
                            std::span<const fs::Path> sources,
                            std::span<const std::string_view> extraFlags = {})
{
    if (argc < 1 || argv[0] == nullptr || sources.empty())
    {
        return;
    }

    const fs::Path executable(argv[0]);
    const std::array<fs::Path, 1> outputs{executable};
    if (!fs::isOutOfDate(outputs, sources))
    {
        return;
    }

    logInfo("build script is out of date, rebuilding '{}'", executable.string());

    const fs::Path backup = fs::Path(executable).concat(kBackupSuffix);
    std::error_code errorCode;
    std::filesystem::rename(executable, backup, errorCode);
    if (errorCode)
    {
        logWarning("cannot move '{}' aside: {}", executable.string(), errorCode.message());
    }

    Command compile;
    compile.appendAll(compilerExecutable(), kStandardFlag, kOptimizeFlag, kOutputFlag,
                      executable.string());
    for (const fs::Path& source : sources)
    {
        compile.append(source.string());
    }
    for (const std::string_view flag : extraFlags)
    {
        compile.append(std::string(flag));
    }

    logInfo("{}", compile.render());
    if (compile.run() != 0)
    {
        logError("rebuilding the build script failed, restoring the previous binary");
        std::filesystem::rename(backup, executable, errorCode);
        std::exit(1);
    }

    std::filesystem::remove(backup, errorCode);

    Command restart;
    for (int index = 0; index < argc; ++index)
    {
        restart.append(argv[index] == nullptr ? std::string() : std::string(argv[index]));
    }

    logInfo("restarting '{}'", executable.string());
    const int exitCode = restart.run();
    std::exit(exitCode < 0 ? 1 : exitCode);
}

// ---------------------------------------------------------------------------
// WebAssembly modules
// ---------------------------------------------------------------------------

/// Describes a WebAssembly module and lowers it to a clang++ invocation, so a
/// build script never has to spell out the freestanding wasm32 flags itself.
class WasmModule
{
public:
    explicit WasmModule(fs::Path output)
        : m_Output(std::move(output))
    {
    }

    WasmModule& source(fs::Path path)
    {
        m_Sources.push_back(std::move(path));
        return *this;
    }

    /// Requests that a symbol stays visible to the host.
    WasmModule& exportSymbol(std::string symbol)
    {
        m_ExportedSymbols.push_back(std::move(symbol));
        return *this;
    }

    /// Adds a flag that is passed to the compiler as is.
    WasmModule& flag(std::string flag)
    {
        m_ExtraFlags.push_back(std::move(flag));
        return *this;
    }

    [[nodiscard]] const fs::Path& output() const { return m_Output; }
    [[nodiscard]] const std::vector<fs::Path>& sources() const { return m_Sources; }
    [[nodiscard]] const std::vector<std::string>& exportedSymbols() const
    {
        return m_ExportedSymbols;
    }
    [[nodiscard]] const std::vector<std::string>& extraFlags() const { return m_ExtraFlags; }

    [[nodiscard]] Command compileCommand() const
    {
        Command command;
        command.appendAll(wasmCompilerExecutable(), kStandardFlag, kOptimizeFlag, kWasmTargetFlag);

        for (const std::string_view flag : kWasmFlags)
        {
            command.append(std::string(flag));
        }
        for (const std::string& symbol : m_ExportedSymbols)
        {
            command.append(std::string(kWasmExportPrefix) + symbol);
        }
        for (const std::string& flag : m_ExtraFlags)
        {
            command.append(flag);
        }

        command.appendAll(kOutputFlag, m_Output.string());
        for (const fs::Path& source : m_Sources)
        {
            command.append(source.string());
        }

        return command;
    }

    /// A ready made target, so a module drops straight into a build graph.
    [[nodiscard]] Target target(std::string name) const
    {
        Target target(std::move(name));
        target.output(m_Output).command(compileCommand());
        for (const fs::Path& source : m_Sources)
        {
            target.input(source);
        }
        return target;
    }

private:
    fs::Path m_Output;
    std::vector<fs::Path> m_Sources;
    std::vector<std::string> m_ExportedSymbols;
    std::vector<std::string> m_ExtraFlags;
};

/// True when the file really is a WebAssembly module, checked by its magic.
[[nodiscard]] inline bool isWasmModule(const fs::Path& path)
{
    const std::optional<std::string> content = fs::readFile(path);
    return content.has_value() && std::string_view(*content).starts_with(kWasmMagic);
}

} // namespace b3

// ---------------------------------------------------------------------------
// Examples
// ---------------------------------------------------------------------------

/// The examples are not separate files on disk: every one of them materialises
/// its own little project in a scratch directory and drives it with its own
/// b3::Builder instance. The build script spawns them as child processes of
/// itself, so each example runs in a fresh process with a fresh graph.
namespace b3::examples
{

inline constexpr std::string_view kExampleFlag = "--example";
inline constexpr std::string_view kAllExamples = "all";
inline constexpr std::string_view kWasmExampleName = "wasm";
inline constexpr std::string_view kScratchDirectoryName = "b3-examples";

inline constexpr std::string_view kHelloSource = R"(#include <iostream>

int main()
{
    std::cout << "hello from a b3 built program" << std::endl;
    return 0;
}
)";

inline constexpr std::string_view kGreeterHeader = R"(#pragma once

void greet();
)";

inline constexpr std::string_view kGreeterSource = R"(#include "Greeter.hpp"

#include <iostream>

void greet()
{
    std::cout << "hello from a b3 built static library" << std::endl;
}
)";

inline constexpr std::string_view kGreeterMainSource = R"(#include "Greeter.hpp"

int main()
{
    greet();
    return 0;
}
)";

/// A freestanding translation unit: it uses no libc, so clang++ can compile it
/// straight to wasm32 without a sysroot.
inline constexpr std::string_view kWasmSource = R"(extern "C" int add(int left, int right)
{
    return left + right;
}

extern "C" int factorial(int value)
{
    int result = 1;
    for (int factor = 2; factor <= value; ++factor)
    {
        result *= factor;
    }
    return result;
}
)";

inline constexpr std::string_view kWasmExportedSymbols[] = {"add", "factorial"};

/// A named example together with the function that runs it.
struct Example
{
    std::string_view m_Name;
    std::string_view m_Description;
    bool (*m_Run)(const fs::Path& directory);
};

[[nodiscard]] inline fs::Path scratchDirectory(std::string_view name)
{
    const fs::Path directory =
        std::filesystem::temp_directory_path() / kScratchDirectoryName / name;
    std::error_code errorCode;
    std::filesystem::remove_all(directory, errorCode);
    fs::makeDirectories(directory);
    return directory;
}

/// Compiles one translation unit into an object file.
[[nodiscard]] inline Target compileTarget(std::string_view name,
                                          const fs::Path& source,
                                          const fs::Path& object,
                                          std::span<const std::string> flags = {})
{
    Command compile;
    compile.appendAll(compilerExecutable(), kStandardFlag);
    for (const std::string_view flag : kWarningFlags)
    {
        compile.append(std::string(flag));
    }
    for (const std::string& flag : flags)
    {
        compile.append(flag);
    }
    compile.appendAll(kCompileOnlyFlag, source.string(), kOutputFlag, object.string());

    Target target{std::string(name)};
    target.output(object).input(source).command(std::move(compile));
    return target;
}

/// Compiles and runs a single file program, then proves that a second build of
/// the very same graph does nothing because everything is up to date.
[[nodiscard]] inline bool runHelloExample(const fs::Path& directory)
{
    const fs::Path source = directory / "Hello.cpp";
    const fs::Path object = directory / "obj" / "Hello.o";
    const fs::Path program = directory / "hello";

    if (!fs::writeFile(source, kHelloSource))
    {
        return false;
    }

    Builder builder;
    builder.addTarget(compileTarget("compile", source, object));

    Command link;
    link.appendAll(compilerExecutable(), kStandardFlag, kOutputFlag, program.string(),
                   object.string());
    Target linkTarget("link");
    linkTarget.output(program).dependsOn("compile").command(std::move(link));
    builder.addTarget(std::move(linkTarget));

    Target run("run");
    run.dependsOn("link").command(Command({program.string()}));
    builder.addTarget(std::move(run));

    if (builder.build("link") != BuildStatus::Rebuilt)
    {
        logError("the first build should have rebuilt everything");
        return false;
    }
    if (builder.build("link") != BuildStatus::UpToDate)
    {
        logError("the second build should have been a no operation");
        return false;
    }

    return builder.build("run") == BuildStatus::Rebuilt;
}

/// Archives two objects into a static library and links a program against it.
[[nodiscard]] inline bool runLibraryExample(const fs::Path& directory)
{
    const fs::Path header = directory / "Greeter.hpp";
    const fs::Path greeterSource = directory / "Greeter.cpp";
    const fs::Path mainSource = directory / "Main.cpp";
    const fs::Path greeterObject = directory / "obj" / "Greeter.o";
    const fs::Path mainObject = directory / "obj" / "Main.o";
    const fs::Path library = directory / "lib" / "libgreeter.a";
    const fs::Path program = directory / "greeter";

    if (!fs::writeFile(header, kGreeterHeader) || !fs::writeFile(greeterSource, kGreeterSource)
        || !fs::writeFile(mainSource, kGreeterMainSource))
    {
        return false;
    }

    const std::vector<std::string> includeFlags{"-I" + directory.string()};

    Builder builder;
    builder.addTarget(compileTarget("compile:greeter", greeterSource, greeterObject, includeFlags))
        .input(header);
    builder.addTarget(compileTarget("compile:main", mainSource, mainObject, includeFlags))
        .input(header);

    Command archive;
    archive.appendAll("ar", "rcs", library.string(), greeterObject.string());
    Target archiveTarget("lib");
    archiveTarget.output(library).dependsOn("compile:greeter").command(std::move(archive));
    builder.addTarget(std::move(archiveTarget));

    Command link;
    link.appendAll(compilerExecutable(), kStandardFlag, kOutputFlag, program.string(),
                   mainObject.string(), library.string());
    Target linkTarget("link");
    linkTarget.output(program).dependsOn("compile:main").dependsOn("lib").command(std::move(link));
    builder.addTarget(std::move(linkTarget));

    Target run("run");
    run.dependsOn("link").command(Command({program.string()}));
    builder.addTarget(std::move(run));

    return builder.build("run") == BuildStatus::Rebuilt;
}

/// Shows phony targets, dry runs and the way a failing command aborts a build.
[[nodiscard]] inline bool runPipelineExample(const fs::Path& directory)
{
    const fs::Path stamp = directory / "stamp.txt";

    Builder builder;

    Target generate("generate");
    generate.output(stamp).command(Command({"touch", stamp.string()}));
    builder.addTarget(std::move(generate));

    Target report("report");
    report.dependsOn("generate").command(Command({"echo", "the stamp is ready"}));
    builder.addTarget(std::move(report));

    Target broken("broken");
    broken.command(Command({"false"}));
    builder.addTarget(std::move(broken));

    builder.setDryRun(true);
    if (builder.build("report") != BuildStatus::Rebuilt || fs::exists(stamp))
    {
        logError("a dry run must not touch the file system");
        return false;
    }

    builder.setDryRun(false);
    if (builder.build("report") != BuildStatus::Rebuilt || !fs::exists(stamp))
    {
        logError("the pipeline should have produced the stamp");
        return false;
    }

    // A phony target always runs again, an out of date check is never applied.
    if (builder.build("report") != BuildStatus::Rebuilt)
    {
        logError("a phony target must always run");
        return false;
    }

    logInfo("the next target fails on purpose");
    return builder.build("broken") == BuildStatus::Failed;
}

/// Builds a freestanding WebAssembly module with clang++ and checks that the
/// result really is one.
[[nodiscard]] inline bool runWasmExample(const fs::Path& directory)
{
    if (!isWasmCompilerAvailable())
    {
        logWarning("'{}' is not on the PATH, skipping the WebAssembly example",
                   wasmCompilerExecutable());
        return true;
    }

    logInfo("using '{}'", wasmCompilerExecutable());

    const fs::Path source = directory / "Math.cpp";
    const fs::Path module = fs::Path(directory / "math").concat(kWasmModuleExtension);

    if (!fs::writeFile(source, kWasmSource))
    {
        return false;
    }

    WasmModule wasmModule(module);
    wasmModule.source(source);
    for (const std::string_view symbol : kWasmExportedSymbols)
    {
        wasmModule.exportSymbol(std::string(symbol));
    }

    Builder builder;
    builder.addTarget(wasmModule.target("wasm"));

    if (builder.build("wasm") != BuildStatus::Rebuilt)
    {
        logError("the module should have been built");
        return false;
    }
    if (!isWasmModule(module))
    {
        logError("'{}' is not a WebAssembly module", module.string());
        return false;
    }
    if (builder.build("wasm") != BuildStatus::UpToDate)
    {
        logError("the second build should have been a no operation");
        return false;
    }

    logInfo("built '{}' exporting {} symbol(s)", module.filename().string(),
            wasmModule.exportedSymbols().size());
    return true;
}

inline constexpr std::array<Example, 4> kExamples{{
    {"hello", "compile, link and run a single file program", &runHelloExample},
    {"library", "archive a static library and link against it", &runLibraryExample},
    {"pipeline", "phony targets, dry runs and failing commands", &runPipelineExample},
    {"wasm", "build a freestanding module with clang++", &runWasmExample},
}};

[[nodiscard]] inline const Example* findExample(std::string_view name)
{
    const auto entry = std::ranges::find(kExamples, name, &Example::m_Name);
    return entry == kExamples.end() ? nullptr : &*entry;
}

/// Runs one example, or every example when the name is "all".
[[nodiscard]] inline int runExample(std::string_view name)
{
    if (name == kAllExamples)
    {
        for (const Example& example : kExamples)
        {
            if (const int exitCode = runExample(example.m_Name); exitCode != 0)
            {
                return exitCode;
            }
        }
        return 0;
    }

    const Example* example = findExample(name);
    if (example == nullptr)
    {
        logError("unknown example '{}'", name);
        logMessage(LogLevel::Error, "examples:");
        for (const Example& known : kExamples)
        {
            logMessage(LogLevel::Error,
                       std::format("  {:<10} {}", known.m_Name, known.m_Description));
        }
        return 2;
    }

    logInfo("running example '{}': {}", example->m_Name, example->m_Description);

    const fs::Path directory = scratchDirectory(example->m_Name);
    const bool succeeded = example->m_Run(directory);

    std::error_code errorCode;
    std::filesystem::remove_all(directory, errorCode);

    if (!succeeded)
    {
        logError("example '{}' failed", example->m_Name);
        return 1;
    }

    logInfo("example '{}' succeeded", example->m_Name);
    return 0;
}

} // namespace b3::examples

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

namespace b3::tests
{

inline constexpr std::string_view kSelfTestFlag = "--self-test";
inline constexpr std::string_view kScratchDirectoryName = "b3-self-test";
inline constexpr std::string_view kOkPrefix = "ok   ";
inline constexpr std::string_view kFailPrefix = "FAIL ";
inline constexpr std::string_view kPayload = "payload";

inline int g_Failures = 0;

inline void check(bool condition, std::string_view what)
{
    if (condition)
    {
        std::cout << kOkPrefix << what << '\n';
        return;
    }
    std::cout << kFailPrefix << what << '\n';
    ++g_Failures;
}

inline void testCommandRendering()
{
    Command command;
    command.appendAll(kDefaultCompiler, kOutputFlag, "build/with space");
    check(command.arguments().size() == 3, "command keeps every argument");
    check(command.render() == R"(c++ -o "build/with space")", "command quotes for logging");
    check(Command().empty(), "default constructed command is empty");
}

inline void testCommandExecution()
{
    const Command trueCommand({"true"});
    check(trueCommand.run() == 0, "successful command returns zero");

    const Command falseCommand({"false"});
    check(falseCommand.run() != 0, "failing command returns non zero");
}

inline void testOutOfDateDetection(const fs::Path& scratch)
{
    const fs::Path input = scratch / "input.txt";
    const fs::Path output = scratch / "output.txt";
    check(fs::writeFile(input, kPayload), "writeFile creates a file");

    const std::array<fs::Path, 1> outputs{output};
    const std::array<fs::Path, 1> inputs{input};
    check(fs::isOutOfDate(outputs, inputs), "missing output is out of date");

    check(fs::writeFile(output, kPayload), "writeFile overwrites a file");
    std::filesystem::last_write_time(output,
                                     *fs::modificationTime(input) + std::chrono::seconds(1));
    check(!fs::isOutOfDate(outputs, inputs), "newer output is up to date");

    std::filesystem::last_write_time(input,
                                     *fs::modificationTime(output) + std::chrono::seconds(1));
    check(fs::isOutOfDate(outputs, inputs), "newer input is out of date");
}

inline void testListFiles(const fs::Path& scratch)
{
    const fs::Path directory = scratch / "listing";
    check(fs::writeFile(directory / "b.cpp", kPayload), "listing fixture b.cpp");
    check(fs::writeFile(directory / "a.cpp", kPayload), "listing fixture a.cpp");
    check(fs::writeFile(directory / "notes.txt", kPayload), "listing fixture notes.txt");

    const std::array<std::string_view, 1> extensions{fs::kSourceExtension};
    const std::vector<fs::Path> sources = fs::listFiles(directory, extensions);
    check(sources.size() == 2, "listFiles filters by extension");
    check(!sources.empty() && sources.front().filename() == "a.cpp", "listFiles sorts its result");
    check(fs::listFiles(directory, {}).size() == 3, "an empty filter accepts every file");
}

inline void testBuilderRebuildsAndCaches(const fs::Path& scratch)
{
    const fs::Path input = scratch / "copy-input.txt";
    const fs::Path output = scratch / "nested" / "copy-output.txt";
    check(fs::writeFile(input, kPayload), "copy fixture");

    Builder builder;
    Target copy("copy");
    Command copyCommand;
    copyCommand.appendAll("cp", input.string(), output.string());
    copy.output(output).input(input).command(std::move(copyCommand));
    builder.addTarget(std::move(copy));

    check(builder.build("copy") == BuildStatus::Rebuilt, "first build runs the command");
    check(fs::exists(output), "build creates the output and its directory");
    check(builder.build("copy") == BuildStatus::UpToDate, "second build is a no operation");

    builder.setAlwaysMake(true);
    check(builder.build("copy") == BuildStatus::Rebuilt, "always make forces a rebuild");
}

inline void testBuilderDependenciesAndFailures()
{
    Builder builder;

    Target first("first");
    first.command(Command({"true"}));
    builder.addTarget(std::move(first));

    Target second("second");
    second.dependsOn("first").command(Command({"true"}));
    builder.addTarget(std::move(second));

    check(builder.build("second") == BuildStatus::Rebuilt, "phony targets always run");
    check(builder.build("missing") == BuildStatus::Failed, "unknown target fails");

    Target broken("broken");
    broken.command(Command({"false"}));
    builder.addTarget(std::move(broken));
    check(builder.build("broken") == BuildStatus::Failed, "failing command fails the target");

    Target left("left");
    left.dependsOn("right");
    builder.addTarget(std::move(left));
    Target right("right");
    right.dependsOn("left");
    builder.addTarget(std::move(right));
    check(builder.build("left") == BuildStatus::Failed, "dependency cycle is detected");
}

inline void testDryRun(const fs::Path& scratch)
{
    const fs::Path output = scratch / "dry-run.txt";

    Builder builder;
    Target touch("touch");
    Command touchCommand;
    touchCommand.appendAll("touch", output.string());
    touch.output(output).command(std::move(touchCommand));
    builder.addTarget(std::move(touch));

    builder.setDryRun(true);
    check(builder.build("touch") == BuildStatus::Rebuilt, "dry run reports a rebuild");
    check(!fs::exists(output), "dry run does not touch the file system");
}

inline void testTargetRegistry()
{
    Builder builder;
    builder.addTarget(Target("alpha"));
    builder.addTarget(Target("beta"));

    check(builder.hasTarget("alpha"), "registered target is found");
    check(builder.findTarget("gamma") == nullptr, "unknown target is not found");
    check(builder.targetNames().size() == 2, "target names are listed");
    check(Target("alpha").isPhony(), "a target without outputs is phony");
}

inline void testExampleRegistry()
{
    check(examples::findExample("hello") != nullptr, "known example is found");
    check(examples::findExample("wasm") != nullptr, "the wasm example is registered");
    check(examples::findExample("nope") == nullptr, "unknown example is not found");
    check(examples::runExample("nope") == 2, "unknown example reports a usage error");
}

inline void testFindExecutable(const fs::Path& scratch)
{
    check(fs::findExecutable("sh").has_value(), "a program on the PATH is found");
    check(!fs::findExecutable("b3-definitely-not-a-program").has_value(),
          "a missing program is not found");
    check(!fs::findExecutable("").has_value(), "an empty name is not a program");

    const fs::Path plainFile = scratch / "not-executable.txt";
    check(fs::writeFile(plainFile, kPayload), "executable lookup fixture");
    check(!fs::findExecutable(plainFile.string()).has_value(),
          "a path that is not executable is rejected");
}

inline void testWasmCommands()
{
    WasmModule module(fs::Path("build/math.wasm"));
    module.source(fs::Path("Math.cpp")).exportSymbol("add").exportSymbol("factorial");

    const std::string command = module.compileCommand().render();
    check(command.starts_with(wasmCompilerExecutable()), "the module is built with clang++");
    check(command.contains(kWasmTargetFlag), "clang++ is pointed at wasm32");
    check(command.contains("-nostdlib") && command.contains("-Wl,--no-entry"),
          "the module is freestanding");
    check(command.contains("-Wl,--export=add") && command.contains("-Wl,--export=factorial"),
          "every requested symbol is exported");
    check(command.contains("-o build/math.wasm") && command.ends_with("Math.cpp"),
          "the module and its sources are passed on");

    const std::string withFlag =
        WasmModule(fs::Path("m.wasm")).flag("-DNDEBUG").compileCommand().render();
    check(withFlag.contains("-DNDEBUG"), "extra flags are passed through");

    const Target target = module.target("wasm");
    check(target.outputs().size() == 1 && target.inputs().size() == 1,
          "a module target carries its output and its sources");
    check(!target.isPhony(), "a module target is not phony");
}

inline void testWasmModuleDetection(const fs::Path& scratch)
{
    const fs::Path notAModule = scratch / "not-a-module.wasm";
    check(fs::writeFile(notAModule, kPayload), "module detection fixture");
    check(!isWasmModule(notAModule), "a text file is not a module");
    check(!isWasmModule(scratch / "missing.wasm"), "a missing file is not a module");

    const fs::Path module = scratch / "module.wasm";
    check(fs::writeFile(module, kWasmMagic), "module magic fixture");
    check(isWasmModule(module), "the magic bytes identify a module");
}

/// Compile time checks: none of these produce any code.
static_assert(levelPrefix(LogLevel::Error) == kErrorPrefix);
static_assert(kUsage.starts_with("usage:"));
static_assert(fs::kSourceExtension == ".cpp");
static_assert(examples::kExamples.size() == 4);
static_assert(examples::kExamples.front().m_Name == "hello");
static_assert(examples::kExamples.back().m_Name == "wasm");
static_assert(kWasmCompiler == "clang++");
static_assert(kWasmTargetFlag.starts_with("--target=wasm32"));
static_assert(kWasmExportPrefix == "-Wl,--export=");
static_assert(kWasmMagic.size() == 4 && kWasmMagic[1] == 'a');
[[nodiscard]] inline int runSelfTest()
{
    const fs::Path scratch = std::filesystem::temp_directory_path() / kScratchDirectoryName;
    std::error_code errorCode;
    std::filesystem::remove_all(scratch, errorCode);
    fs::makeDirectories(scratch);

    testCommandRendering();
    testCommandExecution();
    testOutOfDateDetection(scratch);
    testListFiles(scratch);
    testBuilderRebuildsAndCaches(scratch);
    testBuilderDependenciesAndFailures();
    testDryRun(scratch);
    testTargetRegistry();
    testExampleRegistry();
    testFindExecutable(scratch);
    testWasmCommands();
    testWasmModuleDetection(scratch);

    std::filesystem::remove_all(scratch, errorCode);

    if (g_Failures > 0)
    {
        std::cout << g_Failures << " test(s) failed" << std::endl;
        return 1;
    }

    std::cout << "all tests passed" << std::endl;
    return 0;
}

} // namespace b3::tests

// ---------------------------------------------------------------------------
// The build script for b3 itself
// ---------------------------------------------------------------------------

namespace
{

inline constexpr std::string_view kSelfSource = "b3.cpp";
inline constexpr std::string_view kSelfBinary = "build/b3";
inline constexpr std::string_view kBuildDirectory = "build";
inline constexpr std::string_view kDefaultTarget = "build";
inline constexpr std::string_view kCheckTarget = "check";
inline constexpr std::string_view kExamplesTarget = "examples";
inline constexpr std::string_view kWasmTarget = "wasm";
inline constexpr std::string_view kCleanTarget = "clean";

/// Spawns this very executable again with the given arguments. This is how the
/// example and test targets get their own fresh process.
[[nodiscard]] b3::Command spawnSelf(std::string_view executable,
                                    std::span<const std::string_view> arguments)
{
    b3::Command command;
    command.append(std::string(executable));
    for (const std::string_view argument : arguments)
    {
        command.append(std::string(argument));
    }
    return command;
}

} // namespace

int main(int argc, char* argv[])
{
    using namespace b3;

    // The two in process modes are handled before anything else so that the
    // spawned children never re-enter the build graph.
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument == tests::kSelfTestFlag)
        {
            return tests::runSelfTest();
        }
        if (argument == examples::kExampleFlag)
        {
            const std::string_view name =
                index + 1 < argc ? std::string_view(argv[index + 1]) : examples::kAllExamples;
            return examples::runExample(name);
        }
    }

    const std::array<fs::Path, 1> selfSources{fs::Path(kSelfSource)};
    rebuildYourself(argc, argv, selfSources);

    const std::string executable = argc > 0 && argv[0] != nullptr ? argv[0] : "./b3";

    Builder builder;

    Command compile;
    compile.appendAll(compilerExecutable(), kStandardFlag);
    for (const std::string_view flag : kWarningFlags)
    {
        compile.append(std::string(flag));
    }
    compile.appendAll(kOutputFlag, kSelfBinary, kSelfSource);

    Target build{std::string(kDefaultTarget)};
    build.output(fs::Path(kSelfBinary)).input(fs::Path(kSelfSource)).command(std::move(compile));
    builder.addTarget(std::move(build));

    const std::array<std::string_view, 1> selfTestArguments{tests::kSelfTestFlag};
    Target check{std::string(kCheckTarget)};
    check.command(spawnSelf(executable, selfTestArguments));
    builder.addTarget(std::move(check));

    // One child process per example, so a crashing example cannot take the
    // build script down with it.
    Target runExamples{std::string(kExamplesTarget)};
    for (const examples::Example& example : examples::kExamples)
    {
        const std::array<std::string_view, 2> arguments{examples::kExampleFlag, example.m_Name};
        runExamples.command(spawnSelf(executable, arguments));
    }
    builder.addTarget(std::move(runExamples));

    // A shortcut for the WebAssembly example.
    const std::array<std::string_view, 2> wasmArguments{examples::kExampleFlag,
                                                        examples::kWasmExampleName};
    Target wasm{std::string(kWasmTarget)};
    wasm.command(spawnSelf(executable, wasmArguments));
    builder.addTarget(std::move(wasm));

    Target clean{std::string(kCleanTarget)};
    clean.command(Command({"rm", "-rf", std::string(kBuildDirectory)}));
    builder.addTarget(std::move(clean));

    return builder.runCommandLine(argc, argv, kDefaultTarget);
}
