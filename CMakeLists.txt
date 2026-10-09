cmake_minimum_required(VERSION 3.25)

option(SEMSPLINTER_BUILD_TESTS "Build the test suite" ON)

# An ON option enables the vcpkg manifest feature it names (SPEC §2.3). Must run before project().
macro(semsplinter_vcpkg_feature option feature)
    if(${option})
        list(APPEND VCPKG_MANIFEST_FEATURES ${feature})
    endif()
endmacro()
semsplinter_vcpkg_feature(SEMSPLINTER_BUILD_TESTS tests)

project(SemanticSplinter VERSION 0.1.0 DESCRIPTION "Format-aware diff, merge, and patch" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    # Partial aggregate init ({"/a"} for Address) is intended; members have defaults.
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()

add_subdirectory(src/core)
add_subdirectory(src/cli)

if(SEMSPLINTER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
