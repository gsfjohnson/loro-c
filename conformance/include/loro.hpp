/*
 * loro.hpp — conformance spike (RESHAPE Phases 0–2).
 *
 * A loro-cpp-SHAPED C++ API (namespace `loro`, shared_ptr ownership, typed `LoroValue`,
 * `init()` factories, callback interfaces) implemented as a thin wrapper over loro-c's
 * existing C ABI in <loro/loro.h>. Its purpose is to compile, link and pass loro-cpp's own
 * tests/test_*.cpp against this header, proving the API *shape* and the build wiring before
 * the full RESHAPE in-place rewrite is undertaken.
 *
 * Phase 0–1 surface: LoroDoc, LoroText, LoroMap, Cursor, StyleConfigMap, EphemeralStore, the
 * typed LoroValue/TextDelta/ContainerId families and the ContainerIdLike / LoroValueLike
 * interfaces, plus the typed-value FFI bridge (binary / integer-valued doubles survive).
 *
 * Phase 2 surface (this file): the remaining four container types — LoroList, LoroMovableList,
 * LoroTree (+ TreeId / TreeParentId), LoroCounter — with their accessors and the full
 * insert_* / set_* container matrix; ContainerId <-> "cid:" string conversion; and the
 * VersionVector / Frontiers wrappers plus the LoroDoc methods test_doc exercises (peer_id,
 * commit, state_vv/state_frontiers, vv<->frontiers, fork/fork_at, export/import variants,
 * has_container, typed get_deep_value).
 *
 * Names match ../loro-cpp/build/generated/loro.hpp field-for-field (uniffi quirks included:
 * `delete_()`, `Side::kRight`, `TreeParentId::kNode{id}`, `ContainerType::kUnknown{kind}`,
 * `LoroValue::kI64`). C handle types from <loro/loro.h> live in the global namespace and are
 * referred to here with a leading `::` (e.g. `::LoroText`) to disambiguate from the `loro::`
 * wrapper classes of the same name.
 *
 * Construction discipline: every wrapper holds a raw C handle with a `loro_*_free` deleter and
 * a private constructor. All cross-class construction (and the detached-container `take`)
 * routes through `detail::Factory`, which is the sole `friend` each wrapper grants. Factory is
 * defined after all wrapper classes; methods that use it are therefore defined out-of-line.
 */
#ifndef LORO_CONFORMANCE_LORO_HPP
#define LORO_CONFORMANCE_LORO_HPP

#include <loro/loro.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace loro {

// ------------------------------------------------------------------ error

/// Base loro exception (loro-cpp shape). The conformance spike uses a single class; the full
/// loro_error:: subclass hierarchy is deferred to a later phase.
struct LoroError : std::runtime_error {
    LoroError() : std::runtime_error("") {}
    explicit LoroError(const std::string &what_arg) : std::runtime_error(what_arg) {}
    ~LoroError() override = default;
};

// ----------------------------------------------------- forward declarations

struct LoroDoc;
struct LoroText;
struct LoroMap;
struct LoroList;
struct LoroMovableList;
struct LoroTree;
struct LoroCounter;
struct Cursor;
struct StyleConfigMap;
struct LoroValue;
struct ContainerId;
struct ValueOrContainer;
struct Subscription;
struct EphemeralStore;
struct EphemeralStoreEvent;
struct EphemeralSubscriber;
struct VersionVector;
struct Frontiers;

namespace detail {
struct Factory;  // privileged construction helper; defined after the wrapper classes.
}

// --------------------------------------------------------- enums & POD types

enum class ExpandType : int32_t {
    kBefore = 1,
    kAfter = 2,
    kBoth = 3,
    kNone = 4,
};

enum class Side : int32_t {
    kLeft = 1,
    kMiddle = 2,
    kRight = 3,
};

struct ContainerType {
    struct kText {};
    struct kMap {};
    struct kList {};
    struct kMovableList {};
    struct kTree {};
    struct kCounter {};
    struct kUnknown {
        uint8_t kind;
    };
    ContainerType(kText variant) : variant(variant) {}
    ContainerType(kMap variant) : variant(variant) {}
    ContainerType(kList variant) : variant(variant) {}
    ContainerType(kMovableList variant) : variant(variant) {}
    ContainerType(kTree variant) : variant(variant) {}
    ContainerType(kCounter variant) : variant(variant) {}
    ContainerType(kUnknown variant) : variant(variant) {}

    const std::variant<kText, kMap, kList, kMovableList, kTree, kCounter, kUnknown> &
    get_variant() const {
        return variant;
    }

private:
    std::variant<kText, kMap, kList, kMovableList, kTree, kCounter, kUnknown> variant;
};

struct ContainerId {
    struct kRoot {
        std::string name;
        ContainerType container_type;
    };
    struct kNormal {
        uint64_t peer;
        int32_t counter;
        ContainerType container_type;
    };
    ContainerId(kRoot variant) : variant(std::move(variant)) {}
    ContainerId(kNormal variant) : variant(std::move(variant)) {}

    const std::variant<kRoot, kNormal> &get_variant() const { return variant; }

private:
    std::variant<kRoot, kNormal> variant;
};

/// A tree node id (uniffi `TreeID`).
struct TreeId {
    uint64_t peer;
    int32_t counter;
};

/// The parent of a tree node (uniffi `TreeParentId`): a node, the root, deleted, or
/// not-yet-existent.
struct TreeParentId {
    struct kNode {
        TreeId id;
    };
    struct kRoot {};
    struct kDeleted {};
    struct kUnexist {};
    TreeParentId(kNode variant) : variant(std::move(variant)) {}
    TreeParentId(kRoot variant) : variant(std::move(variant)) {}
    TreeParentId(kDeleted variant) : variant(std::move(variant)) {}
    TreeParentId(kUnexist variant) : variant(std::move(variant)) {}

    const std::variant<kNode, kRoot, kDeleted, kUnexist> &get_variant() const { return variant; }

private:
    std::variant<kNode, kRoot, kDeleted, kUnexist> variant;
};

struct CounterSpan {
    int32_t start;
    int32_t end;
};

struct ImportStatus {
    std::unordered_map<uint64_t, CounterSpan> success;
    std::optional<std::unordered_map<uint64_t, CounterSpan>> pending;
};

struct StyleConfig {
    ExpandType expand;
};

struct AbsolutePosition {
    uint32_t pos;
    Side side;
};

// ------------------------------------------------------------- typed values

struct LoroValue {
    struct kNull {};
    struct kBool {
        bool value;
    };
    struct kDouble {
        double value;
    };
    struct kI64 {
        int64_t value;
    };
    struct kBinary {
        std::vector<uint8_t> value;
    };
    struct kString {
        std::string value;
    };
    struct kList {
        std::vector<LoroValue> value;
    };
    struct kMap {
        std::unordered_map<std::string, LoroValue> value;
    };
    struct kContainer {
        ContainerId value;
    };
    LoroValue(kNull variant) : variant(std::move(variant)) {}
    LoroValue(kBool variant) : variant(std::move(variant)) {}
    LoroValue(kDouble variant) : variant(std::move(variant)) {}
    LoroValue(kI64 variant) : variant(std::move(variant)) {}
    LoroValue(kBinary variant) : variant(std::move(variant)) {}
    LoroValue(kString variant) : variant(std::move(variant)) {}
    LoroValue(kList variant) : variant(std::move(variant)) {}
    LoroValue(kMap variant) : variant(std::move(variant)) {}
    LoroValue(kContainer variant) : variant(std::move(variant)) {}

    LoroValue(const LoroValue &other) : variant(other.variant) {}
    LoroValue(LoroValue &&other) noexcept : variant(std::move(other.variant)) {}
    LoroValue &operator=(const LoroValue &other) {
        variant = other.variant;
        return *this;
    }
    LoroValue &operator=(LoroValue &&other) noexcept {
        variant = std::move(other.variant);
        return *this;
    }

    const std::variant<kNull, kBool, kDouble, kI64, kBinary, kString, kList, kMap, kContainer> &
    get_variant() const {
        return variant;
    }

private:
    std::variant<kNull, kBool, kDouble, kI64, kBinary, kString, kList, kMap, kContainer> variant;
};

struct TextDelta {
    struct kRetain {
        uint32_t retain;
        std::optional<std::unordered_map<std::string, LoroValue>> attributes;
    };
    struct kInsert {
        std::string insert;
        std::optional<std::unordered_map<std::string, LoroValue>> attributes;
    };
    struct kDelete {
        uint32_t delete_;
    };
    TextDelta(kRetain variant) : variant(std::move(variant)) {}
    TextDelta(kInsert variant) : variant(std::move(variant)) {}
    TextDelta(kDelete variant) : variant(std::move(variant)) {}

    const std::variant<kRetain, kInsert, kDelete> &get_variant() const { return variant; }

private:
    std::variant<kRetain, kInsert, kDelete> variant;
};

// --------------------------------------------------------------- interfaces

struct ContainerIdLike {
    virtual ~ContainerIdLike() {}
    virtual ContainerId as_container_id(ContainerType ty) = 0;
};

struct LoroValueLike {
    virtual ~LoroValueLike() {}
    virtual LoroValue as_loro_value() = 0;
};

struct PosQueryResult {
    std::shared_ptr<Cursor> update;
    AbsolutePosition current;
};

// ------------------------------------------------------------------- detail

namespace detail {

/// Current thread's last-error message, or an empty string.
inline std::string last_error_message() {
    const char *m = loro_last_error_message();
    return m ? std::string(m) : std::string();
}

/// Throws LoroError if `s` is not LORO_OK.
inline void check(LoroStatus s) {
    if (s != LORO_OK) {
        std::string msg = last_error_message();
        throw LoroError(msg.empty()
                            ? ("loro error (status " + std::to_string(static_cast<int>(s)) + ")")
                            : msg);
    }
}

/// RAII holder for a LoroBytes buffer returned across the FFI boundary.
class Bytes {
public:
    Bytes() : raw_{nullptr, 0, 0} {}
    ~Bytes() { loro_bytes_free(raw_); }
    Bytes(const Bytes &) = delete;
    Bytes &operator=(const Bytes &) = delete;

    LoroBytes *out() { return &raw_; }

    std::string to_string() const {
        if (raw_.data == nullptr || raw_.len == 0) return {};
        return std::string(reinterpret_cast<const char *>(raw_.data), raw_.len);
    }
    std::vector<std::uint8_t> to_vector() const {
        if (raw_.data == nullptr || raw_.len == 0) return {};
        return std::vector<std::uint8_t>(raw_.data, raw_.data + raw_.len);
    }

private:
    LoroBytes raw_;
};

inline LoroSide to_c_side(Side s) {
    switch (s) {
        case Side::kLeft: return LORO_SIDE_LEFT;
        case Side::kMiddle: return LORO_SIDE_MIDDLE;
        case Side::kRight: return LORO_SIDE_RIGHT;
    }
    return LORO_SIDE_MIDDLE;
}

inline Side from_c_side(LoroSide s) {
    switch (s) {
        case LORO_SIDE_LEFT: return Side::kLeft;
        case LORO_SIDE_RIGHT: return Side::kRight;
        case LORO_SIDE_MIDDLE: return Side::kMiddle;
    }
    return Side::kMiddle;
}

inline LoroExpandType to_c_expand(ExpandType e) {
    switch (e) {
        case ExpandType::kBefore: return LORO_EXPAND_BEFORE;
        case ExpandType::kAfter: return LORO_EXPAND_AFTER;
        case ExpandType::kBoth: return LORO_EXPAND_BOTH;
        case ExpandType::kNone: return LORO_EXPAND_NONE;
    }
    return LORO_EXPAND_AFTER;
}

/// Extracts the root-container name from a ContainerIdLike. The conformance tests only use
/// root (by-name) ids.
inline std::string root_name(const std::shared_ptr<ContainerIdLike> &id, ContainerType ty) {
    ContainerId cid = id->as_container_id(std::move(ty));
    if (auto *root = std::get_if<ContainerId::kRoot>(&cid.get_variant())) {
        return root->name;
    }
    throw LoroError("conformance spike: only root (by-name) ContainerId is supported here");
}

// ---- ContainerType / ContainerId <-> "cid:" string (RESHAPE Phase 2) --------
//
// Matches loro-common 1.13.1 (`ContainerType`/`ContainerID` Display + TryFrom<&str>):
//   root:   "cid:root-{name}:{Type}"      (the LAST ':' splits name from type — names may
//                                           themselves contain ':')
//   normal: "cid:{counter}@{peer}:{Type}"
// Types render as Text/Map/List/MovableList/Tree/Counter/Unknown(k).

inline const char *container_type_name(const ContainerType &ty) {
    const auto &v = ty.get_variant();
    if (std::holds_alternative<ContainerType::kText>(v)) return "Text";
    if (std::holds_alternative<ContainerType::kMap>(v)) return "Map";
    if (std::holds_alternative<ContainerType::kList>(v)) return "List";
    if (std::holds_alternative<ContainerType::kMovableList>(v)) return "MovableList";
    if (std::holds_alternative<ContainerType::kTree>(v)) return "Tree";
    if (std::holds_alternative<ContainerType::kCounter>(v)) return "Counter";
    return "Unknown";  // kUnknown is not exercised by the conformance tests.
}

inline ContainerType container_type_from_name(const std::string &name) {
    if (name == "Text") return ContainerType(ContainerType::kText{});
    if (name == "Map") return ContainerType(ContainerType::kMap{});
    if (name == "List") return ContainerType(ContainerType::kList{});
    if (name == "MovableList") return ContainerType(ContainerType::kMovableList{});
    if (name == "Tree") return ContainerType(ContainerType::kTree{});
    if (name == "Counter") return ContainerType(ContainerType::kCounter{});
    return ContainerType(ContainerType::kUnknown{0});
}

inline ContainerId cid_string_to_container_id(const std::string &cid) {
    if (cid.rfind("cid:", 0) != 0) throw LoroError("not a cid string: " + cid);
    std::string s = cid.substr(4);
    if (s.rfind("root-", 0) == 0) {
        s = s.substr(5);
        std::size_t split = s.rfind(':');
        if (split == std::string::npos || split == 0)
            throw LoroError("malformed root cid: " + cid);
        ContainerType ty = container_type_from_name(s.substr(split + 1));
        return ContainerId(ContainerId::kRoot{s.substr(0, split), std::move(ty)});
    }
    // normal: {counter}@{peer}:{Type}
    std::size_t at = s.find('@');
    std::size_t colon = s.rfind(':');
    if (at == std::string::npos || colon == std::string::npos || at >= colon)
        throw LoroError("malformed normal cid: " + cid);
    int32_t counter = static_cast<int32_t>(std::stoll(s.substr(0, at)));
    uint64_t peer = static_cast<uint64_t>(std::stoull(s.substr(at + 1, colon - at - 1)));
    ContainerType ty = container_type_from_name(s.substr(colon + 1));
    return ContainerId(ContainerId::kNormal{peer, counter, std::move(ty)});
}

inline std::string container_id_to_cid_string(const ContainerId &id) {
    const auto &v = id.get_variant();
    if (auto *r = std::get_if<ContainerId::kRoot>(&v)) {
        return "cid:root-" + r->name + ":" + container_type_name(r->container_type);
    }
    const auto &n = std::get<ContainerId::kNormal>(v);
    return "cid:" + std::to_string(n.counter) + "@" + std::to_string(n.peer) + ":" +
           container_type_name(n.container_type);
}

inline ::LoroTreeID to_c_tree_id(const TreeId &id) {
    ::LoroTreeID c;
    c.peer = id.peer;
    c.counter = id.counter;
    return c;
}

inline TreeId from_c_tree_id(const ::LoroTreeID &c) { return TreeId{c.peer, c.counter}; }

// ---- minimal JSON support (rich-text delta round-trip) ----------------------

/// A tiny JSON value used solely to rebuild TextDelta + scalar attributes from the C ABI's
/// JSON delta output. Not a general-purpose JSON type.
struct JsonValue {
    enum class Kind { Null, Bool, Int, Double, String, Array, Object };
    Kind kind = Kind::Null;
    bool b = false;
    int64_t i = 0;
    double d = 0.0;
    std::string s;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;  // small, insertion-ordered

    const JsonValue *find(const std::string &key) const {
        for (const auto &kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string &src) : s_(src), n_(src.size()) {}

    JsonValue parse() {
        skip_ws();
        JsonValue v = parse_value();
        skip_ws();
        return v;
    }

private:
    const std::string &s_;
    size_t n_;
    size_t i_ = 0;

    [[noreturn]] void err(const char *m) {
        throw LoroError(std::string("conformance JSON parse error: ") + m);
    }
    void skip_ws() {
        while (i_ < n_ && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) {
            ++i_;
        }
    }
    char peek() const { return i_ < n_ ? s_[i_] : '\0'; }

    JsonValue parse_value() {
        skip_ws();
        switch (peek()) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': {
                JsonValue v;
                v.kind = JsonValue::Kind::String;
                v.s = parse_string();
                return v;
            }
            case 't':
            case 'f': return parse_bool();
            case 'n': return parse_null();
            default: return parse_number();
        }
    }

    JsonValue parse_object() {
        JsonValue v;
        v.kind = JsonValue::Kind::Object;
        ++i_;  // consume '{'
        skip_ws();
        if (peek() == '}') {
            ++i_;
            return v;
        }
        for (;;) {
            skip_ws();
            if (peek() != '"') err("expected object key");
            std::string key = parse_string();
            skip_ws();
            if (peek() != ':') err("expected ':'");
            ++i_;
            v.obj.emplace_back(std::move(key), parse_value());
            skip_ws();
            char c = peek();
            if (c == ',') {
                ++i_;
                continue;
            }
            if (c == '}') {
                ++i_;
                break;
            }
            err("expected ',' or '}'");
        }
        return v;
    }

    JsonValue parse_array() {
        JsonValue v;
        v.kind = JsonValue::Kind::Array;
        ++i_;  // consume '['
        skip_ws();
        if (peek() == ']') {
            ++i_;
            return v;
        }
        for (;;) {
            v.arr.push_back(parse_value());
            skip_ws();
            char c = peek();
            if (c == ',') {
                ++i_;
                continue;
            }
            if (c == ']') {
                ++i_;
                break;
            }
            err("expected ',' or ']'");
        }
        return v;
    }

    std::string parse_string() {
        if (peek() != '"') err("expected '\"'");
        ++i_;
        std::string out;
        while (i_ < n_) {
            char c = s_[i_++];
            if (c == '"') return out;
            if (c == '\\') {
                if (i_ >= n_) err("truncated escape");
                char e = s_[i_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (i_ + 4 > n_) err("truncated \\u escape");
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s_[i_++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                            else err("bad hex digit");
                        }
                        // BMP-only UTF-8 encode (sufficient for the conformance tests).
                        if (cp < 0x80) {
                            out.push_back(static_cast<char>(cp));
                        } else if (cp < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: err("unknown escape");
                }
            } else {
                out.push_back(c);
            }
        }
        err("unterminated string");
    }

    JsonValue parse_bool() {
        JsonValue v;
        v.kind = JsonValue::Kind::Bool;
        if (s_.compare(i_, 4, "true") == 0) {
            v.b = true;
            i_ += 4;
        } else if (s_.compare(i_, 5, "false") == 0) {
            v.b = false;
            i_ += 5;
        } else {
            err("bad literal");
        }
        return v;
    }

    JsonValue parse_null() {
        if (s_.compare(i_, 4, "null") == 0) {
            i_ += 4;
            JsonValue v;
            v.kind = JsonValue::Kind::Null;
            return v;
        }
        err("bad literal");
    }

    JsonValue parse_number() {
        size_t start = i_;
        bool is_double = false;
        if (peek() == '-') ++i_;
        while (i_ < n_) {
            char c = s_[i_];
            if (c >= '0' && c <= '9') {
                ++i_;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                is_double = true;
                ++i_;
            } else {
                break;
            }
        }
        if (i_ == start) err("bad number");
        std::string num = s_.substr(start, i_ - start);
        JsonValue v;
        if (is_double) {
            v.kind = JsonValue::Kind::Double;
            v.d = std::stod(num);
        } else {
            v.kind = JsonValue::Kind::Int;
            v.i = std::stoll(num);
        }
        return v;
    }
};

inline JsonValue parse_json(const std::string &s) { return JsonParser(s).parse(); }

/// JsonValue -> typed LoroValue. Numbers without a fractional/exponent part become kI64,
/// otherwise kDouble (used only for rich-text delta attributes, which the tests keep scalar).
inline LoroValue json_to_value(const JsonValue &j) {
    using K = JsonValue::Kind;
    switch (j.kind) {
        case K::Null: return LoroValue(LoroValue::kNull{});
        case K::Bool: return LoroValue(LoroValue::kBool{j.b});
        case K::Int: return LoroValue(LoroValue::kI64{j.i});
        case K::Double: return LoroValue(LoroValue::kDouble{j.d});
        case K::String: return LoroValue(LoroValue::kString{j.s});
        case K::Array: {
            std::vector<LoroValue> out;
            out.reserve(j.arr.size());
            for (const auto &e : j.arr) out.push_back(json_to_value(e));
            return LoroValue(LoroValue::kList{std::move(out)});
        }
        case K::Object: {
            std::unordered_map<std::string, LoroValue> out;
            for (const auto &kv : j.obj) out.emplace(kv.first, json_to_value(kv.second));
            return LoroValue(LoroValue::kMap{std::move(out)});
        }
    }
    return LoroValue(LoroValue::kNull{});
}

inline void json_escape(const std::string &in, std::string &out) {
    out.push_back('"');
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

/// Typed LoroValue -> JSON scalar, for mark()'s value. Supports the scalar kinds the tests
/// produce; compound/binary/container values throw.
inline std::string value_to_json(const LoroValue &v) {
    return std::visit(
        [](auto &&alt) -> std::string {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, LoroValue::kNull>) {
                return "null";
            } else if constexpr (std::is_same_v<T, LoroValue::kBool>) {
                return alt.value ? "true" : "false";
            } else if constexpr (std::is_same_v<T, LoroValue::kI64>) {
                return std::to_string(alt.value);
            } else if constexpr (std::is_same_v<T, LoroValue::kDouble>) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.17g", alt.value);
                return buf;
            } else if constexpr (std::is_same_v<T, LoroValue::kString>) {
                std::string out;
                json_escape(alt.value, out);
                return out;
            } else {
                throw LoroError("conformance spike: only scalar LoroValue kinds cross as JSON");
            }
        },
        v.get_variant());
}

// ---- typed-value bridge -----------------------------------------------------
//
// Translates between the C++ typed `loro::LoroValue` variant and the opaque C `::LoroValue`
// handle from <loro/loro.h>, so values cross the FFI boundary WITHOUT JSON (binary stays
// binary; integer-valued doubles stay doubles).

/// RAII owner for a `::LoroValue*` returned by / built for the C ABI; frees on scope exit.
class CValue {
public:
    explicit CValue(::LoroValue *v) : v_(v) {}
    ~CValue() { loro_value_free(v_); }
    CValue(const CValue &) = delete;
    CValue &operator=(const CValue &) = delete;
    ::LoroValue *get() const { return v_; }

private:
    ::LoroValue *v_;
};

/// Parses a JSON array-of-strings (as produced by `loro_*_keys`) into a vector.
inline std::vector<std::string> parse_string_array(const std::string &json) {
    std::vector<std::string> out;
    if (json.empty()) return out;
    JsonValue root = parse_json(json);
    if (root.kind != JsonValue::Kind::Array) return out;
    for (const auto &e : root.arr) {
        if (e.kind == JsonValue::Kind::String) out.push_back(e.s);
    }
    return out;
}

/// Builds an owned `::LoroValue*` from a typed `loro::LoroValue` (no JSON). Caller frees the
/// result with `loro_value_free` (or via [`CValue`]).
inline ::LoroValue *to_c_value(const LoroValue &v) {
    return std::visit(
        [](auto &&alt) -> ::LoroValue * {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, LoroValue::kNull>) {
                return loro_value_new_null();
            } else if constexpr (std::is_same_v<T, LoroValue::kBool>) {
                return loro_value_new_bool(alt.value);
            } else if constexpr (std::is_same_v<T, LoroValue::kDouble>) {
                return loro_value_new_double(alt.value);
            } else if constexpr (std::is_same_v<T, LoroValue::kI64>) {
                return loro_value_new_i64(alt.value);
            } else if constexpr (std::is_same_v<T, LoroValue::kString>) {
                return loro_value_new_string(alt.value.data(), alt.value.size());
            } else if constexpr (std::is_same_v<T, LoroValue::kBinary>) {
                return loro_value_new_binary(alt.value.data(), alt.value.size());
            } else if constexpr (std::is_same_v<T, LoroValue::kList>) {
                ::LoroValue *list = loro_value_new_list();
                if (!list) throw LoroError("loro_value_new_list returned null");
                for (const auto &el : alt.value) {
                    CValue child(to_c_value(el));
                    check(loro_value_list_push(list, child.get()));
                }
                return list;
            } else if constexpr (std::is_same_v<T, LoroValue::kMap>) {
                ::LoroValue *map = loro_value_new_map();
                if (!map) throw LoroError("loro_value_new_map returned null");
                for (const auto &kv : alt.value) {
                    CValue child(to_c_value(kv.second));
                    check(loro_value_map_insert(map, kv.first.data(), kv.first.size(),
                                                child.get()));
                }
                return map;
            } else {  // kContainer
                throw LoroError("conformance spike: container-valued LoroValue cannot be sent to "
                                "loro-c");
            }
        },
        v.get_variant());
}

/// Reconstructs a typed `loro::LoroValue` from an owned `::LoroValue*` (no JSON). Does not take
/// ownership of `cv` (the caller frees it).
inline LoroValue from_c_value(const ::LoroValue *cv) {
    switch (loro_value_get_type(cv)) {
        case LORO_VALUE_NULL:
            return LoroValue(LoroValue::kNull{});
        case LORO_VALUE_BOOL: {
            bool b = false;
            loro_value_as_bool(cv, &b);
            return LoroValue(LoroValue::kBool{b});
        }
        case LORO_VALUE_DOUBLE: {
            double d = 0.0;
            loro_value_as_double(cv, &d);
            return LoroValue(LoroValue::kDouble{d});
        }
        case LORO_VALUE_I64: {
            int64_t i = 0;
            loro_value_as_i64(cv, &i);
            return LoroValue(LoroValue::kI64{i});
        }
        case LORO_VALUE_STRING: {
            Bytes b;
            check(loro_value_as_string(cv, b.out()));
            return LoroValue(LoroValue::kString{b.to_string()});
        }
        case LORO_VALUE_BINARY: {
            Bytes b;
            check(loro_value_as_binary(cv, b.out()));
            return LoroValue(LoroValue::kBinary{b.to_vector()});
        }
        case LORO_VALUE_LIST: {
            std::vector<LoroValue> out;
            size_t n = loro_value_list_len(cv);
            out.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                ::LoroValue *el = loro_value_list_get(cv, i);
                if (!el) throw LoroError("loro_value_list_get returned null");
                CValue g(el);
                out.push_back(from_c_value(el));
            }
            return LoroValue(LoroValue::kList{std::move(out)});
        }
        case LORO_VALUE_MAP: {
            std::unordered_map<std::string, LoroValue> out;
            Bytes kb;
            check(loro_value_map_keys(cv, kb.out()));
            for (const auto &key : parse_string_array(kb.to_string())) {
                ::LoroValue *mv = loro_value_map_get(cv, key.data(), key.size());
                if (!mv) throw LoroError("loro_value_map_get returned null");
                CValue g(mv);
                out.emplace(key, from_c_value(mv));
            }
            return LoroValue(LoroValue::kMap{std::move(out)});
        }
        case LORO_VALUE_CONTAINER: {
            Bytes b;
            check(loro_value_as_container(cv, b.out()));
            return LoroValue(LoroValue::kContainer{cid_string_to_container_id(b.to_string())});
        }
        default:
            throw LoroError("unknown LoroValue type");
    }
}

}  // namespace detail

// -------------------------------------------------------------- reference types

struct Cursor {
    ~Cursor() { loro_cursor_free(raw_); }
    Cursor(const Cursor &) = delete;
    Cursor &operator=(const Cursor &) = delete;

private:
    explicit Cursor(::LoroCursor *raw) : raw_(raw) {}
    ::LoroCursor *raw_;
    friend struct LoroText;
    friend struct LoroDoc;
};

struct LoroText {
    /// Constructs a *detached* text container (loro-c: `loro_container_new`). Attach it by
    /// passing it to a parent's `insert_text_container`, which consumes it and returns the
    /// attached handle. A detached text is not editable until attached.
    static std::shared_ptr<LoroText> init() {
        ::LoroContainer *c = loro_container_new(LORO_CONTAINER_TEXT);
        if (!c) throw LoroError("loro_container_new(Text) returned null");
        return std::shared_ptr<LoroText>(new LoroText(c));
    }

    void insert(uint32_t pos, const std::string &s) {
        detail::check(loro_text_insert(raw_, pos, s.data(), s.size()));
    }
    void push_str(const std::string &s) {
        detail::check(loro_text_push_str(raw_, s.data(), s.size()));
    }
    void delete_(uint32_t pos, uint32_t len) {
        detail::check(loro_text_delete(raw_, pos, len));
    }
    std::string splice(uint32_t pos, uint32_t len, const std::string &s) {
        detail::Bytes removed;
        detail::check(loro_text_splice(raw_, pos, len, s.data(), s.size(), removed.out()));
        return removed.to_string();
    }
    std::string slice(uint32_t start_index, uint32_t end_index) {
        detail::Bytes b;
        detail::check(loro_text_slice(raw_, start_index, end_index, b.out()));
        return b.to_string();
    }
    uint32_t len_unicode() { return static_cast<uint32_t>(loro_text_len_unicode(raw_)); }
    uint32_t len_utf16() { return static_cast<uint32_t>(loro_text_len_utf16(raw_)); }
    uint32_t len_utf8() { return static_cast<uint32_t>(loro_text_len_utf8(raw_)); }

    std::string to_string() const {
        detail::Bytes b;
        detail::check(loro_text_to_string(raw_, b.out()));
        return b.to_string();
    }

    ContainerId id() {
        detail::Bytes b;
        detail::check(loro_text_id(raw_, b.out()));
        return detail::cid_string_to_container_id(b.to_string());
    }

    void mark(uint32_t from, uint32_t to, const std::string &key,
              const std::shared_ptr<LoroValueLike> &value) {
        LoroValue v = value->as_loro_value();
        std::string json = detail::value_to_json(v);
        detail::check(loro_text_mark(raw_, from, to, key.data(), key.size(), json.data(),
                                     json.size()));
    }
    void unmark(uint32_t from, uint32_t to, const std::string &key) {
        detail::check(loro_text_unmark(raw_, from, to, key.data(), key.size()));
    }

    std::vector<TextDelta> to_delta() {
        detail::Bytes b;
        detail::check(loro_text_to_delta(raw_, b.out()));
        std::vector<TextDelta> out;
        std::string json = b.to_string();
        if (json.empty()) return out;
        detail::JsonValue root = detail::parse_json(json);
        if (root.kind != detail::JsonValue::Kind::Array) return out;
        for (const auto &item : root.arr) {
            if (item.kind != detail::JsonValue::Kind::Object) continue;
            const detail::JsonValue *ins = item.find("insert");
            const detail::JsonValue *ret = item.find("retain");
            const detail::JsonValue *del = item.find("delete");
            const detail::JsonValue *attrs = item.find("attributes");

            std::optional<std::unordered_map<std::string, LoroValue>> attr_map;
            if (attrs && attrs->kind == detail::JsonValue::Kind::Object) {
                std::unordered_map<std::string, LoroValue> m;
                for (const auto &kv : attrs->obj) {
                    m.emplace(kv.first, detail::json_to_value(kv.second));
                }
                attr_map = std::move(m);
            }

            if (ins && ins->kind == detail::JsonValue::Kind::String) {
                out.emplace_back(TextDelta::kInsert{ins->s, std::move(attr_map)});
            } else if (ret) {
                uint32_t n = static_cast<uint32_t>(
                    ret->kind == detail::JsonValue::Kind::Int ? ret->i
                                                              : static_cast<int64_t>(ret->d));
                out.emplace_back(TextDelta::kRetain{n, std::move(attr_map)});
            } else if (del) {
                uint32_t n = static_cast<uint32_t>(
                    del->kind == detail::JsonValue::Kind::Int ? del->i
                                                              : static_cast<int64_t>(del->d));
                out.emplace_back(TextDelta::kDelete{n});
            }
        }
        return out;
    }

    std::shared_ptr<Cursor> get_cursor(uint32_t pos, const Side &side) {
        ::LoroCursor *c = loro_text_get_cursor(raw_, pos, detail::to_c_side(side));
        if (!c) return nullptr;
        return std::shared_ptr<Cursor>(new Cursor(c));
    }

    bool is_attached() { return raw_ ? loro_text_is_attached(raw_) : false; }
    bool is_empty() { return loro_text_is_empty(raw_); }

    ~LoroText() {
        if (raw_) loro_text_free(raw_);
        else if (container_) loro_container_free(container_);
    }
    LoroText(const LoroText &) = delete;
    LoroText &operator=(const LoroText &) = delete;

private:
    explicit LoroText(::LoroText *raw) : raw_(raw) {}        // attached
    explicit LoroText(::LoroContainer *c) : container_(c) {}  // detached (from init())

    /// Releases the detached container for attachment (nulls it). Throws if not detached.
    ::LoroContainer *take_container() {
        if (!container_) throw LoroError("LoroText is not a detached container");
        ::LoroContainer *c = container_;
        container_ = nullptr;
        return c;
    }

    ::LoroText *raw_ = nullptr;             // attached editing handle
    ::LoroContainer *container_ = nullptr;  // detached container awaiting attach
    friend struct detail::Factory;
};

struct LoroMap {
    static std::shared_ptr<LoroMap> init() {
        ::LoroContainer *c = loro_container_new(LORO_CONTAINER_MAP);
        if (!c) throw LoroError("loro_container_new(Map) returned null");
        return std::shared_ptr<LoroMap>(new LoroMap(c));
    }

    void insert(const std::string &key, const std::shared_ptr<LoroValueLike> &value) {
        LoroValue v = value->as_loro_value();
        detail::CValue cv(detail::to_c_value(v));
        detail::check(loro_map_insert_value(map(), key.data(), key.size(), cv.get()));
    }

    std::shared_ptr<ValueOrContainer> get(const std::string &key);  // out-of-line

    void delete_(const std::string &key) {
        detail::check(loro_map_delete(map(), key.data(), key.size()));
    }

    uint32_t len() { return static_cast<uint32_t>(loro_map_len(map())); }
    bool is_empty() { return loro_map_is_empty(map()); }

    std::vector<std::string> keys() {
        detail::Bytes b;
        detail::check(loro_map_keys(map(), b.out()));
        return detail::parse_string_array(b.to_string());
    }

    std::vector<std::shared_ptr<ValueOrContainer>> values();  // out-of-line

    LoroValue get_deep_value() {
        ::LoroValue *cv = loro_map_get_deep_value(map());
        if (!cv) throw LoroError("loro_map_get_deep_value returned null");
        detail::CValue g(cv);
        return detail::from_c_value(cv);
    }

    ContainerId id() {
        detail::Bytes b;
        detail::check(loro_map_id(map(), b.out()));
        return detail::cid_string_to_container_id(b.to_string());
    }

    // Nested-container insertion (out-of-line; needs the complete child types + Factory).
    std::shared_ptr<LoroText> insert_text_container(const std::string &k,
                                                    std::shared_ptr<LoroText> c);
    std::shared_ptr<LoroMap> insert_map_container(const std::string &k,
                                                  std::shared_ptr<LoroMap> c);
    std::shared_ptr<LoroList> insert_list_container(const std::string &k,
                                                    std::shared_ptr<LoroList> c);
    std::shared_ptr<LoroMovableList> insert_movable_list_container(
        const std::string &k, std::shared_ptr<LoroMovableList> c);
    std::shared_ptr<LoroTree> insert_tree_container(const std::string &k,
                                                    std::shared_ptr<LoroTree> c);
    std::shared_ptr<LoroCounter> insert_counter_container(const std::string &k,
                                                          std::shared_ptr<LoroCounter> c);

    std::shared_ptr<LoroText> get_or_create_text_container(const std::string &k,
                                                           std::shared_ptr<LoroText> c);
    std::shared_ptr<LoroMap> get_or_create_map_container(const std::string &k,
                                                         std::shared_ptr<LoroMap> c);
    std::shared_ptr<LoroList> get_or_create_list_container(const std::string &k,
                                                           std::shared_ptr<LoroList> c);
    std::shared_ptr<LoroMovableList> get_or_create_movable_list_container(
        const std::string &k, std::shared_ptr<LoroMovableList> c);
    std::shared_ptr<LoroTree> get_or_create_tree_container(const std::string &k,
                                                           std::shared_ptr<LoroTree> c);
    std::shared_ptr<LoroCounter> get_or_create_counter_container(
        const std::string &k, std::shared_ptr<LoroCounter> c);

    ~LoroMap() {
        if (raw_) loro_map_free(raw_);
        else if (container_) loro_container_free(container_);
    }
    LoroMap(const LoroMap &) = delete;
    LoroMap &operator=(const LoroMap &) = delete;

private:
    explicit LoroMap(::LoroMap *raw) : raw_(raw) {}          // attached
    explicit LoroMap(::LoroContainer *c) : container_(c) {}  // detached (from init())

    ::LoroMap *map() const {
        if (!raw_) throw LoroError("LoroMap is detached (not attached to a document)");
        return raw_;
    }
    ::LoroContainer *take_container() {
        if (!container_) throw LoroError("LoroMap is not a detached container");
        ::LoroContainer *c = container_;
        container_ = nullptr;
        return c;
    }

    // Type-generic helpers shared by the named methods above (out-of-line).
    template <class C>
    std::shared_ptr<C> insert_container_by_key(const std::string &key, std::shared_ptr<C> child);
    template <class C>
    std::shared_ptr<C> get_or_create_container_by_key(const std::string &key,
                                                      std::shared_ptr<C> child);

    ::LoroMap *raw_ = nullptr;
    ::LoroContainer *container_ = nullptr;
    friend struct detail::Factory;
};

struct LoroList {
    static std::shared_ptr<LoroList> init() {
        ::LoroContainer *c = loro_container_new(LORO_CONTAINER_LIST);
        if (!c) throw LoroError("loro_container_new(List) returned null");
        return std::shared_ptr<LoroList>(new LoroList(c));
    }

    void insert(uint32_t pos, const std::shared_ptr<LoroValueLike> &value) {
        LoroValue v = value->as_loro_value();
        detail::CValue cv(detail::to_c_value(v));
        detail::check(loro_list_insert_value(list(), pos, cv.get()));
    }
    void push(const std::shared_ptr<LoroValueLike> &value) {
        LoroValue v = value->as_loro_value();
        detail::CValue cv(detail::to_c_value(v));
        detail::check(loro_list_push_value(list(), cv.get()));
    }
    std::optional<LoroValue> pop() {
        bool present = false;
        ::LoroValue *cv = loro_list_pop_value(list(), &present);
        if (!present) return std::nullopt;
        if (!cv) throw LoroError(detail::last_error_message());
        detail::CValue g(cv);
        return detail::from_c_value(cv);
    }
    void delete_(uint32_t pos, uint32_t len) {
        detail::check(loro_list_delete(list(), pos, len));
    }
    std::shared_ptr<ValueOrContainer> get(uint32_t index);  // out-of-line

    uint32_t len() { return static_cast<uint32_t>(loro_list_len(list())); }
    bool is_empty() { return loro_list_is_empty(list()); }
    void clear() { detail::check(loro_list_clear(list())); }

    std::vector<LoroValue> to_vec() {
        ::LoroValue *cv = loro_list_get_value(list());
        if (!cv) throw LoroError(detail::last_error_message());
        detail::CValue g(cv);
        LoroValue v = detail::from_c_value(cv);
        if (auto *l = std::get_if<LoroValue::kList>(&v.get_variant())) return l->value;
        throw LoroError("loro_list_get_value did not return a list");
    }

    LoroValue get_deep_value() {
        ::LoroValue *cv = loro_list_get_deep_value(list());
        if (!cv) throw LoroError(detail::last_error_message());
        detail::CValue g(cv);
        return detail::from_c_value(cv);
    }

    ContainerId id() {
        detail::Bytes b;
        detail::check(loro_list_id(list(), b.out()));
        return detail::cid_string_to_container_id(b.to_string());
    }

    std::shared_ptr<LoroText> insert_text_container(uint32_t pos, std::shared_ptr<LoroText> c);
    std::shared_ptr<LoroMap> insert_map_container(uint32_t pos, std::shared_ptr<LoroMap> c);
    std::shared_ptr<LoroList> insert_list_container(uint32_t pos, std::shared_ptr<LoroList> c);
    std::shared_ptr<LoroMovableList> insert_movable_list_container(
        uint32_t pos, std::shared_ptr<LoroMovableList> c);
    std::shared_ptr<LoroTree> insert_tree_container(uint32_t pos, std::shared_ptr<LoroTree> c);
    std::shared_ptr<LoroCounter> insert_counter_container(uint32_t pos,
                                                          std::shared_ptr<LoroCounter> c);

    ~LoroList() {
        if (raw_) loro_list_free(raw_);
        else if (container_) loro_container_free(container_);
    }
    LoroList(const LoroList &) = delete;
    LoroList &operator=(const LoroList &) = delete;

private:
    explicit LoroList(::LoroList *raw) : raw_(raw) {}
    explicit LoroList(::LoroContainer *c) : container_(c) {}

    ::LoroList *list() const {
        if (!raw_) throw LoroError("LoroList is detached (not attached to a document)");
        return raw_;
    }
    ::LoroContainer *take_container() {
        if (!container_) throw LoroError("LoroList is not a detached container");
        ::LoroContainer *c = container_;
        container_ = nullptr;
        return c;
    }
    template <class C>
    std::shared_ptr<C> insert_container_at(uint32_t pos, std::shared_ptr<C> child);

    ::LoroList *raw_ = nullptr;
    ::LoroContainer *container_ = nullptr;
    friend struct detail::Factory;
};

struct LoroMovableList {
    static std::shared_ptr<LoroMovableList> init() {
        ::LoroContainer *c = loro_container_new(LORO_CONTAINER_MOVABLE_LIST);
        if (!c) throw LoroError("loro_container_new(MovableList) returned null");
        return std::shared_ptr<LoroMovableList>(new LoroMovableList(c));
    }

    void insert(uint32_t pos, const std::shared_ptr<LoroValueLike> &value) {
        LoroValue v = value->as_loro_value();
        detail::CValue cv(detail::to_c_value(v));
        detail::check(loro_movable_list_insert_value(list(), pos, cv.get()));
    }
    void push(const std::shared_ptr<LoroValueLike> &value) {
        LoroValue v = value->as_loro_value();
        detail::CValue cv(detail::to_c_value(v));
        detail::check(loro_movable_list_push_value(list(), cv.get()));
    }
    void set(uint32_t pos, const std::shared_ptr<LoroValueLike> &value) {
        LoroValue v = value->as_loro_value();
        detail::CValue cv(detail::to_c_value(v));
        detail::check(loro_movable_list_set_value(list(), pos, cv.get()));
    }
    void mov(uint32_t from, uint32_t to) {
        detail::check(loro_movable_list_mov(list(), from, to));
    }
    std::shared_ptr<ValueOrContainer> pop();              // out-of-line
    std::shared_ptr<ValueOrContainer> get(uint32_t index);  // out-of-line

    void delete_(uint32_t pos, uint32_t len) {
        detail::check(loro_movable_list_delete(list(), pos, len));
    }
    uint32_t len() { return static_cast<uint32_t>(loro_movable_list_len(list())); }
    bool is_empty() { return loro_movable_list_is_empty(list()); }
    void clear() { detail::check(loro_movable_list_clear(list())); }

    std::vector<LoroValue> to_vec() {
        ::LoroValue *cv = loro_movable_list_get_value(list());
        if (!cv) throw LoroError(detail::last_error_message());
        detail::CValue g(cv);
        LoroValue v = detail::from_c_value(cv);
        if (auto *l = std::get_if<LoroValue::kList>(&v.get_variant())) return l->value;
        throw LoroError("loro_movable_list_get_value did not return a list");
    }

    LoroValue get_deep_value() {
        ::LoroValue *cv = loro_movable_list_get_deep_value(list());
        if (!cv) throw LoroError(detail::last_error_message());
        detail::CValue g(cv);
        return detail::from_c_value(cv);
    }

    ContainerId id() {
        detail::Bytes b;
        detail::check(loro_movable_list_id(list(), b.out()));
        return detail::cid_string_to_container_id(b.to_string());
    }

    std::shared_ptr<LoroText> insert_text_container(uint32_t pos, std::shared_ptr<LoroText> c);
    std::shared_ptr<LoroMap> insert_map_container(uint32_t pos, std::shared_ptr<LoroMap> c);
    std::shared_ptr<LoroList> insert_list_container(uint32_t pos, std::shared_ptr<LoroList> c);
    std::shared_ptr<LoroMovableList> insert_movable_list_container(
        uint32_t pos, std::shared_ptr<LoroMovableList> c);
    std::shared_ptr<LoroTree> insert_tree_container(uint32_t pos, std::shared_ptr<LoroTree> c);
    std::shared_ptr<LoroCounter> insert_counter_container(uint32_t pos,
                                                          std::shared_ptr<LoroCounter> c);

    std::shared_ptr<LoroText> set_text_container(uint32_t pos, std::shared_ptr<LoroText> c);
    std::shared_ptr<LoroMap> set_map_container(uint32_t pos, std::shared_ptr<LoroMap> c);
    std::shared_ptr<LoroList> set_list_container(uint32_t pos, std::shared_ptr<LoroList> c);
    std::shared_ptr<LoroMovableList> set_movable_list_container(
        uint32_t pos, std::shared_ptr<LoroMovableList> c);
    std::shared_ptr<LoroTree> set_tree_container(uint32_t pos, std::shared_ptr<LoroTree> c);
    std::shared_ptr<LoroCounter> set_counter_container(uint32_t pos,
                                                       std::shared_ptr<LoroCounter> c);

    ~LoroMovableList() {
        if (raw_) loro_movable_list_free(raw_);
        else if (container_) loro_container_free(container_);
    }
    LoroMovableList(const LoroMovableList &) = delete;
    LoroMovableList &operator=(const LoroMovableList &) = delete;

private:
    explicit LoroMovableList(::LoroMovableList *raw) : raw_(raw) {}
    explicit LoroMovableList(::LoroContainer *c) : container_(c) {}

    ::LoroMovableList *list() const {
        if (!raw_) throw LoroError("LoroMovableList is detached (not attached to a document)");
        return raw_;
    }
    ::LoroContainer *take_container() {
        if (!container_) throw LoroError("LoroMovableList is not a detached container");
        ::LoroContainer *c = container_;
        container_ = nullptr;
        return c;
    }
    template <class C>
    std::shared_ptr<C> insert_container_at(uint32_t pos, std::shared_ptr<C> child);
    template <class C>
    std::shared_ptr<C> set_container_at(uint32_t pos, std::shared_ptr<C> child);

    ::LoroMovableList *raw_ = nullptr;
    ::LoroContainer *container_ = nullptr;
    friend struct detail::Factory;
};

struct LoroTree {
    static std::shared_ptr<LoroTree> init() {
        ::LoroContainer *c = loro_container_new(LORO_CONTAINER_TREE);
        if (!c) throw LoroError("loro_container_new(Tree) returned null");
        return std::shared_ptr<LoroTree>(new LoroTree(c));
    }

    TreeId create(TreeParentId parent) {
        ::LoroTreeID pid;
        bool is_node = parent_node(parent, pid);
        ::LoroTreeID out;
        detail::check(loro_tree_create(tree(), is_node ? &pid : nullptr, &out));
        return detail::from_c_tree_id(out);
    }

    TreeId create_at(TreeParentId parent, uint32_t index) {
        ::LoroTreeID pid;
        bool is_node = parent_node(parent, pid);
        ::LoroTreeID out;
        detail::check(loro_tree_create_at(tree(), is_node ? &pid : nullptr, index, &out));
        return detail::from_c_tree_id(out);
    }

    void mov(const TreeId &target, TreeParentId parent) {
        ::LoroTreeID pid;
        bool is_node = parent_node(parent, pid);
        detail::check(loro_tree_mov(tree(), detail::to_c_tree_id(target), is_node ? &pid : nullptr));
    }

    void delete_(const TreeId &target) {
        detail::check(loro_tree_delete(tree(), detail::to_c_tree_id(target)));
    }

    bool contains(const TreeId &target) {
        return loro_tree_contains(tree(), detail::to_c_tree_id(target));
    }

    bool is_node_deleted(const TreeId &target) {
        bool out = false;
        detail::check(loro_tree_is_node_deleted(tree(), detail::to_c_tree_id(target), &out));
        return out;
    }

    TreeParentId parent(const TreeId &target) {
        LoroTreeParentKind kind;
        ::LoroTreeID node;
        if (!loro_tree_parent(tree(), detail::to_c_tree_id(target), &kind, &node)) {
            return TreeParentId(TreeParentId::kUnexist{});
        }
        switch (kind) {
            case LORO_TREE_PARENT_ROOT: return TreeParentId(TreeParentId::kRoot{});
            case LORO_TREE_PARENT_DELETED: return TreeParentId(TreeParentId::kDeleted{});
            case LORO_TREE_PARENT_NODE:
                return TreeParentId(TreeParentId::kNode{detail::from_c_tree_id(node)});
            case LORO_TREE_PARENT_UNEXIST:
            default: return TreeParentId(TreeParentId::kUnexist{});
        }
    }

    std::optional<std::vector<TreeId>> children(TreeParentId parent) {
        ::LoroTreeID pid;
        bool is_node = parent_node(parent, pid);
        const ::LoroTreeID *p = is_node ? &pid : nullptr;
        uintptr_t n = 0;
        LoroStatus s = loro_tree_children_len(tree(), p, &n);
        if (s == LORO_ERR_NOT_FOUND) return std::nullopt;
        detail::check(s);
        std::vector<TreeId> out;
        out.reserve(n);
        for (uintptr_t i = 0; i < n; ++i) {
            ::LoroTreeID c;
            detail::check(loro_tree_child_at(tree(), p, i, &c));
            out.push_back(detail::from_c_tree_id(c));
        }
        return out;
    }

    std::optional<uint32_t> children_num(TreeParentId parent) {
        ::LoroTreeID pid;
        bool is_node = parent_node(parent, pid);
        uintptr_t n = 0;
        LoroStatus s = loro_tree_children_len(tree(), is_node ? &pid : nullptr, &n);
        if (s == LORO_ERR_NOT_FOUND) return std::nullopt;
        detail::check(s);
        return static_cast<uint32_t>(n);
    }

    std::vector<TreeId> roots() {
        std::vector<TreeId> out;
        uintptr_t n = loro_tree_roots_len(tree());
        out.reserve(n);
        for (uintptr_t i = 0; i < n; ++i) {
            ::LoroTreeID id;
            detail::check(loro_tree_root_at(tree(), i, &id));
            out.push_back(detail::from_c_tree_id(id));
        }
        return out;
    }

    std::vector<TreeId> nodes() {
        std::vector<TreeId> out;
        uintptr_t n = loro_tree_nodes_len(tree());
        out.reserve(n);
        for (uintptr_t i = 0; i < n; ++i) {
            ::LoroTreeID id;
            detail::check(loro_tree_node_at(tree(), i, &id));
            out.push_back(detail::from_c_tree_id(id));
        }
        return out;
    }

    std::shared_ptr<LoroMap> get_meta(const TreeId &target);  // out-of-line

    bool is_fractional_index_enabled() {
        return loro_tree_is_fractional_index_enabled(tree());
    }
    void enable_fractional_index(uint8_t jitter) {
        detail::check(loro_tree_enable_fractional_index(tree(), jitter));
    }
    void disable_fractional_index() {
        detail::check(loro_tree_disable_fractional_index(tree()));
    }
    std::optional<std::string> fractional_index(const TreeId &target) {
        detail::Bytes b;
        LoroStatus s = loro_tree_fractional_index(tree(), detail::to_c_tree_id(target), b.out());
        if (s == LORO_ERR_NOT_FOUND) return std::nullopt;
        detail::check(s);
        return b.to_string();
    }

    ContainerId id() {
        detail::Bytes b;
        detail::check(loro_tree_id(tree(), b.out()));
        return detail::cid_string_to_container_id(b.to_string());
    }

    ~LoroTree() {
        if (raw_) loro_tree_free(raw_);
        else if (container_) loro_container_free(container_);
    }
    LoroTree(const LoroTree &) = delete;
    LoroTree &operator=(const LoroTree &) = delete;

private:
    explicit LoroTree(::LoroTree *raw) : raw_(raw) {}
    explicit LoroTree(::LoroContainer *c) : container_(c) {}

    ::LoroTree *tree() const {
        if (!raw_) throw LoroError("LoroTree is detached (not attached to a document)");
        return raw_;
    }
    ::LoroContainer *take_container() {
        if (!container_) throw LoroError("LoroTree is not a detached container");
        ::LoroContainer *c = container_;
        container_ = nullptr;
        return c;
    }
    /// Writes the node id into `out` and returns true for a `kNode` parent; returns false
    /// (root / deleted / unexist all map to the C "null parent" = root) otherwise.
    static bool parent_node(const TreeParentId &p, ::LoroTreeID &out) {
        if (auto *n = std::get_if<TreeParentId::kNode>(&p.get_variant())) {
            out = detail::to_c_tree_id(n->id);
            return true;
        }
        return false;
    }

    ::LoroTree *raw_ = nullptr;
    ::LoroContainer *container_ = nullptr;
    friend struct detail::Factory;
};

struct LoroCounter {
    static std::shared_ptr<LoroCounter> init() {
        ::LoroContainer *c = loro_container_new(LORO_CONTAINER_COUNTER);
        if (!c) throw LoroError("loro_container_new(Counter) returned null");
        return std::shared_ptr<LoroCounter>(new LoroCounter(c));
    }

    void increment(double value) { detail::check(loro_counter_increment(counter(), value)); }
    void decrement(double value) { detail::check(loro_counter_decrement(counter(), value)); }
    double get_value() { return loro_counter_get_value(counter()); }
    bool is_attached() { return raw_ ? loro_counter_is_attached(raw_) : false; }

    ContainerId id() {
        detail::Bytes b;
        detail::check(loro_counter_id(counter(), b.out()));
        return detail::cid_string_to_container_id(b.to_string());
    }

    ~LoroCounter() {
        if (raw_) loro_counter_free(raw_);
        else if (container_) loro_container_free(container_);
    }
    LoroCounter(const LoroCounter &) = delete;
    LoroCounter &operator=(const LoroCounter &) = delete;

private:
    explicit LoroCounter(::LoroCounter *raw) : raw_(raw) {}
    explicit LoroCounter(::LoroContainer *c) : container_(c) {}

    ::LoroCounter *counter() const {
        if (!raw_) throw LoroError("LoroCounter is detached (not attached to a document)");
        return raw_;
    }
    ::LoroContainer *take_container() {
        if (!container_) throw LoroError("LoroCounter is not a detached container");
        ::LoroContainer *c = container_;
        container_ = nullptr;
        return c;
    }

    ::LoroCounter *raw_ = nullptr;
    ::LoroContainer *container_ = nullptr;
    friend struct detail::Factory;
};

// --------------------------------------------------------- value-or-container

/// Result of `LoroMap::get` / `LoroList::get` / value navigation: either a plain typed value
/// or a live child container. Built over the C `LoroValueOrContainer` handle.
struct ValueOrContainer {
    bool is_container() { return loro_value_or_container_is_container(raw_); }

    std::optional<LoroValue> as_value() {
        if (loro_value_or_container_is_container(raw_)) return std::nullopt;
        ::LoroValue *cv = loro_value_or_container_get_value(raw_);
        if (!cv) return std::nullopt;
        detail::CValue g(cv);
        return detail::from_c_value(cv);
    }

    std::shared_ptr<LoroText> as_loro_text();                 // out-of-line
    std::shared_ptr<LoroMap> as_loro_map();                   // out-of-line
    std::shared_ptr<LoroList> as_loro_list();                 // out-of-line
    std::shared_ptr<LoroMovableList> as_loro_movable_list();  // out-of-line
    std::shared_ptr<LoroTree> as_loro_tree();                 // out-of-line
    std::shared_ptr<LoroCounter> as_loro_counter();           // out-of-line

    ~ValueOrContainer() { loro_value_or_container_free(raw_); }
    ValueOrContainer(const ValueOrContainer &) = delete;
    ValueOrContainer &operator=(const ValueOrContainer &) = delete;

private:
    explicit ValueOrContainer(::LoroValueOrContainer *raw) : raw_(raw) {}
    ::LoroContainer *take_container_raw() {
        ::LoroContainer *c = loro_value_or_container_get_container(raw_);
        if (!c) throw LoroError(detail::last_error_message());
        return c;
    }
    ::LoroValueOrContainer *raw_;
    friend struct detail::Factory;
};

// ----------------------------------------------------------------- Factory

namespace detail {

/// The single privileged constructor of every wrapper (each grants it `friend`). Centralises
/// raw-handle wrapping, detached-container release, and type-erased `LoroContainer*` adoption,
/// so the wrapper classes need only one friend each. Defined here, after every wrapper, so all
/// types are complete.
struct Factory {
    static std::shared_ptr<LoroText> wrap(::LoroText *r) {
        return std::shared_ptr<LoroText>(new LoroText(r));
    }
    static std::shared_ptr<LoroMap> wrap(::LoroMap *r) {
        return std::shared_ptr<LoroMap>(new LoroMap(r));
    }
    static std::shared_ptr<LoroList> wrap(::LoroList *r) {
        return std::shared_ptr<LoroList>(new LoroList(r));
    }
    static std::shared_ptr<LoroMovableList> wrap(::LoroMovableList *r) {
        return std::shared_ptr<LoroMovableList>(new LoroMovableList(r));
    }
    static std::shared_ptr<LoroTree> wrap(::LoroTree *r) {
        return std::shared_ptr<LoroTree>(new LoroTree(r));
    }
    static std::shared_ptr<LoroCounter> wrap(::LoroCounter *r) {
        return std::shared_ptr<LoroCounter>(new LoroCounter(r));
    }
    static std::shared_ptr<ValueOrContainer> voc(::LoroValueOrContainer *r) {
        return std::shared_ptr<ValueOrContainer>(new ValueOrContainer(r));
    }

    /// Releases a detached wrapper's container handle for attachment.
    template <class T>
    static ::LoroContainer *take(const std::shared_ptr<T> &c) {
        return c->take_container();
    }

    /// Recovers the container backing a ValueOrContainer (a `LoroContainer*`), then adopts it.
    static ::LoroContainer *voc_container(const std::shared_ptr<ValueOrContainer> &voc) {
        return voc->take_container_raw();
    }

    /// Adopts an attached `LoroContainer*` as a typed wrapper, freeing the erased handle.
    template <class Child>
    static std::shared_ptr<Child> adopt(::LoroContainer *attached) {
        if constexpr (std::is_same_v<Child, LoroText>) {
            ::LoroText *t = loro_container_get_text(attached);
            loro_container_free(attached);
            if (!t) throw LoroError("attached container is not a text");
            return wrap(t);
        } else if constexpr (std::is_same_v<Child, LoroMap>) {
            ::LoroMap *m = loro_container_get_map(attached);
            loro_container_free(attached);
            if (!m) throw LoroError("attached container is not a map");
            return wrap(m);
        } else if constexpr (std::is_same_v<Child, LoroList>) {
            ::LoroList *l = loro_container_get_list(attached);
            loro_container_free(attached);
            if (!l) throw LoroError("attached container is not a list");
            return wrap(l);
        } else if constexpr (std::is_same_v<Child, LoroMovableList>) {
            ::LoroMovableList *l = loro_container_get_movable_list(attached);
            loro_container_free(attached);
            if (!l) throw LoroError("attached container is not a movable list");
            return wrap(l);
        } else if constexpr (std::is_same_v<Child, LoroTree>) {
            ::LoroTree *t = loro_container_get_tree(attached);
            loro_container_free(attached);
            if (!t) throw LoroError("attached container is not a tree");
            return wrap(t);
        } else if constexpr (std::is_same_v<Child, LoroCounter>) {
            ::LoroCounter *c = loro_container_get_counter(attached);
            loro_container_free(attached);
            if (!c) throw LoroError("attached container is not a counter");
            return wrap(c);
        } else {
            static_assert(sizeof(Child) == 0, "unsupported container type");
        }
    }
};

}  // namespace detail

// --------------------------------------------- out-of-line wrapper definitions

inline std::shared_ptr<ValueOrContainer> LoroMap::get(const std::string &key) {
    ::LoroValueOrContainer *voc =
        loro_map_get_value_or_container(map(), key.data(), key.size());
    if (!voc) throw LoroError(detail::last_error_message());
    return detail::Factory::voc(voc);
}

inline std::vector<std::shared_ptr<ValueOrContainer>> LoroMap::values() {
    std::vector<std::shared_ptr<ValueOrContainer>> out;
    for (const auto &key : keys()) out.push_back(get(key));
    return out;
}

template <class C>
std::shared_ptr<C> LoroMap::insert_container_by_key(const std::string &key,
                                                    std::shared_ptr<C> child) {
    ::LoroContainer *attached =
        loro_map_insert_container(map(), key.data(), key.size(), detail::Factory::take(child));
    if (!attached) throw LoroError(detail::last_error_message());
    return detail::Factory::adopt<C>(attached);
}

template <class C>
std::shared_ptr<C> LoroMap::get_or_create_container_by_key(const std::string &key,
                                                           std::shared_ptr<C> child) {
    ::LoroContainer *existing = loro_map_get_container(map(), key.data(), key.size());
    if (existing) return detail::Factory::adopt<C>(existing);
    return insert_container_by_key<C>(key, std::move(child));
}

inline std::shared_ptr<LoroText> LoroMap::insert_text_container(const std::string &k,
                                                                std::shared_ptr<LoroText> c) {
    return insert_container_by_key<LoroText>(k, std::move(c));
}
inline std::shared_ptr<LoroMap> LoroMap::insert_map_container(const std::string &k,
                                                              std::shared_ptr<LoroMap> c) {
    return insert_container_by_key<LoroMap>(k, std::move(c));
}
inline std::shared_ptr<LoroList> LoroMap::insert_list_container(const std::string &k,
                                                                std::shared_ptr<LoroList> c) {
    return insert_container_by_key<LoroList>(k, std::move(c));
}
inline std::shared_ptr<LoroMovableList> LoroMap::insert_movable_list_container(
    const std::string &k, std::shared_ptr<LoroMovableList> c) {
    return insert_container_by_key<LoroMovableList>(k, std::move(c));
}
inline std::shared_ptr<LoroTree> LoroMap::insert_tree_container(const std::string &k,
                                                                std::shared_ptr<LoroTree> c) {
    return insert_container_by_key<LoroTree>(k, std::move(c));
}
inline std::shared_ptr<LoroCounter> LoroMap::insert_counter_container(
    const std::string &k, std::shared_ptr<LoroCounter> c) {
    return insert_container_by_key<LoroCounter>(k, std::move(c));
}

inline std::shared_ptr<LoroText> LoroMap::get_or_create_text_container(
    const std::string &k, std::shared_ptr<LoroText> c) {
    return get_or_create_container_by_key<LoroText>(k, std::move(c));
}
inline std::shared_ptr<LoroMap> LoroMap::get_or_create_map_container(
    const std::string &k, std::shared_ptr<LoroMap> c) {
    return get_or_create_container_by_key<LoroMap>(k, std::move(c));
}
inline std::shared_ptr<LoroList> LoroMap::get_or_create_list_container(
    const std::string &k, std::shared_ptr<LoroList> c) {
    return get_or_create_container_by_key<LoroList>(k, std::move(c));
}
inline std::shared_ptr<LoroMovableList> LoroMap::get_or_create_movable_list_container(
    const std::string &k, std::shared_ptr<LoroMovableList> c) {
    return get_or_create_container_by_key<LoroMovableList>(k, std::move(c));
}
inline std::shared_ptr<LoroTree> LoroMap::get_or_create_tree_container(
    const std::string &k, std::shared_ptr<LoroTree> c) {
    return get_or_create_container_by_key<LoroTree>(k, std::move(c));
}
inline std::shared_ptr<LoroCounter> LoroMap::get_or_create_counter_container(
    const std::string &k, std::shared_ptr<LoroCounter> c) {
    return get_or_create_container_by_key<LoroCounter>(k, std::move(c));
}

inline std::shared_ptr<ValueOrContainer> LoroList::get(uint32_t index) {
    ::LoroValueOrContainer *voc = loro_list_get_value_or_container(list(), index);
    if (!voc) throw LoroError(detail::last_error_message());
    return detail::Factory::voc(voc);
}

template <class C>
std::shared_ptr<C> LoroList::insert_container_at(uint32_t pos, std::shared_ptr<C> child) {
    ::LoroContainer *attached =
        loro_list_insert_container(list(), pos, detail::Factory::take(child));
    if (!attached) throw LoroError(detail::last_error_message());
    return detail::Factory::adopt<C>(attached);
}

inline std::shared_ptr<LoroText> LoroList::insert_text_container(uint32_t pos,
                                                                 std::shared_ptr<LoroText> c) {
    return insert_container_at<LoroText>(pos, std::move(c));
}
inline std::shared_ptr<LoroMap> LoroList::insert_map_container(uint32_t pos,
                                                               std::shared_ptr<LoroMap> c) {
    return insert_container_at<LoroMap>(pos, std::move(c));
}
inline std::shared_ptr<LoroList> LoroList::insert_list_container(uint32_t pos,
                                                                 std::shared_ptr<LoroList> c) {
    return insert_container_at<LoroList>(pos, std::move(c));
}
inline std::shared_ptr<LoroMovableList> LoroList::insert_movable_list_container(
    uint32_t pos, std::shared_ptr<LoroMovableList> c) {
    return insert_container_at<LoroMovableList>(pos, std::move(c));
}
inline std::shared_ptr<LoroTree> LoroList::insert_tree_container(uint32_t pos,
                                                                 std::shared_ptr<LoroTree> c) {
    return insert_container_at<LoroTree>(pos, std::move(c));
}
inline std::shared_ptr<LoroCounter> LoroList::insert_counter_container(
    uint32_t pos, std::shared_ptr<LoroCounter> c) {
    return insert_container_at<LoroCounter>(pos, std::move(c));
}

inline std::shared_ptr<ValueOrContainer> LoroMovableList::get(uint32_t index) {
    ::LoroValueOrContainer *voc = loro_movable_list_get_value_or_container(list(), index);
    if (!voc) throw LoroError(detail::last_error_message());
    return detail::Factory::voc(voc);
}

inline std::shared_ptr<ValueOrContainer> LoroMovableList::pop() {
    bool present = false;
    ::LoroValueOrContainer *voc = loro_movable_list_pop_value_or_container(list(), &present);
    if (!present) return nullptr;
    if (!voc) throw LoroError(detail::last_error_message());
    return detail::Factory::voc(voc);
}

template <class C>
std::shared_ptr<C> LoroMovableList::insert_container_at(uint32_t pos, std::shared_ptr<C> child) {
    ::LoroContainer *attached =
        loro_movable_list_insert_container(list(), pos, detail::Factory::take(child));
    if (!attached) throw LoroError(detail::last_error_message());
    return detail::Factory::adopt<C>(attached);
}

template <class C>
std::shared_ptr<C> LoroMovableList::set_container_at(uint32_t pos, std::shared_ptr<C> child) {
    ::LoroContainer *attached =
        loro_movable_list_set_container(list(), pos, detail::Factory::take(child));
    if (!attached) throw LoroError(detail::last_error_message());
    return detail::Factory::adopt<C>(attached);
}

inline std::shared_ptr<LoroText> LoroMovableList::insert_text_container(
    uint32_t pos, std::shared_ptr<LoroText> c) {
    return insert_container_at<LoroText>(pos, std::move(c));
}
inline std::shared_ptr<LoroMap> LoroMovableList::insert_map_container(
    uint32_t pos, std::shared_ptr<LoroMap> c) {
    return insert_container_at<LoroMap>(pos, std::move(c));
}
inline std::shared_ptr<LoroList> LoroMovableList::insert_list_container(
    uint32_t pos, std::shared_ptr<LoroList> c) {
    return insert_container_at<LoroList>(pos, std::move(c));
}
inline std::shared_ptr<LoroMovableList> LoroMovableList::insert_movable_list_container(
    uint32_t pos, std::shared_ptr<LoroMovableList> c) {
    return insert_container_at<LoroMovableList>(pos, std::move(c));
}
inline std::shared_ptr<LoroTree> LoroMovableList::insert_tree_container(
    uint32_t pos, std::shared_ptr<LoroTree> c) {
    return insert_container_at<LoroTree>(pos, std::move(c));
}
inline std::shared_ptr<LoroCounter> LoroMovableList::insert_counter_container(
    uint32_t pos, std::shared_ptr<LoroCounter> c) {
    return insert_container_at<LoroCounter>(pos, std::move(c));
}

inline std::shared_ptr<LoroText> LoroMovableList::set_text_container(
    uint32_t pos, std::shared_ptr<LoroText> c) {
    return set_container_at<LoroText>(pos, std::move(c));
}
inline std::shared_ptr<LoroMap> LoroMovableList::set_map_container(
    uint32_t pos, std::shared_ptr<LoroMap> c) {
    return set_container_at<LoroMap>(pos, std::move(c));
}
inline std::shared_ptr<LoroList> LoroMovableList::set_list_container(
    uint32_t pos, std::shared_ptr<LoroList> c) {
    return set_container_at<LoroList>(pos, std::move(c));
}
inline std::shared_ptr<LoroMovableList> LoroMovableList::set_movable_list_container(
    uint32_t pos, std::shared_ptr<LoroMovableList> c) {
    return set_container_at<LoroMovableList>(pos, std::move(c));
}
inline std::shared_ptr<LoroTree> LoroMovableList::set_tree_container(
    uint32_t pos, std::shared_ptr<LoroTree> c) {
    return set_container_at<LoroTree>(pos, std::move(c));
}
inline std::shared_ptr<LoroCounter> LoroMovableList::set_counter_container(
    uint32_t pos, std::shared_ptr<LoroCounter> c) {
    return set_container_at<LoroCounter>(pos, std::move(c));
}

inline std::shared_ptr<LoroMap> LoroTree::get_meta(const TreeId &target) {
    ::LoroMap *m = loro_tree_get_meta(tree(), detail::to_c_tree_id(target));
    if (!m) throw LoroError(detail::last_error_message());
    return detail::Factory::wrap(m);
}

inline std::shared_ptr<LoroText> ValueOrContainer::as_loro_text() {
    return detail::Factory::adopt<LoroText>(take_container_raw());
}
inline std::shared_ptr<LoroMap> ValueOrContainer::as_loro_map() {
    return detail::Factory::adopt<LoroMap>(take_container_raw());
}
inline std::shared_ptr<LoroList> ValueOrContainer::as_loro_list() {
    return detail::Factory::adopt<LoroList>(take_container_raw());
}
inline std::shared_ptr<LoroMovableList> ValueOrContainer::as_loro_movable_list() {
    return detail::Factory::adopt<LoroMovableList>(take_container_raw());
}
inline std::shared_ptr<LoroTree> ValueOrContainer::as_loro_tree() {
    return detail::Factory::adopt<LoroTree>(take_container_raw());
}
inline std::shared_ptr<LoroCounter> ValueOrContainer::as_loro_counter() {
    return detail::Factory::adopt<LoroCounter>(take_container_raw());
}

// ----------------------------------------------------------- version / frontiers

/// Version vector (loro-cpp `VersionVector`). Free with `loro_version_vector_free`.
struct VersionVector {
    static std::shared_ptr<VersionVector> init() {
        ::LoroVersionVector *v = loro_version_vector_new();
        if (!v) throw LoroError("loro_version_vector_new returned null");
        return std::shared_ptr<VersionVector>(new VersionVector(v));
    }
    static std::shared_ptr<VersionVector> decode(const std::vector<uint8_t> &bytes) {
        ::LoroVersionVector *v = loro_version_vector_decode(bytes.data(), bytes.size());
        if (!v) throw LoroError(detail::last_error_message());
        return std::shared_ptr<VersionVector>(new VersionVector(v));
    }
    std::vector<uint8_t> encode() {
        detail::Bytes b;
        detail::check(loro_version_vector_encode(raw_, b.out()));
        return b.to_vector();
    }
    bool eq(const std::shared_ptr<VersionVector> &other) {
        int32_t cmp = 0;
        // Equal vectors compare as 0; concurrent vectors return NOT_FOUND (not equal).
        return loro_version_vector_compare(raw_, other->raw_, &cmp) == LORO_OK && cmp == 0;
    }

    ~VersionVector() { loro_version_vector_free(raw_); }
    VersionVector(const VersionVector &) = delete;
    VersionVector &operator=(const VersionVector &) = delete;

private:
    explicit VersionVector(::LoroVersionVector *raw) : raw_(raw) {}
    ::LoroVersionVector *raw_;
    friend struct LoroDoc;
};

/// Frontiers (loro-cpp `Frontiers`). Free with `loro_frontiers_free`.
struct Frontiers {
    static std::shared_ptr<Frontiers> init() {
        ::LoroFrontiers *f = loro_frontiers_new();
        if (!f) throw LoroError("loro_frontiers_new returned null");
        return std::shared_ptr<Frontiers>(new Frontiers(f));
    }
    static std::shared_ptr<Frontiers> decode(const std::vector<uint8_t> &bytes) {
        ::LoroFrontiers *f = loro_frontiers_decode(bytes.data(), bytes.size());
        if (!f) throw LoroError(detail::last_error_message());
        return std::shared_ptr<Frontiers>(new Frontiers(f));
    }
    std::vector<uint8_t> encode() {
        detail::Bytes b;
        detail::check(loro_frontiers_encode(raw_, b.out()));
        return b.to_vector();
    }
    bool is_empty() { return loro_frontiers_is_empty(raw_); }
    /// loro-c has no `Frontiers` comparison primitive, so equality compares the canonical
    /// binary encodings (the conformance tests use eq only for round-trip checks).
    bool eq(const std::shared_ptr<Frontiers> &other) { return encode() == other->encode(); }

    ~Frontiers() { loro_frontiers_free(raw_); }
    Frontiers(const Frontiers &) = delete;
    Frontiers &operator=(const Frontiers &) = delete;

private:
    explicit Frontiers(::LoroFrontiers *raw) : raw_(raw) {}
    ::LoroFrontiers *raw_;
    friend struct LoroDoc;
};

// ------------------------------------------------------------- style config map

struct StyleConfigMap {
    static std::shared_ptr<StyleConfigMap> init() {
        ::LoroStyleConfigMap *m = loro_style_config_map_new();
        if (!m) throw LoroError("loro_style_config_map_new returned null");
        return std::shared_ptr<StyleConfigMap>(new StyleConfigMap(m));
    }

    /// Mirrors loro's default rich-text config (no C-ABI helper exists, so build it here).
    static std::shared_ptr<StyleConfigMap> default_rich_text_config() {
        auto cfg = init();
        cfg->insert_c("bold", LORO_EXPAND_AFTER);
        cfg->insert_c("italic", LORO_EXPAND_AFTER);
        cfg->insert_c("underline", LORO_EXPAND_AFTER);
        cfg->insert_c("comment", LORO_EXPAND_NONE);
        cfg->insert_c("link", LORO_EXPAND_NONE);
        return cfg;
    }

    void insert(const std::string &key, const StyleConfig &value) {
        insert_c(key, detail::to_c_expand(value.expand));
    }

    ~StyleConfigMap() { loro_style_config_map_free(raw_); }
    StyleConfigMap(const StyleConfigMap &) = delete;
    StyleConfigMap &operator=(const StyleConfigMap &) = delete;

private:
    explicit StyleConfigMap(::LoroStyleConfigMap *raw) : raw_(raw) {}
    void insert_c(const std::string &key, LoroExpandType expand) {
        LoroStyleConfig sc;
        sc.expand = expand;
        detail::check(loro_style_config_map_insert(raw_, key.data(), key.size(), sc));
    }
    ::LoroStyleConfigMap *raw_;
    friend struct LoroDoc;
};

// ------------------------------------------------------------------- LoroDoc

struct LoroDoc {
    static std::shared_ptr<LoroDoc> init() {
        ::LoroDoc *d = loro_doc_new();
        if (!d) throw LoroError("loro_doc_new returned null");
        return std::shared_ptr<LoroDoc>(new LoroDoc(d));
    }

    std::shared_ptr<LoroText> get_text(const std::shared_ptr<ContainerIdLike> &id) {
        std::string name = detail::root_name(id, ContainerType(ContainerType::kText{}));
        ::LoroText *t = loro_doc_get_text(raw_, name.data(), name.size());
        if (!t) {
            std::string msg = detail::last_error_message();
            throw LoroError(msg.empty() ? "loro_doc_get_text returned null" : msg);
        }
        return detail::Factory::wrap(t);
    }

    std::shared_ptr<LoroMap> get_map(const std::shared_ptr<ContainerIdLike> &id) {
        std::string name = detail::root_name(id, ContainerType(ContainerType::kMap{}));
        ::LoroMap *m = loro_doc_get_map(raw_, name.data(), name.size());
        if (!m) {
            std::string msg = detail::last_error_message();
            throw LoroError(msg.empty() ? "loro_doc_get_map returned null" : msg);
        }
        return detail::Factory::wrap(m);
    }

    std::shared_ptr<LoroList> get_list(const std::shared_ptr<ContainerIdLike> &id) {
        std::string name = detail::root_name(id, ContainerType(ContainerType::kList{}));
        ::LoroList *l = loro_doc_get_list(raw_, name.data(), name.size());
        if (!l) {
            std::string msg = detail::last_error_message();
            throw LoroError(msg.empty() ? "loro_doc_get_list returned null" : msg);
        }
        return detail::Factory::wrap(l);
    }

    std::shared_ptr<LoroMovableList> get_movable_list(
        const std::shared_ptr<ContainerIdLike> &id) {
        std::string name = detail::root_name(id, ContainerType(ContainerType::kMovableList{}));
        ::LoroMovableList *l = loro_doc_get_movable_list(raw_, name.data(), name.size());
        if (!l) {
            std::string msg = detail::last_error_message();
            throw LoroError(msg.empty() ? "loro_doc_get_movable_list returned null" : msg);
        }
        return detail::Factory::wrap(l);
    }

    std::shared_ptr<LoroTree> get_tree(const std::shared_ptr<ContainerIdLike> &id) {
        std::string name = detail::root_name(id, ContainerType(ContainerType::kTree{}));
        ::LoroTree *t = loro_doc_get_tree(raw_, name.data(), name.size());
        if (!t) {
            std::string msg = detail::last_error_message();
            throw LoroError(msg.empty() ? "loro_doc_get_tree returned null" : msg);
        }
        return detail::Factory::wrap(t);
    }

    std::shared_ptr<LoroCounter> get_counter(const std::shared_ptr<ContainerIdLike> &id) {
        std::string name = detail::root_name(id, ContainerType(ContainerType::kCounter{}));
        ::LoroCounter *c = loro_doc_get_counter(raw_, name.data(), name.size());
        if (!c) {
            std::string msg = detail::last_error_message();
            throw LoroError(msg.empty() ? "loro_doc_get_counter returned null" : msg);
        }
        return detail::Factory::wrap(c);
    }

    uint64_t peer_id() { return loro_doc_peer_id(raw_); }
    void set_peer_id(uint64_t peer) { detail::check(loro_doc_set_peer_id(raw_, peer)); }

    void commit() { detail::check(loro_doc_commit(raw_)); }
    uint32_t get_pending_txn_len() {
        return static_cast<uint32_t>(loro_doc_get_pending_txn_len(raw_));
    }

    std::shared_ptr<VersionVector> state_vv() {
        ::LoroVersionVector *v = loro_doc_state_vv(raw_);
        if (!v) throw LoroError(detail::last_error_message());
        return std::shared_ptr<VersionVector>(new VersionVector(v));
    }
    std::shared_ptr<Frontiers> state_frontiers() {
        ::LoroFrontiers *f = loro_doc_state_frontiers(raw_);
        if (!f) throw LoroError(detail::last_error_message());
        return std::shared_ptr<Frontiers>(new Frontiers(f));
    }
    std::shared_ptr<VersionVector> frontiers_to_vv(const std::shared_ptr<Frontiers> &frontiers) {
        ::LoroVersionVector *v = loro_doc_frontiers_to_vv(raw_, frontiers->raw_);
        if (!v) throw LoroError(detail::last_error_message());
        return std::shared_ptr<VersionVector>(new VersionVector(v));
    }
    std::shared_ptr<Frontiers> vv_to_frontiers(const std::shared_ptr<VersionVector> &vv) {
        ::LoroFrontiers *f = loro_doc_vv_to_frontiers(raw_, vv->raw_);
        if (!f) throw LoroError(detail::last_error_message());
        return std::shared_ptr<Frontiers>(new Frontiers(f));
    }

    std::vector<uint8_t> export_snapshot() {
        detail::Bytes b;
        detail::check(loro_doc_export_snapshot(raw_, b.out()));
        return b.to_vector();
    }
    std::vector<uint8_t> export_snapshot_at(const std::shared_ptr<Frontiers> &frontiers) {
        detail::Bytes b;
        detail::check(loro_doc_export_snapshot_at(raw_, frontiers->raw_, b.out()));
        return b.to_vector();
    }
    std::vector<uint8_t> export_updates(const std::shared_ptr<VersionVector> &from) {
        detail::Bytes b;
        detail::check(loro_doc_export_updates_from(raw_, from->raw_, b.out()));
        return b.to_vector();
    }
    std::string export_json_updates(const std::shared_ptr<VersionVector> &start_vv,
                                    const std::shared_ptr<VersionVector> &end_vv) {
        detail::Bytes b;
        detail::check(loro_doc_export_json_updates(raw_, start_vv->raw_, end_vv->raw_, b.out()));
        return b.to_string();
    }
    ImportStatus import_json_updates(const std::string &json) {
        detail::check(loro_doc_import_json_updates(raw_, json.data(), json.size()));
        return ImportStatus{};
    }

    ImportStatus import(const std::vector<uint8_t> &bytes) {
        detail::check(loro_doc_import(raw_, bytes.data(), bytes.size()));
        // The detailed success/pending span maps are deferred to Phase 4; the conformance
        // tests ignore the return value.
        return ImportStatus{};
    }

    std::shared_ptr<LoroDoc> fork() {
        ::LoroDoc *d = loro_doc_fork(raw_);
        if (!d) throw LoroError(detail::last_error_message());
        return std::shared_ptr<LoroDoc>(new LoroDoc(d));
    }
    std::shared_ptr<LoroDoc> fork_at(const std::shared_ptr<Frontiers> &frontiers) {
        ::LoroDoc *d = loro_doc_fork_at(raw_, frontiers->raw_);
        if (!d) throw LoroError(detail::last_error_message());
        return std::shared_ptr<LoroDoc>(new LoroDoc(d));
    }

    LoroValue get_deep_value() {
        ::LoroValue *cv = loro_doc_get_deep_value(raw_);
        if (!cv) throw LoroError(detail::last_error_message());
        detail::CValue g(cv);
        return detail::from_c_value(cv);
    }

    bool has_container(const ContainerId &id) {
        std::string cid = detail::container_id_to_cid_string(id);
        return loro_doc_has_container(raw_, cid.data(), cid.size());
    }

    void config_text_style(const std::shared_ptr<StyleConfigMap> &text_style) {
        detail::check(loro_doc_config_text_style(raw_, text_style->raw_));
    }

    PosQueryResult get_cursor_pos(const std::shared_ptr<Cursor> &cursor) {
        LoroPosQueryResult out;
        detail::check(loro_doc_get_cursor_pos(raw_, cursor->raw_, &out));
        PosQueryResult r;
        r.update = nullptr;  // loro-c does not return an updated cursor; the tests don't read it.
        r.current =
            AbsolutePosition{static_cast<uint32_t>(out.abs_pos), detail::from_c_side(out.side)};
        return r;
    }

    ~LoroDoc() { loro_doc_free(raw_); }
    LoroDoc(const LoroDoc &) = delete;
    LoroDoc &operator=(const LoroDoc &) = delete;

private:
    explicit LoroDoc(::LoroDoc *raw) : raw_(raw) {}
    ::LoroDoc *raw_;
};

// -------------------------------------------------------------- subscriptions

/// Opaque subscription handle. `unsubscribe()` stops the callback (and frees it); the
/// destructor does the same if still active.
struct Subscription {
    void unsubscribe() {
        if (raw_) {
            loro_subscription_free(raw_);
            raw_ = nullptr;
        }
    }
    void detach() {
        if (raw_) {
            loro_subscription_detach(raw_);
            raw_ = nullptr;
        }
    }

    ~Subscription() {
        if (raw_) loro_subscription_free(raw_);
    }
    Subscription(const Subscription &) = delete;
    Subscription &operator=(const Subscription &) = delete;

private:
    explicit Subscription(::LoroSubscription *raw) : raw_(raw) {}
    ::LoroSubscription *raw_;
    friend struct EphemeralStore;
};

// ----------------------------------------------------------- ephemeral store

/// The added/updated/removed keys reported to an [`EphemeralSubscriber`].
struct EphemeralStoreEvent {
    std::vector<std::string> added;
    std::vector<std::string> updated;
    std::vector<std::string> removed;
};

/// Callback interface for [`EphemeralStore::subscribe`].
struct EphemeralSubscriber {
    virtual ~EphemeralSubscriber() {}
    virtual void on_ephemeral_event(const EphemeralStoreEvent &event) = 0;
};

namespace detail {
/// Heap-held owner of a C++ subscriber, passed as `user_data` to the C callback.
struct EphemeralSubscriberHolder {
    std::shared_ptr<EphemeralSubscriber> sub;
};
}  // namespace detail

/// Trampoline: turns the C `LoroEphemeralStoreEvent*` into an [`EphemeralStoreEvent`] and
/// dispatches to the C++ subscriber. Never lets a C++ exception unwind back into Rust.
extern "C" inline void loro_conf_ephemeral_invoke(const ::LoroEphemeralStoreEvent *ev,
                                                  void *user_data) {
    auto *holder = static_cast<detail::EphemeralSubscriberHolder *>(user_data);
    if (!holder || !holder->sub) return;
    try {
        EphemeralStoreEvent e;
        detail::Bytes added;
        detail::Bytes updated;
        detail::Bytes removed;
        if (loro_ephemeral_event_added(ev, added.out()) == LORO_OK)
            e.added = detail::parse_string_array(added.to_string());
        if (loro_ephemeral_event_updated(ev, updated.out()) == LORO_OK)
            e.updated = detail::parse_string_array(updated.to_string());
        if (loro_ephemeral_event_removed(ev, removed.out()) == LORO_OK)
            e.removed = detail::parse_string_array(removed.to_string());
        holder->sub->on_ephemeral_event(e);
    } catch (...) {
        // Swallow: unwinding across the C ABI boundary is undefined behaviour.
    }
}

/// Trampoline: releases the heap-held subscriber when the subscription is dropped.
extern "C" inline void loro_conf_ephemeral_free(void *user_data) {
    delete static_cast<detail::EphemeralSubscriberHolder *>(user_data);
}

/// Out-of-document, last-write-wins keyed state (cursors, presence, …) with change events.
struct EphemeralStore {
    static std::shared_ptr<EphemeralStore> init(int64_t timeout_ms) {
        ::LoroEphemeralStore *s = loro_ephemeral_store_new(timeout_ms);
        if (!s) throw LoroError("loro_ephemeral_store_new returned null");
        return std::shared_ptr<EphemeralStore>(new EphemeralStore(s));
    }

    void set(const std::string &key, const std::shared_ptr<LoroValueLike> &value) {
        LoroValue v = value->as_loro_value();
        detail::CValue cv(detail::to_c_value(v));
        detail::check(loro_ephemeral_store_set_value(raw_, key.data(), key.size(), cv.get()));
    }

    std::optional<LoroValue> get(const std::string &key) {
        ::LoroValue *cv = loro_ephemeral_store_get_value(raw_, key.data(), key.size());
        if (!cv) return std::nullopt;
        detail::CValue g(cv);
        return detail::from_c_value(cv);
    }

    std::vector<std::string> keys() {
        detail::Bytes b;
        detail::check(loro_ephemeral_store_keys(raw_, b.out()));
        return detail::parse_string_array(b.to_string());
    }

    void delete_(const std::string &key) {
        detail::check(loro_ephemeral_store_delete(raw_, key.data(), key.size()));
    }

    std::vector<uint8_t> encode_all() {
        detail::Bytes b;
        detail::check(loro_ephemeral_store_encode_all(raw_, b.out()));
        return b.to_vector();
    }

    void apply(const std::vector<uint8_t> &data) {
        detail::check(loro_ephemeral_store_apply(raw_, data.data(), data.size()));
    }

    std::shared_ptr<Subscription> subscribe(const std::shared_ptr<EphemeralSubscriber> &sub) {
        auto *holder = new detail::EphemeralSubscriberHolder{sub};
        ::LoroEphemeralSubscriber cb;
        cb.invoke = &loro_conf_ephemeral_invoke;
        cb.user_data = holder;
        cb.free_user_data = &loro_conf_ephemeral_free;
        // On error loro-c drops the callback (running free_user_data, freeing `holder`);
        // do NOT free it again here.
        ::LoroSubscription *s = loro_ephemeral_store_subscribe(raw_, cb);
        if (!s) throw LoroError("loro_ephemeral_store_subscribe returned null");
        return std::shared_ptr<Subscription>(new Subscription(s));
    }

    ~EphemeralStore() { loro_ephemeral_store_free(raw_); }
    EphemeralStore(const EphemeralStore &) = delete;
    EphemeralStore &operator=(const EphemeralStore &) = delete;

private:
    explicit EphemeralStore(::LoroEphemeralStore *raw) : raw_(raw) {}
    ::LoroEphemeralStore *raw_;
};

}  // namespace loro

#endif  // LORO_CONFORMANCE_LORO_HPP
