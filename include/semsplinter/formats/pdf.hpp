#pragma once

// The PDF module (docs/formats/pdf.md): qpdf object model, a basic text extractor, page alignment, and
// metadata, annotation, and form-field merge onto a carrier, written as an incremental update.

#include <semsplinter/format.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace semsplinter::pdf {

// Fixture helper (pdf.md §11): `file` rewritten by qpdf's QPDFWriter, optionally with object streams
// (and so an xref stream) and AES-256 encryption with this user password ("" for permission-only).
std::vector<std::byte> rewrite(std::span<const std::byte> file, bool object_streams,
                               const char* user_password = nullptr);

std::unique_ptr<Format> make_format();

} // namespace semsplinter::pdf
