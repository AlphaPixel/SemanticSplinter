#include <semsplinter/error.hpp>
#include <semsplinter/format.hpp>
#include <semsplinter/text.hpp>

#include <algorithm>
#include <cctype>

namespace semsplinter {

std::vector<std::byte> Format::visual(const Document&, const Document&, const DiffOptions&) const {
    throw Error(ErrorCategory::unsupported,
                "--visual: format '" + std::string(info().name) + "' has no visual diff");
}

void Registry::add(std::unique_ptr<Format> f) {
    formats_.push_back(std::move(f));
}

const Format* Registry::find(std::string_view name) const noexcept {
    for (auto& f : formats_)
        if (f->info().name == name)
            return f.get();
    return nullptr;
}

namespace {
std::string lower_extension(std::string_view name) {
    auto slash = name.find_last_of("/\\");
    if (slash != std::string_view::npos)
        name.remove_prefix(slash + 1);
    auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
        return {};
    std::string ext(name.substr(dot));
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}
} // namespace

const Format& Registry::detect(std::span<const std::byte> content, std::string_view logical_name,
                               std::string_view explicit_name) const {
    if (!explicit_name.empty()) {
        if (auto* f = find(explicit_name))
            return *f;
        throw Error(ErrorCategory::unsupported, "unknown format '" + std::string(explicit_name) + "'");
    }
    auto head = content.first(std::min<std::size_t>(content.size(), 64 * 1024));
    if (auto ext = lower_extension(logical_name); !ext.empty())
        for (auto& f : formats_)
            if (std::find(f->info().extensions.begin(), f->info().extensions.end(), ext) !=
                    f->info().extensions.end() &&
                f->probe(head, logical_name).confidence > 0)
                return *f;
    const Format* best = nullptr;
    int conf = 0;
    for (auto& f : formats_) {
        auto name = f->info().name;
        if (name == "text" || name == "binary")
            continue; // fallbacks, step 4
        if (int c = f->probe(head, logical_name).confidence; c > conf)
            best = f.get(), conf = c;
    }
    if (best)
        return *best;
    auto* fallback = find(looks_binary(head) ? "binary" : "text");
    if (!fallback)
        throw Error(ErrorCategory::internal, "text and binary formats are not registered");
    return *fallback;
}

} // namespace semsplinter
