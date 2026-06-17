/*
 * loro.hpp — Phase 0 conformance spike.
 *
 * A loro-cpp-SHAPED C++ API (namespace `loro`, shared_ptr ownership, typed `LoroValue`,
 * `init()` factories, callback interfaces) implemented as a thin wrapper over loro-c's
 * existing C ABI in <loro/loro.h>. Its sole purpose for Phase 0 is to compile, link and pass
 * loro-cpp's own tests/test_smoke.cpp and tests/test_text.cpp, proving the API *shape* and the
 * build wiring before the full RESHAPE rewrite is undertaken.
 *
 * Scope (Phase 0 only): the surface those two tests exercise — LoroDoc, LoroText, LoroMap
 * (opaque), Cursor, StyleConfigMap, the typed LoroValue/TextDelta/ContainerId families and the
 * ContainerIdLike / LoroValueLike interfaces. Values cross the boundary as JSON here, which is
 * lossless for the kinds these tests use (null/bool/i64/double/string). The provably-lossy
 * cases (binary, integer-valued doubles, cid-prefixed strings) DO NOT occur in test_smoke /
 * test_text and are deliberately out of scope — the typed-value C ABI is Phase 1.
 *
 * Names match ../loro-cpp/build/generated/loro.hpp field-for-field (uniffi quirks included:
 * `delete_()`, `Side::kRight`, `TextDelta::kDelete{delete_}`, `ContainerType::kUnknown{kind}`,
 * `LoroValue::kI64`). The C handle types from <loro/loro.h> live in the global namespace and are
 * referred to here with a leading `::` (e.g. `::LoroText`) to disambiguate from the `loro::`
 * wrapper classes of the same name.
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

/// Base loro exception (loro-cpp shape). Phase 0 uses a single class; the full
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
struct Cursor;
struct StyleConfigMap;
struct LoroValue;
struct ContainerId;
struct ValueOrContainer;
struct Subscription;
struct EphemeralStore;
struct EphemeralStoreEvent;
struct EphemeralSubscriber;

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

/// Extracts the root-container name from a ContainerIdLike. Phase 0 supports only root
/// (by-name) ids — which is all the conformance tests use.
inline std::string root_name(const std::shared_ptr<ContainerIdLike> &id, ContainerType ty) {
    ContainerId cid = id->as_container_id(std::move(ty));
    if (auto *root = std::get_if<ContainerId::kRoot>(&cid.get_variant())) {
        return root->name;
    }
    throw LoroError("Phase 0 spike: only root (by-name) ContainerId is supported");
}

// ---- minimal JSON support (Phase 0: rich-text delta round-trip only) --------

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
        throw LoroError(std::string("Phase 0 JSON parse error: ") + m);
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
                        // BMP-only UTF-8 encode (sufficient for Phase 0 tests).
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
/// otherwise kDouble (Phase 0: integer-valued doubles are not preserved — that is Phase 1).
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

/// Typed LoroValue -> JSON scalar, for mark()'s value. Phase 0 supports the scalar kinds the
/// tests produce; compound/binary/container values throw (Phase 1 typed-value ABI).
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
                throw LoroError("Phase 0 spike: only scalar LoroValue kinds cross as JSON");
            }
        },
        v.get_variant());
}

// ---- typed-value bridge (Phase 1) -------------------------------------------
//
// Translates between the C++ typed `loro::LoroValue` variant and the opaque C
// `::LoroValue` handle from <loro/loro.h>, so values cross the FFI boundary WITHOUT JSON
// (binary stays binary; integer-valued doubles stay doubles). Replaces the lossy Phase 0
// JSON value path for map / ephemeral values.

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
                throw LoroError("Phase 1: container-valued LoroValue cannot be sent to loro-c");
            }
        },
        v.get_variant());
}

/// Reconstructs a typed `loro::LoroValue` from an owned `::LoroValue*` (no JSON). Does not
/// take ownership of `cv` (the caller frees it).
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
        case LORO_VALUE_CONTAINER:
        default:
            throw LoroError("Phase 1: container-valued LoroValue reconstruction is deferred");
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
    /// passing it to `LoroMap::insert_text_container` (etc.), which consumes it and returns
    /// the attached handle. A detached text is not editable until attached.
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

    bool is_attached() { return loro_text_is_attached(raw_); }
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

    /// Releases the detached container for attachment (nulls it so the destructor won't free
    /// it). Throws if this text is not a detached container.
    ::LoroContainer *take_container() {
        if (!container_) throw LoroError("LoroText is not a detached container");
        ::LoroContainer *c = container_;
        container_ = nullptr;
        return c;
    }

    ::LoroText *raw_ = nullptr;            // attached editing handle
    ::LoroContainer *container_ = nullptr;  // detached container awaiting attach
    friend struct LoroDoc;
    friend struct LoroMap;
    friend struct ValueOrContainer;
};

struct LoroMap {
    /// Constructs a *detached* map container (loro-c: `loro_container_new`). Attach it by
    /// passing it to `LoroMap::insert_map_container` (etc.), which consumes it and returns
    /// the attached handle.
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

    /// Returns the entry at `key` (a value or a live child container). Declared here, defined
    /// out-of-line once `ValueOrContainer` is complete.
    std::shared_ptr<ValueOrContainer> get(const std::string &key);

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

    /// loro-cpp returns one `ValueOrContainer` per entry; here it is keys() + get() per key
    /// (the conformance test only checks the count). Defined out-of-line.
    std::vector<std::shared_ptr<ValueOrContainer>> values();

    LoroValue get_deep_value() {
        ::LoroValue *cv = loro_map_get_deep_value(map());
        if (!cv) throw LoroError("loro_map_get_deep_value returned null");
        detail::CValue g(cv);
        return detail::from_c_value(cv);
    }

    std::shared_ptr<LoroText> insert_text_container(const std::string &key,
                                                    const std::shared_ptr<LoroText> &child) {
        ::LoroContainer *attached =
            loro_map_insert_container(map(), key.data(), key.size(), child->take_container());
        if (!attached) throw LoroError(detail::last_error_message());
        ::LoroText *t = loro_container_get_text(attached);
        loro_container_free(attached);
        if (!t) throw LoroError("attached container is not a text");
        return std::shared_ptr<LoroText>(new LoroText(t));
    }

    std::shared_ptr<LoroMap> insert_map_container(const std::string &key,
                                                  const std::shared_ptr<LoroMap> &child) {
        ::LoroContainer *attached =
            loro_map_insert_container(map(), key.data(), key.size(), child->take_container());
        if (!attached) throw LoroError(detail::last_error_message());
        ::LoroMap *m = loro_container_get_map(attached);
        loro_container_free(attached);
        if (!m) throw LoroError("attached container is not a map");
        return std::shared_ptr<LoroMap>(new LoroMap(m));
    }

    ~LoroMap() {
        if (raw_) loro_map_free(raw_);
        else if (container_) loro_container_free(container_);
    }
    LoroMap(const LoroMap &) = delete;
    LoroMap &operator=(const LoroMap &) = delete;

private:
    explicit LoroMap(::LoroMap *raw) : raw_(raw) {}          // attached
    explicit LoroMap(::LoroContainer *c) : container_(c) {}  // detached (from init())

    /// The attached editing handle, or throws if this map is a detached container.
    ::LoroMap *map() const {
        if (!raw_) throw LoroError("LoroMap is detached (not attached to a document)");
        return raw_;
    }

    /// Releases the detached container for attachment (nulls it). Throws if not detached.
    ::LoroContainer *take_container() {
        if (!container_) throw LoroError("LoroMap is not a detached container");
        ::LoroContainer *c = container_;
        container_ = nullptr;
        return c;
    }

    ::LoroMap *raw_ = nullptr;             // attached handle
    ::LoroContainer *container_ = nullptr;  // detached container awaiting attach
    friend struct LoroDoc;
    friend struct ValueOrContainer;
};

// --------------------------------------------------------- value-or-container

/// Result of `LoroMap::get` / value navigation: either a plain typed value or a live child
/// container. Built over the C `LoroValueOrContainer` handle.
struct ValueOrContainer {
    bool is_container() { return loro_value_or_container_is_container(raw_); }

    std::optional<LoroValue> as_value() {
        if (loro_value_or_container_is_container(raw_)) return std::nullopt;
        ::LoroValue *cv = loro_value_or_container_get_value(raw_);
        if (!cv) return std::nullopt;
        detail::CValue g(cv);
        return detail::from_c_value(cv);
    }

    std::shared_ptr<LoroText> as_loro_text() {
        ::LoroContainer *c = loro_value_or_container_get_container(raw_);
        if (!c) throw LoroError(detail::last_error_message());
        ::LoroText *t = loro_container_get_text(c);
        loro_container_free(c);
        if (!t) throw LoroError("value-or-container is not a text container");
        return std::shared_ptr<LoroText>(new LoroText(t));
    }

    std::shared_ptr<LoroMap> as_loro_map() {
        ::LoroContainer *c = loro_value_or_container_get_container(raw_);
        if (!c) throw LoroError(detail::last_error_message());
        ::LoroMap *m = loro_container_get_map(c);
        loro_container_free(c);
        if (!m) throw LoroError("value-or-container is not a map container");
        return std::shared_ptr<LoroMap>(new LoroMap(m));
    }

    ~ValueOrContainer() { loro_value_or_container_free(raw_); }
    ValueOrContainer(const ValueOrContainer &) = delete;
    ValueOrContainer &operator=(const ValueOrContainer &) = delete;

private:
    explicit ValueOrContainer(::LoroValueOrContainer *raw) : raw_(raw) {}
    ::LoroValueOrContainer *raw_;
    friend struct LoroMap;
};

inline std::shared_ptr<ValueOrContainer> LoroMap::get(const std::string &key) {
    ::LoroValueOrContainer *voc =
        loro_map_get_value_or_container(map(), key.data(), key.size());
    if (!voc) throw LoroError(detail::last_error_message());
    return std::shared_ptr<ValueOrContainer>(new ValueOrContainer(voc));
}

inline std::vector<std::shared_ptr<ValueOrContainer>> LoroMap::values() {
    std::vector<std::shared_ptr<ValueOrContainer>> out;
    for (const auto &key : keys()) out.push_back(get(key));
    return out;
}

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
        return std::shared_ptr<LoroText>(new LoroText(t));
    }

    std::shared_ptr<LoroMap> get_map(const std::shared_ptr<ContainerIdLike> &id) {
        std::string name = detail::root_name(id, ContainerType(ContainerType::kMap{}));
        ::LoroMap *m = loro_doc_get_map(raw_, name.data(), name.size());
        if (!m) {
            std::string msg = detail::last_error_message();
            throw LoroError(msg.empty() ? "loro_doc_get_map returned null" : msg);
        }
        return std::shared_ptr<LoroMap>(new LoroMap(m));
    }

    std::vector<uint8_t> export_snapshot() {
        detail::Bytes b;
        detail::check(loro_doc_export_snapshot(raw_, b.out()));
        return b.to_vector();
    }

    ImportStatus import(const std::vector<uint8_t> &bytes) {
        detail::check(loro_doc_import(raw_, bytes.data(), bytes.size()));
        // Phase 0: the detailed success/pending span maps are deferred to Phase 4; the
        // conformance tests ignore the return value.
        return ImportStatus{};
    }

    void config_text_style(const std::shared_ptr<StyleConfigMap> &text_style) {
        detail::check(loro_doc_config_text_style(raw_, text_style->raw_));
    }

    PosQueryResult get_cursor_pos(const std::shared_ptr<Cursor> &cursor) {
        LoroPosQueryResult out;
        detail::check(loro_doc_get_cursor_pos(raw_, cursor->raw_, &out));
        PosQueryResult r;
        r.update = nullptr;  // loro-c does not return an updated cursor; the tests don't read it.
        r.current = AbsolutePosition{static_cast<uint32_t>(out.abs_pos), detail::from_c_side(out.side)};
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
