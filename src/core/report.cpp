#include <semsplinter/report.hpp>
#include <semsplinter/version.hpp>

#include <string_view>
#include <utility>

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
    const bool merge = r.mode == "merge";
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
        if (r.mode == "patch")
            j["conflicts"] = list(r.conflicts);
        j["stats"] = {{"changes", r.changes.size()}, {"format", r.format_stats}};
    }
    if (r.patch)
        add_patch(j, *r.patch);
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

namespace {
// Escapes text for a Markdown table cell or heading: backslash before the punctuation Markdown acts on
// (including the table's `|`), and `<br>` for line breaks.
std::string md(std::string_view s) {
    std::string o;
    for (char ch : s) {
        if (ch == '\r')
            continue;
        if (ch == '\n') {
            o += "<br>";
            continue;
        }
        if (std::string_view("\\`*_[]<>|#~!&$").find(ch) != std::string_view::npos)
            o += '\\';
        o += ch;
    }
    return o;
}

std::string mdj(const json& v) {
    return md(v.is_string() ? v.get<std::string>() : v.dump());
}

std::string md_value(const json& v) { // {"text", "hash"}
    if (!v.contains("text"))
        return mdj(v["hash"]);
    return mdj(v["text"]) + (v.contains("hash") ? " (" + mdj(v["hash"]) + ")" : "");
}

void row(std::string& o, const std::string& k, const std::string& v) {
    o += "| " + k + " | " + v + " |\n";
}

constexpr const char* kTableHead = "| Field | Value |\n|---|---|\n";
} // namespace

std::string render_markdown(const Report& r) {
    const json j = to_json(r);
    const bool merge = r.mode == "merge"; // diff and patch reports list one change set
    std::string o = "# splinter " + md(r.mode) + " report\n\n" + kTableHead;
    row(o, "Format", md(r.format));
    for (auto& i : r.inputs)
        row(o, "Input " + md(i.role),
            md(i.path) + " (" + std::to_string(i.bytes) + " bytes, " + hash_ref(i.hash) + ")");
    if (merge) {
        row(o, "Changed units (ours)", std::to_string(r.changes_ours.size()));
        row(o, "Changed units (theirs)", std::to_string(r.changes_theirs.size()));
        row(o, "Auto-merged", std::to_string(r.auto_merged.size()));
        row(o, "Conflicts", mdj(j["stats"]["conflicts"]));
    } else {
        row(o, "Changed units", std::to_string(r.changes.size()));
        if (!r.conflicts.empty()) // patch mode: rejected hunks
            row(o, "Conflicts", std::to_string(r.conflicts.size()));
    }
    for (auto& [k, v] : r.format_stats.items())
        row(o, md(k), mdj(v));
    if (r.envelope && r.envelope->contains("churn")) {
        std::string churn;
        for (auto& [k, v] : (*r.envelope)["churn"].items())
            churn += (churn.empty() ? "" : ", ") + md(k) + " " + mdj(v);
        row(o, "Churn", churn);
    }
    row(o, "Validation", r.valid ? "ok" : "failed");
    if (r.result)
        row(o, "Result",
            md(r.result->path) + " (" + md(r.result->status) + ", " + hash_ref(r.result->hash) + ")");

    if (!r.warnings.empty()) {
        o += "\n## Warnings\n\n";
        for (auto& w : r.warnings)
            o += "- " + md(w.code) + (w.at.empty() ? "" : " at " + md(w.at)) +
                 (w.message.empty() ? "" : ": " + md(w.message)) + "\n";
    }
    if (!r.conflicts.empty())
        o += "\n## Conflicts\n";
    for (auto& c : r.conflicts) {
        o += "\n### " + md(c.id) + ": " + md(c.at.to_string()) + "\n\n" + kTableHead;
        row(o, "Reason", md(c.reason));
        std::string ids;
        for (auto& id : c.changes)
            ids += (ids.empty() ? "" : ", ") + md(id);
        row(o, "Changes", ids);
        row(o, "Resolved as", md(c.resolved_as) + (c.resolved ? "" : " (unresolved)"));
    }
    auto changes = [&](const char* title, const json& list) {
        if (list.empty())
            return;
        o += "\n## " + std::string(title) + "\n";
        for (auto& c : list) {
            o += "\n### " + mdj(c["id"]) + ": " + mdj(c["op"]) + " " + mdj(c["at"]) + "\n\n" + kTableHead;
            for (auto [k, label] : {std::pair{"side", "Side"},
                                    {"kind", "Kind"},
                                    {"from", "From"},
                                    {"confidence", "Confidence"}})
                if (c.contains(k))
                    row(o, label, mdj(c[k]));
            if (c.contains("before"))
                row(o, "Before", md_value(c["before"]));
            if (c.contains("after"))
                row(o, "After", md_value(c["after"]));
            if (c.contains("depends_on")) {
                std::string deps;
                for (auto& d : c["depends_on"])
                    deps += (deps.empty() ? "" : ", ") + mdj(d);
                row(o, "Depends on", deps);
            }
            for (auto& a : r.auto_merged)
                if (c["id"] == a.change)
                    row(o, "Auto-merged", md(a.rule));
            if (c.contains("metrics"))
                for (auto& [k, v] : c["metrics"].items())
                    row(o, md(k), mdj(v));
        }
    };
    if (merge) {
        changes("Changes (ours)", j["changes_ours"]);
        changes("Changes (theirs)", j["changes_theirs"]);
    } else {
        changes("Changes", j["changes"]);
    }
    return o;
}

} // namespace semsplinter
