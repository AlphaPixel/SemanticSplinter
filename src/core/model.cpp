#include <semsplinter/model.hpp>

#include <algorithm>
#include <tuple>

namespace semsplinter {

std::string_view to_string(Op op) noexcept {
    switch (op) {
    case Op::insert:
        return "insert";
    case Op::erase:
        return "erase";
    case Op::update:
        return "update";
    case Op::move:
        return "move";
    }
    return "update";
}

std::string_view to_string(Side s) noexcept {
    switch (s) {
    case Side::base:
        return "base";
    case Side::ours:
        return "ours";
    case Side::theirs:
        return "theirs";
    }
    return "base";
}

namespace {
template <class... T> std::string join(std::string_view tag, T... v) {
    std::string s(tag);
    char sep = '=';
    ((s += sep, s += std::to_string(v), sep = ','), ...);
    return s;
}
} // namespace

std::string to_string(const Region& r) {
    if (auto* p = std::get_if<Rect>(&r))
        return join("rect", p->x, p->y, p->w, p->h);
    if (auto* p = std::get_if<Range>(&r))
        return join("range", p->start, p->length);
    auto& b = std::get<Box>(r);
    return join("box", b.x, b.y, b.z, b.w, b.h, b.d);
}

std::string Address::to_string() const {
    return region ? path + "#" + semsplinter::to_string(*region) : path;
}

std::string pointer_escape(std::string_view seg) {
    std::string out;
    out.reserve(seg.size());
    for (char c : seg) {
        if (c == '~')
            out += "~0";
        else if (c == '/')
            out += "~1";
        else
            out += c;
    }
    return out;
}

std::vector<std::string> pointer_split(std::string_view path) {
    std::vector<std::string> out;
    for (char c : path) {
        if (c == '/')
            out.emplace_back();
        else if (!out.empty())
            out.back() += c;
    }
    for (auto& s : out)
        for (std::size_t k = 0; (k = s.find('~', k)) != std::string::npos && k + 1 < s.size(); ++k)
            s.replace(k, 2, s[k + 1] == '1' ? "/" : "~");
    return out;
}

bool path_within(std::string_view path, std::string_view ancestor) noexcept {
    if (!path.starts_with(ancestor))
        return false;
    return path.size() == ancestor.size() || ancestor.ends_with('/') || path[ancestor.size()] == '/' ||
           path[ancestor.size()] == '#';
}

std::string make_change_id(Side side, Op op, const Address& at) {
    Hasher h;
    h.add(static_cast<std::uint64_t>(side)).add(static_cast<std::uint64_t>(op)).add(at.to_string());
    return "c-" + h.digest().hex().substr(0, 12);
}

std::string make_conflict_id(const Address& at, std::vector<std::string> ids) {
    std::sort(ids.begin(), ids.end());
    Hasher h;
    h.add(at.to_string());
    for (auto& id : ids)
        h.add(id);
    return "x-" + h.digest().hex().substr(0, 12);
}

Change make_change(Side side, Op op, Address at, std::string kind) {
    Change c;
    c.id = make_change_id(side, op, at);
    c.op = op;
    c.at = std::move(at);
    c.side = side;
    c.kind = std::move(kind);
    return c;
}

Conflict make_conflict(Address at, std::vector<std::string> ids, std::string reason) {
    Conflict c;
    c.id = make_conflict_id(at, ids);
    c.at = std::move(at);
    c.changes = std::move(ids);
    c.reason = std::move(reason);
    return c;
}

void sort_canonical(ChangeSet& cs) {
    std::stable_sort(cs.begin(), cs.end(), [](const Change& a, const Change& b) {
        return std::tuple(a.at.to_string(), a.op) < std::tuple(b.at.to_string(), b.op);
    });
}

std::size_t MergeResult::unresolved_conflicts() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(conflicts.begin(), conflicts.end(), [](const Conflict& c) { return !c.resolved; }));
}

} // namespace semsplinter
