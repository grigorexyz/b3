# b3

A small build system written in C++23, inspired by `make` (targets, prerequisites,
timestamp based rebuilds, phony targets) and by [nob](https://github.com/tsoding/nob.h)
(the build script is a normal C++ program that recompiles itself).

There is no configuration language: a build is described in `b3.cpp` by creating
`b3::Target` objects and handing them to a `b3::Builder`.

## Bootstrap

```sh
c++ -std=c++23 -Iinclude -o b3 b3.cpp src/*.cpp
./b3            # builds build/libb3.a
./b3 check      # builds and runs the tests
./b3 clean      # removes the build directory
```

After the first bootstrap `./b3` recompiles and re-executes itself whenever
`b3.cpp` or any file in `src/` changed, so the compiler command above is only
ever needed once.

## Command line

`b3::Builder::runCommandLine` understands the familiar `make` style flags:

| Flag | Meaning |
| --- | --- |
| `-n`, `--dry-run` | print commands without executing them |
| `-B`, `--always-make` | rebuild every target, ignoring timestamps |
| `-v`, `--verbose` | print trace output explaining rebuild decisions |
| `-q`, `--quiet` | only print warnings and errors |
| `-h`, `--help` | list the available targets |

Any remaining arguments are target names; without one the default target passed
to `runCommandLine` is built.

## Layout

| Path | Contents |
| --- | --- |
| `include/b3/Log.hpp` | leveled logging built on `std::format` |
| `include/b3/FileSystem.hpp` | timestamps, directory creation, out of date checks |
| `include/b3/Command.hpp` | an argument vector that is executed without a shell |
| `include/b3/Target.hpp` | a make style rule: outputs, inputs, dependencies, commands |
| `include/b3/Builder.hpp` | the target graph and its depth first execution |
| `include/b3/SelfRebuild.hpp` | nob style bootstrapping of the build script |
| `src/` | the implementation of the library |
| `tests/BuilderTests.cpp` | dependency free tests, run with `./b3 check` |
| `b3.cpp` | the build script for b3 itself, and the example to copy |

## Describing a build

```cpp
b3::Builder builder;

b3::Command compile;
compile.appendAll("c++", "-std=c++23", "-c", "src/Main.cpp", "-o", "build/Main.o");

b3::Target object("compile:main");
object.output("build/Main.o").input("src/Main.cpp").command(std::move(compile));
builder.addTarget(std::move(object));
```

A target with no outputs is phony and therefore always runs. A target is rebuilt
when one of its outputs is missing, when an input is newer than the outputs, or
when one of its dependencies was rebuilt.

## Conventions

* C++23, no third party dependencies.
* Member variables use the `m_PascalCase` prefix, instances and functions use
  `camelCase`, types use `PascalCase`.
* `Command` and `Builder` hide their state behind the pImpl pattern; ownership
  elsewhere is expressed with standard containers and smart pointers.

## License

BSD 2-Clause, see [LICENSE](LICENSE).
