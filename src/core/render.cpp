// GNU diff output formats. Written from the GNU diffutils manual and black-box comparison with GNU diff 3.12
// (no GNU source read, D-29).
#include <semsplinter/error.hpp>
#include <semsplinter/render.hpp>

#include <algorithm>

namespace semsplinter {

namespace {

constexpr std::string_view kNoNewline = "\\ No newline at end of file\n";

class Renderer {
public:
    Renderer(std::string& out, const DiffSide& a, const DiffSide& b, std::span<const Edit> edits,
             std::span<const char> ign, const RenderOptions& o)
        : out_(out), a_(a), b_(b), edits_(edits), ign_(ign), o_(o) {}

    void normal() {
        for (std::size_t k = 0; k < edits_.size(); ++k) {
            if (ignored(k))
                continue;
            const Edit& e = edits_[k];
            const char op = e.a0 == e.a1 ? 'a' : e.b0 == e.b1 ? 'd' : 'c';
            std::string head = range_normal(e.a0, e.a1) + op + range_normal(e.b0, e.b1);
            colored(o_.color ? &o_.color->ln : nullptr, head);
            for (auto i = e.a0; i < e.a1; ++i)
                line("< ", a_, i, de());
            if (op == 'c')
                out_ += "---\n";
            for (auto j = e.b0; j < e.b1; ++j)
                line("> ", b_, j, ad());
        }
    }

    void unified() {
        header("--- ", a_.label);
        header("+++ ", b_.label);
        for_each_hunk([&](std::size_t first, std::size_t last, std::size_t as, std::size_t ae, std::size_t bs,
                          std::size_t be) {
            std::string head = "@@ -" + range_unified(as, ae) + " +" + range_unified(bs, be) + " @@";
            colored(o_.color ? &o_.color->ln : nullptr, head + suffix(first, last, as));
            std::size_t i = as;
            for (std::size_t k = first; k <= last; ++k) {
                const Edit& e = edits_[k];
                for (; i < e.a0; ++i)
                    line(" ", a_, i, nullptr);
                for (auto x = e.a0; x < e.a1; ++x)
                    line("-", a_, x, de());
                for (auto y = e.b0; y < e.b1; ++y)
                    line("+", b_, y, ad());
                i = e.a1;
            }
            for (; i < ae; ++i)
                line(" ", a_, i, nullptr);
        });
    }

    void context() {
        header("*** ", a_.label);
        header("--- ", b_.label);
        for_each_hunk([&](std::size_t first, std::size_t last, std::size_t as, std::size_t ae, std::size_t bs,
                          std::size_t be) {
            out_ += "***************" + suffix(first, last, as) + "\n";
            bool del = false, ins = false;
            for (std::size_t k = first; k <= last; ++k)
                del |= edits_[k].a0 < edits_[k].a1, ins |= edits_[k].b0 < edits_[k].b1;
            colored(o_.color ? &o_.color->ln : nullptr, "*** " + range_context(as, ae) + " ****");
            if (del)
                side_block(first, last, as, ae, true);
            colored(o_.color ? &o_.color->ln : nullptr, "--- " + range_context(bs, be) + " ----");
            if (ins)
                side_block(first, last, bs, be, false);
        });
    }

    void ed(bool forward) {
        auto one = [&](std::size_t k) {
            if (ignored(k))
                return;
            const Edit& e = edits_[k];
            const char op = e.a0 == e.a1 ? 'a' : e.b0 == e.b1 ? 'd' : 'c';
            std::string range = op == 'a' ? std::to_string(e.a0)
                                : e.a1 - e.a0 == 1
                                    ? std::to_string(e.a0 + 1)
                                    : std::to_string(e.a0 + 1) + (forward ? " " : ",") + std::to_string(e.a1);
            out_ += forward ? op + range : range + op;
            out_ += '\n';
            if (op == 'd')
                return;
            for (auto j = e.b0; j < e.b1; ++j) {
                auto s = b_.lines[j];
                if (!forward && s == ".") {
                    // ed ends text at a lone "."; write "..", end the text, strip the extra dot, keep
                    // appending.
                    out_ += "..\n.\ns/.//\n";
                    if (j + 1 < e.b1)
                        out_ += "a\n";
                    continue;
                }
                out_.append(s);
                out_ += '\n';
                if (j + 1 == e.b1)
                    out_ += ".\n";
            }
        };
        if (forward)
            for (std::size_t k = 0; k < edits_.size(); ++k)
                one(k);
        else
            for (std::size_t k = edits_.size(); k-- > 0;)
                one(k);
    }

    void rcs() {
        for (std::size_t k = 0; k < edits_.size(); ++k) {
            if (ignored(k))
                continue;
            const Edit& e = edits_[k];
            if (e.a1 > e.a0)
                out_ += "d" + std::to_string(e.a0 + 1) + " " + std::to_string(e.a1 - e.a0) + "\n";
            if (e.b1 > e.b0) {
                out_ += "a" + std::to_string(e.a1) + " " + std::to_string(e.b1 - e.b0) + "\n";
                for (auto j = e.b0; j < e.b1; ++j) {
                    out_.append(b_.lines[j]);
                    if (!(j + 1 == b_.lines.size() && b_.missing_newline))
                        out_ += '\n';
                }
            }
        }
    }

    void side_by_side() {
        const std::size_t w = o_.width, t = o_.tabsize ? o_.tabsize : 8;
        off_ = o_.expand_tabs ? w - (w >= 3 ? (w - 3) / 2 : 0) : (w + t + 1) / (2 * t) * t;
        hw_ = std::min(off_ >= 3 ? off_ - 3 : 0, w > off_ ? w - off_ : 0);
        bar_ = (hw_ + off_ - 1) / 2;
        auto partial = [](const DiffSide& s, std::size_t i) {
            return i + 1 == s.lines.size() && s.missing_newline;
        };
        // One row. A row whose shown lines all lack a terminator ends without a newline, as in GNU.
        auto row = [&](const std::size_t* i, char mark, const std::size_t* j) {
            std::size_t col = i ? column(a_.lines[*i]) : 0;
            if (mark != ' ') {
                pad(col, bar_);
                out_ += mark;
                col = bar_ + 1;
            }
            if (j && !b_.lines[*j].empty()) {
                pad(col, off_);
                column(b_.lines[*j], off_);
            }
            if (!((!i || partial(a_, *i)) && (!j || partial(b_, *j))))
                out_ += '\n';
        };
        std::size_t i = 0, j = 0;
        auto common = [&](std::size_t to) {
            for (; i < to; ++i, ++j)
                if (!o_.suppress_common_lines)
                    o_.left_column ? row(&i, '(', nullptr) : row(&i, ' ', &j);
        };
        for (const Edit& e : edits_) {
            common(e.a0);
            std::size_t d = e.a1 - e.a0, n = e.b1 - e.b0, pairs = std::min(d, n);
            for (std::size_t k = 0; k < pairs; ++k) {
                std::size_t x = e.a0 + k, y = e.b0 + k;
                bool px = partial(a_, x), py = partial(b_, y);
                row(&x, px && !py ? '\\' : py && !px ? '/' : '|', &y);
            }
            for (std::size_t k = pairs; k < d; ++k) {
                std::size_t x = e.a0 + k;
                row(&x, '<', nullptr);
            }
            for (std::size_t k = pairs; k < n; ++k) {
                std::size_t y = e.b0 + k;
                row(nullptr, '>', &y);
            }
            i = e.a1, j = e.b1;
        }
        common(a_.lines.size());
    }

private:
    bool ignored(std::size_t k) const { return !ign_.empty() && ign_[k]; }
    const std::string* de() const { return o_.color ? &o_.color->de : nullptr; }
    const std::string* ad() const { return o_.color ? &o_.color->ad : nullptr; }

    void colored(const std::string* sgr, std::string_view text) {
        if (sgr && !sgr->empty())
            out_ += "\033[" + *sgr + "m";
        out_.append(text);
        if (sgr && !sgr->empty())
            out_ += "\033[" + o_.color->rs + "m";
        out_ += '\n';
    }

    void header(std::string_view tag, const std::string& label) {
        colored(o_.color ? &o_.color->hd : nullptr, std::string(tag) + label);
    }

    // One output line: prefix (with -T and --suppress-blank-empty applied), content (with -t), newline,
    // and GNU's marker when it is the side's last line and lacks a terminator.
    void line(std::string_view prefix, const DiffSide& s, std::size_t i, const std::string* sgr) {
        std::string p(prefix);
        if (o_.initial_tab) {
            if (!p.empty() && p.back() == ' ')
                p.back() = '\t';
            else
                p += '\t';
        }
        auto text = s.lines[i];
        if (o_.suppress_blank_empty && text.empty())
            while (!p.empty() && (p.back() == ' ' || p.back() == '\t'))
                p.pop_back();
        const bool on = sgr && !sgr->empty();
        if (on)
            out_ += "\033[" + *sgr + "m";
        out_ += p;
        if (o_.expand_tabs) {
            std::size_t col = 0;
            bool restart = false;
            for (char c : text) {
                if (restart)
                    out_ += p;
                restart = false;
                if (c == '\t') {
                    std::size_t n = o_.tabsize - col % o_.tabsize;
                    out_.append(n, ' ');
                    col += n;
                } else if (c == '\r') { // GNU restarts the line: CR, then the prefix before more text
                    out_ += c;
                    restart = true;
                    col = 0;
                } else {
                    out_ += c;
                    ++col;
                }
            }
        } else {
            out_.append(text);
        }
        if (on)
            out_ += "\033[" + o_.color->rs + "m";
        out_ += '\n';
        if (i + 1 == s.lines.size() && s.missing_newline)
            out_ += kNoNewline;
    }

    static std::string range_normal(std::size_t s, std::size_t e) {
        if (s == e)
            return std::to_string(s);
        if (e - s == 1)
            return std::to_string(s + 1);
        return std::to_string(s + 1) + "," + std::to_string(e);
    }
    static std::string range_unified(std::size_t s, std::size_t e) {
        if (e - s == 1)
            return std::to_string(s + 1);
        return std::to_string(s == e ? s : s + 1) + "," + std::to_string(e - s);
    }
    static std::string range_context(std::size_t s, std::size_t e) {
        if (e - s <= 1)
            return std::to_string(e);
        return std::to_string(s + 1) + "," + std::to_string(e);
    }

    // Hunks: a following change joins when the gap is at most 2*context lines, or, for an ignorable change,
    // less than `context` lines. Hunks of ignorable changes only are dropped.
    template <class F> void for_each_hunk(F&& f) {
        const std::size_t c = o_.context, n = a_.lines.size();
        for (std::size_t k = 0; k < edits_.size();) {
            std::size_t first = k;
            bool real = !ignored(k);
            while (k + 1 < edits_.size()) {
                std::size_t gap = edits_[k + 1].a0 - edits_[k].a1;
                if (ignored(k + 1) ? gap >= c : gap > 2 * c)
                    break;
                real |= !ignored(++k);
            }
            std::size_t last = k++;
            if (!real)
                continue;
            const Edit &e0 = edits_[first], &e1 = edits_[last];
            std::size_t as = e0.a0 > c ? e0.a0 - c : 0, ae = std::min(n, e1.a1 + c);
            std::size_t bs = e0.b0 - (e0.a0 - as), be = e1.b1 + (ae - e1.a1);
            f(first, last, as, ae, bs, be);
        }
    }

    // " FUNCTION" for -p/-F: the last matching line before the hunk, cut to 40 bytes, trailing space trimmed.
    std::string suffix(std::size_t first, std::size_t last, std::size_t as) {
        if (o_.hunk_address)
            return " " + o_.hunk_address(edits_[first], edits_[last]);
        if (!o_.is_function)
            return {};
        for (std::size_t i = as; i-- > 0;) {
            if (!o_.is_function(a_.lines[i]))
                continue;
            std::string s(a_.lines[i].substr(0, 40));
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' ||
                                  s.back() == '\v' || s.back() == '\f'))
                s.pop_back();
            return " " + s;
        }
        return {};
    }

    void side_block(std::size_t first, std::size_t last, std::size_t s, std::size_t e, bool old) {
        const DiffSide& d = old ? a_ : b_;
        const std::string* sgr = old ? de() : ad();
        std::size_t i = s;
        for (std::size_t k = first; k <= last; ++k) {
            const Edit& x = edits_[k];
            std::size_t x0 = old ? x.a0 : x.b0, x1 = old ? x.a1 : x.b1;
            bool both = x.a0 < x.a1 && x.b0 < x.b1;
            for (; i < x0; ++i)
                line("  ", d, i, sgr);
            for (; i < x1; ++i)
                line(both ? "! " : old ? "- " : "+ ", d, i, sgr);
        }
        for (; i < e; ++i)
            line("  ", d, i, sgr);
    }

    // Side by side: prints `s` cut to the column width; returns the end column.
    // `start` is the column's absolute start: a CR sends the terminal to column 0, so GNU pads back to it.
    std::size_t column(std::string_view s, std::size_t start = 0) {
        const std::size_t t = o_.tabsize ? o_.tabsize : 8;
        std::size_t col = 0;
        for (char c : s) {
            auto u = static_cast<unsigned char>(c);
            if (c == '\r') {
                out_ += c;
                pad(0, start);
                col = 0;
            } else if (c == '\t') {
                std::size_t next = (col / t + 1) * t;
                // At the column edge GNU drops the tab, or expands it only up to the edge under -t.
                if (o_.expand_tabs)
                    out_.append(std::min(next, hw_) - col, ' ');
                if (next >= hw_)
                    break;
                if (!o_.expand_tabs)
                    out_ += '\t';
                col = next;
            } else if ((u & 0xC0) == 0x80 ||
                       u < 0x20) { // UTF-8 continuation and control bytes take no column
                out_ += c;
            } else {
                if (col + 1 > hw_)
                    break;
                out_ += c;
                ++col;
            }
        }
        return col;
    }

    void pad(std::size_t from, std::size_t to) {
        const std::size_t t = o_.tabsize ? o_.tabsize : 8;
        if (!o_.expand_tabs)
            for (std::size_t next = (from / t + 1) * t; next <= to; next = (from / t + 1) * t) {
                out_ += '\t';
                from = next;
            }
        if (to > from)
            out_.append(to - from, ' ');
    }

    std::string& out_;
    const DiffSide &a_, &b_;
    std::span<const Edit> edits_;
    std::span<const char> ign_;
    const RenderOptions& o_;
    std::size_t off_ = 0, hw_ = 0, bar_ = 0;
};

} // namespace

void render(std::string& out, OutputStyle style, const DiffSide& a, const DiffSide& b,
            std::span<const Edit> edits, std::span<const char> ignorable, const RenderOptions& o) {
    Renderer r(out, a, b, edits, ignorable, o);
    switch (style) {
    case OutputStyle::normal:
        return r.normal();
    case OutputStyle::unified:
        return r.unified();
    case OutputStyle::context:
        return r.context();
    case OutputStyle::side_by_side:
        return r.side_by_side();
    case OutputStyle::ed:
        return r.ed(false);
    case OutputStyle::forward_ed:
        return r.ed(true);
    case OutputStyle::rcs:
        return r.rcs();
    }
}

Palette parse_palette(std::string_view s, Palette p) {
    while (!s.empty()) {
        auto colon = s.find(':');
        auto item = s.substr(0, colon);
        s = colon == std::string_view::npos ? std::string_view{} : s.substr(colon + 1);
        auto eq = item.find('=');
        if (eq == std::string_view::npos)
            throw Error(ErrorCategory::invalid_input, "invalid palette entry '" + std::string(item) + "'");
        auto key = item.substr(0, eq);
        std::string val(item.substr(eq + 1));
        if (key == "rs")
            p.rs = val;
        else if (key == "hd")
            p.hd = val;
        else if (key == "ad")
            p.ad = val;
        else if (key == "de")
            p.de = val;
        else if (key == "ln")
            p.ln = val;
        else
            throw Error(ErrorCategory::invalid_input, "unknown palette key '" + std::string(key) + "'");
    }
    return p;
}

} // namespace semsplinter
