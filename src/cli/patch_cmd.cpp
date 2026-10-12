// patch personality (CLI.md §5).
#include "cli.hpp"

#include <semsplinter/semsplinter.hpp>
#include <semsplinter/version.hpp>

namespace splinter::cli {

using namespace semsplinter;

int run_patch(const std::vector<std::string>& args, Console& c, const Registry& reg) {
    PatchRequest rq;
    CommonOptions common;
    std::optional<std::string> input;
    bool silent = false;
    const bool posix = c.env("POSIXLY_CORRECT").has_value();
    rq.backup.if_mismatch = !posix;
    try {
        auto tbl = table(Personality::patch, reg);
        auto p = parse(args, tbl, posix);
        for (auto& o : p.opts) {
            const std::string v = o.value.value_or("");
            switch (o.id) {
            case o_strip:
                rq.strip = static_cast<std::size_t>(parse_number(v, "strip count"));
                break;
            case o_fuzz:
                rq.text.fuzz = static_cast<std::size_t>(parse_number(v, "fuzz factor"));
                break;
            case o_ignore_whitespace:
                rq.text.ignore_whitespace = true;
                break;
            case o_patch_style:
                throw Error(ErrorCategory::unsupported,
                            "option '" + o.spelling +
                                "': only unified diffs and splinter envelopes are supported");
            case o_ifdef:
                throw Error(ErrorCategory::unsupported,
                            "option '" + o.spelling + "' (ifdef output) is not supported");
            case o_patch_unified:
            case o_patch_noop:
                break; // time-setting and quoting options are accepted without effect
            case o_forward:
                rq.text.reversed = TextApplyOptions::Reversed::skip;
                break;
            case o_batch:
                rq.text.reversed = TextApplyOptions::Reversed::assume;
                break;
            case o_force:
                rq.text.reversed = TextApplyOptions::Reversed::ignore;
                break;
            case o_reverse:
                rq.reverse = true;
                break;
            case o_input:
                input = v;
                break;
            case o_output:
                rq.output = v;
                break;
            case o_reject_file:
                rq.reject_file = v;
                break;
            case o_merge:
                if (o.value && v != "merge" && v != "diff3")
                    throw UsageError("invalid merge style '" + v + "'");
                rq.mark = true;
                break;
            case o_remove_empty:
                rq.remove_empty = true;
                break;
            case o_backup:
                rq.backup.always = true;
                break;
            case o_backup_if_mismatch:
                rq.backup.if_mismatch = true;
                break;
            case o_no_backup_if_mismatch:
                rq.backup.if_mismatch = false;
                break;
            case o_version_control:
                if (v == "t" || v == "numbered")
                    rq.backup.method = BackupOptions::Method::numbered;
                else if (v == "nil" || v == "existing")
                    rq.backup.method = BackupOptions::Method::existing;
                else if (v == "never" || v == "simple")
                    rq.backup.method = BackupOptions::Method::simple;
                else if (v == "none" || v == "off")
                    rq.backup.always = rq.backup.if_mismatch = false;
                else
                    throw UsageError("invalid version control type '" + v + "'");
                break;
            case o_prefix:
                rq.backup.prefix = v;
                break;
            case o_basename_prefix:
                rq.backup.basename_prefix = v;
                break;
            case o_suffix:
                rq.backup.suffix = v;
                break;
            case o_silent:
                silent = true;
                break;
            case o_dry_run:
                rq.dry_run = true;
                break;
            case o_posix:
                rq.backup.if_mismatch = false;
                break;
            case o_directory:
                rq.directory = to_path(v);
                break;
            case o_help:
                c.out(help_text());
                return 0;
            case o_version:
                c.out("splinter (Semantic Splinter) " + std::string(version()) + "\n");
                return 0;
            default:
                if (!common.apply(o, reg))
                    throw UsageError("unhandled option " + o.spelling);
            }
        }
        auto& ops = p.operands;
        if (ops.size() > 2)
            throw UsageError("extra operand '" + ops[2] + "'");
        if (!ops.empty())
            rq.target = ops[0];
        if (ops.size() == 2) {
            if (input)
                throw UsageError("patch file given twice ('" + *input + "' and '" + ops[1] + "')");
            input = ops[1];
        }
        rq.limits = common.limits;
        rq.format_options = common.format_options;
        auto src = read_input(input.value_or("-"), rq.limits, c);
        rq.patch.assign(reinterpret_cast<const char*>(src.bytes().data()), src.size());
        auto r = apply_patch(reg, rq);
        const bool report_out = (common.json && common.json->empty()) || (common.md && common.md->empty());
        if (!silent) // with -o -, --json or --md, the patched file or the report owns standard output
            rq.output == "-" || report_out ? c.err(r.messages) : c.out(r.messages);
        if (!r.output.empty())
            c.out({reinterpret_cast<const char*>(r.output.data()), r.output.size()});
        if (common.json) {
            nlohmann::ordered_json all = nlohmann::ordered_json::array();
            for (auto& rep : r.reports)
                all.push_back(to_json(rep));
            write_json(c, common.json, (all.size() == 1 ? all[0] : all).dump(2));
        }
        if (common.md) {
            std::string md;
            for (auto& rep : r.reports)
                md += render_markdown(rep);
            write_json(c, common.md, md);
        }
        return r.failed ? 1 : 0;
    } catch (const Error& e) {
        diag(c, e.what());
        return exit_code(e.category(), common.extended_exit);
    }
}

} // namespace splinter::cli
