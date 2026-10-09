#include <semsplinter/report.hpp>
#include <semsplinter/version.hpp>

namespace semsplinter {

using json = nlohmann::ordered_json;

std::string hash_ref(const Hash128& h) {
    return "xxh3:" + h.hex();
}

namespace {
json value_json(const Value& v) {
    json j = json::object();
    if (v.text)
        j["text"] = *v.text;
    if (v.hash)
        j["hash"] = hash_ref(*v.hash);
    return j;
}
} // namespace

json to_json(const Change& c) {
    json j;
    j["id"] = c.id;
    j["op"] = to_string(c.op);
    j["at"] = c.at.to_string();
    if (c.from)
        j["from"] = c.from->to_string();
    j["side"] = to_string(c.side);
    if (!c.kind.empty())
        j["kind"] = c.kind;
    if (auto b = value_json(c.before); !b.empty())
        j["before"] = std::move(b);
    if (auto a = value_json(c.after); !a.empty())
        j["after"] = std::move(a);
    if (!c.depends_on.empty())
        j["depends_on"] = c.depends_on;
    if (c.confidence)
        j["confidence"] = *c.confidence;
    if (!c.metrics.empty())
        j["metrics"] = c.metrics;
    return j;
}

json to_json(const Conflict& c) {
    return {
        {"id", c.id},           {"at", c.at.to_string()},       {"changes", c.changes},  {"reason", c.reason},
        {"choices", c.choices}, {"resolved_as", c.resolved_as}, {"resolved", c.resolved}};
}

json to_json(const Report& r) {
    const bool merge = r.mode != "diff";
    auto list = [](const auto& v) {
        json a = json::array();
        for (auto& x : v)
            a.push_back(to_json(x));
        return a;
    };
    json j;
    j["schema"] = kReportSchema;
    j["tool"] = {{"name", "splinter"}, {"version", std::string(version())}};
    j["mode"] = r.mode;
    j["format"] = r.format;
    json in = json::object();
    for (auto& i : r.inputs)
        in[i.role] = {{"path", i.path}, {"hash", hash_ref(i.hash)}, {"bytes", i.bytes}};
    j["inputs"] = std::move(in);
    j["options"] = r.options;
    std::size_t unresolved = 0;
    if (merge) {
        j["changes_ours"] = list(r.changes_ours);
        j["changes_theirs"] = list(r.changes_theirs);
        json am = json::array();
        for (auto& a : r.auto_merged)
            am.push_back({{"change", a.change}, {"rule", a.rule}});
        j["auto_merged"] = std::move(am);
        j["conflicts"] = list(r.conflicts);
        for (auto& c : r.conflicts)
            unresolved += !c.resolved;
        j["stats"] = {{"changed_units_ours", r.changes_ours.size()},
                      {"changed_units_theirs", r.changes_theirs.size()},
                      {"conflicts", unresolved},
                      {"format", r.format_stats}};
    } else {
        j["changes"] = list(r.changes);
        j["stats"] = {{"changes", r.changes.size()}, {"format", r.format_stats}};
    }
    if (r.envelope)
        j["envelope"] = *r.envelope;
    json w = json::array();
    for (auto& x : r.warnings) {
        json e = {{"code", x.code}};
        if (!x.at.empty())
            e["at"] = x.at;
        if (!x.message.empty())
            e["message"] = x.message;
        w.push_back(std::move(e));
    }
    j["validation"] = {{"ok", r.valid}, {"warnings", std::move(w)}};
    if (r.result)
        j["result"] = {
            {"path", r.result->path}, {"hash", hash_ref(r.result->hash)}, {"status", r.result->status}};
    return j;
}

} // namespace semsplinter
