#pragma once

#include "b3/Target.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace b3
{

/// Outcome of building a single target.
enum class BuildStatus
{
    UpToDate,
    Rebuilt,
    Failed,
};

/// Owns the target graph and executes it. The graph storage and the traversal
/// bookkeeping live behind a pImpl so that adding new scheduling strategies
/// does not break the ABI of the public interface.
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

    /// Convenience entry point for build scripts: parses the usual flags
    /// (-n, -B, -v, -q, -h) plus target names and builds them in order.
    /// Returns a process exit code.
    [[nodiscard]] int runCommandLine(int argc, char* argv[], std::string_view defaultTarget);

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace b3
