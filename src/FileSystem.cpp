#include "b3/FileSystem.hpp"

#include "b3/Log.hpp"

#include <algorithm>
#include <system_error>

namespace b3::fs
{

std::optional<FileTime> modificationTime(const Path& path)
{
    std::error_code errorCode;
    const FileTime time = std::filesystem::last_write_time(path, errorCode);
    if (errorCode)
    {
        return std::nullopt;
    }
    return time;
}

bool exists(const Path& path)
{
    std::error_code errorCode;
    return std::filesystem::exists(path, errorCode);
}

bool makeDirectories(const Path& path)
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

bool isOutOfDate(std::span<const Path> outputs, std::span<const Path> inputs)
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

std::vector<Path> listFiles(const Path& directory, std::span<const std::string> extensions)
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
                                   [&path](const std::string& extension)
                                   { return path.extension() == extension; });
        if (matches)
        {
            files.push_back(path);
        }
    }

    std::ranges::sort(files);
    return files;
}

} // namespace b3::fs
