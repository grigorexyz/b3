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
./b3 wasm        # builds a freestanding WebAssembly module
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
| `wasm` | build a freestanding WebAssembly module with clang++ |

```sh
./b3 --example hello
./b3 --example all
```

## WebAssembly

b3 targets WebAssembly with a plain `clang++` and nothing else — no emscripten,
no SDK to install, no sysroot to activate. A stock LLVM with the WebAssembly
backend and `wasm-ld`, which a recent `clang++` already ships with, is the whole
toolchain.

A `b3::WasmModule` describes a module — its output, its sources and the symbols
it exports — and lowers it to a single invocation:

```
clang++ -std=c++23 -O2 --target=wasm32-unknown-unknown \
        -nostdlib -fno-exceptions -fno-rtti -Wl,--no-entry -Wl,--allow-undefined \
        -Wl,--export=add -Wl,--export=factorial -o build/math.wasm src/Math.cpp
```

The modules this produces are freestanding: no libc, no `_start` symbol, and
every symbol the host should see has to be exported by name. Set `WASM_CXX` when
the cross compiler is not simply called `clang++`; the `wasm` example skips
itself when no compiler is found.

```sh
./b3 wasm
WASM_CXX=clang++-18 ./b3 wasm
```

## Describing a build

```cpp
b3::Builder builder;

b3::Command compile;
compile.AppendAll(b3::compiler_executable(), b3::kStandardFlag, b3::kCompileOnlyFlag,
                  "src/Main.cpp", b3::kOutputFlag, "build/Main.o");

b3::Target object("compile:main");
object.Output("build/Main.o").Input("src/Main.cpp").AddCommand(std::move(compile));
builder.AddTarget(std::move(object));
```

A target with no outputs is phony and therefore always runs. A target is rebuilt
when one of its outputs is missing, when an input is newer than the outputs, or
when one of its dependencies was rebuilt.

A WebAssembly module is described the same way, without spelling out the flags:

```cpp
b3::WasmModule module("build/math.wasm");
module.Source("src/Math.cpp").ExportSymbol("add").ExportSymbol("factorial");

builder.AddTarget(module.AsTarget("wasm"));
```

## Conventions

* C++23, one file, no third party dependencies.
* There is no `std::string` anywhere. Every string is a `std::string_view`, and
  the few that only exist at run time — a rendered command line, a concatenated
  flag, the contents of a file — are copied once into `b3::text::Arena`, which
  owns them until the process exits.
* Arena blocks are NUL terminated, so an interned view is also a valid
  `const char*`: `Command::Run` hands the very same characters to `execvp`
  without a copy.
* Log messages are formatted into a stack buffer with `std::format_to_n`, so
  even printing never allocates a string.
* Every fixed string is a `constexpr std::string_view`, from the log prefixes and
  the command line flags to the sources the examples write out; `Command::AppendAll`
  accepts them directly.
* `level_prefix`, the flag comparisons and the example registry are `constexpr`,
  and a handful of `static_assert`s check them at compile time.
* Naming: types are `PascalCase` and so are their member functions
  (`Command::Append`, `Builder::AddTarget`). Free functions are `snake_case`
  (`log_info`, `fs::write_file`, `rebuild_yourself`). Member variables carry the
  `m_PascalCase` prefix, and instances — including lambdas — are `camelCase`.
* Two member functions are not named after what they do to avoid hiding a type
  in class scope: `Target::AddCommand` takes a `Command`, and
  `WasmModule::AsTarget` returns a `Target`.
* `Command` and `Builder` hide their state behind the pImpl pattern; ownership
  elsewhere is expressed with standard containers and smart pointers.

## License

BSD 2-Clause, see [LICENSE](LICENSE).
