#pragma once

// Fixture helpers for the raster-misc format tests: synthetic images and a format fixture by name.

#include <semsplinter/format.hpp>
#include <semsplinter/formats/raster.hpp>

#include <algorithm>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace rtest {

using namespace semsplinter;
using raster::Cell;
using raster::Raster;

inline Cell rgba(int r, int g, int b, int a = 255) {
    return {static_cast<std::uint16_t>(r * 257), static_cast<std::uint16_t>(g * 257),
            static_cast<std::uint16_t>(b * 257), static_cast<std::uint16_t>(a * 257)};
}

inline Raster image(std::uint32_t w, std::uint32_t h,
                    const std::function<Cell(std::uint32_t, std::uint32_t)>& fn) {
    Raster r(w, h, false);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            r.set(std::uint64_t{y} * w + x, fn(x, y));
    return r;
}

inline Raster pattern(std::uint32_t w = 40, std::uint32_t h = 30) {
    return image(w, h, [](std::uint32_t x, std::uint32_t y) {
        return rgba((x * 6) & 0xFF, (y * 8) & 0xFF, ((x + y) * 3) & 0xFF);
    });
}

// Few colors, so palette encodings hold it.
inline Raster blocks(std::uint32_t w = 40, std::uint32_t h = 30) {
    return image(w, h, [](std::uint32_t x, std::uint32_t y) {
        static const Cell c[] = {rgba(0, 0, 0), rgba(255, 255, 255), rgba(200, 30, 30), rgba(30, 200, 30)};
        return c[(x / 5 + y / 5) % 4];
    });
}

inline void paint(Raster& r, std::uint32_t x0, std::uint32_t y0, std::uint32_t w, std::uint32_t h, Cell c) {
    for (auto y = y0; y < y0 + h; ++y)
        for (auto x = x0; x < x0 + w; ++x)
            r.set(std::uint64_t{y} * r.width + x, c);
}

struct Fx {
    Registry reg = Registry::with_builtin_formats();
    Limits limits;
    OptionValues opts;
    Context ctx{reg, limits, 0, opts};
    const Format& f;
    std::string ext;

    explicit Fx(std::string_view name) : f(*reg.find(name)), ext("x." + std::string(name)) {}

    std::unique_ptr<Document> load(const std::vector<std::byte>& b) {
        return f.load(ByteSource::from_memory(b, ext), {ctx});
    }
    ChangeSet diff(const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
        return f.diff(*load(a), *load(b), {ctx});
    }
    MergeResult merge(const std::vector<std::byte>& b, const std::vector<std::byte>& o,
                      const std::vector<std::byte>& t, ConflictOptions c = {}) {
        auto db = load(b), dob = load(o), dt = load(t);
        return f.merge(*db, *dob, *dt, MergeOptions{ctx, std::move(c), {}});
    }
    std::vector<std::byte> bytes(const MergeResult& r) {
        VectorSink s;
        f.save(*r.document, s, {ctx});
        return s.data;
    }
    std::string dump(const std::vector<std::byte>& b) {
        std::ostringstream os;
        f.dump(*load(b), os, {ctx});
        return os.str();
    }
    std::string dump(const Document& d) {
        std::ostringstream os;
        f.dump(d, os, {ctx});
        return os.str();
    }
};

inline std::size_t count_kind(const ChangeSet& cs, std::string_view kind) {
    return static_cast<std::size_t>(
        std::count_if(cs.begin(), cs.end(), [&](const Change& c) { return c.kind == kind; }));
}

inline bool only_encoding(const ChangeSet& cs) {
    return std::all_of(cs.begin(), cs.end(), [](const Change& c) { return c.kind == "encoding"; });
}

// The dump's pixel lines ("pixels ..." and "tile ..."), independent of the encoding header line.
inline std::string pixel_lines(const std::string& dump) {
    std::istringstream in(dump);
    std::string out;
    for (std::string line; std::getline(in, line);) {
        auto t = line.substr(line.find_first_not_of(' '));
        if (t.starts_with("pixels ") || t.starts_with("tile "))
            out += t + "\n";
    }
    return out;
}

inline const Change* change_at(const ChangeSet& cs, std::string_view path) {
    for (auto& c : cs)
        if (c.at.path == path)
            return &c;
    return nullptr;
}

} // namespace rtest
