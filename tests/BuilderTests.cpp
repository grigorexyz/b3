/// Minimal self contained tests for the b3 library. They are intentionally
/// dependency free so that the bootstrap compiler command stays a one liner.

#include "b3/Builder.hpp"
#include "b3/Command.hpp"
#include "b3/FileSystem.hpp"
#include "b3/Log.hpp"
#include "b3/Target.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{

int g_Failures = 0;

void check(bool condition, std::string_view what)
{
    if (condition)
    {
        std::cout << "ok   " << what << '\n';
        return;
    }
    std::cout << "FAIL " << what << '\n';
    ++g_Failures;
}

void writeFile(const b3::fs::Path& path, std::string_view content)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << content;
}

b3::fs::Path makeScratchDirectory()
{
    const b3::fs::Path directory = std::filesystem::temp_directory_path() / "b3-tests";
    std::filesystem::remove_all(directory);
    b3::fs::makeDirectories(directory);
    return directory;
}

void testCommandRendering()
{
    b3::Command command;
    command.appendAll("c++", "-o", "build/with space");
    check(command.arguments().size() == 3, "command keeps every argument");
    check(command.render() == R"(c++ -o "build/with space")", "command quotes for logging");
    check(b3::Command().empty(), "default constructed command is empty");
}

void testCommandExecution()
{
    b3::Command trueCommand({"true"});
    check(trueCommand.run() == 0, "successful command returns zero");

    b3::Command falseCommand({"false"});
    check(falseCommand.run() != 0, "failing command returns non zero");
}

void testOutOfDateDetection(const b3::fs::Path& scratch)
{
    const b3::fs::Path input = scratch / "input.txt";
    const b3::fs::Path output = scratch / "output.txt";
    writeFile(input, "hello");

    const std::vector<b3::fs::Path> outputs{output};
    const std::vector<b3::fs::Path> inputs{input};
    check(b3::fs::isOutOfDate(outputs, inputs), "missing output is out of date");

    writeFile(output, "world");
    std::filesystem::last_write_time(output, *b3::fs::modificationTime(input)
                                                 + std::chrono::seconds(1));
    check(!b3::fs::isOutOfDate(outputs, inputs), "newer output is up to date");

    std::filesystem::last_write_time(input, *b3::fs::modificationTime(output)
                                                + std::chrono::seconds(1));
    check(b3::fs::isOutOfDate(outputs, inputs), "newer input is out of date");
}

void testBuilderRebuildsAndCaches(const b3::fs::Path& scratch)
{
    const b3::fs::Path input = scratch / "copy-input.txt";
    const b3::fs::Path output = scratch / "nested" / "copy-output.txt";
    writeFile(input, "payload");

    b3::Builder builder;
    b3::Target copy("copy");
    b3::Command copyCommand;
    copyCommand.appendAll("cp", input.string(), output.string());
    copy.output(output).input(input).command(std::move(copyCommand));
    builder.addTarget(std::move(copy));

    check(builder.build("copy") == b3::BuildStatus::Rebuilt, "first build runs the command");
    check(b3::fs::exists(output), "build creates the output and its directory");
    check(builder.build("copy") == b3::BuildStatus::UpToDate, "second build is a no operation");

    builder.setAlwaysMake(true);
    check(builder.build("copy") == b3::BuildStatus::Rebuilt, "always make forces a rebuild");
    builder.setAlwaysMake(false);
}

void testBuilderDependenciesAndFailures()
{
    b3::Builder builder;

    b3::Target first("first");
    first.command(b3::Command({"true"}));
    builder.addTarget(std::move(first));

    b3::Target second("second");
    second.dependsOn("first").command(b3::Command({"true"}));
    builder.addTarget(std::move(second));

    check(builder.build("second") == b3::BuildStatus::Rebuilt, "phony targets always run");
    check(builder.build("missing") == b3::BuildStatus::Failed, "unknown target fails");

    b3::Target broken("broken");
    broken.command(b3::Command({"false"}));
    builder.addTarget(std::move(broken));
    check(builder.build("broken") == b3::BuildStatus::Failed, "failing command fails the target");

    b3::Target left("left");
    left.dependsOn("right");
    builder.addTarget(std::move(left));
    b3::Target right("right");
    right.dependsOn("left");
    builder.addTarget(std::move(right));
    check(builder.build("left") == b3::BuildStatus::Failed, "dependency cycle is detected");
}

void testDryRun(const b3::fs::Path& scratch)
{
    const b3::fs::Path output = scratch / "dry-run.txt";

    b3::Builder builder;
    b3::Target touch("touch");
    b3::Command touchCommand;
    touchCommand.appendAll("touch", output.string());
    touch.output(output).command(std::move(touchCommand));
    builder.addTarget(std::move(touch));

    builder.setDryRun(true);
    check(builder.build("touch") == b3::BuildStatus::Rebuilt, "dry run reports a rebuild");
    check(!b3::fs::exists(output), "dry run does not touch the file system");
}

void testTargetRegistry()
{
    b3::Builder builder;
    builder.addTarget(b3::Target("alpha"));
    builder.addTarget(b3::Target("beta"));

    check(builder.hasTarget("alpha"), "registered target is found");
    check(builder.findTarget("gamma") == nullptr, "unknown target is not found");
    check(builder.targetNames().size() == 2, "target names are listed");
}

} // namespace

int main()
{
    b3::setLogLevel(b3::LogLevel::Error);

    const b3::fs::Path scratch = makeScratchDirectory();

    testCommandRendering();
    testCommandExecution();
    testOutOfDateDetection(scratch);
    testBuilderRebuildsAndCaches(scratch);
    testBuilderDependenciesAndFailures();
    testDryRun(scratch);
    testTargetRegistry();

    std::filesystem::remove_all(scratch);

    if (g_Failures > 0)
    {
        std::cout << g_Failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "all tests passed\n";
    return 0;
}
