#pragma once

#include "b3/Command.hpp"
#include "b3/FileSystem.hpp"

#include <string>
#include <vector>

namespace b3
{

/// A make style rule: a named node that produces output files from input files
/// and from other targets by running a list of commands.
class Target
{
public:
    explicit Target(std::string name);

    [[nodiscard]] const std::string& name() const { return m_Name; }

    /// A phony target is always considered out of date, like .PHONY in make.
    [[nodiscard]] bool isPhony() const { return m_Outputs.empty(); }

    Target& output(fs::Path path);
    Target& input(fs::Path path);
    Target& dependsOn(std::string targetName);
    Target& command(Command command);

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

} // namespace b3
