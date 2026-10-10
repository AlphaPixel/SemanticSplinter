# The CLI as a library, so tests can drive it in-process.
add_library(splinter-cli STATIC args.cpp tables.cpp cli.cpp diff_cmd.cpp merge_cmd.cpp process.cpp)
target_include_directories(splinter-cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(splinter-cli PUBLIC SemanticSplinter::formats splinter-git)

add_executable(splinter main.cpp)
if(WIN32)
    target_sources(splinter PRIVATE splinter.manifest) # UTF-8 active code page (D-24)
endif()
target_link_libraries(splinter PRIVATE splinter-cli)

# Personality aliases (CLI.md §1): symlinks on POSIX, hard links (or copies) on Windows.
include(GNUInstallDirs)
install(TARGETS splinter RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
foreach(alias splinter-diff splinter-diff3 splinter-merge splinter-patch)
    if(WIN32)
        install(CODE "
            set(dir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
            file(CREATE_LINK \"\${dir}/splinter.exe\" \"\${dir}/${alias}.exe\" COPY_ON_ERROR)
            message(STATUS \"Installing: \${dir}/${alias}.exe\")")
    else()
        install(CODE "
            set(dir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
            file(CREATE_LINK splinter \"\${dir}/${alias}\" SYMBOLIC)
            message(STATUS \"Installing: \${dir}/${alias}\")")
    endif()
endforeach()
