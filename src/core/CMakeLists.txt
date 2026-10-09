find_package(nlohmann_json CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)

add_library(semantic-splinter STATIC
    basics.cpp
    diff.cpp
    emit.cpp
    envelope.cpp
    io.cpp
    merge.cpp
    model.cpp
    report.cpp
    sequence.cpp
    version.cpp
)
add_library(SemanticSplinter::core ALIAS semantic-splinter)

target_include_directories(semantic-splinter PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(semantic-splinter
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE xxHash::xxhash
)
target_compile_definitions(semantic-splinter PRIVATE SEMSPLINTER_VERSION="${PROJECT_VERSION}")
