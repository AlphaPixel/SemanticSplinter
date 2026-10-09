// diff3 merges (text.md §6). GNU diff3 behaviour is reproduced from its manual and black-box comparison with
// GNU diffutils 3.12; Git's merge-file behaviour from its documentation and black-box comparison (D-29).
#include <semsplinter/error.hpp>
#include <semsplinter/formats/text.hpp>

#include <algorithm>
#include <array>
#include <cctype>

namespace semsplinter::text {

namespace {

enum class Kind : std::uint8_t { ours_only, theirs_only, same, conflict };

struct Block {
    Kind kind;
    std::size_t o0, o1, m0, m1, y0, y1;
    std::size_t em0, em1, ey0, ey1; // edit indices (base->ours, base->theirs) in this block
};

struct Input {
    std::array<const TextDoc*, 3> doc; // base, ours, theirs
    std::array<std::vector<std::uint32_t>, 3> cls;
    std::vector<Edit> om, oy;
};

Input prepare(const TextDoc& base, const TextDoc& ours, const TextDoc& theirs, const LineDiffOptions& algo,
              bool ignore_eol) {
    Input in{{&base, &ours, &theirs}, {}, {}, {}};
    LineClassifier cls;
    for (int s = 0; s < 3; ++s) {
        auto& d = *in.doc[s];
        in.cls[s].resize(d.lines.size());
        for (std::size_t i = 0; i < d.lines.size(); ++i)
            in.cls[s][i] = cls.classify(d.line(i), ignore_eol || d.lines[i].eol != Eol::none);
    }
    in.om = diff_lines(in.cls[0], in.cls[1], algo);
    in.oy = diff_lines(in.cls[0], in.cls[2], algo);
    return in;
}

std::vector<Block> blocks(const Input& in) {
    std::vector<Block> out;
    for (auto& d : diff3_blocks(in.om, in.oy)) {
        Block b{Kind::conflict, d.o0, d.o1, d.m0, d.m1, d.y0, d.y1, d.em0, d.em1, d.ey0, d.ey1};
        if (!d.ours())
            b.kind = Kind::theirs_only;
        else if (!d.theirs())
            b.kind = Kind::ours_only;
        else if (b.m1 - b.m0 == b.y1 - b.y0 && std::equal(in.cls[1].begin() + static_cast<long>(b.m0),
                                                          in.cls[1].begin() + static_cast<long>(b.m1),
                                                          in.cls[2].begin() + static_cast<long>(b.y0)))
            b.kind = Kind::same;
        out.push_back(b);
    }
    return out;
}

// Output plan: shared or one-sided runs of lines, and conflicts.
struct Seg {
    bool conflict = false;
    int side = 1;        // common run: 0 base, 1 ours, 2 theirs
    bool shared = false; // common run present identically in ours and theirs (zealous merging may span it)
    std::size_t s0 = 0, s1 = 0;
    std::size_t o0 = 0, o1 = 0, m0 = 0, m1 = 0, y0 = 0, y1 = 0;
    bool gnu_same = false;  // GNU -A: identical change bracketed against the base
    bool show_base = false; // diff3 / zdiff3 / GNU -A
    std::size_t conflict_index = 0;
};

bool has_alnum(const TextDoc& d, std::size_t s, std::size_t e) {
    for (auto i = s; i < e; ++i)
        for (char c : d.line(i))
            if (std::isalnum(static_cast<unsigned char>(c)))
                return true;
    return false;
}

Address base_at(std::size_t o0, std::size_t o1) {
    return {"/lines", Range{o0, o1 - o0}};
}

class Emitter {
public:
    Emitter(const Input& in, const MergeOptions& o, std::optional<Eol> eol_target)
        : in_(in), o_(o), eol_(eol_target) {}

    void line(int side, std::size_t i) {
        settle();
        const TextDoc& d = *in_.doc[static_cast<std::size_t>(side)];
        const Line& l = d.lines[i];
        Eol eol = l.eol;
        if (eol_ && eol != Eol::none)
            eol = *eol_;
        if (!eol_ && d.decoded.empty()) {
            auto b = d.src.bytes();
            auto term = l.eol == Eol::crlf ? 2 : l.eol == Eol::none ? 0 : 1;
            out_.append(reinterpret_cast<const char*>(b.data()) + l.off,
                        l.len + static_cast<std::size_t>(term));
        } else {
            out_.append(d.line(i));
            out_ += terminator(eol);
        }
        dangling_ = eol == Eol::none;
    }

    void marker(char c, const std::string& label) {
        settle();
        out_.append(o_.marker_size, c);
        if (!label.empty())
            out_ += ' ' + label;
        out_ += o_.crlf_markers ? "\r\n" : "\n";
    }

    // Final newline of the result under --eol=auto (the /final-newline unit).
    void finish(bool final_newline) {
        if (!eol_)
            return;
        if (!final_newline) {
            while (!out_.empty() && (out_.back() == '\n' || out_.back() == '\r'))
                out_.pop_back();
        } else if (dangling_) {
            out_ += terminator(*eol_);
        }
    }

    std::string& text() { return out_; }

private:
    static std::string_view terminator(Eol e) {
        return e == Eol::crlf ? "\r\n" : e == Eol::cr ? "\r" : e == Eol::lf ? "\n" : "";
    }
    void settle() {
        // Git keeps markers on their own line; GNU diff3 writes the bytes as they are.
        if (dangling_ && o_.engine != Engine::gnu)
            out_ += o_.crlf_markers ? "\r\n" : "\n";
        dangling_ = false;
    }

    const Input& in_;
    const MergeOptions& o_;
    std::optional<Eol> eol_;
    std::string out_;
    bool dangling_ = false;
};

// Whole-file EOL conversion by one side (>= 90% of its terminated lines changed style) is lifted out.
struct EolPlan {
    std::optional<Eol> target;
    int converted_by = 0; // 1 ours, 2 theirs, 3 both (same style)
    bool conflict = false;
};

EolPlan plan_eol(const Input& in) {
    auto style = [&](int s) -> std::optional<Eol> {
        auto& d = *in.doc[static_cast<std::size_t>(s)];
        std::array<std::size_t, 4> n{};
        std::size_t total = 0;
        for (auto& l : d.lines)
            if (l.eol != Eol::none)
                ++n[static_cast<std::size_t>(l.eol)], ++total;
        for (Eol e : {Eol::lf, Eol::crlf, Eol::cr})
            if (total && n[static_cast<std::size_t>(e)] * 10 >= total * 9)
                return e;
        return std::nullopt;
    };
    auto b = style(0), m = style(1), y = style(2);
    EolPlan p;
    bool cm = b && m && *m != *b, cy = b && y && *y != *b;
    if (cm && cy) {
        p.target = m, p.converted_by = *m == *y ? 3 : 1, p.conflict = *m != *y;
    } else if (cm) {
        p.target = m, p.converted_by = 1;
    } else if (cy) {
        p.target = y, p.converted_by = 2;
    }
    return p;
}

} // namespace

TextMerge merge(const TextDoc& base, const TextDoc& ours, const TextDoc& theirs, const MergeOptions& o) {
    const bool gnu = o.engine == Engine::gnu;
    Input in = prepare(base, ours, theirs, o.algorithm, o.eol_auto);
    auto bl = blocks(in);
    TextMerge tm;
    MergeResult& r = tm.result;

    // Changes per side, in base->side edit order.
    auto make = [&](Side side, const std::vector<Edit>& es) {
        ChangeSet cs;
        for (auto& e : es) {
            Op op = e.a0 == e.a1 ? Op::insert : e.b0 == e.b1 ? Op::erase : Op::update;
            Address at = op == Op::erase ? base_at(e.a0, e.a1) : Address{"/lines", Range{e.b0, e.b1 - e.b0}};
            auto c = make_change(side, op, std::move(at), "lines");
            c.metrics["lines_deleted"] = e.a1 - e.a0;
            c.metrics["lines_inserted"] = e.b1 - e.b0;
            cs.push_back(std::move(c));
        }
        return cs;
    };
    r.changes_ours = make(Side::ours, in.om);
    r.changes_theirs = make(Side::theirs, in.oy);

    // Plan segments.
    std::vector<Seg> segs;
    auto common = [&](int side, std::size_t s0, std::size_t s1, bool shared) {
        if (s1 > s0)
            segs.push_back({false, side, shared, s0, s1});
    };
    auto conflict_seg = [&](const Block& b, std::size_t m0, std::size_t m1, std::size_t y0, std::size_t y1,
                            bool show_base) {
        Seg s{true};
        s.o0 = b.o0, s.o1 = b.o1, s.m0 = m0, s.m1 = m1, s.y0 = y0, s.y1 = y1, s.show_base = show_base;
        segs.push_back(s);
    };
    std::size_t m = 0;
    for (std::size_t k = 0; k < bl.size(); ++k) {
        const Block& b = bl[k];
        common(1, m, b.m0, true);
        m = b.m1;
        auto ids = [&] {
            std::vector<std::string> v;
            for (auto e = b.em0; e < b.em1; ++e)
                v.push_back(r.changes_ours[e].id);
            for (auto e = b.ey0; e < b.ey1; ++e)
                v.push_back(r.changes_theirs[e].id);
            return v;
        };
        switch (b.kind) {
        case Kind::ours_only:
            common(1, b.m0, b.m1, false);
            for (auto e = b.em0; e < b.em1; ++e)
                r.auto_merged.push_back({r.changes_ours[e].id, "independent"});
            break;
        case Kind::theirs_only:
            if (gnu && o.gnu_mode == Diff3Mode::overlap_only)
                common(1, b.m0, b.m1, false);
            else
                common(2, b.y0, b.y1, false);
            for (auto e = b.ey0; e < b.ey1; ++e)
                r.auto_merged.push_back({r.changes_theirs[e].id, "independent"});
            break;
        case Kind::same:
            if (gnu && o.gnu_mode == Diff3Mode::show_all && o.conflict.resolve_all == ResolveAll::none &&
                o.conflict.resolutions.empty() && o.conflict.policy == Policy::mark) {
                Seg s{true};
                s.gnu_same = true, s.o0 = b.o0, s.o1 = b.o1, s.m0 = b.m0, s.m1 = b.m1;
                segs.push_back(s);
            } else {
                common(1, b.m0, b.m1, true);
            }
            for (auto id : ids())
                r.auto_merged.push_back({id, "identical"});
            break;
        case Kind::conflict: {
            if (gnu && (o.gnu_mode == Diff3Mode::ed || o.gnu_mode == Diff3Mode::overlap_only)) {
                common(2, b.y0, b.y1, false);
                break;
            }
            if (gnu && o.gnu_mode == Diff3Mode::easy_only) {
                common(1, b.m0, b.m1, false);
                break;
            }
            const bool diff3_style = gnu ? o.gnu_mode == Diff3Mode::show_all : o.style != MarkerStyle::merge;
            if (!gnu && o.style != MarkerStyle::diff3) {
                // Zealous refinement: diff ours against theirs inside the conflict. zdiff3 trims only the
                // common head and tail so that the base section stays whole.
                std::span<const std::uint32_t> cm(in.cls[1].data() + b.m0, b.m1 - b.m0),
                    cy(in.cls[2].data() + b.y0, b.y1 - b.y0);
                std::vector<Edit> sub;
                if (o.style == MarkerStyle::zdiff3) {
                    std::size_t p = 0, s = 0;
                    while (p < cm.size() && p < cy.size() && cm[p] == cy[p])
                        ++p;
                    while (s < cm.size() - p && s < cy.size() - p &&
                           cm[cm.size() - 1 - s] == cy[cy.size() - 1 - s])
                        ++s;
                    sub.push_back({p, cm.size() - s, p, cy.size() - s});
                } else {
                    sub = diff_lines(cm, cy, {DiffAlgorithm::myers, false, Compaction::gnu});
                }
                std::size_t at = 0;
                for (auto& e : sub) {
                    common(1, b.m0 + at, b.m0 + e.a0, true);
                    conflict_seg(b, b.m0 + e.a0, b.m0 + e.a1, b.y0 + e.b0, b.y0 + e.b1, diff3_style);
                    at = e.a1;
                }
                common(1, b.m0 + at, b.m1, true);
            } else {
                conflict_seg(b, b.m0, b.m1, b.y0, b.y1, diff3_style);
            }
            break;
        }
        }
    }
    common(1, m, ours.lines.size(), true);

    // Git's zealous-alnum simplification: conflicts separated only by a shared run of fewer than four lines,
    // or of lines without alphanumerics, become one conflict.
    if (!gnu && o.style == MarkerStyle::merge) {
        std::vector<Seg> merged;
        for (std::size_t k = 0; k < segs.size(); ++k) {
            const Seg& s = segs[k];
            if (s.conflict && merged.size() >= 2 && !merged.back().conflict && merged.back().shared &&
                merged[merged.size() - 2].conflict) {
                const Seg& gap = merged.back();
                if (gap.s1 - gap.s0 < 4 || !has_alnum(ours, gap.s0, gap.s1)) {
                    Seg c = merged[merged.size() - 2];
                    c.o0 = std::min(c.o0, s.o0), c.o1 = std::max(c.o1, s.o1);
                    c.m1 = s.m1, c.y1 = s.y1;
                    merged.pop_back();
                    merged.back() = c;
                    continue;
                }
            }
            merged.push_back(s);
        }
        segs = std::move(merged);
    }

    // Conflicts, then the policy (mark by default for text).
    std::map<std::string, std::size_t> seen;
    for (auto& s : segs) {
        if (!s.conflict || s.gnu_same)
            continue;
        std::vector<std::string> ids;
        // Change ids of the edits that produced this conflict.
        for (std::size_t e = 0; e < in.om.size(); ++e)
            if (in.om[e].a0 <= s.o1 && in.om[e].a1 >= s.o0)
                ids.push_back(r.changes_ours[e].id);
        for (std::size_t e = 0; e < in.oy.size(); ++e)
            if (in.oy[e].a0 <= s.o1 && in.oy[e].a1 >= s.o0)
                ids.push_back(r.changes_theirs[e].id);
        Conflict c;
        c.at = base_at(s.o0, s.o1);
        auto key_ids = ids;
        auto key = c.at.to_string();
        if (std::size_t n = seen[key]++)
            key_ids.push_back("part-" + std::to_string(n)); // refined parts of one block
        c.id = make_conflict_id(c.at, key_ids);
        c.changes = std::move(ids);
        c.reason = s.o0 == s.o1 ? "add-add" : "both-modified";
        c.choices = {"ours", "theirs", "base", "mark", "union"};
        s.conflict_index = r.conflicts.size();
        r.conflicts.push_back(std::move(c));
    }
    apply_conflict_policy(r.conflicts, o.conflict, r.warnings);

    // EOL and final newline (--eol=auto).
    EolPlan ep;
    if (o.eol_auto) {
        ep = plan_eol(in);
        if (ep.converted_by) {
            Change c =
                make_change(ep.converted_by == 2 ? Side::theirs : Side::ours, Op::update, {"/eol"}, "eol");
            c.after = Value{std::string(to_string(*ep.target)), std::nullopt};
            r.auto_merged.push_back({c.id, ep.conflict ? "eol-conflict-ours" : "eol-convert"});
            (ep.converted_by == 2 ? r.changes_theirs : r.changes_ours).push_back(std::move(c));
            if (ep.conflict)
                r.warnings.push_back(
                    {"eol-conflict", "/eol", "both sides converted line endings; kept ours"});
        }
    }

    Emitter em(in, o, ep.target);
    auto lines = [&](int side, std::size_t s0, std::size_t s1) {
        for (auto i = s0; i < s1; ++i)
            em.line(side, i);
    };
    for (auto& s : segs) {
        if (!s.conflict) {
            lines(s.side, s.s0, s.s1);
            continue;
        }
        if (s.gnu_same) { // GNU -A: "<<<<<<< older / base / ======= / mine / >>>>>>> yours"
            em.marker('<', o.labels[1]);
            lines(0, s.o0, s.o1);
            em.marker('=', {});
            lines(1, s.m0, s.m1);
            em.marker('>', o.labels[2]);
            ++tm.marked;
            continue;
        }
        const auto& how = r.conflicts[s.conflict_index].resolved_as;
        if (how == "ours") {
            lines(1, s.m0, s.m1);
        } else if (how == "theirs") {
            lines(2, s.y0, s.y1);
        } else if (how == "base") {
            lines(0, s.o0, s.o1);
        } else if (how == "union") {
            lines(1, s.m0, s.m1);
            lines(2, s.y0, s.y1);
        } else {
            em.marker('<', o.labels[0]);
            lines(1, s.m0, s.m1);
            if (s.show_base) {
                em.marker('|', o.labels[1]);
                lines(0, s.o0, s.o1);
            }
            em.marker('=', {});
            lines(2, s.y0, s.y1);
            em.marker('>', o.labels[2]);
            ++tm.marked;
        }
    }
    if (o.eol_auto) {
        bool fb = !base.missing_newline(), fm = !ours.missing_newline(), fy = !theirs.missing_newline();
        em.finish(fm != fb ? fm : fy);
    }

    // Encoding: ours' encoding and BOM unless only theirs changed them (KeyedMerge on /encoding).
    std::string& out = em.text();
    const TextDoc& enc_src =
        ours.enc.encoding == base.enc.encoding && ours.enc.bom == base.enc.bom ? theirs : ours;
    if (base.enc.encoding != ours.enc.encoding || base.enc.encoding != theirs.enc.encoding ||
        ours.enc.encoding != theirs.enc.encoding)
        r.warnings.push_back({"encoding-converted", "/encoding",
                              "inputs use different encodings; merged as UTF-8 and written as " +
                                  std::string(to_string(enc_src.enc.encoding))});
    const bool all_raw = base.decoded.empty() && ours.decoded.empty() && theirs.decoded.empty();
    std::string encoded = all_raw ? std::move(out) : encode_utf8_as(out, enc_src.enc.encoding);
    auto bom = enc_src.src.bytes().first(enc_src.enc.bom);
    tm.bytes.reserve(bom.size() + encoded.size());
    tm.bytes.assign(bom.begin(), bom.end());
    auto eb = as_bytes(encoded);
    tm.bytes.insert(tm.bytes.end(), eb.begin(), eb.end());
    return tm;
}

// ---- GNU diff3 report and ed scripts ----

namespace {

std::string range(std::size_t s, std::size_t e) {
    if (s == e)
        return std::to_string(s);
    if (e - s == 1)
        return std::to_string(s + 1);
    return std::to_string(s + 1) + "," + std::to_string(e);
}

void text_lines(std::string& out, const TextDoc& d, std::size_t s, std::size_t e, std::string_view prefix) {
    for (auto i = s; i < e; ++i) {
        out += prefix;
        out.append(d.line(i));
        out += '\n';
        if (i + 1 == d.lines.size() && d.missing_newline())
            out += "\\ No newline at end of file\n";
    }
}

Input gnu_input(const TextDoc& base, const TextDoc& ours, const TextDoc& theirs) {
    return prepare(base, ours, theirs, {DiffAlgorithm::myers, true, Compaction::gnu, 0}, false);
}

} // namespace

std::string diff3_report(const TextDoc& base, const TextDoc& ours, const TextDoc& theirs,
                         const std::array<std::string, 3>&, bool initial_tab) {
    Input in = gnu_input(base, ours, theirs);
    std::string out;
    const std::string_view pre = initial_tab ? "\t" : "  ";
    for (const Block& b : blocks(in)) {
        // File numbers: 1 mine (ours), 2 older (base), 3 yours (theirs). `twin` pairs the two equal files.
        int odd = b.kind == Kind::ours_only     ? 1
                  : b.kind == Kind::theirs_only ? 3
                  : b.kind == Kind::same        ? 2
                                                : 0;
        out += odd ? "====" + std::to_string(odd) + "\n" : "====\n";
        const std::array<std::pair<std::size_t, std::size_t>, 3> rg{
            {{b.m0, b.m1}, {b.o0, b.o1}, {b.y0, b.y1}}};
        const std::array<const TextDoc*, 3> docs{&ours, &base, &theirs};
        auto head = [&](int f) {
            auto [s, e] = rg[static_cast<std::size_t>(f - 1)];
            out += std::to_string(f) + ":" + range(s, e) + (s == e ? "a\n" : "c\n");
        };
        bool done[4] = {};
        for (int f = 1; f <= 3; ++f) {
            if (done[f])
                continue;
            head(f);
            done[f] = true;
            if (odd && f != odd)
                for (int g = f + 1; g <= 3; ++g)
                    if (g != odd) {
                        head(g);
                        done[g] = true;
                    }
            auto [s, e] = rg[static_cast<std::size_t>(f - 1)];
            text_lines(out, *docs[static_cast<std::size_t>(f - 1)], s, e, pre);
        }
    }
    return out;
}

std::string diff3_ed(const TextDoc& base, const TextDoc& ours, const TextDoc& theirs, Diff3Mode mode,
                     const std::array<std::string, 3>& labels, bool write_quit) {
    Input in = gnu_input(base, ours, theirs);
    auto bl = blocks(in);
    std::string out;
    auto mk = [&](char c, const std::string& label) {
        out.append(7, c);
        if (!label.empty())
            out += ' ' + label;
        out += '\n';
    };
    auto change = [&](const Block& b) {
        if (b.m0 == b.m1)
            out += std::to_string(b.m0) + "a\n";
        else
            out += range(b.m0, b.m1) + (b.y0 == b.y1 ? "d\n" : "c\n");
        if (b.y0 != b.y1 || b.m0 == b.m1) {
            text_lines(out, theirs, b.y0, b.y1, "");
            out += ".\n";
        }
    };
    for (auto it = bl.rbegin(); it != bl.rend(); ++it) {
        const Block& b = *it;
        switch (b.kind) {
        case Kind::ours_only:
            break;
        case Kind::theirs_only:
            if (mode != Diff3Mode::overlap_only)
                change(b);
            break;
        case Kind::same:
            if (mode == Diff3Mode::show_all) {
                out += std::to_string(b.m1) + "a\n";
                mk('>', labels[2]);
                out += ".\n" + std::to_string(b.m0) + "a\n";
                mk('<', labels[1]);
                text_lines(out, base, b.o0, b.o1, "");
                mk('=', {});
                out += ".\n";
            }
            break;
        case Kind::conflict:
            if (mode == Diff3Mode::show_all || mode == Diff3Mode::show_overlap) {
                out += std::to_string(b.m1) + "a\n";
                if (mode == Diff3Mode::show_all) {
                    mk('|', labels[1]);
                    text_lines(out, base, b.o0, b.o1, "");
                }
                mk('=', {});
                text_lines(out, theirs, b.y0, b.y1, "");
                mk('>', labels[2]);
                out += ".\n" + std::to_string(b.m0) + "a\n";
                mk('<', labels[0]);
                out += ".\n";
            } else if (mode != Diff3Mode::easy_only) {
                change(b);
            }
            break;
        }
    }
    if (write_quit)
        out += "w\nq\n";
    return out;
}

} // namespace semsplinter::text
