/*
 * loro/loro_ext.hpp — loro-cpp-shaped ergonomics layer.
 *
 * Ported near-verbatim from loro-cpp's include/loro/loro_ext.hpp. Pure C++ over the typed
 * `loro::LoroValue` / container wrappers from <loro.hpp>, with no C ABI of their own:
 *   - LoroValue construction / inspection / formatting helpers.
 *   - ContainerId / ContainerIdLike factories (root_*, container_id_like) and the templated
 *     container insertion helpers (insert_container<T> / get_or_create_container<T> /
 *     set_container<T>).
 *   - Lambda → callback-interface adapters (on_diff, on_local_update, on_undo_push/pop, ...)
 *     and the subscribe_* / set_on_push / set_on_pop shortcuts.
 */
#ifndef LORO_LORO_EXT_HPP
#define LORO_LORO_EXT_HPP

#include <loro.hpp>

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
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
    return LoroValue(LoroValue::kList{std::make_shared<std::vector<LoroValue>>(std::move(v))});
}
inline LoroValue value_from(std::unordered_map<std::string, LoroValue> v) {
    return LoroValue(LoroValue::kMap{
        std::make_shared<std::unordered_map<std::string, LoroValue>>(std::move(v))});
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
    if (auto *p = std::get_if<LoroValue::kList>(&v.get_variant())) return *p->value;
    return std::nullopt;
}
inline std::optional<std::unordered_map<std::string, LoroValue>>
value_as_map(const LoroValue &v) {
    if (auto *p = std::get_if<LoroValue::kMap>(&v.get_variant())) return *p->value;
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
        for (const auto &el : *l->value) {
            if (!first) os << ", ";
            first = false;
            os << value_to_string(el);
        }
        os << ']';
    } else if (auto *m = std::get_if<LoroValue::kMap>(&var)) {
        os << '{';
        bool first = true;
        for (const auto &kv : *m->value) {
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

// ============================================================================
// Subscriptions (RESHAPE Phase 3)
// ============================================================================
//
// Lambda → callback-interface adapters and `subscribe_*` shortcuts, ported from
// ../loro-cpp/include/loro/loro_ext.hpp. Each adapter wraps a std::function in a one-off
// subclass of the matching pure-virtual interface (idiom shared with `value_like`).

inline std::shared_ptr<Subscriber> on_diff(std::function<void(const DiffEvent &)> fn) {
    struct Impl : public Subscriber {
        std::function<void(const DiffEvent &)> fn;
        explicit Impl(std::function<void(const DiffEvent &)> f) : fn(std::move(f)) {}
        void on_diff(const DiffEvent &diff) override {
            if (fn) fn(diff);
        }
    };
    return std::make_shared<Impl>(std::move(fn));
}

inline std::shared_ptr<LocalUpdateCallback> on_local_update(
    std::function<void(const std::vector<uint8_t> &)> fn) {
    struct Impl : public LocalUpdateCallback {
        std::function<void(const std::vector<uint8_t> &)> fn;
        explicit Impl(std::function<void(const std::vector<uint8_t> &)> f) : fn(std::move(f)) {}
        void on_local_update(const std::vector<uint8_t> &update) override {
            if (fn) fn(update);
        }
    };
    return std::make_shared<Impl>(std::move(fn));
}

inline std::shared_ptr<EphemeralSubscriber> on_ephemeral(
    std::function<void(const EphemeralStoreEvent &)> fn) {
    struct Impl : public EphemeralSubscriber {
        std::function<void(const EphemeralStoreEvent &)> fn;
        explicit Impl(std::function<void(const EphemeralStoreEvent &)> f) : fn(std::move(f)) {}
        void on_ephemeral_event(const EphemeralStoreEvent &event) override {
            if (fn) fn(event);
        }
    };
    return std::make_shared<Impl>(std::move(fn));
}

inline std::shared_ptr<FirstCommitFromPeerCallback> on_first_commit_from_peer(
    std::function<void(const FirstCommitFromPeerPayload &)> fn) {
    struct Impl : public FirstCommitFromPeerCallback {
        std::function<void(const FirstCommitFromPeerPayload &)> fn;
        explicit Impl(std::function<void(const FirstCommitFromPeerPayload &)> f)
            : fn(std::move(f)) {}
        void on_first_commit_from_peer(const FirstCommitFromPeerPayload &p) override {
            if (fn) fn(p);
        }
    };
    return std::make_shared<Impl>(std::move(fn));
}

inline std::shared_ptr<PreCommitCallback> on_pre_commit(
    std::function<void(const PreCommitCallbackPayload &)> fn) {
    struct Impl : public PreCommitCallback {
        std::function<void(const PreCommitCallbackPayload &)> fn;
        explicit Impl(std::function<void(const PreCommitCallbackPayload &)> f) : fn(std::move(f)) {}
        void on_pre_commit(const PreCommitCallbackPayload &p) override {
            if (fn) fn(p);
        }
    };
    return std::make_shared<Impl>(std::move(fn));
}

// subscribe_* shortcuts on LoroDoc.
inline std::shared_ptr<Subscription> subscribe_root(LoroDoc &doc,
                                                    std::function<void(const DiffEvent &)> fn) {
    return doc.subscribe_root(on_diff(std::move(fn)));
}
inline std::shared_ptr<Subscription> subscribe(LoroDoc &doc, ContainerId cid,
                                               std::function<void(const DiffEvent &)> fn) {
    return doc.subscribe(std::move(cid), on_diff(std::move(fn)));
}
inline std::shared_ptr<Subscription> subscribe_local_update(
    LoroDoc &doc, std::function<void(const std::vector<uint8_t> &)> fn) {
    return doc.subscribe_local_update(on_local_update(std::move(fn)));
}

// EphemeralStore subscribe shortcuts (mirror the LoroDoc forms above).
inline std::shared_ptr<Subscription> subscribe(
    EphemeralStore &store, std::function<void(const EphemeralStoreEvent &)> fn) {
    return store.subscribe(on_ephemeral(std::move(fn)));
}
inline std::shared_ptr<Subscription> subscribe_local_update(
    EphemeralStore &store, std::function<void(const std::vector<uint8_t> &)> fn) {
    return store.subscribe_local_updates(on_local_update(std::move(fn)));
}
inline std::shared_ptr<Subscription> subscribe_first_commit_from_peer(
    LoroDoc &doc, std::function<void(const FirstCommitFromPeerPayload &)> fn) {
    return doc.subscribe_first_commit_from_peer(on_first_commit_from_peer(std::move(fn)));
}
inline std::shared_ptr<Subscription> subscribe_pre_commit(
    LoroDoc &doc, std::function<void(const PreCommitCallbackPayload &)> fn) {
    return doc.subscribe_pre_commit(on_pre_commit(std::move(fn)));
}

// Per-container subscribe shortcuts (every container exposes `subscribe(shared_ptr<Subscriber>)`).
inline std::shared_ptr<Subscription> subscribe(LoroText &c,
                                               std::function<void(const DiffEvent &)> fn) {
    return c.subscribe(on_diff(std::move(fn)));
}
inline std::shared_ptr<Subscription> subscribe(LoroMap &c,
                                               std::function<void(const DiffEvent &)> fn) {
    return c.subscribe(on_diff(std::move(fn)));
}
inline std::shared_ptr<Subscription> subscribe(LoroList &c,
                                               std::function<void(const DiffEvent &)> fn) {
    return c.subscribe(on_diff(std::move(fn)));
}
inline std::shared_ptr<Subscription> subscribe(LoroMovableList &c,
                                               std::function<void(const DiffEvent &)> fn) {
    return c.subscribe(on_diff(std::move(fn)));
}
inline std::shared_ptr<Subscription> subscribe(LoroTree &c,
                                               std::function<void(const DiffEvent &)> fn) {
    return c.subscribe(on_diff(std::move(fn)));
}
inline std::shared_ptr<Subscription> subscribe(LoroCounter &c,
                                               std::function<void(const DiffEvent &)> fn) {
    return c.subscribe(on_diff(std::move(fn)));
}

// ============================================================================
// Undo listeners (RESHAPE Phase 4)
// ============================================================================
//
// Lambda → OnPush/OnPop adapters and set_on_push/set_on_pop shortcuts, ported from
// ../loro-cpp/include/loro/loro_ext.hpp (same one-off-subclass idiom as the Phase 3 adapters).

inline std::shared_ptr<OnPush> on_undo_push(
    std::function<UndoItemMeta(const UndoOrRedo &, const CounterSpan &, std::optional<DiffEvent>)>
        fn) {
    struct Impl : public OnPush {
        std::function<UndoItemMeta(const UndoOrRedo &, const CounterSpan &,
                                   std::optional<DiffEvent>)>
            fn;
        explicit Impl(std::function<UndoItemMeta(const UndoOrRedo &, const CounterSpan &,
                                                 std::optional<DiffEvent>)>
                          f)
            : fn(std::move(f)) {}
        UndoItemMeta on_push(const UndoOrRedo &uor, const CounterSpan &span,
                             std::optional<DiffEvent> diff) override {
            if (fn) return fn(uor, span, std::move(diff));
            return UndoItemMeta{LoroValue(LoroValue::kNull{}), {}};
        }
    };
    return std::make_shared<Impl>(std::move(fn));
}

inline std::shared_ptr<OnPop> on_undo_pop(
    std::function<void(const UndoOrRedo &, const CounterSpan &, const UndoItemMeta &)> fn) {
    struct Impl : public OnPop {
        std::function<void(const UndoOrRedo &, const CounterSpan &, const UndoItemMeta &)> fn;
        explicit Impl(
            std::function<void(const UndoOrRedo &, const CounterSpan &, const UndoItemMeta &)> f)
            : fn(std::move(f)) {}
        void on_pop(const UndoOrRedo &uor, const CounterSpan &span,
                    const UndoItemMeta &meta) override {
            if (fn) fn(uor, span, meta);
        }
    };
    return std::make_shared<Impl>(std::move(fn));
}

// set_on_*  shortcuts on UndoManager.
inline void set_on_push(
    UndoManager &um,
    std::function<UndoItemMeta(const UndoOrRedo &, const CounterSpan &, std::optional<DiffEvent>)>
        fn) {
    um.set_on_push(on_undo_push(std::move(fn)));
}
inline void set_on_pop(
    UndoManager &um,
    std::function<void(const UndoOrRedo &, const CounterSpan &, const UndoItemMeta &)> fn) {
    um.set_on_pop(on_undo_pop(std::move(fn)));
}

// ============================================================================
// Result<T> / try_call — fallible-method adapter
// ============================================================================
//
// The wrappers throw LoroError (a runtime_error subclass) on Rust failure. `try_call`
// wraps a callable and converts the throw into a Result<T> so callers can branch without
// writing their own try/catch. LoroError is preserved as a std::exception_ptr; rethrow()
// restores it. Ported from ../loro-cpp/include/loro/loro_ext.hpp.

template <class T>
class Result {
public:
    Result(T v) : data_(std::move(v)) {}
    Result(std::exception_ptr e) : data_(e) {}

    bool ok() const noexcept { return std::holds_alternative<T>(data_); }
    explicit operator bool() const noexcept { return ok(); }

    T &value() & { return std::get<T>(data_); }
    const T &value() const & { return std::get<T>(data_); }
    T &&value() && { return std::move(std::get<T>(data_)); }

    std::exception_ptr error() const noexcept {
        if (auto *p = std::get_if<std::exception_ptr>(&data_)) return *p;
        return {};
    }

    void rethrow() const {
        if (auto *p = std::get_if<std::exception_ptr>(&data_)) {
            std::rethrow_exception(*p);
        }
    }

    std::string error_what() const {
        if (auto *p = std::get_if<std::exception_ptr>(&data_)) {
            try { std::rethrow_exception(*p); }
            catch (const std::exception &e) { return e.what(); }
            catch (...) { return "<non-std exception>"; }
        }
        return {};
    }

private:
    std::variant<T, std::exception_ptr> data_;
};

template <>
class Result<void> {
public:
    Result() = default;
    Result(std::exception_ptr e) : err_(e) {}

    bool ok() const noexcept { return !err_; }
    explicit operator bool() const noexcept { return ok(); }

    std::exception_ptr error() const noexcept { return err_; }
    void rethrow() const { if (err_) std::rethrow_exception(err_); }

    std::string error_what() const {
        if (!err_) return {};
        try { std::rethrow_exception(err_); }
        catch (const std::exception &e) { return e.what(); }
        catch (...) { return "<non-std exception>"; }
    }

private:
    std::exception_ptr err_{};
};

template <class F>
inline auto try_call(F &&f) -> Result<std::invoke_result_t<F>> {
    using R = std::invoke_result_t<F>;
    try {
        if constexpr (std::is_void_v<R>) {
            std::forward<F>(f)();
            return Result<void>{};
        } else {
            return Result<R>{std::forward<F>(f)()};
        }
    } catch (...) {
        return Result<R>{std::current_exception()};
    }
}

}  // namespace ext
}  // namespace loro

#endif  // LORO_LORO_EXT_HPP
