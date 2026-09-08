#include "b3/Target.hpp"

#include <utility>

namespace b3
{

Target::Target(std::string name)
    : m_Name(std::move(name))
{
}

Target& Target::output(fs::Path path)
{
    m_Outputs.push_back(std::move(path));
    return *this;
}

Target& Target::input(fs::Path path)
{
    m_Inputs.push_back(std::move(path));
    return *this;
}

Target& Target::dependsOn(std::string targetName)
{
    m_Dependencies.push_back(std::move(targetName));
    return *this;
}

Target& Target::command(Command command)
{
    m_Commands.push_back(std::move(command));
    return *this;
}

} // namespace b3
