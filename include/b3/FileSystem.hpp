#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace b3::fs
{

using Path = std::filesystem::path;
using FileTime = std::filesystem::file_time_type;

/// Returns the last write time of a path, or std::nullopt when it does not exist.
std::optional<FileTime> modificationTime(const Path& path);

bool exists(const Path& path);

/// Creates a directory and all of its missing parents. Returns false on failure.
bool makeDirectories(const Path& path);

/// True when any of the inputs is newer than the oldest output, or when an
/// output is missing. Missing inputs are reported through the returned value as
/// "needs rebuild" so that the command can produce a proper error message.
bool isOutOfDate(std::span<const Path> outputs, std::span<const Path> inputs);

/// Lists files in a directory (non recursive) whose extension matches one of
/// the given extensions. Extensions must include the leading dot.
std::vector<Path> listFiles(const Path& directory, std::span<const std::string> extensions);

} // namespace b3::fs
