cmake_minimum_required(VERSION 3.25)

# Without an explicit choice, and with no vcpkg toolchain or VCPKG_ROOT in reach, default to distro packages
# so a plain `cmake ..` works out of the box on Linux; otherwise default to vcpkg as before (BUILDING.md).
if(NOT DEFINED CACHE{SEMSPLINTER_SYSTEM_DEPS}
   AND NOT CMAKE_TOOLCHAIN_FILE
   AND NOT DEFINED ENV{VCPKG_ROOT}
   AND NOT CMAKE_PREFIX_PATH
   AND NOT DEFINED ENV{CMAKE_PREFIX_PATH})
    set(_semsplinter_system_deps_default ON)
    set(_semsplinter_system_deps_autodetected TRUE)
else()
    set(_semsplinter_system_deps_default OFF)
endif()

option(SEMSPLINTER_SYSTEM_DEPS "Use installed (distro) packages instead of vcpkg; see BUILDING.md"
       ${_semsplinter_system_deps_default})

if(_semsplinter_system_deps_autodetected AND SEMSPLINTER_SYSTEM_DEPS)
    message(STATUS "No vcpkg toolchain found (VCPKG_ROOT unset): defaulting to distro packages "
                    "(SEMSPLINTER_SYSTEM_DEPS=ON). Pass -DSEMSPLINTER_SYSTEM_DEPS=OFF and set VCPKG_ROOT "
                    "for the pinned, tested dependency versions (see BUILDING.md).")
endif()
unset(_semsplinter_system_deps_default)
unset(_semsplinter_system_deps_autodetected)

# Dependencies come from vcpkg in manifest mode (BUILDING.md) unless SEMSPLINTER_SYSTEM_DEPS is set. Without
# vcpkg's toolchain, find_package() fails with errors that never mention vcpkg, so check here: a plain
# `cmake ..` uses $VCPKG_ROOT when it is set.
if(NOT SEMSPLINTER_SYSTEM_DEPS)
    if(NOT CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
        file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" _vcpkg_toolchain)
        set(CMAKE_TOOLCHAIN_FILE "${_vcpkg_toolchain}" CACHE FILEPATH "vcpkg toolchain, from VCPKG_ROOT")
    endif()
    if(CMAKE_TOOLCHAIN_FILE AND NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
        message(FATAL_ERROR "Toolchain file not found: ${CMAKE_TOOLCHAIN_FILE}
"
                            "Set VCPKG_ROOT to a bootstrapped vcpkg checkout (see BUILDING.md) and configure again "
                            "in a fresh build directory.")
    elseif(NOT CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_PREFIX_PATH AND NOT DEFINED ENV{CMAKE_PREFIX_PATH})
        message(FATAL_ERROR "No vcpkg toolchain: this project gets its dependencies from vcpkg in manifest mode.
"
                            "Clone and bootstrap vcpkg, set VCPKG_ROOT to it, then run `cmake --preset <name>` "
                            "(see BUILDING.md). To use distro packages instead, pass -DSEMSPLINTER_SYSTEM_DEPS=ON; "
                            "to supply the dependencies yourself, pass CMAKE_PREFIX_PATH.")
    endif()
endif()

option(SEMSPLINTER_BUILD_TESTS "Build the test suite" ON)
option(SEMSPLINTER_FORMAT_PNG "PNG format module (libpng, zlib)" ON)
option(SEMSPLINTER_FORMAT_ZIP "ZIP format module (zlib, zstd)" ON)
option(SEMSPLINTER_FORMAT_TAR "TAR format module (zlib, zstd, liblzma, bzip2)" ON)
option(SEMSPLINTER_FORMAT_JSON "JSON format module (no dependencies)" ON)
option(SEMSPLINTER_FORMAT_XML "XML format module (expat)" ON)
option(SEMSPLINTER_FORMAT_CSV "CSV/TSV format module (no dependencies)" ON)
option(SEMSPLINTER_FORMAT_JPEG "JPEG format module (libjpeg-turbo)" ON)
option(SEMSPLINTER_FORMAT_SVG "SVG format module (needs XML; zlib for .svgz)" ON)
option(SEMSPLINTER_FORMAT_SQLITE "SQLite format module (sqlite3)" ON)
option(SEMSPLINTER_FORMAT_MESH "OBJ/MTL, PLY, STL mesh module (no dependencies)" ON)
option(SEMSPLINTER_FORMAT_PDF "PDF format module (qpdf)" ON)
option(SEMSPLINTER_FORMAT_BMP "BMP format module (no dependencies)" ON)
option(SEMSPLINTER_FORMAT_TGA "TGA format module (no dependencies)" ON)
option(SEMSPLINTER_FORMAT_QOI "QOI format module (no dependencies)" ON)
option(SEMSPLINTER_FORMAT_GIF "GIF format module (giflib)" ON)
option(SEMSPLINTER_FORMAT_TIFF "TIFF format module (libtiff, zlib)" ON)
option(SEMSPLINTER_FORMAT_WEBP "WebP format module (libwebp)" ON)

# An ON option enables the vcpkg manifest feature it names (SPEC §2.3). Must run before project().
macro(semsplinter_vcpkg_feature option feature)
    if(${option})
        list(APPEND VCPKG_MANIFEST_FEATURES ${feature})
    endif()
endmacro()
semsplinter_vcpkg_feature(SEMSPLINTER_BUILD_TESTS tests)
semsplinter_vcpkg_feature(SEMSPLINTER_FORMAT_PNG png)
semsplinter_vcpkg_feature(SEMSPLINTER_FORMAT_ZIP zip)
semsplinter_vcpkg_feature(SEMSPLINTER_FORMAT_TAR tar)
semsplinter_vcpkg_feature(SEMSPLINTER_FORMAT_JSON json)
semsplinter_vcpkg_feature(SEMSPLINTER_FORMAT_XML xml)
semsplinter_vcpkg_feature(SEMSPLINTER_FORMAT_CSV csv)
semsplinter_vcpkg_feature(SEMSPLINTER_FORMAT_JPEG jpeg)
semsplinter_vcpkg_feature(SEMSPLINTER_FORMAT_SVG svg)
semsplinter_vcpkg_feature(SEMSPLINTER_FORMAT_SQLITE sqlite)
semsplinter_vcpkg_feature(SEMSPLINTER_FORMAT_MESH mesh)
semsplinter_vcpkg_feature(SEMSPLINTER_FORMAT_PDF pdf)
semsplinter_vcpkg_feature(SEMSPLINTER_FORMAT_BMP bmp)
semsplinter_vcpkg_feature(SEMSPLINTER_FORMAT_TGA tga)
semsplinter_vcpkg_feature(SEMSPLINTER_FORMAT_QOI qoi)
semsplinter_vcpkg_feature(SEMSPLINTER_FORMAT_GIF gif)
semsplinter_vcpkg_feature(SEMSPLINTER_FORMAT_TIFF tiff)
semsplinter_vcpkg_feature(SEMSPLINTER_FORMAT_WEBP webp)

project(SemanticSplinter VERSION 0.1.0 DESCRIPTION "Format-aware diff, merge, and patch" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # no C++20 modules; scanning needs clang-scan-deps, absent with clang++-N (#117)

if(SEMSPLINTER_SYSTEM_DEPS)
    include(cmake/SystemDeps.cmake)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    # Partial aggregate init ({"/a"} for Address) is intended; members have defaults.
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()

add_subdirectory(src/core)
add_subdirectory(src/formats)
add_subdirectory(src/git)
add_subdirectory(src/cli)

if(SEMSPLINTER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
