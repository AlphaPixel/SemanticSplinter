#include <semsplinter/format.hpp>
#include <semsplinter/formats/binary.hpp>
#include <semsplinter/formats/text.hpp>

namespace semsplinter {

// ponytail: hand-written while only the mandatory modules exist; generate it from the enabled
// SEMSPLINTER_FORMAT_* options (SPEC §2.3) when the first optional module lands.
Registry Registry::with_builtin_formats() {
    Registry r;
    r.add(text::make_format());
    r.add(binary::make_format());
    return r;
}

} // namespace semsplinter
