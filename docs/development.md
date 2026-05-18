# Development

## Formatting and linting

The project uses `clang-format` for C source formatting and `clang-tidy`
for static analysis. Both tools are wired through CMake.

Common commands:

```shell
make format
make format-check
make format-warn
make lint
```

`make format` rewrites source files in place. `make format-check` checks
formatting and fails on differences. `make lint` runs `clang-tidy` against
the sources in `src`.

The commands configure the required CMake build directory automatically:

```shell
make configure_debug
make configure_release
```

On macOS, the Homebrew `llvm` package may not be added to `PATH`
automatically. If needed, expose the LLVM tools explicitly:

```shell
export PATH="$(brew --prefix llvm)/bin:$PATH"
```
