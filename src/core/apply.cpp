// Patch application (SPEC §4, §6; CLI.md §5): GNU patch's file selection, messages, rejects, and backups.
#include <semsplinter/error.hpp>
#include <semsplinter/semsplinter.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace semsplinter {

namespace fs = std::filesystem;

namespace {

fs::path path_of(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8_of(const fs::path& p) {
    auto u = p.u8string();
    return {u.begin(), u.end()};
}

// GNU -pNUM: drops NUM leading components; nullopt when the name has fewer, or is /dev/null.
std::optional<std::string> strip_name(const std::string& name, std::optional<std::size_t> p) {
    if (name == "/dev/null" || name.empty())
        return std::nullopt;
    std::size_t at = 0;
    for (std::size_t k = 0; p && k < *p; ++k) {
        auto slash = name.find('/', at);
        if (slash == std::string::npos)
            return std::nullopt;
        at = slash + 1;
        while (at < name.size() && name[at] == '/')
            ++at;
    }
    return name.substr(at);
}

std::size_t components(const std::string& s) {
    return static_cast<std::size_t>(std::count(s.begin(), s.end(), '/')) + 1;
}

std::string basename_of(const std::string& s) {
    auto slash = s.rfind('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
}

bool related(const std::string& a, const std::string& b) {
    auto pixels = [](const std::string& p) { return p == "/shape" || path_within(p, "/pixels"); };
    return path_within(a, b) || path_within(b, a) || (pixels(a) && pixels(b));
}

bool regions_meet(const std::optional<Region>& a, const std::optional<Region>& b) {
    if (!a || !b)
        return true;
    auto* ra = std::get_if<Rect>(&*a);
    auto* rb = std::get_if<Rect>(&*b);
    if (!ra || !rb)
        return true;
    return ra->x < rb->x + rb->w && rb->x < ra->x + ra->w && ra->y < rb->y + rb->h && rb->y < ra->y + ra->h;
}

std::string hunks(std::size_t n) {
    return n == 1 ? " hunk" : " hunks";
}

// Report entries for the patch-mode report (SPEC §7.1, mode "patch").
Change unit_change(const UnitHunk& u) {
    Change c;
    c.id = u.id, c.op = u.op, c.at = u.at;
    c.before.text = u.before_text, c.after.text = u.after_text;
    if (u.before)
        c.before.hash = hash128(*u.before);
    if (u.after)
        c.after.hash = hash128(*u.after);
    return c;
}

Change hunk_change(const TextHunk& h) {
    const Op op = !h.old_len ? Op::insert : !h.new_len ? Op::erase : Op::update;
    // ponytail: the range the header names, not where an offset placed the hunk
    const Range r = op == Op::erase ? Range{h.old_start ? h.old_start - 1 : 0, h.old_len}
                                    : Range{h.new_start ? h.new_start - 1 : 0, h.new_len};
    return make_change(Side::theirs, op, {"/lines", r}, "lines");
}

class Applier {
public:
    Applier(const Registry& reg, const PatchRequest& rq) : reg_(reg), rq_(rq) {}

    PatchOutcome run() {
        auto sections = parse_patch(rq_.patch, rq_.limits.max_input);
        if (sections.empty())
            throw Error(ErrorCategory::invalid_input, "Only garbage was found in the patch input.");
        for (auto& s : sections) {
            if (rq_.reverse)
                s.reverse();
            section(s);
        }
        if (!rq_.dry_run) {
            if (rq_.output && *rq_.output != "-")
                atomic_write(resolve(*rq_.output), out_file_);
            for (auto& [path, text] : rejects_)
                atomic_write(resolve(path), as_bytes(text));
        }
        return std::move(out_);
    }

private:
    fs::path resolve(const std::string& name) const {
        auto p = path_of(name);
        return rq_.directory.empty() || p.is_absolute() ? p : rq_.directory / p;
    }

    bool exists(const std::string& name) const {
        std::error_code ec;
        return fs::is_regular_file(resolve(name), ec);
    }

    // GNU's choice among the existing names: fewest components, then shortest basename, then shortest.
    std::optional<std::string> target(const PatchSection& s) const {
        if (rq_.target)
            return rq_.target;
        std::vector<std::string> found;
        auto old_name = strip_name(s.old_name, rq_.strip), new_name = strip_name(s.new_name, rq_.strip);
        for (auto& n : {old_name, new_name})
            if (n && exists(*n))
                found.push_back(*n);
        if (!found.empty())
            return *std::min_element(found.begin(), found.end(),
                                     [](const std::string& a, const std::string& b) {
                                         return std::tuple(components(a), basename_of(a).size(), a.size()) <
                                                std::tuple(components(b), basename_of(b).size(), b.size());
                                     });
        if (s.old_name == "/dev/null")
            return new_name;
        return std::nullopt;
    }

    void summary(std::size_t failed, std::size_t total, bool ignored, const std::optional<std::string>& rej) {
        out_.messages += std::to_string(failed) + " out of " + std::to_string(total) + hunks(total) +
                         (ignored ? " ignored" : " FAILED");
        if (rej)
            out_.messages += " -- saving rejects to file " + *rej;
        out_.messages += "\n";
        out_.failed += failed;
        if (ignored)
            rep().warnings.push_back(
                {"hunks-ignored", "", std::to_string(total) + hunks(total) + " ignored"});
        else if (rep().result)
            rep().result->status = "conflicts";
    }

    Report& rep() { return out_.reports.back(); }

    void warn(const Warning& w) {
        rep().warnings.push_back(w);
        out_.warnings.push_back(w);
    }

    // Where rejects of `name` go: -r FILE, or OUTPUT.rej / NAME.rej; nullopt for -r - and --dry-run.
    std::optional<std::string> reject_path(const std::string& name) const {
        if (rq_.dry_run || (rq_.reject_file && *rq_.reject_file == "-"))
            return std::nullopt;
        if (rq_.reject_file)
            return rq_.reject_file;
        return (rq_.output && *rq_.output != "-" ? *rq_.output : name) + ".rej";
    }

    void reject(const std::string& name, const PatchSection& s, const std::vector<std::size_t>& failed) {
        auto rej = reject_path(name);
        if (!rej)
            return;
        // GNU names unified rejects after the target; envelopes keep their names and hashes.
        PatchSection r = s;
        if (s.format.empty())
            r.old_name = r.new_name = name;
        r.hunks.clear(), r.units.clear();
        for (auto k : failed) {
            if (s.format.empty())
                r.hunks.push_back(s.hunks[k]);
            else
                r.units.push_back(s.units[k]);
        }
        rejects_[*rej] += write_section(r);
    }

    void header(const std::string& name) {
        out_.messages += (rq_.dry_run ? "checking file " : "patching file ") +
                         (rq_.output ? *rq_.output + " (read from " + name + ")" : name) + "\n";
    }

    void backup(const fs::path& file, const std::string& name) {
        if (!backed_.insert(file).second)
            return;
        const auto& b = rq_.backup;
        fs::path dest;
        auto numbered = [&](bool only_if_present) -> std::optional<fs::path> {
            std::size_t top = 0;
            const std::string stem = utf8_of(file.filename()) + ".~";
            std::error_code ec;
            for (auto& e : fs::directory_iterator(
                     file.parent_path().empty() ? fs::path(".") : file.parent_path(), ec)) {
                auto n = utf8_of(e.path().filename());
                if (n.starts_with(stem) && n.ends_with("~") && n.size() > stem.size() + 1) {
                    auto digits = n.substr(stem.size(), n.size() - stem.size() - 1);
                    if (std::all_of(digits.begin(), digits.end(),
                                    [](char c) { return c >= '0' && c <= '9'; }) &&
                        digits.size() < 10)
                        top = std::max<std::size_t>(top, std::stoul(digits));
                }
            }
            if (only_if_present && !top)
                return std::nullopt;
            auto p = file;
            p += ".~" + std::to_string(top + 1) + "~";
            return p;
        };
        std::optional<fs::path> num;
        if (b.method == BackupOptions::Method::numbered)
            num = numbered(false);
        else if (b.method == BackupOptions::Method::existing)
            num = numbered(true);
        if (num) {
            dest = *num;
        } else if (!b.prefix.empty()) {
            dest = resolve(b.prefix + name);
        } else if (!b.basename_prefix.empty()) {
            dest = file.parent_path() / path_of(b.basename_prefix + utf8_of(file.filename()));
        } else {
            dest = file;
            dest += path_of(b.suffix);
        }
        std::error_code ec;
        if (dest.has_parent_path())
            fs::create_directories(dest.parent_path(), ec);
        fs::copy_file(file, dest, fs::copy_options::overwrite_existing, ec);
        if (ec)
            throw Error(ErrorCategory::io, "can't back up " + name + ": " + ec.message());
    }

    void commit(const std::string& name, std::span<const std::byte> bytes, bool existed, bool mismatch,
                bool remove) {
        rep().result = ResultInfo{rq_.output.value_or(name), hash128(bytes), "clean"};
        if (rq_.dry_run)
            return;
        if (rq_.output) {
            auto& sink = *rq_.output == "-" ? out_.output : out_file_;
            sink.insert(sink.end(), bytes.begin(), bytes.end());
            return;
        }
        const fs::path file = resolve(name);
        if (existed && (rq_.backup.always || (rq_.backup.if_mismatch && mismatch)))
            backup(file, name);
        std::error_code ec;
        if (remove) {
            fs::remove(file, ec);
            return;
        }
        if (file.has_parent_path())
            fs::create_directories(file.parent_path(), ec);
        atomic_write(file, bytes);
    }

    void section(PatchSection& s) {
        auto& r = out_.reports.emplace_back();
        r.mode = "patch";
        r.format = s.format.empty() ? "text" : s.format;
        const std::size_t total = s.hunk_count();
        auto name = target(s);
        if (!name) {
            out_.messages += "can't find file to patch at input line " + std::to_string(s.line + 2) + "\n";
            if (!rq_.strip)
                out_.messages += "Perhaps you should have used the -p or --strip option?\n";
            out_.messages += "The text leading up to this was:\n--------------------------\n";
            for (auto& l : s.preamble)
                out_.messages += "|" + l + "\n";
            out_.messages += "--------------------------\nNo file to patch.  Skipping patch.\n";
            summary(total, total, true, std::nullopt);
            return;
        }
        if (s.format.empty())
            text(s, *name);
        else
            envelope(s, *name);
    }

    void text(PatchSection& s, const std::string& name) {
        if (rq_.mark)
            throw Error(ErrorCategory::unsupported, "--merge is not implemented for unified diffs yet");
        const std::size_t total = s.hunk_count();
        const bool existed = exists(name);
        std::string content;
        if (existed) {
            auto src = ByteSource::from_file(resolve(name), rq_.limits);
            content.assign(reinterpret_cast<const char*>(src.bytes().data()), src.size());
            rep().inputs = {{"target", name, hash128(src.bytes()), src.size()}};
        }
        if (s.old_name == "/dev/null" && !content.empty() &&
            rq_.text.reversed != TextApplyOptions::Reversed::ignore) {
            out_.messages += "The next patch would create the file " + name +
                             ",\nwhich already exists!  Skipping patch.\n";
            summary(total, total, true, std::nullopt);
            return;
        }
        header(name);
        TextApplyOptions o = rq_.text;
        o.reverse_given |= rq_.reverse;
        auto r = apply_text(split_lines(content), s, o);
        out_.messages += r.messages;
        if (r.skipped) {
            reject(name, s, r.failed);
            summary(total, total, true, reject_path(name));
            return;
        }
        for (std::size_t k = 0; k < total; ++k) {
            auto c = hunk_change(s.hunks[k]);
            if (std::find(r.failed.begin(), r.failed.end(), k) == r.failed.end())
                rep().changes.push_back(std::move(c));
            else
                rep().conflicts.push_back(make_conflict(c.at, {c.id}, "hunk-failed"));
        }
        std::string result;
        for (auto& l : r.lines)
            result += l;
        commit(name, as_bytes(result), existed, r.mismatch,
               result.empty() && (rq_.remove_empty || s.new_name == "/dev/null"));
        if (!r.failed.empty()) {
            reject(name, s, r.failed);
            summary(r.failed.size(), total, false, reject_path(name));
        }
    }

    void envelope(PatchSection& s, const std::string& name) {
        const std::size_t total = s.hunk_count();
        const Format* f = reg_.find(s.format);
        if (!f)
            throw Error(ErrorCategory::unsupported, "patch: format '" + s.format + "' is not built in");
        if (!exists(name))
            throw Error(ErrorCategory::io, name + ": No such file");
        Limits budget = rq_.limits;
        Context ctx{reg_, budget, 0, rq_.format_options};
        const LoadOptions lo{ctx, {}};
        auto src = ByteSource::from_file(resolve(name), rq_.limits);
        const Hash128 h = hash128(src.bytes());
        header(name);
        rep().inputs = {{"target", name, h, src.size()}};
        if (s.exact && s.old_hash == h) { // --exact on its own base: the target's bytes, unchanged
            auto in = src.bytes();
            const auto& e = *s.exact;
            if (e.offset <= in.size() && e.removed <= in.size() - e.offset) {
                std::vector<std::byte> out(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(e.offset));
                out.insert(out.end(), e.bytes.begin(), e.bytes.end());
                out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(e.offset + e.removed),
                           in.end());
                if (hash128(out) == s.new_hash) {
                    for (auto& u : s.units)
                        rep().changes.push_back(unit_change(u));
                    commit(name, out, true, false, false);
                    return;
                }
            }
        }
        Limits target_budget = rq_.limits.fresh(); // per document (SPEC §9, #51)
        Context target_ctx{reg_, target_budget, 0, rq_.format_options};
        auto td = f->load(src, {target_ctx, {}});
        // Already applied (GNU's reversed-patch check, -N): every unit holds its after value.
        auto applied = [&] {
            return std::all_of(s.units.begin(), s.units.end(),
                               [&](const UnitHunk& u) { return f->payload(*td, u.at) == u.after; });
        };
        if (s.old_hash != h && !rq_.reverse && rq_.text.reversed != TextApplyOptions::Reversed::ignore &&
            applied()) {
            if (rq_.text.reversed == TextApplyOptions::Reversed::assume) {
                out_.messages += "Reversed (or previously applied) patch detected!  Assuming -R.\n";
                s.reverse();
            } else {
                out_.messages += "Reversed (or previously applied) patch detected!  Skipping patch.\n";
                std::vector<std::size_t> all(total);
                for (std::size_t k = 0; k < total; ++k)
                    all[k] = k;
                reject(name, s, all);
                summary(total, total, true, reject_path(name));
                return;
            }
        }
        std::vector<UnitValue> before, after;
        for (auto& u : s.units) {
            before.push_back({u.at, u.before ? &*u.before : nullptr, u.op});
            after.push_back({u.at, u.after ? &*u.after : nullptr, u.op});
        }
        std::unique_ptr<Document> result;
        std::vector<std::size_t> failed;
        const bool exact = s.old_hash == h;
        if (exact) {
            result = f->implant(*td, after, lo);
        } else {
            out_.messages += "base-mismatch: applying by identity\n";
            warn({"base-mismatch", "", name + ": applied by identity"});
            auto bv = f->implant(*td, before, lo), av = f->implant(*td, after, lo);
            ConflictOptions co;
            co.policy = rq_.mark ? Policy::mark : Policy::ours;
            MergeOptions mo{ctx, co, {"target", "patch-before", "patch-after"}};
            auto mr = f->merge(*bv, *td, *av, mo);
            for (std::size_t k = 0; k < total; ++k) {
                const Conflict* hit = nullptr;
                for (auto& c : mr.conflicts)
                    if (!c.resolved && related(c.at.path, s.units[k].at.path) &&
                        regions_meet(c.at.region, s.units[k].at.region))
                        hit = &c;
                if (!hit)
                    continue;
                failed.push_back(k);
                rep().conflicts.push_back(*hit);
                out_.messages += "Hunk #" + std::to_string(k + 1) +
                                 (rq_.mark ? " NOT MERGED at " : " FAILED at ") + s.units[k].at.to_string() +
                                 " (" + hit->reason + ").\n";
            }
            for (auto& w : mr.warnings)
                warn(w);
            result = std::move(mr.document);
        }
        for (std::size_t k = 0; k < total; ++k)
            if (std::find(failed.begin(), failed.end(), k) == failed.end())
                rep().changes.push_back(unit_change(s.units[k]));
        VectorSink sink;
        f->save(*result, sink, {ctx});
        commit(name, sink.data, true, !exact, false);
        if (!failed.empty()) {
            if (!rq_.mark)
                reject(name, s, failed);
            summary(failed.size(), total, false, rq_.mark ? std::nullopt : reject_path(name));
        }
    }

    const Registry& reg_;
    const PatchRequest& rq_;
    PatchOutcome out_;
    std::vector<std::byte> out_file_;
    std::map<std::string, std::string> rejects_;
    std::set<fs::path> backed_;
};

} // namespace

PatchOutcome apply_patch(const Registry& reg, const PatchRequest& rq) {
    return Applier(reg, rq).run();
}

} // namespace semsplinter
