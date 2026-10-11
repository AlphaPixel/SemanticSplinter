#pragma once

// SVG and SVGZ (docs/formats/svg.md): a dialect of the xml module (ids and references, styles, classes, path
// data, editor state, conflict layers) plus the gzip layer of .svgz.

#include <semsplinter/format.hpp>

#include <memory>

namespace semsplinter::svg {

std::unique_ptr<Format> make_format();

} // namespace semsplinter::svg
