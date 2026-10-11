# Mandatory format modules (SPEC §2.3). Each is a static library.
add_library(semantic-splinter-text STATIC text/text.cpp text/merge.cpp)
add_library(SemanticSplinter::text ALIAS semantic-splinter-text)
target_link_libraries(semantic-splinter-text PUBLIC SemanticSplinter::core)

add_library(semantic-splinter-binary STATIC binary/binary.cpp)
add_library(SemanticSplinter::binary ALIAS semantic-splinter-binary)
target_link_libraries(semantic-splinter-binary PUBLIC SemanticSplinter::core)

set(formats text binary)

# Raster helper (no format, no external dependencies): canonical grid, regions, heatmap, tile dump.
add_library(semantic-splinter-raster STATIC raster/raster.cpp)
add_library(SemanticSplinter::raster ALIAS semantic-splinter-raster)
target_link_libraries(semantic-splinter-raster PUBLIC SemanticSplinter::core)

if(SEMSPLINTER_FORMAT_PNG)
    find_package(PNG REQUIRED)
    find_package(ZLIB REQUIRED)
    add_library(semantic-splinter-png STATIC png/png.cpp png/codec.cpp)
    add_library(SemanticSplinter::png ALIAS semantic-splinter-png)
    target_link_libraries(semantic-splinter-png PUBLIC SemanticSplinter::raster PRIVATE PNG::PNG ZLIB::ZLIB)
    list(APPEND formats png)
endif()

# Container member layer and codecs shared by zip and tar (containers.md; no format of its own).
if(SEMSPLINTER_FORMAT_ZIP OR SEMSPLINTER_FORMAT_TAR)
    find_package(ZLIB REQUIRED)
    find_package(zstd CONFIG REQUIRED)
    add_library(semantic-splinter-container STATIC container/container.cpp)
    target_link_libraries(semantic-splinter-container PUBLIC SemanticSplinter::core PRIVATE ZLIB::ZLIB zstd::libzstd)
endif()

if(SEMSPLINTER_FORMAT_ZIP)
    add_library(semantic-splinter-zip STATIC zip/zip.cpp)
    add_library(SemanticSplinter::zip ALIAS semantic-splinter-zip)
    target_link_libraries(semantic-splinter-zip PUBLIC semantic-splinter-container)
    list(APPEND formats zip)
endif()

if(SEMSPLINTER_FORMAT_TAR)
    find_package(BZip2 REQUIRED)
    find_package(liblzma CONFIG REQUIRED)
    add_library(semantic-splinter-tar STATIC tar/tar.cpp)
    add_library(SemanticSplinter::tar ALIAS semantic-splinter-tar)
    target_link_libraries(semantic-splinter-tar PUBLIC semantic-splinter-container PRIVATE BZip2::BZip2 liblzma::liblzma)
    list(APPEND formats tar)
endif()

if(SEMSPLINTER_FORMAT_JSON)
    add_library(semantic-splinter-json STATIC json/json.cpp)
    add_library(SemanticSplinter::json ALIAS semantic-splinter-json)
    target_link_libraries(semantic-splinter-json PUBLIC SemanticSplinter::core)
    list(APPEND formats json)
endif()

if(SEMSPLINTER_FORMAT_XML)
    find_package(expat CONFIG REQUIRED)
    add_library(semantic-splinter-xml STATIC xml/xml.cpp)
    add_library(SemanticSplinter::xml ALIAS semantic-splinter-xml)
    target_link_libraries(semantic-splinter-xml PUBLIC SemanticSplinter::core PRIVATE expat::expat)
    list(APPEND formats xml)
endif()

if(SEMSPLINTER_FORMAT_CSV)
    add_library(semantic-splinter-csv STATIC csv/csv.cpp)
    add_library(SemanticSplinter::csv ALIAS semantic-splinter-csv)
    target_link_libraries(semantic-splinter-csv PUBLIC SemanticSplinter::core)
    list(APPEND formats csv)
endif()

if(SEMSPLINTER_FORMAT_JPEG)
    if(NOT SEMSPLINTER_FORMAT_PNG)
        message(FATAL_ERROR "SEMSPLINTER_FORMAT_JPEG needs SEMSPLINTER_FORMAT_PNG (its visuals are PNG files)")
    endif()
    find_package(JPEG REQUIRED)
    add_library(semantic-splinter-jpeg STATIC jpeg/jpeg.cpp jpeg/codec.cpp)
    add_library(SemanticSplinter::jpeg ALIAS semantic-splinter-jpeg)
    target_link_libraries(semantic-splinter-jpeg PUBLIC SemanticSplinter::raster PRIVATE SemanticSplinter::png JPEG::JPEG)
    list(APPEND formats jpeg)
endif()

if(SEMSPLINTER_FORMAT_SVG)
    if(NOT SEMSPLINTER_FORMAT_XML)
        message(FATAL_ERROR "SEMSPLINTER_FORMAT_SVG needs SEMSPLINTER_FORMAT_XML")
    endif()
    find_package(ZLIB REQUIRED)
    add_library(semantic-splinter-svg STATIC svg/svg.cpp)
    add_library(SemanticSplinter::svg ALIAS semantic-splinter-svg)
    target_link_libraries(semantic-splinter-svg PUBLIC SemanticSplinter::xml PRIVATE ZLIB::ZLIB)
    list(APPEND formats svg)
endif()

if(SEMSPLINTER_FORMAT_SQLITE)
    enable_language(C)
    find_package(unofficial-sqlite3 CONFIG REQUIRED)
    # Vendored cksumvfs.c (public domain, SQLite 3.53.4 ext/misc), built without our warning flags.
    add_library(semantic-splinter-cksumvfs OBJECT sqlite/cksum.c)
    set_property(TARGET semantic-splinter-cksumvfs PROPERTY COMPILE_OPTIONS "")
    target_link_libraries(semantic-splinter-cksumvfs PRIVATE unofficial::sqlite3::sqlite3)
    add_library(semantic-splinter-sqlite STATIC sqlite/sqlite.cpp $<TARGET_OBJECTS:semantic-splinter-cksumvfs>)
    add_library(SemanticSplinter::sqlite ALIAS semantic-splinter-sqlite)
    target_link_libraries(semantic-splinter-sqlite PUBLIC SemanticSplinter::core PRIVATE unofficial::sqlite3::sqlite3)
    list(APPEND formats sqlite)
endif()

if(SEMSPLINTER_FORMAT_MESH)
    # geomcore: geometry metrics helper (no format, no external dependencies; Q-MESH-3).
    add_library(semantic-splinter-geomcore STATIC geomcore/geomcore.cpp)
    target_link_libraries(semantic-splinter-geomcore PUBLIC SemanticSplinter::core)
    add_library(semantic-splinter-mesh STATIC mesh/mesh.cpp)
    add_library(SemanticSplinter::mesh ALIAS semantic-splinter-mesh)
    target_link_libraries(semantic-splinter-mesh PUBLIC SemanticSplinter::core PRIVATE semantic-splinter-geomcore)
    list(APPEND formats mesh)
endif()

if(SEMSPLINTER_FORMAT_PDF)
    if(NOT SEMSPLINTER_FORMAT_XML)
        message(FATAL_ERROR "SEMSPLINTER_FORMAT_PDF needs SEMSPLINTER_FORMAT_XML (XMP metadata)")
    endif()
    find_package(qpdf CONFIG REQUIRED)
    find_package(ZLIB REQUIRED)
    find_package(JPEG REQUIRED)
    # qpdf's exported target names zlib and libjpeg as bare library names (zs/zsd, jpeg) that the linker
    # cannot find on its own; point them at the found packages.
    set_property(TARGET qpdf::libqpdf PROPERTY INTERFACE_LINK_LIBRARIES ZLIB::ZLIB JPEG::JPEG)
    add_library(semantic-splinter-pdf STATIC pdf/pdf.cpp pdf/text.cpp)
    add_library(SemanticSplinter::pdf ALIAS semantic-splinter-pdf)
    target_link_libraries(semantic-splinter-pdf PUBLIC SemanticSplinter::core PRIVATE SemanticSplinter::xml qpdf::libqpdf)
    list(APPEND formats pdf)
endif()

# raster-misc (raster-misc.md): the shared TIFF/Exif IFD layer and a dependency-free PNG writer for visuals
# join the raster helper; one engine library serves the six format modules.
target_sources(semantic-splinter-raster PRIVATE raster/ifd.cpp raster/png_writer.cpp)
set(rastermisc_formats "")
foreach(f bmp tga qoi gif tiff webp)
    string(TOUPPER ${f} F)
    if(SEMSPLINTER_FORMAT_${F})
        list(APPEND rastermisc_formats ${f})
    endif()
endforeach()
if(rastermisc_formats)
    add_library(semantic-splinter-rastermisc STATIC rastermisc/common.cpp)
    target_link_libraries(semantic-splinter-rastermisc PUBLIC SemanticSplinter::raster)
endif()
foreach(f IN LISTS rastermisc_formats)
    add_library(semantic-splinter-${f} STATIC rastermisc/${f}.cpp)
    add_library(SemanticSplinter::${f} ALIAS semantic-splinter-${f})
    target_link_libraries(semantic-splinter-${f} PUBLIC semantic-splinter-rastermisc)
    list(APPEND formats ${f})
endforeach()
if(SEMSPLINTER_FORMAT_GIF)
    find_package(GIF REQUIRED)
    target_link_libraries(semantic-splinter-gif PRIVATE GIF::GIF)
endif()
if(SEMSPLINTER_FORMAT_TIFF)
    find_package(TIFF REQUIRED)
    find_package(ZLIB REQUIRED)
    target_link_libraries(semantic-splinter-tiff PRIVATE TIFF::TIFF ZLIB::ZLIB)
endif()
if(SEMSPLINTER_FORMAT_WEBP)
    find_package(WebP CONFIG REQUIRED)
    target_link_libraries(semantic-splinter-webp PRIVATE WebP::webp)
endif()

# Registry::with_builtin_formats() over every enabled module, in registration order (SPEC §2.3, §11).
set(includes "")
set(calls "")
foreach(f IN LISTS formats)
    string(APPEND includes "#include <semsplinter/formats/${f}.hpp>\n")
    string(APPEND calls "    r.add(${f}::make_format());\n")
endforeach()
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/formats_registry.cpp CONTENT [[
// Generated by src/formats/CMakeLists.txt.
#include <semsplinter/format.hpp>
@includes@
namespace semsplinter {

Registry Registry::with_builtin_formats() {
    Registry r;
@calls@    return r;
}

} // namespace semsplinter
]] @ONLY)

add_library(semantic-splinter-formats STATIC ${CMAKE_CURRENT_BINARY_DIR}/formats_registry.cpp)
add_library(SemanticSplinter::formats ALIAS semantic-splinter-formats)
list(TRANSFORM formats PREPEND SemanticSplinter:: OUTPUT_VARIABLE format_targets)
target_link_libraries(semantic-splinter-formats PUBLIC ${format_targets})
