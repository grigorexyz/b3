#pragma once

#include "b3/FileSystem.hpp"

#include <span>
#include <string>
#include <vector>

namespace b3
{

/// nob style bootstrapping: when one of the sources of the build script is
/// newer than the running executable, the script is recompiled and re-executed
/// with the original arguments. On success this function does not return.
///
/// \param argc         argc as received by main
/// \param argv         argv as received by main, argv[0] is the executable
/// \param sources      the source files the build script is compiled from
/// \param extraFlags   additional compiler flags, for example include paths
void rebuildYourself(int argc,
                     char* argv[],
                     std::span<const fs::Path> sources,
                     std::span<const std::string> extraFlags = {});

} // namespace b3
