/*
 * loro/loro_ext.hpp — loro-cpp-shaped ergonomics layer (Phase 1 subset: value helpers).
 *
 * Ported near-verbatim from ../loro-cpp/include/loro/loro_ext.hpp. Phase 1 ships only the
 * `loro::ext` LoroValue construction / inspection helpers — pure C++ over the typed
 * `loro::LoroValue` from <loro.hpp>, no C ABI of their own. The lambda→callback adapters,
 * subscribe_* shortcuts, insert_container<T> templates, Result/try_call, and undo helpers
 * are added in later phases (they need container / subscription / undo surface).
 */
#ifndef LORO_CONFORMANCE_LORO_EXT_HPP
#define LORO_CONFORMANCE_LORO_EXT_HPP

#include <loro.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace loro {
namespace ext {

// ============================================================================
// LoroValue construction
// ============================================================================

inline LoroValue value_null() { return LoroValue(LoroValue::kNull{}); }
inline LoroValue value_from(std::nullptr_t) { return value_null(); }
inline LoroValue value_from(bool v) { return LoroValue(LoroValue::kBool{v}); }
inline LoroValue value_from(int v) {
    return LoroValue(LoroValue::kI64{static_cast<int64_t>(v)});
}
inline LoroValue value_from(int64_t v) { return LoroValue(LoroValue::kI64{v}); }
inline LoroValue value_from(double v) { return LoroValue(LoroValue::kDouble{v}); }
inline LoroValue value_from(std::string v) {
    return LoroValue(LoroValue::kString{std::move(v)});
}
inline LoroValue value_from(const char *v) {
    return LoroValue(LoroValue::kString{std::string(v)});
}
inline LoroValue value_from(std::string_view v) {
    return LoroValue(LoroValue::kString{std::string(v)});
}
inline LoroValue value_from(std::vector<uint8_t> v) {
    return LoroValue(LoroValue::kBinary{std::move(v)});
}
inline LoroValue value_from(std::vector<LoroValue> v) {
    return LoroValue(LoroValue::kList{std::move(v)});
}
inline LoroValue value_from(std::unordered_map<std::string, LoroValue> v) {
    return LoroValue(LoroValue::kMap{std::move(v)});
}
inline LoroValue value_from(ContainerId v) {
    return LoroValue(LoroValue::kContainer{std::move(v)});
}

// LoroValueLike — adapter wrapping any LoroValue so it can be passed to methods like
// LoroMap::insert / LoroList::push.
inline std::shared_ptr<LoroValueLike> value_like(LoroValue v) {
    struct Impl : public LoroValueLike {
        LoroValue value;
        explicit Impl(LoroValue v) : value(std::move(v)) {}
        LoroValue as_loro_value() override { return value; }
    };
    return std::make_shared<Impl>(std::move(v));
}

// One-stop overload: build a LoroValueLike from any of the value_from inputs.
template <class T>
inline std::shared_ptr<LoroValueLike> value_like_from(T &&v) {
    return value_like(value_from(std::forward<T>(v)));
}

// ============================================================================
// LoroValue inspection
// ============================================================================

inline bool value_is_null(const LoroValue &v) {
    return std::holds_alternative<LoroValue::kNull>(v.get_variant());
}
inline std::optional<bool> value_as_bool(const LoroValue &v) {
    if (auto *p = std::get_if<LoroValue::kBool>(&v.get_variant())) return p->value;
    return std::nullopt;
}
inline std::optional<int64_t> value_as_i64(const LoroValue &v) {
    if (auto *p = std::get_if<LoroValue::kI64>(&v.get_variant())) return p->value;
    return std::nullopt;
}
inline std::optional<double> value_as_double(const LoroValue &v) {
    if (auto *p = std::get_if<LoroValue::kDouble>(&v.get_variant())) return p->value;
    return std::nullopt;
}
inline std::optional<std::string> value_as_string(const LoroValue &v) {
    if (auto *p = std::get_if<LoroValue::kString>(&v.get_variant())) return p->value;
    return std::nullopt;
}
inline std::optional<std::vector<uint8_t>> value_as_binary(const LoroValue &v) {
    if (auto *p = std::get_if<LoroValue::kBinary>(&v.get_variant())) return p->value;
    return std::nullopt;
}
inline std::optional<std::vector<LoroValue>> value_as_list(const LoroValue &v) {
    if (auto *p = std::get_if<LoroValue::kList>(&v.get_variant())) return p->value;
    return std::nullopt;
}
inline std::optional<std::unordered_map<std::string, LoroValue>>
value_as_map(const LoroValue &v) {
    if (auto *p = std::get_if<LoroValue::kMap>(&v.get_variant())) return p->value;
    return std::nullopt;
}
inline std::optional<ContainerId> value_as_container(const LoroValue &v) {
    if (auto *p = std::get_if<LoroValue::kContainer>(&v.get_variant())) return p->value;
    return std::nullopt;
}

// ============================================================================
// LoroValue formatting
// ============================================================================

inline std::string container_type_to_string(const ContainerType &ty) {
    const auto &v = ty.get_variant();
    if (std::holds_alternative<ContainerType::kText>(v)) return "Text";
    if (std::holds_alternative<ContainerType::kMap>(v)) return "Map";
    if (std::holds_alternative<ContainerType::kList>(v)) return "List";
    if (std::holds_alternative<ContainerType::kMovableList>(v)) return "MovableList";
    if (std::holds_alternative<ContainerType::kTree>(v)) return "Tree";
    if (std::holds_alternative<ContainerType::kCounter>(v)) return "Counter";
    return "Unknown";
}

// Human-readable LoroValue formatter. Recursive; renders containers as `<container ...>`
// so circular references via ContainerId can't loop.
inline std::string value_to_string(const LoroValue &v) {
    std::ostringstream os;
    const auto &var = v.get_variant();
    if (std::holds_alternative<LoroValue::kNull>(var)) {
        os << "null";
    } else if (auto *b = std::get_if<LoroValue::kBool>(&var)) {
        os << (b->value ? "true" : "false");
    } else if (auto *d = std::get_if<LoroValue::kDouble>(&var)) {
        os << d->value;
    } else if (auto *i = std::get_if<LoroValue::kI64>(&var)) {
        os << i->value;
    } else if (auto *bin = std::get_if<LoroValue::kBinary>(&var)) {
        os << "<binary " << bin->value.size() << " bytes>";
    } else if (auto *s = std::get_if<LoroValue::kString>(&var)) {
        os << '"' << s->value << '"';
    } else if (auto *l = std::get_if<LoroValue::kList>(&var)) {
        os << '[';
        bool first = true;
        for (const auto &el : l->value) {
            if (!first) os << ", ";
            first = false;
            os << value_to_string(el);
        }
        os << ']';
    } else if (auto *m = std::get_if<LoroValue::kMap>(&var)) {
        os << '{';
        bool first = true;
        for (const auto &kv : m->value) {
            if (!first) os << ", ";
            first = false;
            os << '"' << kv.first << "\": " << value_to_string(kv.second);
        }
        os << '}';
    } else if (auto *c = std::get_if<LoroValue::kContainer>(&var)) {
        const auto &cid = c->value.get_variant();
        if (auto *r = std::get_if<ContainerId::kRoot>(&cid)) {
            os << "<container root \"" << r->name << "\":"
               << container_type_to_string(r->container_type) << '>';
        } else if (auto *n = std::get_if<ContainerId::kNormal>(&cid)) {
            os << "<container " << n->peer << '@' << n->counter << ':'
               << container_type_to_string(n->container_type) << '>';
        } else {
            os << "<container>";
        }
    }
    return os.str();
}

}  // namespace ext
}  // namespace loro

#endif  // LORO_CONFORMANCE_LORO_EXT_HPP
