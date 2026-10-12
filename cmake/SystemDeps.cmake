# SEMSPLINTER_SYSTEM_DEPS (BUILDING.md): dependencies from distro packages instead of vcpkg. Most ship a CMake config
# or have a CMake find module; these four do not, or name their targets differently. A config file in CMake's
# redirects directory wins over any other search, so find_package(<name> CONFIG) gets pkg-config under vcpkg's names.
function(semsplinter_pkgconfig_package package module target)
    string(TOLOWER "${package}" lower)
    file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/${lower}-config.cmake" "
if(NOT TARGET ${target})
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(${module} REQUIRED IMPORTED_TARGET GLOBAL ${module})
    add_library(${target} ALIAS PkgConfig::${module})
endif()
")
endfunction()

semsplinter_pkgconfig_package(xxHash libxxhash xxHash::xxhash)
semsplinter_pkgconfig_package(zstd libzstd zstd::libzstd) # zstd 1.5.5 exports only zstd::libzstd_shared/_static
semsplinter_pkgconfig_package(liblzma liblzma liblzma::liblzma)
semsplinter_pkgconfig_package(unofficial-sqlite3 sqlite3 unofficial::sqlite3::sqlite3)

add_compile_definitions(POINTERHOLDER_TRANSITION=4) # qpdf 11 warns without it; qpdf 12 dropped PointerHolder
