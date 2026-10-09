#include "args.hpp"

namespace splinter::cli {

const Opt* find_long(std::string_view name, std::span<const Opt> table) {
    const Opt* hit = nullptr;
    bool ambiguous = false;
    for (auto& o : table) {
        if (o.long_name.empty() || !o.long_name.starts_with(name))
            continue;
        if (o.long_name.size() == name.size())
            return &o;
        if (!hit)
            hit = &o;
        else if (hit->id != o.id || hit->arg != o.arg)
            ambiguous = true;
    }
    if (ambiguous)
        throw UsageError("ambiguous option -- " + std::string(name));
    return hit;
}

Parsed parse(std::span<const std::string> args, std::span<const Opt> table, bool posixly_correct) {
    Parsed p;
    auto short_opt = [&](char c) -> const Opt* {
        for (auto& o : table)
            if (o.short_name == c)
                return &o;
        return nullptr;
    };
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--") {
            for (++i; i < args.size(); ++i)
                p.operands.push_back(args[i]);
            break;
        }
        if (a.size() < 2 || a[0] != '-') { // operand, including "-" (stdin)
            if (posixly_correct) {
                for (; i < args.size(); ++i)
                    p.operands.push_back(args[i]);
                break;
            }
            p.operands.push_back(a);
            continue;
        }
        p.option_tokens.push_back(a);
        if (a[1] == '-') {
            std::string_view body = std::string_view(a).substr(2);
            auto eq = body.find('=');
            std::string_view name = body.substr(0, eq);
            const Opt* o = find_long(name, table);
            if (!o)
                throw UsageError("unknown option -- " + std::string(name));
            ParsedOpt po{o->id, std::nullopt, "--" + std::string(o->long_name)};
            if (eq != std::string_view::npos) {
                if (o->arg == Arg::none)
                    throw UsageError("option doesn't take an argument -- " + std::string(o->long_name));
                po.value = std::string(body.substr(eq + 1));
            } else if (o->arg == Arg::required) {
                if (i + 1 >= args.size())
                    throw UsageError("option requires an argument -- " + std::string(o->long_name));
                po.value = args[++i];
                p.option_tokens.push_back(*po.value);
            }
            p.opts.push_back(std::move(po));
            continue;
        }
        for (std::size_t k = 1; k < a.size(); ++k) {
            const Opt* o = short_opt(a[k]);
            if (!o)
                throw UsageError(std::string("unknown option -- ") + a[k]);
            ParsedOpt po{o->id, std::nullopt, std::string("-") + a[k]};
            if (o->arg != Arg::none) {
                if (k + 1 < a.size()) {
                    po.value = a.substr(k + 1);
                } else if (o->arg == Arg::required) {
                    if (i + 1 >= args.size())
                        throw UsageError(std::string("option requires an argument -- ") + a[k]);
                    po.value = args[++i];
                    p.option_tokens.push_back(*po.value);
                }
                p.opts.push_back(std::move(po));
                break;
            }
            p.opts.push_back(std::move(po));
        }
    }
    return p;
}

} // namespace splinter::cli
