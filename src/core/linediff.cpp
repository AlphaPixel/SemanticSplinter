#include <semsplinter/hash.hpp>
#include <semsplinter/linediff.hpp>

#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace semsplinter {

namespace {
bool gnu_space(char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r';
}
} // namespace

std::string LineClassifier::normalize(std::string_view s) const {
    std::string out;
    out.reserve(s.size());
    if (norm_.ignore_all_space) {
        for (char c : s)
            if (!gnu_space(c))
                out += c;
    } else if (norm_.ignore_space_change) {
        bool pending = false;
        for (char c : s) {
            if (gnu_space(c)) {
                pending = true;
                continue;
            }
            if (pending)
                out += ' ';
            pending = false;
            out += c;
        }
    } else {
        if (norm_.ignore_tab_expansion) {
            std::size_t col = 0;
            for (char c : s) {
                if (c == '\t') {
                    std::size_t n = norm_.tabsize - col % norm_.tabsize;
                    out.append(n, ' ');
                    col += n;
                } else {
                    out += c;
                    col = c == '\b' ? (col ? col - 1 : 0) : col + 1;
                }
            }
        } else {
            out.assign(s);
        }
        if (norm_.ignore_trailing_space)
            while (!out.empty() && gnu_space(out.back()))
                out.pop_back();
    }
    if (norm_.ignore_case)
        for (char& c : out)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::uint32_t LineClassifier::classify(std::string_view line, bool newline) {
    const bool plain = !(norm_.ignore_case || norm_.ignore_tab_expansion || norm_.ignore_trailing_space ||
                         norm_.ignore_space_change || norm_.ignore_all_space);
    std::string_view key = line;
    if (!plain) {
        owned_.push_back(normalize(line));
        key = owned_.back();
    }
    // GNU treats a missing final newline as a difference, except under -Z, -b, and -w.
    const bool nl =
        newline || norm_.ignore_trailing_space || norm_.ignore_space_change || norm_.ignore_all_space;
    auto [it, fresh] = first_.try_emplace(hash128(key).lo ^ static_cast<std::uint64_t>(nl),
                                          static_cast<std::uint32_t>(keys_.size()));
    if (!fresh)
        for (std::uint32_t id = it->second; id != kNone; id = next_[id])
            if (keys_[id].second == nl && keys_[id].first == key) {
                if (!plain)
                    owned_.pop_back();
                return id;
            }
    auto id = static_cast<std::uint32_t>(keys_.size());
    keys_.emplace_back(key, nl);
    next_.push_back(fresh ? kNone : it->second); // chain the bucket
    it->second = id;
    return id;
}

namespace {

// Slides each change group of `f` (flags `cf`) as GNU describes: up to merge with earlier groups, then down,
// merging with later groups, repeated until the group stops growing. A group that ends up aligned with a
// change group of the other file (`co`) is moved there.
void compact(std::vector<char>& cf, std::span<const std::uint32_t> f, const std::vector<char>& co) {
    const std::size_t n = f.size();
    std::vector<std::size_t> uo; // unchanged lines of the other file, plus a sentinel
    for (std::size_t j = 0; j < co.size(); ++j)
        if (!co[j])
            uo.push_back(j);
    uo.push_back(co.size());
    // Does the other file have changed lines in the gap before its c-th unchanged line?
    auto aligned = [&](std::size_t c) { return c < uo.size() && uo[c] > (c ? uo[c - 1] + 1 : 0); };
    std::size_t i = 0, unchanged = 0;
    while (true) {
        while (i < n && !cf[i])
            ++i, ++unchanged;
        if (i == n)
            break;
        std::size_t start = i, end = i, c = unchanged;
        while (end < n && cf[end])
            ++end;
        std::size_t len;
        long match = -1; // group end at which it lines up with a change in the other file
        do {
            len = end - start;
            while (start > 0 && f[start - 1] == f[end - 1]) {
                cf[--start] = 1;
                cf[--end] = 0;
                --c;
                while (start > 0 && cf[start - 1])
                    --start;
            }
            match = aligned(c) ? static_cast<long>(end) : -1;
            while (end < n && f[start] == f[end]) {
                cf[start++] = 0;
                cf[end++] = 1;
                ++c;
                while (end < n && cf[end])
                    ++end;
                if (aligned(c))
                    match = static_cast<long>(end);
            }
        } while (len != end - start);
        if (match >= 0) {
            while (static_cast<long>(end) > match) {
                cf[--start] = 1;
                cf[--end] = 0;
                --c;
            }
        }
        i = end;
        unchanged = c;
    }
}

} // namespace

std::vector<Edit> edits_from_flags(const std::vector<char>& ca, const std::vector<char>& cb) {
    std::vector<Edit> out;
    std::size_t i = 0, j = 0;
    const std::size_t n = ca.size(), m = cb.size();
    while (i < n || j < m) {
        if (i < n && j < m && !ca[i] && !cb[j]) {
            ++i, ++j;
            continue;
        }
        Edit e{i, i, j, j};
        while (e.a1 < n && ca[e.a1])
            ++e.a1;
        while (e.b1 < m && cb[e.b1])
            ++e.b1;
        out.push_back(e);
        i = e.a1, j = e.b1;
    }
    return out;
}

std::vector<Edit> diff_lines(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b,
                             const LineDiffOptions& opt) {
    // GNU analyses only the region between the byte-identical head and tail of the two files, plus `horizon`
    // lines of each; discarding and compaction then see that region only, which shapes the result.
    auto ra = opt.raw_a.empty() ? a : opt.raw_a, rb = opt.raw_b.empty() ? b : opt.raw_b;
    std::size_t p = 0, s = 0;
    while (p < ra.size() && p < rb.size() && ra[p] == rb[p])
        ++p;
    while (s < ra.size() - p && s < rb.size() - p && ra[ra.size() - 1 - s] == rb[rb.size() - 1 - s])
        ++s;
    p -= std::min(p, opt.horizon), s -= std::min(s, opt.horizon);
    auto ra_ = a.subspan(p, a.size() - p - s), rb_ = b.subspan(p, b.size() - p - s);

    std::vector<char> ca(ra_.size(), 1), cb(rb_.size(), 1);
    // Reduced sequences: with `discard`, lines whose class never occurs on the other side are changes up
    // front.
    std::vector<std::size_t> ia, ib;
    const bool discard = opt.discard && opt.algorithm != DiffAlgorithm::minimal;
    std::vector<std::uint8_t> seen; // per class id (dense, from LineClassifier): bit 1 in a, bit 2 in b
    if (discard) {
        std::uint32_t top = 0;
        for (auto c : ra_)
            top = std::max(top, c);
        for (auto c : rb_)
            top = std::max(top, c);
        seen.resize(std::size_t{top} + 1);
        for (auto c : ra_)
            seen[c] |= 1;
        for (auto c : rb_)
            seen[c] |= 2;
    }
    for (std::size_t i = 0; i < ra_.size(); ++i)
        if (!discard || seen[ra_[i]] == 3)
            ia.push_back(i);
    for (std::size_t j = 0; j < rb_.size(); ++j)
        if (!discard || seen[rb_[j]] == 3)
            ib.push_back(j);
    std::vector<Token> ta(ia.size()), tb(ib.size());
    for (std::size_t k = 0; k < ia.size(); ++k)
        ta[k] = ra_[ia[k]];
    for (std::size_t k = 0; k < ib.size(); ++k)
        tb[k] = rb_[ib[k]];
    for (auto [x, y] : diff_match(ta, tb, opt.algorithm))
        ca[ia[x]] = cb[ib[y]] = 0;
    if (opt.compaction == Compaction::gnu) {
        compact(ca, ra_, cb);
        compact(cb, rb_, ca);
    }
    auto edits = edits_from_flags(ca, cb);
    for (auto& e : edits)
        e.a0 += p, e.a1 += p, e.b0 += p, e.b1 += p;
    return edits;
}

std::vector<Diff3Block> diff3_blocks(std::span<const Edit> om, std::span<const Edit> oy, std::size_t guard) {
    std::vector<Diff3Block> out;
    std::size_t i = 0, j = 0;
    long dm = 0, dy = 0;
    auto delta = [](const Edit& e) {
        return static_cast<long>(e.b1 - e.b0) - static_cast<long>(e.a1 - e.a0);
    };
    while (i < om.size() || j < oy.size()) {
        std::size_t lo = std::min(i < om.size() ? om[i].a0 : SIZE_MAX, j < oy.size() ? oy[j].a0 : SIZE_MAX);
        std::size_t hi = lo, i0 = i, j0 = j;
        long bm = 0, by = 0;
        bool guarded = false;
        for (bool grew = true; grew;) {
            grew = false;
            // Only the other side's edits join through the guard band; one side's edits are already apart.
            while (i < om.size() && (om[i].a0 <= hi || (j > j0 && om[i].a0 <= hi + guard))) {
                guarded |= om[i].a0 > hi;
                hi = std::max(hi, om[i].a1), bm += delta(om[i++]);
                grew = true;
            }
            while (j < oy.size() && (oy[j].a0 <= hi || (i > i0 && oy[j].a0 <= hi + guard))) {
                guarded |= oy[j].a0 > hi;
                hi = std::max(hi, oy[j].a1), by += delta(oy[j++]);
                grew = true;
            }
        }
        Diff3Block b{};
        b.o0 = lo, b.o1 = hi;
        b.m0 = static_cast<std::size_t>(static_cast<long>(lo) + dm);
        b.m1 = static_cast<std::size_t>(static_cast<long>(hi) + dm + bm);
        b.y0 = static_cast<std::size_t>(static_cast<long>(lo) + dy);
        b.y1 = static_cast<std::size_t>(static_cast<long>(hi) + dy + by);
        b.em0 = i0, b.em1 = i, b.ey0 = j0, b.ey1 = j;
        b.guard_joined = guarded;
        dm += bm, dy += by;
        out.push_back(b);
    }
    return out;
}

} // namespace semsplinter
