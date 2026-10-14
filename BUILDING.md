# Building Semantic Splinter

All third-party dependencies (zlib, zstd, xxHash, nlohmann-json, libpng, expat, qpdf, ...) come from
**[vcpkg](https://vcpkg.io) in manifest mode**. `vcpkg.json` lists them, pinned to one vcpkg commit
(`builtin-baseline`, D-52), and CMake finds them with `find_package(... CONFIG)` through vcpkg's toolchain file.
vcpkg remains the tested and release configuration, and it's the only supported path on Windows and macOS.

On Linux, you don't need it: without `VCPKG_ROOT` set, both a plain `cmake ..` and the `linux-gcc`/`linux-clang`
presets fall back automatically to distro packages ([section 5](#5-without-vcpkg-distro-packages)), so a fresh
clone builds with nothing but the system's `-dev` packages. Set `VCPKG_ROOT` (step 2 below) to build the
vcpkg/pinned configuration instead: the CMake presets then pass `$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`
as the toolchain, and the first configure builds the dependencies into `build/<preset>/vcpkg_installed/` (this
takes 10–30 minutes once; later builds reuse vcpkg's binary cache). Windows and macOS always require it.

## 1. Tools

| | Windows | Linux (Ubuntu/Debian) | macOS |
|---|---|---|---|
| Compiler | Visual Studio 2022 17.8+, "Desktop development with C++" | GCC 11+ or Clang 14+ | AppleClang 15+ (Xcode command line tools) |
| CMake 3.25+ | included with Visual Studio | `cmake` (Ubuntu 24.04 has 3.28; on 22.04 use Kitware's APT repository or `pip install cmake`) | `brew install cmake` |
| Ninja | not needed (Visual Studio generator) | `ninja-build` | `brew install ninja` |
| vcpkg's own needs | Git | `git curl zip unzip tar pkg-config` | `git pkg-config` (`brew install pkg-config`) |

Ubuntu/Debian in one line:

```bash
sudo apt-get install -y build-essential git cmake ninja-build curl zip unzip tar pkg-config
sudo apt-get install -y clang      # only for the linux-clang preset
```

## 2. vcpkg

Clone it **in full** (no `--depth`): the pinned baseline needs vcpkg's port history. Any location works.

Linux and macOS:

```bash
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
echo 'export VCPKG_ROOT="$HOME/vcpkg"' >> ~/.bashrc    # or ~/.zshrc; then open a new shell
```

Windows (PowerShell):

```powershell
git clone https://github.com/microsoft/vcpkg C:\src\vcpkg
C:\src\vcpkg\bootstrap-vcpkg.bat -disableMetrics
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "C:\src\vcpkg", "User")   # then open a new shell
```

Visual Studio also ships a vcpkg and its developer prompt sets `VCPKG_ROOT` to it; a standalone clone as above is
more predictable, so set `VCPKG_ROOT` explicitly.

## 3. Configure, build, test

| Platform | Presets (Debug / Release) |
|---|---|
| Windows | `windows-msvc` / `windows-msvc-release` |
| Linux, GCC | `linux-gcc` / `linux-gcc-release` |
| Linux, Clang | `linux-clang` / `linux-clang-release` |
| macOS | `macos` / `macos-release` |

The configure preset has no `-release` suffix (the build directories are multi-config); build and test presets do:

```bash
cmake --preset linux-gcc                    # configure: vcpkg if VCPKG_ROOT is set, distro packages otherwise
cmake --build --preset linux-gcc-release    # or linux-gcc for Debug
ctest --preset linux-gcc-release
```

On Linux this works with nothing beyond [section 1](#1-tools)'s packages installed — no vcpkg checkout required.
`windows-msvc` and `macos` always need `VCPKG_ROOT` (step 2 below); there's no distro-package fallback for them.

The tool lands in `build/<preset>/src/cli/<Debug|Release>/splinter` (`splinter.exe` on Windows). Visual Studio
2022 can open the folder directly and uses the same presets.

**Without a preset**, a plain configure works too and follows the same rule: `cmake -S . -B build -G Ninja &&
cmake --build build`, using vcpkg if `VCPKG_ROOT` is set and distro packages otherwise — the same as passing
`-DSEMSPLINTER_SYSTEM_DEPS=ON` explicitly (section 5).

**Versioned compilers.** The `linux-clang` preset calls `clang++`. If only a versioned binary exists (for
example `clang++-19`), override it: `cmake --preset linux-clang -DCMAKE_CXX_COMPILER=clang++-19`. Don't set
`CC`/`CXX` to that Clang for the whole configure: vcpkg then builds the C libraries with it too, and libwebp fails
with Clang 19 (#117). The preset builds them with the system C compiler, as CI does.

**Fewer formats.** Every format is a module behind an option and a vcpkg feature, all on by default. Turn off what
you don't need to skip its dependencies, e.g. `-DSEMSPLINTER_FORMAT_PDF=OFF` (qpdf), and `-DSEMSPLINTER_BUILD_TESTS=OFF`
(Catch2). Option names are at the top of `CMakeLists.txt`.

## 4. Troubleshooting

| Message | Cause and fix |
|---|---|
| `No vcpkg toolchain: this project gets its dependencies from vcpkg ...` | `VCPKG_ROOT` is not set in this shell. Do step 2, open a new shell, delete the build directory, configure again. |
| `Toolchain file not found: /scripts/buildsystems/vcpkg.cmake` | Same: a preset was used with `VCPKG_ROOT` unset or pointing at the wrong place. |
| `Could not find a package configuration file provided by "xxHash"` (or zstd, nlohmann_json) | The build directory was first configured without the vcpkg toolchain; CMake reads the toolchain only on the first configure. Delete the build directory and configure again. |
| `error: ... builtin-baseline ... failed to fetch` or `... is not a commit` | vcpkg was cloned shallow or is too old: `git -C "$VCPKG_ROOT" fetch --unshallow` (or `git pull`), then configure again. |
| `CMake was unable to find a build program corresponding to "Ninja Multi-Config"` | Install Ninja (step 1). |
| `building <port>:x64-linux failed` | Look at the log path vcpkg prints. Usually a missing tool from step 1 (`pkg-config`, `zip`, ...). |
| `clang++: not found` with `linux-clang` | Install `clang`, or pass the versioned compiler as above. |

## 5. Without vcpkg: distro packages

`-DSEMSPLINTER_SYSTEM_DEPS=ON` skips vcpkg and uses the installed `-dev` packages. Four of them ship no usable
CMake config (xxHash, zstd, liblzma, SQLite); `cmake/SystemDeps.cmake` finds those through pkg-config. Tested on
Ubuntu 24.04 with GCC 13 and Clang 19, all formats on.

Ubuntu 24.04:

```bash
sudo apt-get install -y build-essential cmake ninja-build pkg-config     nlohmann-json3-dev libxxhash-dev libzstd-dev zlib1g-dev libpng-dev libexpat1-dev libjpeg-turbo8-dev     libsqlite3-dev libqpdf-dev libgif-dev libtiff-dev libwebp-dev liblzma-dev libbz2-dev catch2
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSEMSPLINTER_SYSTEM_DEPS=ON
cmake --build build && ctest --test-dir build
```

Distro versions differ from the pinned ports (for example libwebp 1.3.2 and qpdf 11.9 against 1.6.0 and 12.4.0).
Diff and merge work the same, but a re-encoded stream (deflate, lossless WebP) can differ byte for byte from a
vcpkg build, so don't compare outputs across the two. With libjpeg-turbo before 3.0 (Ubuntu 24.04 has 2.1.5),
12-bit JPEGs diff and merge but get no pixel metrics or `--visual`. Report build failures on other distributions
as issues.
