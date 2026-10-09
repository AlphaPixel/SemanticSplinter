#pragma once

// Line diff on equivalence classes (text.md §3, §5.1): normalization, class assignment, edit scripts with
// GNU-style discarding and boundary compaction. Used by the text module and by dump-line rendering.

#include <semsplinter/sequence.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace semsplinter {

// Half-open: lines [a0, a1) of A are replaced by lines [b0, b1) of B.
struct Edit {
    std::size_t a0 = 0, a1 = 0, b0 = 0, b1 = 0;
    bool operator==(const Edit&) const = default;
};

// Comparison-only normalization (text.md §4). Output always prints original bytes.
struct LineNormalize {
    bool ignore_case = false;           // -i (ASCII fold)
    bool ignore_tab_expansion = false;  // -E
    bool ignore_trailing_space = false; // -Z
    bool ignore_space_change = false;   // -b
    bool ignore_all_space = false;      // -w
    std::size_t tabsize = 8;
};

// Assigns class IDs in order of first appearance. Equal IDs mean equal normalized content (byte-compared,
// so hash collisions never merge different lines). Lines are kept as views: they must outlive the classifier.
class LineClassifier {
public:
    explicit LineClassifier(LineNormalize n = {}) : norm_(n) {}

    // `newline` is whether the line had a terminator. GNU treats a missing final newline as a difference,
    // except under -Z, -b, and -w, where it counts as trailing white space.
    std::uint32_t classify(std::string_view line, bool newline);
    std::string normalize(std::string_view line) const;
    void reserve(std::size_t lines) {
        first_.reserve(lines);
        keys_.reserve(lines);
        next_.reserve(lines);
    }

private:
    LineNormalize norm_;
    static constexpr std::uint32_t kNone = UINT32_MAX;
    std::unordered_map<std::uint64_t, std::uint32_t> first_; // hash -> newest class in the bucket
    std::vector<std::uint32_t> next_;                        // bucket chains
    std::vector<std::pair<std::string_view, bool>> keys_;    // content, terminated
    std::deque<std::string> owned_;                          // normalized keys
};

enum class Compaction : std::uint8_t {
    none,
    gnu, // slide each change group to merge with neighbours, else as far down as possible
};

struct LineDiffOptions {
    DiffAlgorithm algorithm = DiffAlgorithm::myers;
    bool discard = true; // GNU: lines absent from the other side are changes; diff only the rest
    Compaction compaction = Compaction::gnu;
    // GNU analyses only what lies between the identical head and tail of the files, keeping `horizon` lines
    // of each (max of --horizon-lines and the output context). SIZE_MAX analyses everything.
    std::size_t horizon = SIZE_MAX;
    // Byte-identity classes used to find that head and tail when `a`/`b` are normalized (-i, -w, ...).
    std::span<const std::uint32_t> raw_a, raw_b;
};

std::vector<Edit> diff_lines(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b,
                             const LineDiffOptions& = {});

// Edits from per-line change flags (true = changed).
std::vector<Edit> edits_from_flags(const std::vector<char>& ca, const std::vector<char>& cb);

// diff3 chunking over two edit scripts from one base (base->ours, base->theirs): edits whose base ranges
// overlap or touch form one block (GNU diff3 and Git agree); with `guard` > 0, edits up to `guard` elements
// apart join too. Ranges are half-open in base (o), ours (m), and theirs (y) coordinates.
struct Diff3Block {
    std::size_t o0, o1, m0, m1, y0, y1;
    std::size_t em0, em1, ey0, ey1; // edit index ranges in each script
    bool guard_joined = false;      // joined only because of the guard band
    bool ours() const noexcept { return em1 > em0; }
    bool theirs() const noexcept { return ey1 > ey0; }
};
std::vector<Diff3Block> diff3_blocks(std::span<const Edit> om, std::span<const Edit> oy,
                                     std::size_t guard = 0);

} // namespace semsplinter
