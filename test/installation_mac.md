# Installation on macOS

Install the compiler tools and all build dependencies:

```zsh
brew install cmake pkg-config libevent libpq cjson llvm
```

Then configure, build, test, and optionally install:

```zsh
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --install cmake-builds/release --prefix /usr/local
```

Homebrew's `libpq` is keg-only. The CMake configuration detects its prefix
automatically; no global `PATH`, include-path, or library-path modification is
required.
