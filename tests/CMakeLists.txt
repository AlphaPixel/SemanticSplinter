find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_executable(semsplinter-tests
    cli/test_args.cpp
    cli/test_cli.cpp
    cli/test_golden.cpp
    core/test_basics.cpp
    core/test_envelope.cpp
    core/test_merge.cpp
    core/test_report.cpp
    core/test_sequence.cpp
    core/test_version.cpp
    formats/test_binary.cpp
    formats/test_text.cpp
)
target_link_libraries(semsplinter-tests PRIVATE splinter-cli Catch2::Catch2WithMain)
target_compile_definitions(semsplinter-tests PRIVATE SEMSPLINTER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
catch_discover_tests(semsplinter-tests)

add_test(NAME cli.version COMMAND splinter --version)
set_tests_properties(cli.version PROPERTIES
    PASS_REGULAR_EXPRESSION "^splinter \\(Semantic Splinter\\) ${PROJECT_VERSION}\n$")
