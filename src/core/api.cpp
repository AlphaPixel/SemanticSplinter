#include <semsplinter/envelope.hpp>
#include <semsplinter/error.hpp>
#include <semsplinter/io.hpp>
#include <semsplinter/semsplinter.hpp>
#include <semsplinter/text.hpp>

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace semsplinter {

namespace {

const Format& by_name(const Registry& reg, std::string_view name) {
    auto* f = reg.find(name);
    if (!f)
        throw Error(ErrorCategory::internal, "format '" + std::string(name) + "' is not registered");
    return *f;
}

bool plain(const Format& f) {
    return f.info().name == "text" || f.info().name == "binary";
}

InputInfo info(std::string role, const Input& i) {
    return {std::move(role), i.path, hash128(i.source.bytes()), i.source.size()};
}

// Loads one input on its own budget: limits are per document (SPEC §9), so the inputs of a merge and its
// result do not add up (#51). The diff or merge step itself has another.
std::unique_ptr<Document> load(const Registry& reg, const Format& f, const Input& in, const Limits& limits,
                               const OptionValues& options, const TextLoadOptions& text) {
    Limits budget = limits.fresh();
    Context ctx{reg, budget, 0, options};
    return f.load(in.source, {ctx, text});
}

} // namespace

const Format& detect_format(const Registry& reg, std::initializer_list<const Input*> in,
                            const std::string& format, bool force_text, std::string* note) {
    if (force_text)
        return by_name(reg, "text");
    const Format* first = nullptr;
    bool binary = false, mixed = false, text = true;
    std::string names;
    for (auto* i : in) {
        const Format& f = reg.detect(i->source.bytes(), i->path, format);
        binary |= f.info().name == "binary";
        mixed |= first && first != &f;
        text &= !looks_binary(i->source.bytes());
        names += (names.empty() ? "" : ", ") + std::string(f.info().name);
        first = first ? first : &f;
    }
    if (binary || !mixed)
        return binary ? by_name(reg, "binary") : *first;
    if (!text)
        return by_name(reg, "binary");
    // Text underneath (json beside a side whose conflict markers broke parsing): a text merge beats bytes
    // (#41).
    if (note)
        *note = "inputs detect as different formats (" + names + "); comparing as text";
    return by_name(reg, "text");
}

DiffResult diff(const Registry& reg, const DiffRequest& rq) {
    Limits budget = rq.limits;
    Context ctx{reg, budget, 0, rq.format_options};
    std::string note;
    const Format& f = detect_format(reg, {&rq.a, &rq.b}, rq.format, rq.force_text, &note);
    if (rq.visual && plain(f))
        throw Error(ErrorCategory::unsupported, "--visual: no visual diff for text or binary files");
    auto da = load(reg, f, rq.a, rq.limits, rq.format_options, rq.text_load);
    auto db = load(reg, f, rq.b, rq.limits, rq.format_options, rq.text_load);
    const DiffOptions dopt{ctx, rq.text};
    auto cmp = f.compare(*da, *db, dopt);

    DiffResult r;
    r.format = f.info().name;
    r.differs = cmp->differs();
    Churn churn;
    auto *ma = f.envelope(*da), *mb = f.envelope(*db);
    if (ma && mb)
        churn = attribute_churn(rq.a.source.bytes(), *ma, rq.b.source.bytes(), *mb);
    if (!note.empty())
        r.warnings.push_back({"", {"formats-mixed", "", note}});
    auto ia = f.inspect(*da), ib = f.inspect(*db);
    for (auto [j, path] : {std::pair{&ia, &rq.a.path}, std::pair{&ib, &rq.b.path}})
        if (j->contains("warnings"))
            for (auto& w : (*j)["warnings"])
                r.warnings.push_back(
                    {*path, {w.value("code", ""), w.value("at", ""), w.value("message", "")}});
    if (rq.report) {
        Report rep;
        rep.mode = "diff";
        rep.format = r.format;
        rep.inputs = {info("a", rq.a), info("b", rq.b)};
        rep.options = cmp->options();
        rep.changes = cmp->changes();
        rep.format_stats = cmp->stats();
        if (ma && mb) {
            nlohmann::ordered_json env;
            env["integrity"] = {{"a", ia.value("integrity", nlohmann::ordered_json("not-checked"))},
                                {"b", ib.value("integrity", nlohmann::ordered_json("not-checked"))}};
            env["churn"] = churn.to_json();
            rep.envelope = std::move(env);
        }
        for (auto& [path, w] : r.warnings)
            rep.warnings.push_back(w);
        r.report = std::move(rep);
    }
    if (rq.visual)
        r.visual = f.visual(*da, *db, dopt);
    if (rq.patch && r.format != "text" && r.differs) {
        PatchSection s;
        s.old_name = rq.a.path, s.new_name = rq.b.path;
        s.old_hash = hash128(rq.a.source.bytes()), s.new_hash = hash128(rq.b.source.bytes());
        s.format = r.format;
        auto one_line = [](const std::optional<std::string>& t) {
            return t && t->find('\n') == std::string::npos ? t : std::nullopt;
        };
        for (auto& c : cmp->changes()) {
            UnitHunk u{c.op, c.at, c.id, one_line(c.before.text), one_line(c.after.text), {}, {}};
            if (c.op != Op::insert)
                u.before = f.payload(*da, c.at);
            if (c.op != Op::erase)
                u.after = f.payload(*db, c.at);
            s.units.push_back(std::move(u));
        }
        if (rq.exact) {
            // ponytail: one splice over the first..last changed byte; per-stream splices if they get large
            auto a = rq.a.source.bytes(), b = rq.b.source.bytes();
            std::size_t pre = 0, suf = 0;
            while (pre < a.size() && pre < b.size() && a[pre] == b[pre])
                ++pre;
            while (suf < a.size() - pre && suf < b.size() - pre &&
                   a[a.size() - 1 - suf] == b[b.size() - 1 - suf])
                ++suf;
            s.exact = ExactBytes{
                pre,
                a.size() - pre - suf,
                {b.begin() + static_cast<std::ptrdiff_t>(pre), b.end() - static_cast<std::ptrdiff_t>(suf)}};
        }
        r.patch = write_section(s);
        if (r.report)
            r.report->patch = std::move(s);
    }
    if (rq.render) {
        RenderRequest req = *rq.render;
        req.churn = &churn;
        r.errors = cmp->render(r.text, req);
    }
    if (rq.patch && r.report && r.format == "text") // the unified rendering, as one section
        if (auto t = parse_patch(r.text, rq.limits.max_input); !t.empty())
            r.report->patch = std::move(t[0]);
    return r;
}

MergeOutcome merge(const Registry& reg, const MergeRequest& rq) {
    Limits budget = rq.limits;
    Context ctx{reg, budget, 0, rq.format_options};
    std::string note;
    const Format& f = detect_format(reg, {&rq.base, &rq.ours, &rq.theirs}, rq.format, rq.force_text, &note);
    if (rq.text_only && f.info().name != "text")
        throw Error(ErrorCategory::unsupported, "Cannot merge binary files: " + rq.ours.path);
    if (rq.visual && plain(f))
        throw Error(ErrorCategory::unsupported, "--visual: no visual diff for text or binary files");
    auto db = load(reg, f, rq.base, rq.limits, rq.format_options, rq.text_load);
    auto dob = load(reg, f, rq.ours, rq.limits, rq.format_options, rq.text_load);
    auto dt = load(reg, f, rq.theirs, rq.limits, rq.format_options, rq.text_load);
    // --conflict=mark on a format without markers: ours plus the merge visual as a sidecar (png.md §6.6).
    std::filesystem::path sidecar_path;
    if (rq.conflict.policy == Policy::mark) {
        if (rq.mark_visual)
            sidecar_path = *rq.mark_visual;
        else if (rq.output)
            sidecar_path = *rq.output, sidecar_path += ".splinter-conflicts.png";
    }
    const bool sidecar = !sidecar_path.empty();
    using V = MergeOptions::Visual;
    MergeOptions mo{ctx, rq.conflict, rq.labels,
                    rq.visual ? V::always
                    : sidecar ? V::on_conflict
                              : V::none,
                    rq.text};
    auto r = f.merge(*db, *dob, *dt, mo);
    VectorSink sink;
    f.save(*r.document, sink, {ctx});

    MergeOutcome out;
    out.format = f.info().name;
    out.bytes = std::move(sink.data);
    out.conflicts = r.conflict_count();
    if (rq.output)
        atomic_write(*rq.output, out.bytes);
    if (rq.visual)
        atomic_write(*rq.visual, r.visual);
    if (sidecar && out.conflicts && !r.visual.empty())
        atomic_write(sidecar_path, r.visual);

    Report& rep = out.report;
    rep.mode = "merge";
    rep.format = out.format;
    rep.inputs = {info("base", rq.base), info("ours", rq.ours), info("theirs", rq.theirs)};
    static const char* policies[] = {"ours", "theirs", "base", "mark"};
    rep.options["conflict"] = policies[static_cast<int>(rq.conflict.policy)];
    auto guard = rq.format_options.find("guard");
    rep.options["guard"] = guard == rq.format_options.end() ? 0 : std::stoull(guard->second);
    rep.changes_ours = r.changes_ours;
    rep.changes_theirs = r.changes_theirs;
    rep.auto_merged = r.auto_merged;
    rep.conflicts = r.conflicts;
    if (!note.empty())
        r.warnings.insert(r.warnings.begin(), {"formats-mixed", "", note});
    rep.warnings = r.warnings;
    auto *mo_ours = f.envelope(*dob), *mo_out = f.envelope(*r.document);
    if (mo_ours && mo_out)
        rep.envelope = nlohmann::ordered_json{
            {"churn", attribute_churn(rq.ours.source.bytes(), *mo_ours, out.bytes, *mo_out).to_json()}};
    rep.result =
        ResultInfo{rq.output_name, hash128(out.bytes), r.unresolved_conflicts() ? "conflicts" : "clean"};
    if (rq.conflicts_file)
        atomic_write(*rq.conflicts_file, as_bytes(to_json(rep).dump(2) + "\n"));
    out.warnings = std::move(r.warnings);
    return out;
}

std::string dump(const Registry& reg, const Input& in, const std::string& format, bool force_text,
                 const Limits& limits, const OptionValues& options) {
    Limits budget = limits;
    Context ctx{reg, budget, 0, options};
    const Format& f = force_text ? by_name(reg, "text") : reg.detect(in.source.bytes(), in.path, format);
    auto doc = f.load(in.source, {ctx});
    std::ostringstream os;
    f.dump(*doc, os, {ctx});
    return os.str();
}

nlohmann::ordered_json inspect(const Registry& reg, const Input& in, const std::string& format, bool layout,
                               const Limits& limits, const OptionValues& options) {
    Limits budget = limits;
    Context ctx{reg, budget, 0, options};
    const Format& f = reg.detect(in.source.bytes(), in.path, format);
    auto doc = f.load(in.source, {ctx});
    nlohmann::ordered_json j;
    j["path"] = in.path;
    j["format"] = f.info().name;
    j["bytes"] = in.source.size();
    j["hash"] = hash_ref(hash128(in.source.bytes()));
    const auto fields = f.inspect(*doc);
    for (auto& [k, v] : fields.items())
        j[k] = v;
    if (layout) {
        const EnvelopeMap whole = EnvelopeMap::whole_content(in.source.size());
        const EnvelopeMap* map = f.envelope(*doc) ? f.envelope(*doc) : &whole;
        auto& l = j["layout"] = nlohmann::ordered_json::array();
        for (auto& r : map->ranges)
            l.push_back({{"offset", r.offset},
                         {"length", r.length},
                         {"class", to_string(r.cls)},
                         {"unit", r.unit},
                         {"layer", r.layer}});
    }
    return j;
}

std::string render_inspect(const nlohmann::ordered_json& j) {
    std::string o;
    for (auto& [k, v] : j.items()) {
        if (k == "layout" || k == "chunks")
            continue;
        if (k == "integrity" && v.is_object()) {
            std::string at;
            for (auto& x : v["at"])
                at += (at.empty() ? "" : ",") + x.get<std::string>();
            o += "integrity " + v.value("status", "") + " " + at + "\n";
        } else if (k == "warnings") {
            for (auto& w : v)
                o += "warning " + w.value("code", "") + " " + w.value("at", "") + ": " +
                     w.value("message", "") + "\n";
        } else if (k == "units") {
            for (auto& u : v)
                o += "unit " + u.value("at", "") + " " + u.value("value", "") + "\n";
        } else if (!v.is_structured()) {
            o += k + " " + (v.is_string() ? v.get<std::string>() : v.dump()) + "\n";
        }
    }
    if (j.contains("layout"))
        for (auto& r : j["layout"]) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%10llu %8llu %-11s ", r["offset"].get<unsigned long long>(),
                          r["length"].get<unsigned long long>(), r["class"].get<std::string>().c_str());
            auto layer = r["layer"].get<std::string>();
            o += buf + r["unit"].get<std::string>() + (layer.empty() ? "" : " " + layer) + "\n";
        }
    return o;
}

} // namespace semsplinter
