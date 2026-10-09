find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_executable(semsplinter-tests
    core/test_version.cpp
)
target_link_libraries(semsplinter-tests PRIVATE SemanticSplinter::core Catch2::Catch2WithMain)
catch_discover_tests(semsplinter-tests)

add_test(NAME cli.version COMMAND splinter --version)
set_tests_properties(cli.version PROPERTIES
    PASS_REGULAR_EXPRESSION "^splinter \\(Semantic Splinter\\) ${PROJECT_VERSION}\n$")
