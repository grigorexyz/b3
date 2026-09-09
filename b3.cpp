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
/// Conventions: types use PascalCase and so do their member functions. Free
/// functions use snake_case, member variables use the m_PascalCase prefix, and
/// instances — including lambdas — use camelCase. Every fixed string in this
/// file is a constexpr std::string_view so that nothing is allocated or copied
/// for it at run time.
///
/// There is no std::string in b3. Strings are std::string_view throughout, and
/// the few that have to be built while the program runs are copied once into
/// b3::text::Arena, which owns the characters and NUL terminates them so the
/// same views can be handed to C interfaces such as execvp as const char*.

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
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

[[nodiscard]] constexpr std::string_view level_prefix(LogLevel level) noexcept
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
inline void set_log_level(LogLevel level)
{
    const std::scoped_lock lock(detail::g_LogMutex);
    detail::g_MinimumLevel = level;
}

[[nodiscard]] inline LogLevel log_level()
{
    const std::scoped_lock lock(detail::g_LogMutex);
    return detail::g_MinimumLevel;
}

inline void log_message(LogLevel level, std::string_view message)
{
    const std::scoped_lock lock(detail::g_LogMutex);
    if (level < detail::g_MinimumLevel)
    {
        return;
    }

    // Flushing keeps the ordering of our own output and the output of the
    // child processes we spawn intact.
    std::ostream& stream = level >= LogLevel::Warning ? std::cerr : std::cout;
    stream << level_prefix(level) << message << std::endl;
}

/// The largest message b3 ever prints. Formatting into a buffer of this size
/// keeps std::format from having to return an owning string; longer messages
/// are truncated, which only ever affects logging.
inline constexpr std::size_t kLogBufferSize = 8192;

template <typename... Args>
void log_formatted(LogLevel level, std::format_string<Args...> format, Args&&... args)
{
    if (level < log_level())
    {
        return;
    }

    std::array<char, kLogBufferSize> buffer{};
    const auto result =
        std::format_to_n(buffer.data(), buffer.size(), format, std::forward<Args>(args)...);
    const std::size_t size =
        std::min(static_cast<std::size_t>(result.size), buffer.size());
    log_message(level, std::string_view(buffer.data(), size));
}

template <typename... Args>
void log_trace(std::format_string<Args...> format, Args&&... args)
{
    log_formatted(LogLevel::Trace, format, std::forward<Args>(args)...);
}

template <typename... Args>
void log_info(std::format_string<Args...> format, Args&&... args)
{
    log_formatted(LogLevel::Info, format, std::forward<Args>(args)...);
}

template <typename... Args>
void log_warning(std::format_string<Args...> format, Args&&... args)
{
    log_formatted(LogLevel::Warning, format, std::forward<Args>(args)...);
}

template <typename... Args>
void log_error(std::format_string<Args...> format, Args&&... args)
{
    log_formatted(LogLevel::Error, format, std::forward<Args>(args)...);
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

namespace text
{

/// Owns every stretch of characters that b3 has to build while it runs, such
/// as a rendered command line or a concatenated flag. Views handed out by the
/// arena stay valid until the process exits and are always NUL terminated, so
/// data() is a legal const char* for execvp and friends.
class Arena
{
public:
    Arena() = default;
    ~Arena() = default;

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    /// Copies the concatenation of the pieces into the arena.
    [[nodiscard]] std::string_view Store(std::span<const std::string_view> pieces)
    {
        std::size_t size = 0;
        for (const std::string_view piece : pieces)
        {
            size += piece.size();
        }

        std::unique_ptr<char[]> block = std::make_unique<char[]>(size + 1);
        char* cursor = block.get();
        for (const std::string_view piece : pieces)
        {
            cursor = std::ranges::copy(piece, cursor).out;
        }
        *cursor = '\0';

        const std::string_view stored(block.get(), size);

        const std::scoped_lock lock(m_Mutex);
        m_Blocks.push_back(std::move(block));
        return stored;
    }

    [[nodiscard]] std::string_view Store(std::string_view piece)
    {
        const std::array<std::string_view, 1> pieces{piece};
        return Store(std::span<const std::string_view>(pieces));
    }

private:
    std::mutex m_Mutex;
    std::vector<std::unique_ptr<char[]>> m_Blocks;
};

/// The one arena of the process. b3 is a short lived build tool, so nothing is
/// ever released before it exits.
[[nodiscard]] inline Arena& arena()
{
    static Arena instance;
    return instance;
}

/// Copies a view into the arena, which makes its lifetime independent of
/// whatever produced the characters.
[[nodiscard]] inline std::string_view intern(std::string_view value)
{
    return arena().Store(value);
}

/// Concatenates any number of views into a single interned view.
template <typename... Parts>
[[nodiscard]] std::string_view join(Parts&&... parts)
{
    const std::array<std::string_view, sizeof...(Parts)> pieces{
        std::string_view(std::forward<Parts>(parts))...};
    return arena().Store(std::span<const std::string_view>(pieces));
}

/// The NUL terminated characters behind an interned view.
[[nodiscard]] inline const char* c_string(std::string_view interned)
{
    return interned.empty() && interned.data() == nullptr ? "" : interned.data();
}

} // namespace text

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

/// The characters of a path, without copying them. The view borrows from the
/// path, so it must not outlive it; pass it through b3::text::intern when it
/// has to.
[[nodiscard]] inline std::string_view text(const Path& path)
{
#if defined(_WIN32)
    // The native encoding is not char there, so this is the one place where a
    // conversion cannot be avoided. The result is interned right away.
    return b3::text::intern(path.string());
#else
    return path.native();
#endif
}

/// Returns the last write time of a path, or std::nullopt when it does not exist.
[[nodiscard]] inline std::optional<FileTime> modification_time(const Path& path)
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
inline bool make_directories(const Path& path)
{
    if (path.empty())
    {
        return true;
    }

    std::error_code errorCode;
    std::filesystem::create_directories(path, errorCode);
    if (errorCode)
    {
        log_error("cannot create directory '{}': {}", text(path), errorCode.message());
        return false;
    }
    return true;
}

/// Writes a text file, creating its parent directory when needed.
inline bool write_file(const Path& path, std::string_view content)
{
    if (path.has_parent_path() && !make_directories(path.parent_path()))
    {
        return false;
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        log_error("cannot write '{}'", text(path));
        return false;
    }

    stream << content;
    return stream.good();
}

/// Reads a whole file into the text arena, or std::nullopt when it cannot be
/// opened.
[[nodiscard]] inline std::optional<std::string_view> read_file(const Path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        return std::nullopt;
    }

    const std::streamoff size = stream.tellg();
    if (size < 0)
    {
        return std::nullopt;
    }
    stream.seekg(0, std::ios::beg);

    std::vector<char> content(static_cast<std::size_t>(size));
    if (size > 0 && !stream.read(content.data(), size))
    {
        return std::nullopt;
    }
    return b3::text::intern(std::string_view(content.data(), content.size()));
}

/// Looks a program up the way a shell would: names that already contain a
/// separator are only checked for existence, every other name is searched for
/// in the directories of the PATH environment variable.
[[nodiscard]] inline std::optional<Path> find_executable(std::string_view name)
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

    const char* pathVariable = std::getenv(b3::text::c_string(kPathEnvironmentVariable));
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
[[nodiscard]] inline bool is_out_of_date(std::span<const Path> outputs, std::span<const Path> inputs)
{
    if (outputs.empty())
    {
        return true;
    }

    std::optional<FileTime> oldestOutput;
    for (const Path& output : outputs)
    {
        const std::optional<FileTime> time = modification_time(output);
        if (!time.has_value())
        {
            log_trace("output '{}' is missing", text(output));
            return true;
        }
        if (!oldestOutput.has_value() || *time < *oldestOutput)
        {
            oldestOutput = time;
        }
    }

    for (const Path& input : inputs)
    {
        const std::optional<FileTime> time = modification_time(input);
        if (!time.has_value())
        {
            log_trace("input '{}' is missing", text(input));
            return true;
        }
        if (*time > *oldestOutput)
        {
            log_trace("input '{}' is newer than the outputs", text(input));
            return true;
        }
    }

    return false;
}

/// Lists the files of a directory, non recursive, filtered by extension.
/// Extensions include the leading dot; an empty span accepts every file.
[[nodiscard]] inline std::vector<Path> list_files(const Path& directory,
                                                 std::span<const std::string_view> extensions)
{
    std::vector<Path> files;

    std::error_code errorCode;
    const std::filesystem::directory_iterator end;
    std::filesystem::directory_iterator iterator(directory, errorCode);
    if (errorCode)
    {
        log_warning("cannot list '{}': {}", text(directory), errorCode.message());
        return files;
    }

    for (; iterator != end; iterator.increment(errorCode))
    {
        if (errorCode)
        {
            log_warning("cannot list '{}': {}", text(directory), errorCode.message());
            break;
        }
        if (!iterator->is_regular_file())
        {
            continue;
        }

        const Path& path = iterator->path();
        const Path extension = path.extension();
        const bool matches = extensions.empty()
            || std::ranges::any_of(extensions,
                                   [&extension](std::string_view candidate)
                                   { return text(extension) == candidate; });
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
/// no shell quoting is ever required. Every argument is interned, which makes
/// the command independent of the lifetime of whatever produced it and lets
/// run() hand the very same characters to execvp. The process handling state
/// lives behind a pImpl so that the public interface stays free of platform
/// headers.
class Command
{
public:
    Command();
    Command(std::initializer_list<std::string_view> arguments);
    ~Command();

    Command(const Command& other);
    Command& operator=(const Command& other);
    Command(Command&& other) noexcept;
    Command& operator=(Command&& other) noexcept;

    /// Appends a single argument, copying its characters into the arena.
    Command& Append(std::string_view argument);

    /// Appends any number of arguments convertible to std::string_view, which
    /// makes constexpr std::string_view constants usable directly.
    template <typename... Args>
    Command& AppendAll(Args&&... arguments)
    {
        (Append(std::string_view(std::forward<Args>(arguments))), ...);
        return *this;
    }

    [[nodiscard]] std::span<const std::string_view> Arguments() const;

    [[nodiscard]] bool Empty() const;

    /// Human readable rendering of the command, used for logging only.
    [[nodiscard]] std::string_view Render() const;

    /// Runs the command synchronously and returns its exit code. A negative
    /// value means the process could not be started or was terminated.
    [[nodiscard]] int Run() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

struct Command::Impl
{
    std::vector<std::string_view> m_Arguments;
};

namespace detail
{

/// Quotes an argument for logging only, never for execution. An argument that
/// needs no quoting is returned as is, so nothing is copied for it.
inline void render_argument(std::string_view argument, std::vector<char>& out)
{
    const bool needsQuotes =
        argument.empty() || argument.find_first_of(kQuotingCharacters) != std::string_view::npos;
    if (!needsQuotes)
    {
        out.insert(out.end(), argument.begin(), argument.end());
        return;
    }

    out.push_back('"');
    for (const char character : argument)
    {
        if (character == '"' || character == '\\')
        {
            out.push_back('\\');
        }
        out.push_back(character);
    }
    out.push_back('"');
}

} // namespace detail

inline Command::Command()
    : m_Impl(std::make_unique<Impl>())
{
}

inline Command::Command(std::initializer_list<std::string_view> arguments)
    : m_Impl(std::make_unique<Impl>())
{
    m_Impl->m_Arguments.reserve(arguments.size());
    for (const std::string_view argument : arguments)
    {
        Append(argument);
    }
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

inline Command& Command::Append(std::string_view argument)
{
    m_Impl->m_Arguments.push_back(text::intern(argument));
    return *this;
}

inline std::span<const std::string_view> Command::Arguments() const
{
    return m_Impl->m_Arguments;
}

inline bool Command::Empty() const
{
    return m_Impl->m_Arguments.empty();
}

inline std::string_view Command::Render() const
{
    std::vector<char> rendered;
    for (const std::string_view argument : m_Impl->m_Arguments)
    {
        if (!rendered.empty())
        {
            rendered.push_back(' ');
        }
        detail::render_argument(argument, rendered);
    }
    return text::intern(std::string_view(rendered.data(), rendered.size()));
}

inline int Command::Run() const
{
    const std::vector<std::string_view>& arguments = m_Impl->m_Arguments;
    if (arguments.empty())
    {
        log_error("refusing to run an empty command");
        return -1;
    }

    // Every argument came out of the arena and is therefore NUL terminated,
    // so the characters can be pointed at directly. execvp only wants char*
    // for historical reasons and never writes through them.
    std::vector<char*> rawArguments;
    rawArguments.reserve(arguments.size() + 1);
    for (const std::string_view argument : arguments)
    {
        rawArguments.push_back(const_cast<char*>(text::c_string(argument)));
    }
    rawArguments.push_back(nullptr);

#if defined(_WIN32)
    const intptr_t status = _spawnvp(_P_WAIT, rawArguments[0], rawArguments.data());
    if (status < 0)
    {
        log_error("cannot run '{}'", arguments.front());
        return -1;
    }
    return static_cast<int>(status);
#else
    const pid_t childPid = ::fork();
    if (childPid < 0)
    {
        log_error("cannot fork for '{}'", arguments.front());
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
            log_error("cannot wait for '{}'", arguments.front());
            return -1;
        }
    }

    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status))
    {
        log_error("'{}' was terminated by signal {}", arguments.front(), WTERMSIG(status));
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
    explicit Target(std::string_view name)
        : m_Name(text::intern(name))
    {
    }

    [[nodiscard]] std::string_view Name() const { return m_Name; }

    /// A target without outputs is phony and therefore always runs, like a
    /// .PHONY target in make.
    [[nodiscard]] bool IsPhony() const { return m_Outputs.empty(); }

    Target& Output(fs::Path path)
    {
        m_Outputs.push_back(std::move(path));
        return *this;
    }

    Target& Input(fs::Path path)
    {
        m_Inputs.push_back(std::move(path));
        return *this;
    }

    Target& DependsOn(std::string_view targetName)
    {
        m_Dependencies.push_back(text::intern(targetName));
        return *this;
    }

    /// Named AddCommand rather than Command, because a member function named
    /// after its own parameter type would hide that type in class scope.
    Target& AddCommand(Command command)
    {
        m_Commands.push_back(std::move(command));
        return *this;
    }

    [[nodiscard]] const std::vector<fs::Path>& Outputs() const { return m_Outputs; }
    [[nodiscard]] const std::vector<fs::Path>& Inputs() const { return m_Inputs; }
    [[nodiscard]] std::span<const std::string_view> Dependencies() const { return m_Dependencies; }
    [[nodiscard]] std::span<const Command> Commands() const { return m_Commands; }

private:
    std::string_view m_Name;
    std::vector<fs::Path> m_Outputs;
    std::vector<fs::Path> m_Inputs;
    std::vector<std::string_view> m_Dependencies;
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
[[nodiscard]] inline std::string_view wasm_compiler_executable()
{
    if (const char* fromEnvironment =
            std::getenv(text::c_string(kWasmCompilerEnvironmentVariable));
        fromEnvironment != nullptr && *fromEnvironment != '\0')
    {
        return fromEnvironment;
    }
    return kWasmCompiler;
}

[[nodiscard]] inline bool is_wasm_compiler_available()
{
    return fs::find_executable(wasm_compiler_executable()).has_value();
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
    Target& AddTarget(Target target);

    [[nodiscard]] bool HasTarget(std::string_view name) const;
    [[nodiscard]] const Target* FindTarget(std::string_view name) const;
    [[nodiscard]] std::vector<std::string_view> TargetNames() const;

    /// When set, commands are only printed and never executed (make -n).
    void SetDryRun(bool dryRun);
    [[nodiscard]] bool DryRun() const;

    /// When set, every target is rebuilt regardless of file timestamps (make -B).
    void SetAlwaysMake(bool alwaysMake);
    [[nodiscard]] bool AlwaysMake() const;

    /// Builds the named target and everything it depends on, depth first.
    /// Cycles and unknown dependencies are reported as BuildStatus::Failed.
    [[nodiscard]] BuildStatus Build(std::string_view name);

    /// Convenience entry point for build scripts: parses the usual make style
    /// flags plus target names and builds them in order, returning an exit code.
    [[nodiscard]] int RunCommandLine(int argc, char* argv[], std::string_view defaultTarget);

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
    // The keys are interned target names, so the views outlive the maps.
    std::map<std::string_view, Target, std::less<>> m_Targets;
    std::map<std::string_view, detail::VisitState, std::less<>> m_States;
    std::map<std::string_view, BuildStatus, std::less<>> m_Results;
    bool m_DryRun = false;
    bool m_AlwaysMake = false;

    [[nodiscard]] BuildStatus BuildTarget(std::string_view name);
    [[nodiscard]] bool RunCommands(const Target& target);
};

inline bool Builder::Impl::RunCommands(const Target& target)
{
    if (!m_DryRun)
    {
        for (const fs::Path& output : target.Outputs())
        {
            if (output.has_parent_path() && !fs::make_directories(output.parent_path()))
            {
                return false;
            }
        }
    }

    for (const Command& command : target.Commands())
    {
        log_info("{}", command.Render());
        if (m_DryRun)
        {
            continue;
        }

        const int exitCode = command.Run();
        if (exitCode != 0)
        {
            log_error("target '{}' failed with exit code {}", target.Name(), exitCode);
            return false;
        }
    }

    return true;
}

inline BuildStatus Builder::Impl::BuildTarget(std::string_view name)
{
    if (const auto result = m_Results.find(name); result != m_Results.end())
    {
        return result->second;
    }

    const auto entry = m_Targets.find(name);
    if (entry == m_Targets.end())
    {
        log_error("unknown target '{}'", name);
        return BuildStatus::Failed;
    }

    const std::string_view targetName = entry->second.Name();
    if (m_States[targetName] == detail::VisitState::InProgress)
    {
        log_error("dependency cycle detected at target '{}'", targetName);
        m_Results[targetName] = BuildStatus::Failed;
        return BuildStatus::Failed;
    }
    m_States[targetName] = detail::VisitState::InProgress;

    const Target& target = entry->second;

    bool dependencyRebuilt = false;
    std::vector<fs::Path> inputs = target.Inputs();

    for (const std::string_view dependency : target.Dependencies())
    {
        const BuildStatus status = BuildTarget(dependency);
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
            const std::vector<fs::Path>& dependencyOutputs = dependencyEntry->second.Outputs();
            inputs.insert(inputs.end(), dependencyOutputs.begin(), dependencyOutputs.end());
        }
    }

    m_States[targetName] = detail::VisitState::Done;

    const bool outOfDate = m_AlwaysMake || dependencyRebuilt || target.IsPhony()
        || fs::is_out_of_date(target.Outputs(), inputs);
    if (!outOfDate)
    {
        log_trace("target '{}' is up to date", targetName);
        m_Results[targetName] = BuildStatus::UpToDate;
        return BuildStatus::UpToDate;
    }

    const BuildStatus status = RunCommands(target) ? BuildStatus::Rebuilt : BuildStatus::Failed;
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

inline Target& Builder::AddTarget(Target target)
{
    const std::string_view name = target.Name();
    m_Impl->m_Results.clear();
    const auto [entry, inserted] = m_Impl->m_Targets.insert_or_assign(name, std::move(target));
    if (!inserted)
    {
        log_warning("target '{}' was redefined", name);
    }
    return entry->second;
}

inline bool Builder::HasTarget(std::string_view name) const
{
    return m_Impl->m_Targets.contains(name);
}

inline const Target* Builder::FindTarget(std::string_view name) const
{
    const auto entry = m_Impl->m_Targets.find(name);
    return entry == m_Impl->m_Targets.end() ? nullptr : &entry->second;
}

inline std::vector<std::string_view> Builder::TargetNames() const
{
    std::vector<std::string_view> names;
    names.reserve(m_Impl->m_Targets.size());
    for (const auto& [name, target] : m_Impl->m_Targets)
    {
        names.push_back(name);
    }
    return names;
}

inline void Builder::SetDryRun(bool dryRun)
{
    m_Impl->m_DryRun = dryRun;
}

inline bool Builder::DryRun() const
{
    return m_Impl->m_DryRun;
}

inline void Builder::SetAlwaysMake(bool alwaysMake)
{
    m_Impl->m_AlwaysMake = alwaysMake;
}

inline bool Builder::AlwaysMake() const
{
    return m_Impl->m_AlwaysMake;
}

inline BuildStatus Builder::Build(std::string_view name)
{
    m_Impl->m_States.clear();
    m_Impl->m_Results.clear();
    return m_Impl->BuildTarget(name);
}

inline int Builder::RunCommandLine(int argc, char* argv[], std::string_view defaultTarget)
{
    std::vector<std::string_view> requestedTargets;

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument == kDryRunShortFlag || argument == kDryRunLongFlag)
        {
            SetDryRun(true);
        }
        else if (argument == kAlwaysMakeShortFlag || argument == kAlwaysMakeLongFlag)
        {
            SetAlwaysMake(true);
        }
        else if (argument == kVerboseShortFlag || argument == kVerboseLongFlag)
        {
            set_log_level(LogLevel::Trace);
        }
        else if (argument == kQuietShortFlag || argument == kQuietLongFlag)
        {
            set_log_level(LogLevel::Warning);
        }
        else if (argument == kHelpShortFlag || argument == kHelpLongFlag)
        {
            log_message(LogLevel::Error, kUsage);
            log_message(LogLevel::Error, "targets:");
            for (const std::string_view name : TargetNames())
            {
                log_error("  {}", name);
            }
            return 0;
        }
        else if (argument.starts_with('-'))
        {
            log_error("unknown option '{}'", argument);
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

    for (const std::string_view name : requestedTargets)
    {
        const BuildStatus status = Build(name);
        if (status == BuildStatus::Failed)
        {
            return 1;
        }
        if (status == BuildStatus::UpToDate)
        {
            log_info("'{}' is up to date", name);
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

[[nodiscard]] inline std::string_view compiler_executable()
{
    if (const char* fromEnvironment = std::getenv(text::c_string(kCompilerEnvironmentVariable));
        fromEnvironment != nullptr && *fromEnvironment != '\0')
    {
        return fromEnvironment;
    }
    return kDefaultCompiler;
}

/// When one of the sources of the build script is newer than the running
/// executable, the script is recompiled and re-executed with the original
/// arguments. On success this function does not return.
inline void rebuild_yourself(int argc,
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
    if (!fs::is_out_of_date(outputs, sources))
    {
        return;
    }

    log_info("build script is out of date, rebuilding '{}'", fs::text(executable));

    const fs::Path backup = fs::Path(executable).concat(kBackupSuffix);
    std::error_code errorCode;
    std::filesystem::rename(executable, backup, errorCode);
    if (errorCode)
    {
        log_warning("cannot move '{}' aside: {}", fs::text(executable), errorCode.message());
    }

    Command compile;
    compile.AppendAll(compiler_executable(), kStandardFlag, kOptimizeFlag, kOutputFlag,
                      fs::text(executable));
    for (const fs::Path& source : sources)
    {
        compile.Append(fs::text(source));
    }
    for (const std::string_view flag : extraFlags)
    {
        compile.Append(flag);
    }

    log_info("{}", compile.Render());
    if (compile.Run() != 0)
    {
        log_error("rebuilding the build script failed, restoring the previous binary");
        std::filesystem::rename(backup, executable, errorCode);
        std::exit(1);
    }

    std::filesystem::remove(backup, errorCode);

    Command restart;
    for (int index = 0; index < argc; ++index)
    {
        restart.Append(argv[index] == nullptr ? std::string_view() : std::string_view(argv[index]));
    }

    log_info("restarting '{}'", fs::text(executable));
    const int exitCode = restart.Run();
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

    WasmModule& Source(fs::Path path)
    {
        m_Sources.push_back(std::move(path));
        return *this;
    }

    /// Requests that a symbol stays visible to the host.
    WasmModule& ExportSymbol(std::string_view symbol)
    {
        m_ExportedSymbols.push_back(text::intern(symbol));
        return *this;
    }

    /// Adds a flag that is passed to the compiler as is.
    WasmModule& Flag(std::string_view flag)
    {
        m_ExtraFlags.push_back(text::intern(flag));
        return *this;
    }

    [[nodiscard]] const fs::Path& Output() const { return m_Output; }
    [[nodiscard]] const std::vector<fs::Path>& Sources() const { return m_Sources; }
    [[nodiscard]] std::span<const std::string_view> ExportedSymbols() const
    {
        return m_ExportedSymbols;
    }
    [[nodiscard]] std::span<const std::string_view> ExtraFlags() const { return m_ExtraFlags; }

    [[nodiscard]] Command CompileCommand() const
    {
        Command command;
        command.AppendAll(wasm_compiler_executable(), kStandardFlag, kOptimizeFlag, kWasmTargetFlag);

        for (const std::string_view flag : kWasmFlags)
        {
            command.Append(flag);
        }
        for (const std::string_view symbol : m_ExportedSymbols)
        {
            command.Append(text::join(kWasmExportPrefix, symbol));
        }
        for (const std::string_view flag : m_ExtraFlags)
        {
            command.Append(flag);
        }

        command.AppendAll(kOutputFlag, fs::text(m_Output));
        for (const fs::Path& source : m_Sources)
        {
            command.Append(fs::text(source));
        }

        return command;
    }

    /// A ready made target, so a module drops straight into a build graph.
    /// Named AsTarget for the same reason Target::AddCommand is not Command.
    [[nodiscard]] Target AsTarget(std::string_view name) const
    {
        Target target(name);
        target.Output(m_Output).AddCommand(CompileCommand());
        for (const fs::Path& source : m_Sources)
        {
            target.Input(source);
        }
        return target;
    }

private:
    fs::Path m_Output;
    std::vector<fs::Path> m_Sources;
    std::vector<std::string_view> m_ExportedSymbols;
    std::vector<std::string_view> m_ExtraFlags;
};

/// True when the file really is a WebAssembly module, checked by its magic.
[[nodiscard]] inline bool is_wasm_module(const fs::Path& path)
{
    const std::optional<std::string_view> content = fs::read_file(path);
    return content.has_value() && content->starts_with(kWasmMagic);
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

[[nodiscard]] inline fs::Path scratch_directory(std::string_view name)
{
    const fs::Path directory =
        std::filesystem::temp_directory_path() / kScratchDirectoryName / name;
    std::error_code errorCode;
    std::filesystem::remove_all(directory, errorCode);
    fs::make_directories(directory);
    return directory;
}

/// Compiles one translation unit into an object file.
[[nodiscard]] inline Target compile_target(std::string_view name,
                                          const fs::Path& source,
                                          const fs::Path& object,
                                          std::span<const std::string_view> flags = {})
{
    Command compile;
    compile.AppendAll(compiler_executable(), kStandardFlag);
    for (const std::string_view flag : kWarningFlags)
    {
        compile.Append(flag);
    }
    for (const std::string_view flag : flags)
    {
        compile.Append(flag);
    }
    compile.AppendAll(kCompileOnlyFlag, fs::text(source), kOutputFlag, fs::text(object));

    Target target{name};
    target.Output(object).Input(source).AddCommand(std::move(compile));
    return target;
}

/// Compiles and runs a single file program, then proves that a second build of
/// the very same graph does nothing because everything is up to date.
[[nodiscard]] inline bool run_hello_example(const fs::Path& directory)
{
    const fs::Path source = directory / "Hello.cpp";
    const fs::Path object = directory / "obj" / "Hello.o";
    const fs::Path program = directory / "hello";

    if (!fs::write_file(source, kHelloSource))
    {
        return false;
    }

    Builder builder;
    builder.AddTarget(compile_target("compile", source, object));

    Command link;
    link.AppendAll(compiler_executable(), kStandardFlag, kOutputFlag, fs::text(program),
                   fs::text(object));
    Target linkTarget("link");
    linkTarget.Output(program).DependsOn("compile").AddCommand(std::move(link));
    builder.AddTarget(std::move(linkTarget));

    Target run("run");
    run.DependsOn("link").AddCommand(Command({fs::text(program)}));
    builder.AddTarget(std::move(run));

    if (builder.Build("link") != BuildStatus::Rebuilt)
    {
        log_error("the first build should have rebuilt everything");
        return false;
    }
    if (builder.Build("link") != BuildStatus::UpToDate)
    {
        log_error("the second build should have been a no operation");
        return false;
    }

    return builder.Build("run") == BuildStatus::Rebuilt;
}

/// Archives two objects into a static library and links a program against it.
[[nodiscard]] inline bool run_library_example(const fs::Path& directory)
{
    const fs::Path header = directory / "Greeter.hpp";
    const fs::Path greeterSource = directory / "Greeter.cpp";
    const fs::Path mainSource = directory / "Main.cpp";
    const fs::Path greeterObject = directory / "obj" / "Greeter.o";
    const fs::Path mainObject = directory / "obj" / "Main.o";
    const fs::Path library = directory / "lib" / "libgreeter.a";
    const fs::Path program = directory / "greeter";

    if (!fs::write_file(header, kGreeterHeader) || !fs::write_file(greeterSource, kGreeterSource)
        || !fs::write_file(mainSource, kGreeterMainSource))
    {
        return false;
    }

    const std::array<std::string_view, 1> includeFlags{text::join("-I", fs::text(directory))};

    Builder builder;
    builder.AddTarget(compile_target("compile:greeter", greeterSource, greeterObject, includeFlags))
        .Input(header);
    builder.AddTarget(compile_target("compile:main", mainSource, mainObject, includeFlags))
        .Input(header);

    Command archive;
    archive.AppendAll("ar", "rcs", fs::text(library), fs::text(greeterObject));
    Target archiveTarget("lib");
    archiveTarget.Output(library).DependsOn("compile:greeter").AddCommand(std::move(archive));
    builder.AddTarget(std::move(archiveTarget));

    Command link;
    link.AppendAll(compiler_executable(), kStandardFlag, kOutputFlag, fs::text(program),
                   fs::text(mainObject), fs::text(library));
    Target linkTarget("link");
    linkTarget.Output(program).DependsOn("compile:main").DependsOn("lib").AddCommand(std::move(link));
    builder.AddTarget(std::move(linkTarget));

    Target run("run");
    run.DependsOn("link").AddCommand(Command({fs::text(program)}));
    builder.AddTarget(std::move(run));

    return builder.Build("run") == BuildStatus::Rebuilt;
}

/// Shows phony targets, dry runs and the way a failing command aborts a build.
[[nodiscard]] inline bool run_pipeline_example(const fs::Path& directory)
{
    const fs::Path stamp = directory / "stamp.txt";

    Builder builder;

    Target generate("generate");
    generate.Output(stamp).AddCommand(Command({"touch", fs::text(stamp)}));
    builder.AddTarget(std::move(generate));

    Target report("report");
    report.DependsOn("generate").AddCommand(Command({"echo", "the stamp is ready"}));
    builder.AddTarget(std::move(report));

    Target broken("broken");
    broken.AddCommand(Command({"false"}));
    builder.AddTarget(std::move(broken));

    builder.SetDryRun(true);
    if (builder.Build("report") != BuildStatus::Rebuilt || fs::exists(stamp))
    {
        log_error("a dry run must not touch the file system");
        return false;
    }

    builder.SetDryRun(false);
    if (builder.Build("report") != BuildStatus::Rebuilt || !fs::exists(stamp))
    {
        log_error("the pipeline should have produced the stamp");
        return false;
    }

    // A phony target always runs again, an out of date check is never applied.
    if (builder.Build("report") != BuildStatus::Rebuilt)
    {
        log_error("a phony target must always run");
        return false;
    }

    log_info("the next target fails on purpose");
    return builder.Build("broken") == BuildStatus::Failed;
}

/// Builds a freestanding WebAssembly module with clang++ and checks that the
/// result really is one.
[[nodiscard]] inline bool run_wasm_example(const fs::Path& directory)
{
    if (!is_wasm_compiler_available())
    {
        log_warning("'{}' is not on the PATH, skipping the WebAssembly example",
                   wasm_compiler_executable());
        return true;
    }

    log_info("using '{}'", wasm_compiler_executable());

    const fs::Path source = directory / "Math.cpp";
    const fs::Path module = fs::Path(directory / "math").concat(kWasmModuleExtension);

    if (!fs::write_file(source, kWasmSource))
    {
        return false;
    }

    WasmModule wasmModule(module);
    wasmModule.Source(source);
    for (const std::string_view symbol : kWasmExportedSymbols)
    {
        wasmModule.ExportSymbol(symbol);
    }

    Builder builder;
    builder.AddTarget(wasmModule.AsTarget("wasm"));

    if (builder.Build("wasm") != BuildStatus::Rebuilt)
    {
        log_error("the module should have been built");
        return false;
    }
    if (!is_wasm_module(module))
    {
        log_error("'{}' is not a WebAssembly module", fs::text(module));
        return false;
    }
    if (builder.Build("wasm") != BuildStatus::UpToDate)
    {
        log_error("the second build should have been a no operation");
        return false;
    }

    const fs::Path moduleName = module.filename();
    log_info("built '{}' exporting {} symbol(s)", fs::text(moduleName),
            wasmModule.ExportedSymbols().size());
    return true;
}

inline constexpr std::array<Example, 4> kExamples{{
    {"hello", "compile, link and run a single file program", &run_hello_example},
    {"library", "archive a static library and link against it", &run_library_example},
    {"pipeline", "phony targets, dry runs and failing commands", &run_pipeline_example},
    {"wasm", "build a freestanding module with clang++", &run_wasm_example},
}};

[[nodiscard]] inline const Example* find_example(std::string_view name)
{
    const auto entry = std::ranges::find(kExamples, name, &Example::m_Name);
    return entry == kExamples.end() ? nullptr : &*entry;
}

/// Runs one example, or every example when the name is "all".
[[nodiscard]] inline int run_example(std::string_view name)
{
    if (name == kAllExamples)
    {
        for (const Example& example : kExamples)
        {
            if (const int exitCode = run_example(example.m_Name); exitCode != 0)
            {
                return exitCode;
            }
        }
        return 0;
    }

    const Example* example = find_example(name);
    if (example == nullptr)
    {
        log_error("unknown example '{}'", name);
        log_message(LogLevel::Error, "examples:");
        for (const Example& known : kExamples)
        {
            log_error("  {:<10} {}", known.m_Name, known.m_Description);
        }
        return 2;
    }

    log_info("running example '{}': {}", example->m_Name, example->m_Description);

    const fs::Path directory = scratch_directory(example->m_Name);
    const bool succeeded = example->m_Run(directory);

    std::error_code errorCode;
    std::filesystem::remove_all(directory, errorCode);

    if (!succeeded)
    {
        log_error("example '{}' failed", example->m_Name);
        return 1;
    }

    log_info("example '{}' succeeded", example->m_Name);
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
        std::cout << kOkPrefix << what << std::endl;
        return;
    }
    std::cout << kFailPrefix << what << std::endl;
    ++g_Failures;
}

inline void test_text_arena()
{
    const std::string_view interned = text::intern("interned");
    check(interned == "interned", "the arena keeps the characters");
    check(text::c_string(interned)[interned.size()] == '\0', "an interned view is NUL terminated");
    check(text::join(kWasmExportPrefix, "add") == "-Wl,--export=add", "join concatenates");
    check(text::join().empty(), "joining nothing gives an empty view");

    // The characters of the source are gone by the time the view is read.
    std::string_view copied;
    {
        const std::vector<char> temporary{'g', 'o', 'n', 'e'};
        copied = text::intern(std::string_view(temporary.data(), temporary.size()));
    }
    check(copied == "gone", "an interned view outlives its source");
}

inline void test_command_rendering()
{
    Command command;
    command.AppendAll(kDefaultCompiler, kOutputFlag, "build/with space");
    check(command.Arguments().size() == 3, "command keeps every argument");
    check(command.Render() == R"(c++ -o "build/with space")", "command quotes for logging");
    check(Command().Empty(), "default constructed command is empty");

    // Arguments are copied, so a command never borrows from its caller.
    Command borrowed;
    {
        const fs::Path source = fs::Path("build") / "Main.cpp";
        borrowed.Append(fs::text(source));
    }
    check(borrowed.Render() == "build/Main.cpp", "an argument outlives what produced it");
}

inline void test_command_execution()
{
    const Command trueCommand({"true"});
    check(trueCommand.Run() == 0, "successful command returns zero");

    const Command falseCommand({"false"});
    check(falseCommand.Run() != 0, "failing command returns non zero");
}

inline void test_out_of_date_detection(const fs::Path& scratch)
{
    const fs::Path input = scratch / "input.txt";
    const fs::Path output = scratch / "output.txt";
    check(fs::write_file(input, kPayload), "write_file creates a file");

    const std::array<fs::Path, 1> outputs{output};
    const std::array<fs::Path, 1> inputs{input};
    check(fs::is_out_of_date(outputs, inputs), "missing output is out of date");

    check(fs::write_file(output, kPayload), "write_file overwrites a file");
    std::filesystem::last_write_time(output,
                                     *fs::modification_time(input) + std::chrono::seconds(1));
    check(!fs::is_out_of_date(outputs, inputs), "newer output is up to date");

    std::filesystem::last_write_time(input,
                                     *fs::modification_time(output) + std::chrono::seconds(1));
    check(fs::is_out_of_date(outputs, inputs), "newer input is out of date");
}

inline void test_list_files(const fs::Path& scratch)
{
    const fs::Path directory = scratch / "listing";
    check(fs::write_file(directory / "b.cpp", kPayload), "listing fixture b.cpp");
    check(fs::write_file(directory / "a.cpp", kPayload), "listing fixture a.cpp");
    check(fs::write_file(directory / "notes.txt", kPayload), "listing fixture notes.txt");

    const std::array<std::string_view, 1> extensions{fs::kSourceExtension};
    const std::vector<fs::Path> sources = fs::list_files(directory, extensions);
    check(sources.size() == 2, "list_files filters by extension");
    check(!sources.empty() && sources.front().filename() == "a.cpp", "list_files sorts its result");
    check(fs::list_files(directory, {}).size() == 3, "an empty filter accepts every file");
}

inline void test_builder_rebuilds_and_caches(const fs::Path& scratch)
{
    const fs::Path input = scratch / "copy-input.txt";
    const fs::Path output = scratch / "nested" / "copy-output.txt";
    check(fs::write_file(input, kPayload), "copy fixture");

    Builder builder;
    Target copy("copy");
    Command copyCommand;
    copyCommand.AppendAll("cp", fs::text(input), fs::text(output));
    copy.Output(output).Input(input).AddCommand(std::move(copyCommand));
    builder.AddTarget(std::move(copy));

    check(builder.Build("copy") == BuildStatus::Rebuilt, "first build runs the command");
    check(fs::exists(output), "build creates the output and its directory");
    check(builder.Build("copy") == BuildStatus::UpToDate, "second build is a no operation");

    builder.SetAlwaysMake(true);
    check(builder.Build("copy") == BuildStatus::Rebuilt, "always make forces a rebuild");
}

inline void test_builder_dependencies_and_failures()
{
    Builder builder;

    Target first("first");
    first.AddCommand(Command({"true"}));
    builder.AddTarget(std::move(first));

    Target second("second");
    second.DependsOn("first").AddCommand(Command({"true"}));
    builder.AddTarget(std::move(second));

    check(builder.Build("second") == BuildStatus::Rebuilt, "phony targets always run");
    check(builder.Build("missing") == BuildStatus::Failed, "unknown target fails");

    Target broken("broken");
    broken.AddCommand(Command({"false"}));
    builder.AddTarget(std::move(broken));
    check(builder.Build("broken") == BuildStatus::Failed, "failing command fails the target");

    Target left("left");
    left.DependsOn("right");
    builder.AddTarget(std::move(left));
    Target right("right");
    right.DependsOn("left");
    builder.AddTarget(std::move(right));
    check(builder.Build("left") == BuildStatus::Failed, "dependency cycle is detected");
}

inline void test_dry_run(const fs::Path& scratch)
{
    const fs::Path output = scratch / "dry-run.txt";

    Builder builder;
    Target touch("touch");
    Command touchCommand;
    touchCommand.AppendAll("touch", fs::text(output));
    touch.Output(output).AddCommand(std::move(touchCommand));
    builder.AddTarget(std::move(touch));

    builder.SetDryRun(true);
    check(builder.Build("touch") == BuildStatus::Rebuilt, "dry run reports a rebuild");
    check(!fs::exists(output), "dry run does not touch the file system");
}

inline void test_target_registry()
{
    Builder builder;
    builder.AddTarget(Target("alpha"));
    builder.AddTarget(Target("beta"));

    check(builder.HasTarget("alpha"), "registered target is found");
    check(builder.FindTarget("gamma") == nullptr, "unknown target is not found");
    check(builder.TargetNames().size() == 2, "target names are listed");
    check(Target("alpha").IsPhony(), "a target without outputs is phony");
}

inline void test_example_registry()
{
    check(examples::find_example("hello") != nullptr, "known example is found");
    check(examples::find_example("wasm") != nullptr, "the wasm example is registered");
    check(examples::find_example("nope") == nullptr, "unknown example is not found");
    check(examples::run_example("nope") == 2, "unknown example reports a usage error");
}

inline void test_find_executable(const fs::Path& scratch)
{
    check(fs::find_executable("sh").has_value(), "a program on the PATH is found");
    check(!fs::find_executable("b3-definitely-not-a-program").has_value(),
          "a missing program is not found");
    check(!fs::find_executable("").has_value(), "an empty name is not a program");

    const fs::Path plainFile = scratch / "not-executable.txt";
    check(fs::write_file(plainFile, kPayload), "executable lookup fixture");
    check(!fs::find_executable(fs::text(plainFile)).has_value(),
          "a path that is not executable is rejected");
}

inline void test_wasm_commands()
{
    WasmModule module(fs::Path("build/math.wasm"));
    module.Source(fs::Path("Math.cpp")).ExportSymbol("add").ExportSymbol("factorial");

    const std::string_view command = module.CompileCommand().Render();
    check(command.starts_with(wasm_compiler_executable()), "the module is built with clang++");
    check(command.contains(kWasmTargetFlag), "clang++ is pointed at wasm32");
    check(command.contains("-nostdlib") && command.contains("-Wl,--no-entry"),
          "the module is freestanding");
    check(command.contains("-Wl,--export=add") && command.contains("-Wl,--export=factorial"),
          "every requested symbol is exported");
    check(command.contains("-o build/math.wasm") && command.ends_with("Math.cpp"),
          "the module and its sources are passed on");

    const std::string_view withFlag =
        WasmModule(fs::Path("m.wasm")).Flag("-DNDEBUG").CompileCommand().Render();
    check(withFlag.contains("-DNDEBUG"), "extra flags are passed through");

    const Target target = module.AsTarget("wasm");
    check(target.Outputs().size() == 1 && target.Inputs().size() == 1,
          "a module target carries its output and its sources");
    check(!target.IsPhony(), "a module target is not phony");
}

inline void test_wasm_module_detection(const fs::Path& scratch)
{
    const fs::Path notAModule = scratch / "not-a-module.wasm";
    check(fs::write_file(notAModule, kPayload), "module detection fixture");
    check(!is_wasm_module(notAModule), "a text file is not a module");
    check(!is_wasm_module(scratch / "missing.wasm"), "a missing file is not a module");

    const fs::Path module = scratch / "module.wasm";
    check(fs::write_file(module, kWasmMagic), "module magic fixture");
    check(is_wasm_module(module), "the magic bytes identify a module");
}

/// Compile time checks: none of these produce any code.
static_assert(level_prefix(LogLevel::Error) == kErrorPrefix);
static_assert(kUsage.starts_with("usage:"));
static_assert(fs::kSourceExtension == ".cpp");
static_assert(examples::kExamples.size() == 4);
static_assert(examples::kExamples.front().m_Name == "hello");
static_assert(examples::kExamples.back().m_Name == "wasm");
static_assert(kWasmCompiler == "clang++");
static_assert(kWasmTargetFlag.starts_with("--target=wasm32"));
static_assert(kWasmExportPrefix == "-Wl,--export=");
static_assert(kWasmMagic.size() == 4 && kWasmMagic[1] == 'a');
[[nodiscard]] inline int run_self_test()
{
    const fs::Path scratch = std::filesystem::temp_directory_path() / kScratchDirectoryName;
    std::error_code errorCode;
    std::filesystem::remove_all(scratch, errorCode);
    fs::make_directories(scratch);

    test_text_arena();
    test_command_rendering();
    test_command_execution();
    test_out_of_date_detection(scratch);
    test_list_files(scratch);
    test_builder_rebuilds_and_caches(scratch);
    test_builder_dependencies_and_failures();
    test_dry_run(scratch);
    test_target_registry();
    test_example_registry();
    test_find_executable(scratch);
    test_wasm_commands();
    test_wasm_module_detection(scratch);

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
[[nodiscard]] b3::Command spawn_self(std::string_view executable,
                                    std::span<const std::string_view> arguments)
{
    b3::Command command;
    command.Append(executable);
    for (const std::string_view argument : arguments)
    {
        command.Append(argument);
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
            return tests::run_self_test();
        }
        if (argument == examples::kExampleFlag)
        {
            const std::string_view name =
                index + 1 < argc ? std::string_view(argv[index + 1]) : examples::kAllExamples;
            return examples::run_example(name);
        }
    }

    const std::array<fs::Path, 1> selfSources{fs::Path(kSelfSource)};
    rebuild_yourself(argc, argv, selfSources);

    const std::string_view executable =
        argc > 0 && argv[0] != nullptr ? std::string_view(argv[0]) : std::string_view("./b3");

    Builder builder;

    Command compile;
    compile.AppendAll(compiler_executable(), kStandardFlag);
    for (const std::string_view flag : kWarningFlags)
    {
        compile.Append(flag);
    }
    compile.AppendAll(kOutputFlag, kSelfBinary, kSelfSource);

    Target build{kDefaultTarget};
    build.Output(fs::Path(kSelfBinary)).Input(fs::Path(kSelfSource)).AddCommand(std::move(compile));
    builder.AddTarget(std::move(build));

    const std::array<std::string_view, 1> selfTestArguments{tests::kSelfTestFlag};
    Target check{kCheckTarget};
    check.AddCommand(spawn_self(executable, selfTestArguments));
    builder.AddTarget(std::move(check));

    // One child process per example, so a crashing example cannot take the
    // build script down with it.
    Target runExamples{kExamplesTarget};
    for (const examples::Example& example : examples::kExamples)
    {
        const std::array<std::string_view, 2> arguments{examples::kExampleFlag, example.m_Name};
        runExamples.AddCommand(spawn_self(executable, arguments));
    }
    builder.AddTarget(std::move(runExamples));

    // A shortcut for the WebAssembly example.
    const std::array<std::string_view, 2> wasmArguments{examples::kExampleFlag,
                                                        examples::kWasmExampleName};
    Target wasm{kWasmTarget};
    wasm.AddCommand(spawn_self(executable, wasmArguments));
    builder.AddTarget(std::move(wasm));

    Target clean{kCleanTarget};
    clean.AddCommand(Command({"rm", "-rf", kBuildDirectory}));
    builder.AddTarget(std::move(clean));

    return builder.RunCommandLine(argc, argv, kDefaultTarget);
}
