#include "b3/SelfRebuild.hpp"

#include "b3/Command.hpp"
#include "b3/Log.hpp"

#include <cstdlib>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace b3
{

namespace
{

const char* compilerExecutable()
{
    if (const char* fromEnvironment = std::getenv("CXX"); fromEnvironment != nullptr
        && *fromEnvironment != '\0')
    {
        return fromEnvironment;
    }
    return "c++";
}

bool sourcesAreNewer(const fs::Path& executable, std::span<const fs::Path> sources)
{
    const std::vector<fs::Path> outputs{executable};
    return fs::isOutOfDate(outputs, sources);
}

} // namespace

void rebuildYourself(int argc,
                     char* argv[],
                     std::span<const fs::Path> sources,
                     std::span<const std::string> extraFlags)
{
    if (argc < 1 || argv[0] == nullptr || sources.empty())
    {
        return;
    }

    const fs::Path executable(argv[0]);
    if (!sourcesAreNewer(executable, sources))
    {
        return;
    }

    logInfo("build script is out of date, rebuilding '{}'", executable.string());

    const fs::Path backup = fs::Path(executable).concat(".old");
    std::error_code errorCode;
    std::filesystem::rename(executable, backup, errorCode);
    if (errorCode)
    {
        logWarning("cannot move '{}' aside: {}", executable.string(), errorCode.message());
    }

    Command compile;
    compile.appendAll(compilerExecutable(), "-std=c++23", "-O2", "-o", executable.string());
    for (const fs::Path& source : sources)
    {
        compile.append(source.string());
    }
    for (const std::string& flag : extraFlags)
    {
        compile.append(flag);
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

} // namespace b3
