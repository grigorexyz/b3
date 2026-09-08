/// The b3 build script: it describes how to build b3 itself using the b3
/// library, in the spirit of nob. Bootstrap it once with
///
///     c++ -std=c++23 -Iinclude -o b3 b3.cpp src/*.cpp
///
/// and afterwards simply run ./b3, which recompiles itself whenever one of its
/// own sources changed.

#include "b3/Builder.hpp"
#include "b3/Command.hpp"
#include "b3/FileSystem.hpp"
#include "b3/Log.hpp"
#include "b3/SelfRebuild.hpp"
#include "b3/Target.hpp"

#include <string>
#include <vector>

namespace
{

const std::string kBuildDirectory = "build";
const std::string kLibrary = kBuildDirectory + "/libb3.a";
const std::string kTestBinary = kBuildDirectory + "/b3-tests";

std::vector<b3::fs::Path> librarySources()
{
    const std::vector<std::string> extensions{".cpp"};
    return b3::fs::listFiles("src", extensions);
}

std::string objectPathFor(const b3::fs::Path& source)
{
    return kBuildDirectory + "/obj/" + source.stem().string() + ".o";
}

b3::Command compileCommand(const std::string& source, const std::string& object)
{
    b3::Command command;
    command.appendAll("c++", "-std=c++23", "-Wall", "-Wextra", "-Iinclude", "-c", source, "-o",
                      object);
    return command;
}

} // namespace

int main(int argc, char* argv[])
{
    const std::vector<b3::fs::Path> sources = librarySources();

    std::vector<b3::fs::Path> selfSources = sources;
    selfSources.emplace_back("b3.cpp");
    const std::vector<std::string> selfFlags{"-Iinclude"};
    b3::rebuildYourself(argc, argv, selfSources, selfFlags);

    b3::Builder builder;

    b3::Target archive("lib");
    b3::Command archiveCommand;
    archiveCommand.appendAll("ar", "rcs", kLibrary);

    for (const b3::fs::Path& source : sources)
    {
        const std::string object = objectPathFor(source);
        const std::string name = "compile:" + source.string();

        b3::Target compile(name);
        compile.output(object).input(source).command(compileCommand(source.string(), object));
        builder.addTarget(std::move(compile));

        archive.dependsOn(name);
        archiveCommand.append(object);
    }

    archive.output(kLibrary).command(std::move(archiveCommand));
    builder.addTarget(std::move(archive));

    b3::Target tests("tests");
    b3::Command testsCommand;
    testsCommand.appendAll("c++", "-std=c++23", "-Wall", "-Wextra", "-Iinclude", "-o", kTestBinary,
                           "tests/BuilderTests.cpp", kLibrary);
    tests.output(kTestBinary)
        .input("tests/BuilderTests.cpp")
        .dependsOn("lib")
        .command(std::move(testsCommand));
    builder.addTarget(std::move(tests));

    b3::Target check("check");
    b3::Command runTests;
    runTests.append(kTestBinary);
    check.dependsOn("tests").command(std::move(runTests));
    builder.addTarget(std::move(check));

    b3::Target clean("clean");
    b3::Command removeBuild;
    removeBuild.appendAll("rm", "-rf", kBuildDirectory);
    clean.command(std::move(removeBuild));
    builder.addTarget(std::move(clean));

    return builder.runCommandLine(argc, argv, "lib");
}
