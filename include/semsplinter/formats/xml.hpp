#pragma once

// The generic XML module (docs/formats/xml.md): expat tokenizer plus an own span tree (D-26), identity
// matching, KeyedMerge on attributes, SequenceMerge on children, and a splice writer that re-emits untouched
// subtrees byte for byte. Dialects (SVG, ...) configure it through the hooks below instead of reimplementing
// it.

#include <semsplinter/format.hpp>
#include <semsplinter/hash.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace semsplinter::xml {

inline constexpr std::string_view kXmlNs = "http://www.w3.org/XML/1998/namespace";
inline constexpr std::string_view kXmlnsNs = "http://www.w3.org/2000/xmlns/";

struct QName {
    std::string uri, local, prefix; // prefix as written ("" for none)
    bool operator==(const QName&) const = default;
};

// Byte range in the side's original input encoding.
struct Span {
    std::uint32_t off = 0, len = 0;
    std::uint32_t end() const noexcept { return off + len; }
};

struct Attr {
    std::string qname; // as written: "xlink:href", "xmlns:p"
    QName name;        // resolved; a namespace declaration is {kXmlnsNs, prefix, "xmlns"}
    std::string value; // decoded and normalized by the parser
    Span raw, lead_ws; // raw: name through the closing quote
    char quote = '"';
    bool ns_decl = false;
};

struct Node {
    enum class Kind : std::uint8_t { document, element, text, cdata, comment, pi, doctype };
    Kind kind = Kind::element;
    std::string qname;                   // element name or PI target, as written
    QName name;                          // element: resolved name
    std::vector<Attr> attrs;             // source order, namespace declarations included
    std::vector<std::uint32_t> children; // node indices; formatting whitespace dropped (xml.md §4)
    std::string text;                    // decoded: text, CDATA, comment, PI data, raw DOCTYPE
    Span whole, start_tag, end_tag;      // end_tag.len == 0 for an empty-element tag
    std::uint32_t name_end = 0;          // element: end of the name in the start tag
    std::uint32_t attrs_end = 0;         // element: end of the last attribute (or of the name)
    Span lead_ws;                        // formatting whitespace before this node (xml.md §7)
    Span trail_ws;                       // element: formatting whitespace before its end tag
    Hash128 hash;                        // normalized subtree hash (xml.md §4)
    std::uint32_t parent = 0;
    bool preserve = false; // xml:space="preserve" in scope
    // Prefixes the subtree uses but does not declare, with their URIs (namespace fix-up, xml.md §7).
    std::vector<std::pair<std::string, std::string>> free_ns;
};

// An attribute that identifies an element among its siblings (xml.md §3.1).
struct IdRule {
    std::string_view uri, local;
};

// Text written around ours' version of a conflict under --conflict=mark (UTF-8).
struct Mark {
    std::string before, after;
};

// Where a conflict is marked.
struct MarkSite {
    const Node* parent = nullptr; // ours' node that holds the marked unit (the document node at the top)
    // Theirs' version as XML, with the namespace declarations it needs: the conflicting children, or the
    // owning element for an attribute conflict. Empty when theirs deleted it.
    std::string_view element;
};

// How the dialect types an attribute value (xml.md §6.1).
enum class AttrKind : std::uint8_t {
    opaque,     // one value
    id,         // the element's document-wide id: dependency checks and add-add renumbering (SPEC §2.3)
    token_list, // whitespace-separated set (`class`): tokens added and removed independently
    style_list, // `prop:value;...`: KeyedMerge over properties
};

enum class TextMode : std::uint8_t { opaque, words }; // words: two-sided text edits merge word by word

// Dialect hooks (xml.md §3.3). The base class is the generic dialect.
class Dialect {
public:
    virtual ~Dialect() = default;
    virtual std::string_view name() const { return "generic"; }
    // Confidence 0..100 that a document with this root element belongs to the dialect.
    virtual int probe(const Node& root) const;
    // Id attributes in priority order for the children of `parent`. One counts for a sibling group only
    // when its values are unique there on every side (default: xml:id, id, name, key, uid, guid, Include).
    virtual std::span<const IdRule> id_rules(const Node& parent) const;
    // Child order of `el` carries no meaning: both sides' insertions at one place are kept, ours first.
    virtual bool unordered(const Node& el) const;
    // Whitespace-only text under `el` is content rather than formatting.
    virtual bool significant_whitespace(const Node& el) const;
    // The marking of a conflict; `theirs` is theirs' alternative as XML text. Default: an XML comment.
    virtual Mark mark(std::string_view conflict_id, std::string_view theirs, const MarkSite& site) const;
    // An internal general entity in the DOCTYPE (xml.md §8); `count` and `bytes` are the totals so far, this
    // one included. External and parameter entities are always refused. Default: refused (SPEC §9).
    virtual bool accept_entity(std::string_view name, std::string_view value, std::size_t count,
                               std::size_t bytes) const;
    virtual AttrKind attr_kind(const Node& el, const Attr& a) const; // default: opaque
    virtual TextMode text_mode(const Node& el) const;                // default: opaque
    // The comparison form of a value (xml.md §4): equal forms are equal values. nullopt marks volatile state
    // (editor view settings): left out of hashes, diffs, and dumps; a two-sided change keeps ours, noted
    // `editor-state`. Default: token lists and styles sorted, anything else as is.
    virtual std::optional<std::string> normalize(const Node& el, const Attr& a) const;
    // A value both sides changed (base null when both added it), merged by type; nullopt: conflict.
    // Default: token lists and styles by attr_kind.
    virtual std::optional<std::string> merge_attr(const Node& el, const Attr* base, const Attr& ours,
                                                  const Attr& theirs) const;
    // Content computed from other content: never merged below this element; two-sided changes conflict
    // (`dependency`). Default: none.
    virtual bool is_derived(const Node& el) const;
    // The value with every id it references replaced by `id(old)`. Also how references are found (xml.md
    // §3.2). Default: no references.
    virtual std::string rewrite_ids(const Node& el, const Attr& a,
                                    const std::function<std::string(std::string_view)>& id) const;
    // Theirs' new id when both sides added different elements with this id (SPEC §2.3); `taken` holds every
    // id in use. Empty: keep it, so the elements conflict where they meet (the default).
    virtual std::string renumber(std::string_view id, const std::set<std::string>& taken) const;
};

// Generic XML.
std::unique_ptr<Format> make_format();
// An XML-based format with its own name, extensions, and dialect (for dialect modules such as SVG).
std::unique_ptr<Format> make_format(FormatInfo info, std::shared_ptr<const Dialect> dialect);
// The nodes of a document loaded by an XML-based format (nodes[0] is the document node).
std::span<const Node> nodes(const Document&);

} // namespace semsplinter::xml
