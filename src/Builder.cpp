#include "b3/Builder.hpp"

#include "b3/Log.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace b3
{

namespace
{

enum class VisitState
{
    Unvisited,
    InProgress,
    Done,
};

} // namespace

struct Builder::Impl
{
    std::map<std::string, Target, std::less<>> m_Targets;
    std::map<std::string, VisitState, std::less<>> m_States;
    std::map<std::string, BuildStatus, std::less<>> m_Results;
    bool m_DryRun = false;
    bool m_AlwaysMake = false;

    BuildStatus buildTarget(std::string_view name);
    bool runCommands(const Target& target);
};

bool Builder::Impl::runCommands(const Target& target)
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

BuildStatus Builder::Impl::buildTarget(std::string_view name)
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
    if (m_States[targetName] == VisitState::InProgress)
    {
        logError("dependency cycle detected at target '{}'", targetName);
        m_Results[targetName] = BuildStatus::Failed;
        return BuildStatus::Failed;
    }
    m_States[targetName] = VisitState::InProgress;

    const Target& target = entry->second;

    bool dependencyRebuilt = false;
    std::vector<fs::Path> inputs = target.inputs();

    for (const std::string& dependency : target.dependencies())
    {
        const BuildStatus status = buildTarget(dependency);
        if (status == BuildStatus::Failed)
        {
            m_States[targetName] = VisitState::Done;
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

    m_States[targetName] = VisitState::Done;

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

Builder::Builder()
    : m_Impl(std::make_unique<Impl>())
{
}

Builder::~Builder() = default;

Builder::Builder(Builder&& other) noexcept = default;

Builder& Builder::operator=(Builder&& other) noexcept = default;

Target& Builder::addTarget(Target target)
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

bool Builder::hasTarget(std::string_view name) const
{
    return m_Impl->m_Targets.contains(name);
}

const Target* Builder::findTarget(std::string_view name) const
{
    const auto entry = m_Impl->m_Targets.find(name);
    return entry == m_Impl->m_Targets.end() ? nullptr : &entry->second;
}

std::vector<std::string> Builder::targetNames() const
{
    std::vector<std::string> names;
    names.reserve(m_Impl->m_Targets.size());
    for (const auto& [name, target] : m_Impl->m_Targets)
    {
        names.push_back(name);
    }
    return names;
}

void Builder::setDryRun(bool dryRun)
{
    m_Impl->m_DryRun = dryRun;
}

bool Builder::dryRun() const
{
    return m_Impl->m_DryRun;
}

void Builder::setAlwaysMake(bool alwaysMake)
{
    m_Impl->m_AlwaysMake = alwaysMake;
}

bool Builder::alwaysMake() const
{
    return m_Impl->m_AlwaysMake;
}

BuildStatus Builder::build(std::string_view name)
{
    m_Impl->m_States.clear();
    m_Impl->m_Results.clear();
    return m_Impl->buildTarget(name);
}

int Builder::runCommandLine(int argc, char* argv[], std::string_view defaultTarget)
{
    std::vector<std::string> requestedTargets;

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument == "-n" || argument == "--dry-run")
        {
            setDryRun(true);
        }
        else if (argument == "-B" || argument == "--always-make")
        {
            setAlwaysMake(true);
        }
        else if (argument == "-v" || argument == "--verbose")
        {
            setLogLevel(LogLevel::Trace);
        }
        else if (argument == "-q" || argument == "--quiet")
        {
            setLogLevel(LogLevel::Warning);
        }
        else if (argument == "-h" || argument == "--help")
        {
            logMessage(LogLevel::Error,
                       "usage: b3 [-n|--dry-run] [-B|--always-make] [-v|--verbose] "
                       "[-q|--quiet] [target...]");
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

} // namespace b3
