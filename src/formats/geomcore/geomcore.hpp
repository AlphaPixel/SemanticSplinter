#pragma once

// geomcore (SPEC §2.3, mesh-simple.md §5, Q-MESH-3): geometry metrics shared by mesh formats. No format
// types here: a mesh is positions plus triangles.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace semsplinter::geom {

using Vec3 = std::array<double, 3>;
using Tri = std::array<std::uint32_t, 3>;

struct Mesh {
    std::vector<Vec3> v;
    std::vector<Tri> tri; // indices into v
};

struct Box {
    Vec3 lo{0, 0, 0}, hi{0, 0, 0};
    bool empty = true;
    void add(const Vec3&);
};

Box bbox(const Mesh&);
double area(const Mesh&);
double volume(const Mesh&); // signed (divergence theorem); meaningful for closed meshes

struct Edges {
    std::uint64_t edges = 0, boundary = 0, nonmanifold = 0; // undirected; 1 face; more than 2 faces
};
Edges edges(const Mesh&);

// Symmetric distance between the surfaces: Hausdorff (max) and RMS over every vertex plus surface samples of
// each side, measured to the other side's triangles (to its points when it has none). Exact sampling up to
// 200k triangles per side (vertices, edge midpoints, centroids); above that every vertex plus 16k
// deterministic area-weighted samples (Q-MESH-2). nullopt when one side is empty and the other is not.
struct Distance {
    double hausdorff = 0, rms = 0;
    bool sampled = false;
};
std::optional<Distance> distance(const Mesh& a, const Mesh& b);

// Per-vertex displacement when both meshes have the same vertex count and the same triangles. Moved
// vertices (above `tol`) are grouped into regions: connected components over the triangle edges.
struct DisplacementRegion {
    Box box; // of the moved vertices, at their new positions
    double max = 0;
    std::size_t vertices = 0;
};
struct Displacement {
    double max = 0, mean = 0;
    std::size_t moved = 0;
    std::vector<DisplacementRegion> regions; // in order of their first vertex
};
std::optional<Displacement> displacement(const Mesh& a, const Mesh& b, double tol);

} // namespace semsplinter::geom
