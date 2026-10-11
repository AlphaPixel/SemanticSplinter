#pragma once

// The simple-mesh module (docs/formats/mesh-simple.md): own readers for OBJ/MTL, PLY (ASCII, binary LE/BE),
// and STL (ASCII, binary); geometry metrics from the geomcore helper (bounding box, area, volume, Hausdorff,
// displacement regions); OBJ objects merged by name with face-index shifting; MTL merged per material and
// statement; PLY, STL, and shared-pool OBJ merged as one unit.

#include <semsplinter/format.hpp>

#include <memory>

namespace semsplinter::mesh {

std::unique_ptr<Format> make_format();

} // namespace semsplinter::mesh
