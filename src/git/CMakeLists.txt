# The Git adapter (CLI.md §7). It sees only the public headers in include/ (D-53): an #include of anything
# private fails to compile.
add_library(splinter-git STATIC git.cpp)
target_include_directories(splinter-git PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(splinter-git PUBLIC SemanticSplinter::formats)
