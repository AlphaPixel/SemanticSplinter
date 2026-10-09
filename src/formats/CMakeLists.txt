# Mandatory format modules (SPEC §2.3). Each is a static library.
add_library(semantic-splinter-text STATIC text/text.cpp text/merge.cpp)
add_library(SemanticSplinter::text ALIAS semantic-splinter-text)
target_link_libraries(semantic-splinter-text PUBLIC SemanticSplinter::core)

add_library(semantic-splinter-binary STATIC binary/binary.cpp)
add_library(SemanticSplinter::binary ALIAS semantic-splinter-binary)
target_link_libraries(semantic-splinter-binary PUBLIC SemanticSplinter::core)

# Registry::with_builtin_formats() over every enabled module.
add_library(semantic-splinter-formats STATIC registry.cpp)
add_library(SemanticSplinter::formats ALIAS semantic-splinter-formats)
target_link_libraries(semantic-splinter-formats PUBLIC SemanticSplinter::text SemanticSplinter::binary)
