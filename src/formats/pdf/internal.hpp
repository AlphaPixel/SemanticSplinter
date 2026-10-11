#pragma once

#include <semsplinter/hash.hpp>

#include <qpdf/QPDFPageObjectHelper.hh>

#include <string>
#include <vector>

namespace semsplinter::pdf {

// What a page draws, from its content streams (pdf.md §5 "Text extraction", basic qpdf extractor).
struct PageContent {
    std::vector<std::string> lines;  // extracted text, one line per baseline, in content order
    std::vector<std::string> images; // "WxH xxh3:<16 hex>" per image drawn, in content order
    Hash128 fingerprint;             // operators and operands, resource names replaced by resource hashes
    bool unmappable = false;         // text in a font that has no /ToUnicode and is not a simple font
};

PageContent read_page(QPDFPageObjectHelper& page);

// Hash of a stream's raw (still encoded) bytes, first 16 hex digits.
std::string raw_hash16(QPDFObjectHandle stream);

} // namespace semsplinter::pdf
