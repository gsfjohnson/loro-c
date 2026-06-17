/*
 * loro/loro_ext.hpp — loro-cpp-shaped ergonomics layer (Phases 1–2 subset).
 *
 * Ported near-verbatim from ../loro-cpp/include/loro/loro_ext.hpp. Pure C++ over the typed
 * `loro::LoroValue` / container wrappers from <loro.hpp>, with no C ABI of their own.
 *   Phase 1: LoroValue construction / inspection / formatting helpers.
 *   Phase 2: ContainerId / ContainerIdLike factories (root_*, container_id_like) and the
 *            templated container insertion helpers (insert_container<T> /
 *            get_or_create_container<T> / set_container<T>) over the new container surface.
 * The lambda→callback adapters, subscribe_* shortcuts, Result/try_call, and undo helpers are
 * added in later phases (they need the subscription / undo surface).
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
#include <utility>
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

// ============================================================================
// ContainerId / ContainerIdLike helpers
// ============================================================================

inline ContainerId root_id(std::string name, ContainerType ty) {
    return ContainerId(ContainerId::kRoot{std::move(name), std::move(ty)});
}
inline ContainerId root_text(std::string name) {
    return root_id(std::move(name), ContainerType(ContainerType::kText{}));
}
inline ContainerId root_map(std::string name) {
    return root_id(std::move(name), ContainerType(ContainerType::kMap{}));
}
inline ContainerId root_list(std::string name) {
    return root_id(std::move(name), ContainerType(ContainerType::kList{}));
}
inline ContainerId root_movable_list(std::string name) {
    return root_id(std::move(name), ContainerType(ContainerType::kMovableList{}));
}
inline ContainerId root_tree(std::string name) {
    return root_id(std::move(name), ContainerType(ContainerType::kTree{}));
}
inline ContainerId root_counter(std::string name) {
    return root_id(std::move(name), ContainerType(ContainerType::kCounter{}));
}

// `LoroDoc::get_text` etc. take a ContainerIdLike that composes the full ContainerId once
// loro reports the container type. For "root container by name", this adapter echoes the
// requested type, so one instance works for get_text / get_map / get_list / ...
inline std::shared_ptr<ContainerIdLike> root(std::string name) {
    struct Impl : public ContainerIdLike {
        std::string name;
        explicit Impl(std::string n) : name(std::move(n)) {}
        ContainerId as_container_id(ContainerType ty) override {
            return ContainerId(ContainerId::kRoot{name, std::move(ty)});
        }
    };
    return std::make_shared<Impl>(std::move(name));
}

// Adapter for callers that already hold a fully-resolved ContainerId.
inline std::shared_ptr<ContainerIdLike> container_id_like(ContainerId cid) {
    struct Impl : public ContainerIdLike {
        ContainerId cid;
        explicit Impl(ContainerId c) : cid(std::move(c)) {}
        ContainerId as_container_id(ContainerType /*ty*/) override { return cid; }
    };
    return std::make_shared<Impl>(std::move(cid));
}

// ============================================================================
// Templated container insertion
// ============================================================================
//
// The wrappers expose per-type methods (`insert_text_container`, `insert_map_container`, ...).
// The traits below dispatch on the child type so callers can write
// `insert_container<LoroText>(map, "title")` once.

template <class C>
struct container_traits;

#define LORO_EXT_CONTAINER_TRAITS(Type, suffix)                                       \
    template <>                                                                       \
    struct container_traits<Type> {                                                   \
        static std::shared_ptr<Type> create() { return Type::init(); }                \
        static std::shared_ptr<Type> map_insert(LoroMap &m, const std::string &k,     \
                                                std::shared_ptr<Type> child) {        \
            return m.insert_##suffix##_container(k, std::move(child));                \
        }                                                                             \
        static std::shared_ptr<Type> map_get_or_create(LoroMap &m, const std::string &k, \
                                                       std::shared_ptr<Type> child) { \
            return m.get_or_create_##suffix##_container(k, std::move(child));         \
        }                                                                             \
        static std::shared_ptr<Type> list_insert(LoroList &l, uint32_t pos,           \
                                                 std::shared_ptr<Type> child) {       \
            return l.insert_##suffix##_container(pos, std::move(child));              \
        }                                                                             \
        static std::shared_ptr<Type> mlist_insert(LoroMovableList &l, uint32_t pos,   \
                                                  std::shared_ptr<Type> child) {      \
            return l.insert_##suffix##_container(pos, std::move(child));              \
        }                                                                             \
        static std::shared_ptr<Type> mlist_set(LoroMovableList &l, uint32_t pos,      \
                                               std::shared_ptr<Type> child) {         \
            return l.set_##suffix##_container(pos, std::move(child));                 \
        }                                                                             \
    };

LORO_EXT_CONTAINER_TRAITS(LoroText, text)
LORO_EXT_CONTAINER_TRAITS(LoroMap, map)
LORO_EXT_CONTAINER_TRAITS(LoroList, list)
LORO_EXT_CONTAINER_TRAITS(LoroMovableList, movable_list)
LORO_EXT_CONTAINER_TRAITS(LoroTree, tree)
LORO_EXT_CONTAINER_TRAITS(LoroCounter, counter)

#undef LORO_EXT_CONTAINER_TRAITS

template <class C>
inline std::shared_ptr<C> insert_container(LoroMap &parent, const std::string &key,
                                           std::shared_ptr<C> child) {
    return container_traits<C>::map_insert(parent, key, std::move(child));
}
template <class C>
inline std::shared_ptr<C> insert_container(LoroMap &parent, const std::string &key) {
    return container_traits<C>::map_insert(parent, key, container_traits<C>::create());
}

template <class C>
inline std::shared_ptr<C> get_or_create_container(LoroMap &parent, const std::string &key,
                                                  std::shared_ptr<C> child) {
    return container_traits<C>::map_get_or_create(parent, key, std::move(child));
}
template <class C>
inline std::shared_ptr<C> get_or_create_container(LoroMap &parent, const std::string &key) {
    return container_traits<C>::map_get_or_create(parent, key, container_traits<C>::create());
}

template <class C>
inline std::shared_ptr<C> insert_container(LoroList &parent, uint32_t pos,
                                           std::shared_ptr<C> child) {
    return container_traits<C>::list_insert(parent, pos, std::move(child));
}
template <class C>
inline std::shared_ptr<C> insert_container(LoroList &parent, uint32_t pos) {
    return container_traits<C>::list_insert(parent, pos, container_traits<C>::create());
}

template <class C>
inline std::shared_ptr<C> insert_container(LoroMovableList &parent, uint32_t pos,
                                           std::shared_ptr<C> child) {
    return container_traits<C>::mlist_insert(parent, pos, std::move(child));
}
template <class C>
inline std::shared_ptr<C> insert_container(LoroMovableList &parent, uint32_t pos) {
    return container_traits<C>::mlist_insert(parent, pos, container_traits<C>::create());
}

template <class C>
inline std::shared_ptr<C> set_container(LoroMovableList &parent, uint32_t pos,
                                        std::shared_ptr<C> child) {
    return container_traits<C>::mlist_set(parent, pos, std::move(child));
}
template <class C>
inline std::shared_ptr<C> set_container(LoroMovableList &parent, uint32_t pos) {
    return container_traits<C>::mlist_set(parent, pos, container_traits<C>::create());
}

}  // namespace ext
}  // namespace loro

#endif  // LORO_CONFORMANCE_LORO_EXT_HPP
