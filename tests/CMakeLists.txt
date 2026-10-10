find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_executable(semsplinter-tests
    no_dialogs.cpp
    cli/test_args.cpp
    cli/test_cli.cpp
    cli/test_golden.cpp
    cli/test_patch.cpp
    core/test_basics.cpp
    core/test_envelope.cpp
    core/test_merge.cpp
    core/test_report.cpp
    core/test_sequence.cpp
    core/test_version.cpp
    formats/test_api.cpp
    formats/test_binary.cpp
    formats/test_json.cpp
    formats/test_png.cpp
    formats/test_text.cpp
    formats/test_xml.cpp
    git/test_adapter.cpp
    git/test_git.cpp
)
if(SEMSPLINTER_FORMAT_ZIP AND SEMSPLINTER_FORMAT_PNG AND SEMSPLINTER_FORMAT_JSON) # the exit test nests a PNG and a JSON
    target_sources(semsplinter-tests PRIVATE formats/test_zip.cpp)
endif()
if(SEMSPLINTER_FORMAT_TAR)
    target_sources(semsplinter-tests PRIVATE formats/test_tar.cpp)
endif()
if(SEMSPLINTER_FORMAT_CSV)
    target_sources(semsplinter-tests PRIVATE formats/test_csv.cpp)
endif()
target_link_libraries(semsplinter-tests PRIVATE splinter-cli Catch2::Catch2WithMain)
target_compile_definitions(semsplinter-tests PRIVATE SEMSPLINTER_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    SPLINTER_EXE="$<TARGET_FILE:splinter>") # the driver the [git] tests configure
add_dependencies(semsplinter-tests splinter)
catch_discover_tests(semsplinter-tests)

add_test(NAME cli.version COMMAND splinter --version)
set_tests_properties(cli.version PROPERTIES
    PASS_REGULAR_EXPRESSION "^splinter \\(Semantic Splinter\\) ${PROJECT_VERSION}\n$")
