// The basic text extractor over qpdf's content-stream parser (pdf.md §5): tracks the text and graphics
// matrices for baselines, decodes strings through /ToUnicode or WinAnsi, and has no layout analysis.

#include "internal.hpp"

#include <qpdf/Buffer.hh>
#include <qpdf/QPDFExc.hh>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <set>

namespace semsplinter::pdf {

std::string raw_hash16(QPDFObjectHandle stream) {
    try {
        auto b = stream.getRawStreamData();
        return hash128(std::span(reinterpret_cast<const std::byte*>(b->getBuffer()), b->getSize()))
            .hex()
            .substr(0, 16);
    } catch (const std::exception&) {
        return "unreadable";
    }
}

namespace {

void utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | cp >> 6), out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | cp >> 12), out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | cp >> 18), out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F)), out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// WinAnsiEncoding 0x80..0x9F (the rest is Latin-1); 0 where undefined.
constexpr std::uint16_t kWinAnsi[32] = {0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
                                        0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

std::uint32_t hex_value(const std::string& h) {
    std::uint32_t v = 0;
    for (char c : h)
        v = v << 4 | static_cast<std::uint32_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    return v;
}

// UTF-16BE hex digits as UTF-8.
std::string utf16_hex(const std::string& h) {
    std::string out;
    for (std::size_t i = 0; i + 4 <= h.size(); i += 4) {
        std::uint32_t u = hex_value(h.substr(i, 4));
        if (u >= 0xD800 && u < 0xDC00 && i + 8 <= h.size()) {
            const std::uint32_t lo = hex_value(h.substr(i + 4, 4));
            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            i += 4;
        }
        utf8(out, u);
    }
    return out;
}

struct Font {
    unsigned code_bytes = 1;
    bool cmap = false, simple = true;
    std::map<std::uint32_t, std::string> map;

    std::string decode(const std::string& s, bool& unmappable) const {
        std::string out;
        for (std::size_t i = 0; i + code_bytes <= s.size(); i += code_bytes) {
            std::uint32_t code = 0;
            for (unsigned k = 0; k < code_bytes; ++k)
                code = code << 8 | static_cast<unsigned char>(s[i + k]);
            if (auto f = map.find(code); f != map.end())
                out += f->second;
            else if (simple && (code >= 0xA0 || (code >= 0x20 && code < 0x7F)))
                utf8(out, code);
            else if (simple && code >= 0x80 && code < 0xA0 && kWinAnsi[code - 0x80])
                utf8(out, kWinAnsi[code - 0x80]);
            else if (code == '\t' || code == '\n' || code == '\r')
                out += ' ';
            else
                out += '?', unmappable |= !cmap;
        }
        return out;
    }
};

// bfchar and bfrange entries of a ToUnicode CMap. Tokens: hex strings and brackets; the rest is skipped.
// ponytail: one code length per font (from the first codespacerange); mixed-length CMaps decode wrong.
void parse_cmap(const std::string& s, Font& f) {
    std::vector<std::string> toks;
    std::string section;
    auto flush = [&] {
        if (section == "codespacerange" && !toks.empty() && toks[0] != "[")
            f.code_bytes = std::max<unsigned>(1, static_cast<unsigned>(toks[0].size() / 2));
        if (section == "bfchar")
            for (std::size_t i = 0; i + 1 < toks.size(); i += 2)
                f.map[hex_value(toks[i])] = utf16_hex(toks[i + 1]);
        if (section == "bfrange")
            for (std::size_t i = 0; i + 2 < toks.size();) {
                const std::uint32_t lo = hex_value(toks[i]), hi = hex_value(toks[i + 1]);
                if (hi < lo || hi - lo > 65535)
                    break;
                if (toks[i + 2] == "[") {
                    std::size_t k = i + 3;
                    for (std::uint32_t c = lo; k < toks.size() && toks[k] != "]"; ++c, ++k)
                        f.map[c] = utf16_hex(toks[k]);
                    i = k + 1;
                } else {
                    std::string dst = toks[i + 2];
                    for (std::uint32_t c = lo; c <= hi; ++c) {
                        f.map[c] = utf16_hex(dst);
                        for (std::size_t p = dst.size(); p-- > 0;) { // increment the last UTF-16 unit
                            const char d = dst[p];
                            if (d == '9')
                                dst[p] = 'A';
                            else if (d == 'F' || d == 'f') {
                                dst[p] = '0';
                                continue;
                            } else
                                ++dst[p];
                            break;
                        }
                    }
                    i += 3;
                }
            }
        toks.clear();
    };
    for (std::size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (c == '<' && i + 1 < s.size() && s[i + 1] != '<') {
            auto end = s.find('>', i);
            if (end == std::string::npos)
                break;
            std::string h;
            for (auto k = i + 1; k < end; ++k)
                if (std::isxdigit(static_cast<unsigned char>(s[k])))
                    h += s[k];
            toks.push_back(h);
            i = end + 1;
        } else if (c == '[' || c == ']') {
            toks.push_back(std::string(1, c));
            ++i;
        } else if (std::isalpha(static_cast<unsigned char>(c))) {
            auto k = i;
            while (k < s.size() && std::isalnum(static_cast<unsigned char>(s[k])))
                ++k;
            const std::string word = s.substr(i, k - i);
            if (word.starts_with("begin"))
                section = word.substr(5), toks.clear();
            else if (word.starts_with("end") && word.size() > 3)
                flush(), section.clear();
            i = k;
        } else {
            ++i;
        }
    }
}

struct Matrix {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Matrix operator*(const Matrix& q) const {
        return {a * q.a + b * q.c, a * q.b + b * q.d,       c * q.a + d * q.c,
                c * q.b + d * q.d, e * q.a + f * q.c + q.e, e * q.b + f * q.d + q.f};
    }
};

constexpr int kMaxFormDepth = 8;

class Reader final : public QPDFObjectHandle::ParserCallbacks {
public:
    Reader(PageContent& out, QPDFObjectHandle resources, Matrix ctm, int depth, std::set<QPDFObjGen>& active)
        : out_(out), res_(resources), active_(active), depth_(depth) {
        ctm_ = ctm;
    }

    void handleObject(QPDFObjectHandle o) override {
        if (o.isOperator())
            op(o.getOperatorValue());
        else if (o.isInlineImage())
            hash_.add("inline").add(o.getInlineImageValue());
        else
            args_.push_back(o);
    }
    void handleEOF() override {}

    Hash128 digest() const { return hash_.digest(); }
    std::string& line() { return line_; }

private:
    double num(std::size_t i) { // not const: qpdf 11 getters are non-const (#143)
        return i < args_.size() && args_[i].isNumber() ? args_[i].getNumericValue() : 0;
    }

    QPDFObjectHandle resource(const char* kind, std::size_t arg) {
        if (arg >= args_.size() || !args_[arg].isName() || !res_.isDictionary())
            return QPDFObjectHandle::newNull();
        auto group = res_.getKey(kind);
        return group.isDictionary() ? group.getKey(args_[arg].getName()) : QPDFObjectHandle::newNull();
    }

    void op(const std::string& name) {
        hash_.add(name);
        for (std::size_t i = 0; i < args_.size(); ++i) // resource names are hashed by what they name
            if (!((name == "Tf" && i == 0) || (name == "Do" && i == 0)))
                hash_.add(args_[i].unparse());
        if (name == "q")
            stack_.push_back(ctm_);
        else if (name == "Q" && !stack_.empty())
            ctm_ = stack_.back(), stack_.pop_back();
        else if (name == "cm" && args_.size() == 6)
            ctm_ = Matrix{num(0), num(1), num(2), num(3), num(4), num(5)} * ctm_;
        else if (name == "BT")
            tm_ = tlm_ = {};
        else if (name == "Td" || name == "TD") {
            if (name == "TD")
                leading_ = -num(1);
            tlm_ = tm_ = Matrix{1, 0, 0, 1, num(0), num(1)} * tlm_;
            moved_ = true;
        } else if (name == "Tm" && args_.size() == 6) {
            tlm_ = tm_ = {num(0), num(1), num(2), num(3), num(4), num(5)};
            moved_ = true;
        } else if (name == "T*") {
            next_line();
        } else if (name == "TL") {
            leading_ = num(0);
        } else if (name == "Tf") {
            font_ = font(resource("/Font", 0));
        } else if (name == "Tj" && !args_.empty()) {
            show(args_[0]);
        } else if (name == "'" && !args_.empty()) {
            next_line(), show(args_[0]);
        } else if (name == "\"" && args_.size() == 3) {
            next_line(), show(args_[2]);
        } else if (name == "TJ" && !args_.empty() && args_[0].isArray()) {
            for (auto& e : args_[0].getArrayAsVector())
                if (e.isString())
                    show(e);
                else if (e.isNumber() && e.getNumericValue() < -200 && !line_.empty() && line_.back() != ' ')
                    line_ += ' ';
        } else if (name == "Do") {
            xobject(resource("/XObject", 0));
        }
        args_.clear();
    }

    void next_line() {
        tlm_ = tm_ = Matrix{1, 0, 0, 1, 0, -leading_} * tlm_;
        moved_ = true;
    }

    const Font* font(QPDFObjectHandle f) {
        if (!f.isDictionary())
            return nullptr;
        hash_.add(f.getKey("/BaseFont").unparse()).add(f.getKey("/Encoding").unparse());
        auto key = f.getObjGen();
        auto& slot = fonts_[key.isIndirect() ? key : QPDFObjGen(-1, static_cast<int>(fonts_.size()))];
        if (slot)
            return slot.get();
        slot = std::make_unique<Font>();
        const std::string subtype = f.getKey("/Subtype").isName() ? f.getKey("/Subtype").getName() : "";
        if (subtype == "/Type0")
            slot->simple = false, slot->code_bytes = 2;
        auto tu = f.getKey("/ToUnicode");
        if (tu.isStream()) {
            try {
                auto b = tu.getStreamData();
                parse_cmap({reinterpret_cast<const char*>(b->getBuffer()), b->getSize()}, *slot);
                slot->cmap = true;
            } catch (const std::exception&) {
            }
        }
        return slot.get();
    }

    void show(QPDFObjectHandle s) {
        if (!s.isString())
            return;
        const Matrix trm = tm_ * ctm_;
        if (have_y_ && std::fabs(trm.f - y_) > 1.0) {
            out_.lines.push_back(line_);
            line_.clear();
        } else if (moved_ && !line_.empty() && line_.back() != ' ') {
            line_ += ' ';
        }
        have_y_ = true, y_ = trm.f, moved_ = false;
        static const Font fallback;
        line_ += (font_ ? *font_ : fallback).decode(s.getStringValue(), out_.unmappable);
    }

    void xobject(QPDFObjectHandle x) {
        if (!x.isStream())
            return;
        auto dict = x.getDict();
        const std::string sub = dict.getKey("/Subtype").isName() ? dict.getKey("/Subtype").getName() : "";
        if (sub == "/Image") {
            auto w = dict.getKey("/Width"), h = dict.getKey("/Height");
            const std::string img = std::to_string(w.isInteger() ? w.getIntValue() : 0) + "x" +
                                    std::to_string(h.isInteger() ? h.getIntValue() : 0) +
                                    " xxh3:" + raw_hash16(x);
            out_.images.push_back(img);
            hash_.add(img);
        } else if (sub == "/Form") {
            hash_.add("form:" + raw_hash16(x));
            if (depth_ >= kMaxFormDepth || !active_.insert(x.getObjGen()).second)
                return;
            Matrix m;
            auto mx = dict.getKey("/Matrix");
            if (mx.isArray() && mx.getArrayNItems() == 6) {
                auto v = mx.getArrayAsVector();
                auto n = [&](int i) { return v[i].isNumber() ? v[i].getNumericValue() : 0; };
                m = {n(0), n(1), n(2), n(3), n(4), n(5)};
            }
            auto res = dict.getKey("/Resources");
            Reader inner(out_, res.isDictionary() ? res : res_, m * ctm_, depth_ + 1, active_);
            inner.line_ = std::move(line_);
            inner.have_y_ = have_y_, inner.y_ = y_;
            try {
                QPDFObjectHandle::parseContentStream(x, &inner);
            } catch (const std::exception&) {
            }
            line_ = std::move(inner.line_);
            have_y_ = inner.have_y_, y_ = inner.y_;
            hash_.add(inner.digest().hex());
            active_.erase(x.getObjGen());
        }
    }

    PageContent& out_;
    QPDFObjectHandle res_;
    std::set<QPDFObjGen>& active_;
    int depth_;
    std::vector<QPDFObjectHandle> args_;
    Matrix ctm_, tm_, tlm_;
    std::vector<Matrix> stack_;
    double leading_ = 0, y_ = 0;
    bool have_y_ = false, moved_ = false;
    const Font* font_ = nullptr;
    std::map<QPDFObjGen, std::unique_ptr<Font>> fonts_;
    std::string line_;
    Hasher hash_;
};

// Whitespace collapsed and trimmed.
std::string collapse(const std::string& s) {
    std::string out;
    for (char c : s) {
        const bool space = c == ' ' || c == '\t' || c == '\r' || c == '\n';
        if (space && (out.empty() || out.back() == ' '))
            continue;
        out += space ? ' ' : c;
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

} // namespace

PageContent read_page(QPDFPageObjectHelper& page) {
    PageContent out;
    std::set<QPDFObjGen> active;
    auto res = page.getAttribute("/Resources", false);
    Reader r(out, res, {}, 0, active);
    try {
        page.parseContents(&r);
    } catch (const std::exception&) {
        out.lines.push_back("[content stream unreadable]");
    }
    out.lines.push_back(r.line());
    std::vector<std::string> lines;
    for (auto& l : out.lines)
        if (auto c = collapse(l); !c.empty())
            lines.push_back(std::move(c));
    out.lines = std::move(lines);
    out.fingerprint = r.digest();
    return out;
}

} // namespace semsplinter::pdf
