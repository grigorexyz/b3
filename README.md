# b3

A small build system written in C++23, inspired by `make` (targets, prerequisites,
timestamp based rebuilds, phony targets) and by [nob](https://github.com/tsoding/nob.h)
(the build script is a normal C++ program that recompiles itself).

Everything — the library, the build script for b3 itself, the tests and the
examples — lives in the single translation unit [`b3.cpp`](b3.cpp). There is no
configuration language: a build is described by creating `b3::Target` objects
and handing them to a `b3::Builder`.

## Bootstrap

```sh
c++ -std=c++23 -o b3 b3.cpp
./b3             # compiles b3 into build/b3
./b3 check       # runs the tests in a child process
./b3 examples    # spawns one child process per example
./b3 wasm        # builds a WebAssembly module
./b3 clean       # removes the build directory
```

After the first bootstrap `./b3` recompiles and re-executes itself whenever
`b3.cpp` changed, so the compiler command above is only ever needed once.

## Command line

`b3::Builder::runCommandLine` understands the familiar `make` style flags:

| Flag | Meaning |
| --- | --- |
| `-n`, `--dry-run` | print commands without executing them |
| `-B`, `--always-make` | rebuild every target, ignoring timestamps |
| `-v`, `--verbose` | print trace output explaining rebuild decisions |
| `-q`, `--quiet` | only print warnings and errors |
| `-h`, `--help` | list the available targets |
| `--wasm-toolchain <name>` | force `emscripten` or `clang` for WebAssembly builds |

Two flags are handled before the graph is even built, because they select an in
process mode instead of a build:

| Flag | Meaning |
| --- | --- |
| `--self-test` | run the test suite |
| `--example <name>` | run one example, or all of them with `all` |

Any remaining arguments are target names; without one the default target is built.

## Examples

The examples are not files on disk. Each one materialises its own little project
in a scratch directory under the temporary directory, from sources kept in
`constexpr std::string_view` literals, and drives it with its own `b3::Builder`
instance. The `examples` target spawns b3 itself once per example, so every
example runs in a fresh process with a fresh graph.

| Example | What it shows |
| --- | --- |
| `hello` | compile, link and run a single file program, then prove the second build does nothing |
| `library` | archive a static library and link a program against it |
| `pipeline` | phony targets, dry runs and how a failing command aborts a build |
| `wasm` | build a WebAssembly module with emscripten or with clang++ |

```sh
./b3 --example hello
./b3 --example all
```

## WebAssembly

A `b3::WasmModule` describes a module — its output, its sources and the symbols
it exports — and lowers it to a command for one of two toolchains:

| Toolchain | Compiler | How the module is built |
| --- | --- | --- |
| `emscripten` | `em++` | `-sSTANDALONE_WASM`, exports as `-sEXPORTED_FUNCTIONS=_add,...` |
| `clang` | `clang++` | `--target=wasm32-unknown-unknown -nostdlib -Wl,--no-entry`, exports as `-Wl,--export=add` |

The clang path is the alternative to emscripten: it needs nothing but a stock
LLVM with the WebAssembly backend and `wasm-ld`, which a recent `clang++` already
ships with. In exchange the module is freestanding — no libc, no `_start`
symbol, and every symbol the host should see has to be exported by name.

The toolchain is picked in this order: the `--wasm-toolchain` flag, the `B3_WASM`
environment variable, then emscripten if `em++` is on the `PATH`, and clang++ as
the fallback. The compiler itself can be overridden with `EMXX` respectively
`WASM_CXX`, and the `wasm` example skips itself when neither compiler is
installed.

```sh
./b3 wasm                            # whichever toolchain is installed
./b3 wasm --wasm-toolchain clang     # force the freestanding clang++ path
B3_WASM=emscripten ./b3 wasm         # force em++
```

## Describing a build

```cpp
b3::Builder builder;

b3::Command compile;
compile.appendAll(b3::compilerExecutable(), b3::kStandardFlag, b3::kCompileOnlyFlag,
                  "src/Main.cpp", b3::kOutputFlag, "build/Main.o");

b3::Target object("compile:main");
object.output("build/Main.o").input("src/Main.cpp").command(std::move(compile));
builder.addTarget(std::move(object));
```

A target with no outputs is phony and therefore always runs. A target is rebuilt
when one of its outputs is missing, when an input is newer than the outputs, or
when one of its dependencies was rebuilt.

A WebAssembly module is described the same way, without spelling out the flags
of either toolchain:

```cpp
b3::WasmModule module("build/math.wasm");
module.source("src/Math.cpp").exportSymbol("add").exportSymbol("factorial");

builder.addTarget(module.target("wasm", b3::detectWasmToolchain().value()));
```

## Conventions

* C++23, one file, no third party dependencies.
* Every fixed string is a `constexpr std::string_view`, from the log prefixes and
  the command line flags to the sources the examples write out; `Command::appendAll`
  accepts them directly.
* `levelPrefix`, the flag comparisons and the example registry are `constexpr`,
  and a handful of `static_assert`s check them at compile time.
* Member variables use the `m_PascalCase` prefix, instances and functions use
  `camelCase`, types use `PascalCase`.
* `Command` and `Builder` hide their state behind the pImpl pattern; ownership
  elsewhere is expressed with standard containers and smart pointers.

## License

BSD 2-Clause, see [LICENSE](LICENSE).
