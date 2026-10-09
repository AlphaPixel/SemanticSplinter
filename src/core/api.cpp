#include <semsplinter/envelope.hpp>
#include <semsplinter/error.hpp>
#include <semsplinter/io.hpp>
#include <semsplinter/semsplinter.hpp>

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

// The format all inputs share; inputs of different formats (or any binary one) compare as bytes.
const Format& common_format(const Registry& reg, std::initializer_list<const Input*> in,
                            const std::string& format, bool force_text) {
    if (force_text)
        return by_name(reg, "text");
    const Format* first = nullptr;
    bool mixed = false;
    for (auto* i : in) {
        const Format& f = reg.detect(i->source.bytes(), i->path, format);
        mixed |= f.info().name == "binary" || (first && first != &f);
        first = first ? first : &f;
    }
    return mixed ? by_name(reg, "binary") : *first;
}

bool plain(const Format& f) {
    return f.info().name == "text" || f.info().name == "binary";
}

InputInfo info(std::string role, const Input& i) {
    return {std::move(role), i.path, hash128(i.source.bytes()), i.source.size()};
}

} // namespace

DiffResult diff(const Registry& reg, const DiffRequest& rq) {
    Limits budget = rq.limits;
    Context ctx{reg, budget, 0, rq.format_options};
    const Format& f = common_format(reg, {&rq.a, &rq.b}, rq.format, rq.force_text);
    if (rq.visual && plain(f))
        throw Error(ErrorCategory::unsupported, "--visual: no visual diff for text or binary files");
    const LoadOptions lo{ctx, rq.text_load};
    auto da = f.load(rq.a.source, lo), db = f.load(rq.b.source, lo);
    const DiffOptions dopt{ctx, rq.text};
    auto cmp = f.compare(*da, *db, dopt);

    DiffResult r;
    r.format = f.info().name;
    r.differs = cmp->differs();
    Churn churn;
    auto *ma = f.envelope(*da), *mb = f.envelope(*db);
    if (ma && mb)
        churn = attribute_churn(rq.a.source.bytes(), *ma, rq.b.source.bytes(), *mb);
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
    if (rq.render) {
        RenderRequest req = *rq.render;
        req.churn = &churn;
        r.errors = cmp->render(r.text, req);
    }
    return r;
}

MergeOutcome merge(const Registry& reg, const MergeRequest& rq) {
    Limits budget = rq.limits;
    Context ctx{reg, budget, 0, rq.format_options};
    const Format& f = common_format(reg, {&rq.base, &rq.ours, &rq.theirs}, rq.format, rq.force_text);
    if (rq.text_only && f.info().name != "text")
        throw Error(ErrorCategory::unsupported, "Cannot merge binary files: " + rq.ours.path);
    if (rq.visual && plain(f))
        throw Error(ErrorCategory::unsupported, "--visual: no visual diff for text or binary files");
    const LoadOptions lo{ctx, rq.text_load};
    auto db = f.load(rq.base.source, lo), dob = f.load(rq.ours.source, lo), dt = f.load(rq.theirs.source, lo);
    // --conflict=mark on a format without markers: ours plus the merge visual as a sidecar (png.md §6.6).
    const bool sidecar = rq.conflict.policy == Policy::mark && rq.output;
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
    if (sidecar && out.conflicts && !r.visual.empty()) {
        auto p = *rq.output;
        p += ".splinter-conflicts.png";
        atomic_write(p, r.visual);
    }

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

nlohmann::ordered_json inspect(const Registry& reg, const Input& in, const std::string& format, bool layout) {
    Limits budget;
    OptionValues options;
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
