add_library(semantic-splinter STATIC
    version.cpp
)
add_library(SemanticSplinter::core ALIAS semantic-splinter)

target_include_directories(semantic-splinter PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_definitions(semantic-splinter PRIVATE SEMSPLINTER_VERSION="${PROJECT_VERSION}")
