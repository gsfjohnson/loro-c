/*
 * loro.hpp — loro-c's public C++ API: a faithful loro-cpp drop-in.
 *
 * A loro-cpp-SHAPED C++ API (namespace `loro`, shared_ptr ownership, typed `LoroValue`,
 * `init()` factories, callback interfaces) implemented as a thin wrapper over loro-c's
 * C ABI in <loro/loro.h>. Names, ownership and member spellings match loro-cpp's generated
 * header so a translation unit written against loro-cpp compiles unchanged against loro-c
 * (see pm/RESHAPE.md). The ergonomic free-function layer lives in <loro/loro_ext.hpp>.
 *
 * Surface: LoroDoc; the six container types — LoroText, LoroMap, LoroList, LoroMovableList,
 * LoroTree (+ TreeId / TreeParentId), LoroCounter — with their accessors and the full
 * insert_* / set_* container matrix; the typed LoroValue / TextDelta / ContainerId families
 * and the ContainerIdLike / LoroValueLike interfaces (with a typed-value FFI bridge so binary
 * and integer-valued doubles survive — no lossy JSON round-trip); Cursor, StyleConfigMap,
 * VersionVector / Frontiers, EphemeralStore, Awareness, UndoManager, subscriptions / commit
 * hooks, jsonpath and fractional index; ContainerId <-> "cid:" string conversion.
 *
 * Names match loro-cpp's generated header field-for-field (uniffi quirks included:
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
#ifndef LORO_LORO_HPP
#define LORO_LORO_HPP

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

/// A JSONPath query failure (loro-cpp `JsonPathError`). Standalone (like loro-cpp) so app code
/// `catch (loro::JsonPathError &)` resolves. The C ABI does not report which uniffi variant
/// failed, so [`LoroDoc::jsonpath`] throws the base type.
struct JsonPathError : std::runtime_error {
    JsonPathError() : std::runtime_error("") {}
    explicit JsonPathError(const std::string &what_arg) : std::runtime_error(what_arg) {}
    ~JsonPathError() override = default;
};

namespace json_path_error {
/// The path string was not valid JSONPath (loro-cpp `json_path_error::InvalidJsonPath`).
struct InvalidJsonPath : JsonPathError {
    using JsonPathError::JsonPathError;
};
/// The path was valid but could not be evaluated (loro-cpp `json_path_error::EvaluationError`).
struct EvaluationError : JsonPathError {
    using JsonPathError::JsonPathError;
};
}  // namespace json_path_error

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
struct Awareness;
struct VersionVector;
struct Frontiers;

// RESHAPE Phase 3 — subscriptions / events / commit hooks.
struct Subscriber;
struct LocalUpdateCallback;
struct FirstCommitFromPeerCallback;
struct PreCommitCallback;
struct ChangeMeta;
struct ChangeModifier;
struct PreCommitCallbackPayload;

// RESHAPE Phase 4 — undo manager.
struct UndoManager;
struct OnPush;
struct OnPop;
struct UndoItemMeta;
struct CursorWithPos;

namespace detail {
struct Factory;  // privileged construction helper; defined after the wrapper classes.
struct FrontiersFactory;          // builds a Frontiers from an owned ::LoroFrontiers* (Phase 3).
struct SubscriptionFactory;       // builds a Subscription from a ::LoroSubscription* (Phase 3).
struct PreCommitPayloadBuilder;   // builds a ChangeModifier from a callback-scoped payload (Phase 3).
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

/// Indexing coordinate system for text positions (loro-cpp `PosType`). Mirrors the C ABI
/// `LoroPosType` — the common subset of `loro::cursor::PosType`.
enum class PosType : int32_t {
    kBytes = 0,
    kUnicode = 1,
    kUtf16 = 2,
};

/// The causal ordering of two version vectors (loro-cpp `Ordering`). GOTCHA: values are 1/2/3 —
/// the C ABI `loro_version_vector_compare` writes -1/0/1, so map (see
/// [`VersionVector::partial_cmp`]), never cast.
enum class Ordering : int32_t {
    kLess = 1,
    kEqual = 2,
    kGreater = 3,
};

/// Whether a pushed/popped undo item belongs to the undo or the redo stack (loro-cpp
/// `UndoOrRedo`). GOTCHA: the C ABI `LoroUndoOrRedo` is `LORO_UNDO=0 / LORO_REDO=1`; map via
/// [`detail::from_c_undo_or_redo`], never cast.
enum class UndoOrRedo : int32_t {
    kUndo = 1,
    kRedo = 2,
};

/// How a diff event was triggered (loro-cpp `EventTriggerKind`). Mirrors the C ABI
/// `LoroEventTriggerKind`.
enum class EventTriggerKind : int32_t {
    kLocal = 0,
    kImport = 1,
    kCheckout = 2,
};

/// The kind of a container diff (loro-cpp `Diff` union tag). Mirrors the C ABI `LoroDiffKind`.
/// Phase 3 ships the kind only; the typed `Diff` payload is deferred (see [`ContainerDiff`]).
enum class DiffKind : int32_t {
    kList = 0,
    kText = 1,
    kMap = 2,
    kTree = 3,
    kCounter = 4,
    kUnknown = 5,
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

/// An operation id (loro-cpp `Id`). Mirrors the C ABI `LoroId`. Brace-initialisable as
/// `loro::Id{peer, counter}`.
struct Id {
    uint64_t peer;
    int32_t counter;
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

/// One step in a diff [`PathItem`] (uniffi `Index`): a map key, a list index (uniffi quirk
/// `kSeq`), or a tree node.
struct Index {
    struct kKey {
        std::string key;
    };
    struct kSeq {
        uint32_t index;
    };
    struct kNode {
        TreeId target;
    };
    Index(kKey variant) : variant(std::move(variant)) {}
    Index(kSeq variant) : variant(variant) {}
    Index(kNode variant) : variant(variant) {}

    const std::variant<kKey, kSeq, kNode> &get_variant() const { return variant; }

private:
    std::variant<kKey, kSeq, kNode> variant;
};

/// A single hop on the path from the root to a changed container (uniffi `PathItem`).
struct PathItem {
    ContainerId container;
    Index index;
};

struct CounterSpan {
    int32_t start;
    int32_t end;
};

/// A contiguous span of one peer's ops (uniffi `IdSpan`). NOTE: `counter` is a nested
/// [`CounterSpan`] — NOT the flat `counter_start`/`counter_end` the C ABI `LoroIdSpan` uses.
struct IdSpan {
    uint64_t peer;
    CounterSpan counter;
};

/// The two-way delta between version vectors (uniffi `VersionVectorDiff`): the spans to add when
/// moving right-to-left (`retreat`) and left-to-right (`forward`).
struct VersionVectorDiff {
    std::unordered_map<uint64_t, CounterSpan> retreat;
    std::unordered_map<uint64_t, CounterSpan> forward;
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

/// The peer of a first-commit-from-peer event (loro-cpp `FirstCommitFromPeerPayload`).
struct FirstCommitFromPeerPayload {
    uint64_t peer;
};

/// One container's diff within a [`DiffEvent`] (loro-cpp `ContainerDiff`).
///
/// Phase 3 ships the *envelope*: the changed container (`target`), the path from the root
/// (`path`), whether the container type is unknown (`is_unknown`), and the diff `kind`. The
/// typed `Diff` payload (the list/text/map/tree delta items with values) is deferred per
/// RESHAPE.md (gated on an audit of the app's `on_diff` handlers); a caller needing it today
/// can read the C ABI `loro_container_diff_to_json`.
struct ContainerDiff {
    ContainerId target;
    std::vector<PathItem> path;
    bool is_unknown;
    DiffKind kind;
};

/// An owned diff event (loro-cpp `DiffEvent`). Unlike loro-c's callback-scoped C view, this is a
/// fully-owned value the subscriber may stash beyond the callback (e.g. `current_target`).
struct DiffEvent {
    EventTriggerKind triggered_by;
    std::string origin;
    std::optional<ContainerId> current_target;
    std::vector<ContainerDiff> events;
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
        // Reference-counted indirection mirrors loro's `Arc<Vec<LoroValue>>` (O(1) clone) and
        // breaks the recursive instantiation: a `std::shared_ptr<X>` member permits an incomplete
        // `X`, so the `std::vector`/`std::unordered_map` is only instantiated at the `make_shared`
        // call sites (where `LoroValue` is complete). A direct container member would force the
        // container's instantiation here, where `LoroValue` is still incomplete — which libstdc++
        // rejects (libc++/MSVC tolerate it).
        std::shared_ptr<std::vector<LoroValue>> value;
    };
    struct kMap {
        std::shared_ptr<std::unordered_map<std::string, LoroValue>> value;
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

/// A cursor paired with its resolved absolute position (loro-cpp `CursorWithPos`).
struct CursorWithPos {
    std::shared_ptr<Cursor> cursor;
    AbsolutePosition pos;
};

/// Metadata an [`OnPush`] listener attaches to an undo item (loro-cpp `UndoItemMeta`): a typed
/// `value` plus any `cursors` to restore. Cursors round-trip through push→pop; loro transforms
/// each stored cursor by intervening ops, so on_pop reports its replayed absolute position.
struct UndoItemMeta {
    LoroValue value;
    std::vector<CursorWithPos> cursors;
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

// ----------------------------------------------- subscription callback interfaces

/// Diff subscriber (loro-cpp `Subscriber`). `on_diff` receives an owned [`DiffEvent`].
struct Subscriber {
    virtual ~Subscriber() {}
    virtual void on_diff(const DiffEvent &diff) = 0;
};

/// Local-update subscriber (loro-cpp `LocalUpdateCallback`): the raw update bytes of each local
/// commit.
struct LocalUpdateCallback {
    virtual ~LocalUpdateCallback() {}
    virtual void on_local_update(const std::vector<uint8_t> &update) = 0;
};

/// First-commit-from-peer subscriber (loro-cpp `FirstCommitFromPeerCallback`).
struct FirstCommitFromPeerCallback {
    virtual ~FirstCommitFromPeerCallback() {}
    virtual void on_first_commit_from_peer(const FirstCommitFromPeerPayload &payload) = 0;
};

/// Pre-commit hook (loro-cpp `PreCommitCallback`); may rewrite the commit's message/timestamp
/// via the payload's [`ChangeModifier`].
struct PreCommitCallback {
    virtual ~PreCommitCallback() {}
    virtual void on_pre_commit(const PreCommitCallbackPayload &payload) = 0;
};

/// Undo on-push listener (loro-cpp `OnPush`). Fires when an item is pushed onto the undo (or
/// redo) stack; returns the [`UndoItemMeta`] to attach to it. `diff_event` is present only when
/// the push came from a local edit.
struct OnPush {
    virtual ~OnPush() {}
    virtual UndoItemMeta on_push(const UndoOrRedo &undo_or_redo, const CounterSpan &span,
                                 std::optional<DiffEvent> diff_event) = 0;
};

/// Undo on-pop listener (loro-cpp `OnPop`). Fires when an item is popped during undo/redo;
/// receives the [`UndoItemMeta`] a prior `on_push` attached.
struct OnPop {
    virtual ~OnPop() {}
    virtual void on_pop(const UndoOrRedo &undo_or_redo, const CounterSpan &span,
                        const UndoItemMeta &undo_meta) = 0;
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

inline LoroPosType to_c_pos_type(PosType p) {
    switch (p) {
        case PosType::kBytes: return LORO_POS_BYTES;
        case PosType::kUnicode: return LORO_POS_UNICODE;
        case PosType::kUtf16: return LORO_POS_UTF16;
    }
    return LORO_POS_UNICODE;
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

/// Maps the C `LoroContainerType` enum to a `ContainerType` variant. Returns nullopt for
/// `LORO_CONTAINER_UNKNOWN` (and any unrecognised value) — i.e. "not a known live container".
inline std::optional<ContainerType> container_type_from_c(LoroContainerType ty) {
    switch (ty) {
        case LORO_CONTAINER_MAP: return ContainerType(ContainerType::kMap{});
        case LORO_CONTAINER_LIST: return ContainerType(ContainerType::kList{});
        case LORO_CONTAINER_TEXT: return ContainerType(ContainerType::kText{});
        case LORO_CONTAINER_MOVABLE_LIST: return ContainerType(ContainerType::kMovableList{});
        case LORO_CONTAINER_TREE: return ContainerType(ContainerType::kTree{});
        case LORO_CONTAINER_COUNTER: return ContainerType(ContainerType::kCounter{});
        case LORO_CONTAINER_UNKNOWN: break;
    }
    return std::nullopt;
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

inline ::LoroId to_c_id(const Id &id) { return ::LoroId{id.peer, id.counter}; }

inline Id from_c_id(const ::LoroId &c) { return Id{c.peer, c.counter}; }

/// Flattens the nested loro-cpp [`IdSpan`] into the C ABI's flat `LoroIdSpan`.
inline ::LoroIdSpan to_c_id_span(const IdSpan &s) {
    return ::LoroIdSpan{s.peer, s.counter.start, s.counter.end};
}

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
            return LoroValue(
                LoroValue::kList{std::make_shared<std::vector<LoroValue>>(std::move(out))});
        }
        case K::Object: {
            std::unordered_map<std::string, LoroValue> out;
            for (const auto &kv : j.obj) out.emplace(kv.first, json_to_value(kv.second));
            return LoroValue(LoroValue::kMap{
                std::make_shared<std::unordered_map<std::string, LoroValue>>(std::move(out))});
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
            } else if constexpr (std::is_same_v<T, LoroValue::kList>) {
                std::string out = "[";
                bool first = true;
                for (const auto &el : *alt.value) {
                    if (!first) out += ",";
                    first = false;
                    out += value_to_json(el);
                }
                out += "]";
                return out;
            } else if constexpr (std::is_same_v<T, LoroValue::kMap>) {
                std::string out = "{";
                bool first = true;
                for (const auto &kv : *alt.value) {
                    if (!first) out += ",";
                    first = false;
                    json_escape(kv.first, out);  // appends a quoted/escaped key
                    out += ":";
                    out += value_to_json(kv.second);
                }
                out += "}";
                return out;
            } else {  // kBinary, kContainer
                throw LoroError("value_to_json: binary/container LoroValue has no JSON form "
                                "(mark values must be scalar, list, or map)");
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
                for (const auto &el : *alt.value) {
                    CValue child(to_c_value(el));
                    check(loro_value_list_push(list, child.get()));
                }
                return list;
            } else if constexpr (std::is_same_v<T, LoroValue::kMap>) {
                ::LoroValue *map = loro_value_new_map();
                if (!map) throw LoroError("loro_value_new_map returned null");
                for (const auto &kv : *alt.value) {
                    CValue child(to_c_value(kv.second));
                    check(loro_value_map_insert(map, kv.first.data(), kv.first.size(),
                                                child.get()));
                }
                return map;
            } else {  // kContainer
                throw LoroError("to_c_value: container-valued LoroValue cannot be written as a "
                                "value; attach child containers via insert_container instead");
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
            return LoroValue(
                LoroValue::kList{std::make_shared<std::vector<LoroValue>>(std::move(out))});
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
            return LoroValue(LoroValue::kMap{
                std::make_shared<std::unordered_map<std::string, LoroValue>>(std::move(out))});
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

namespace detail {

/// Maps the C ABI `LoroUndoOrRedo` (`LORO_UNDO=0 / LORO_REDO=1`) onto loro-cpp's
/// [`UndoOrRedo`] (`kUndo=1 / kRedo=2`). The numeric values differ — map, never cast.
inline UndoOrRedo from_c_undo_or_redo(::LoroUndoOrRedo k) {
    return k == LORO_REDO ? UndoOrRedo::kRedo : UndoOrRedo::kUndo;
}

/// Builds an owned [`ImportStatus`] from a `::LoroImportStatus*` (the handle a
/// `loro_doc_import_*_status` call produced) and frees the handle. A null handle yields an
/// empty status.
inline ImportStatus import_status_from_c(::LoroImportStatus *st) {
    ImportStatus result;
    if (!st) return result;
    uintptr_t n = loro_import_status_success_len(st);
    for (uintptr_t i = 0; i < n; ++i) {
        ::LoroPeerCounterSpan e;
        if (loro_import_status_success_at(st, i, &e)) {
            result.success.emplace(e.peer, CounterSpan{e.start, e.end});
        }
    }
    if (loro_import_status_has_pending(st)) {
        std::unordered_map<uint64_t, CounterSpan> pending;
        uintptr_t pn = loro_import_status_pending_len(st);
        for (uintptr_t i = 0; i < pn; ++i) {
            ::LoroPeerCounterSpan e;
            if (loro_import_status_pending_at(st, i, &e)) {
                pending.emplace(e.peer, CounterSpan{e.start, e.end});
            }
        }
        result.pending = std::move(pending);
    }
    loro_import_status_free(st);
    return result;
}

}  // namespace detail

// -------------------------------------------------------------- reference types

struct Cursor {
    /// Builds a cursor from its parts (loro-cpp `Cursor::init`): the optional anchor element `id`
    /// (`std::nullopt` = a stable start/end anchor), the `container` it lives in, the `side` to
    /// anchor on, and the `origin_pos` at build time.
    static std::shared_ptr<Cursor> init(std::optional<Id> id, ContainerId container,
                                        const Side &side, uint32_t origin_pos) {
        std::string cid = detail::container_id_to_cid_string(container);
        ::LoroId anchor;
        const ::LoroId *idp = nullptr;
        if (id) {
            anchor = detail::to_c_id(*id);
            idp = &anchor;
        }
        ::LoroCursor *c =
            loro_cursor_new(idp, cid.data(), cid.size(), detail::to_c_side(side), origin_pos);
        if (!c) throw LoroError(detail::last_error_message());
        return std::shared_ptr<Cursor>(new Cursor(c));
    }
    static std::shared_ptr<Cursor> decode(const std::vector<uint8_t> &data) {
        ::LoroCursor *c = loro_cursor_decode(data.data(), data.size());
        if (!c) throw LoroError(detail::last_error_message());
        return std::shared_ptr<Cursor>(new Cursor(c));
    }
    std::vector<uint8_t> encode() {
        detail::Bytes b;
        detail::check(loro_cursor_encode(raw_, b.out()));
        return b.to_vector();
    }

    ~Cursor() { loro_cursor_free(raw_); }
    Cursor(const Cursor &) = delete;
    Cursor &operator=(const Cursor &) = delete;

private:
    explicit Cursor(::LoroCursor *raw) : raw_(raw) {}
    ::LoroCursor *raw_;
    friend struct LoroText;
    friend struct LoroList;
    friend struct LoroMovableList;
    friend struct LoroDoc;
    friend struct detail::Factory;
};

struct LoroText {
    /// Constructs a *detached* text container (loro-c: `loro_container_new`). It is editable
    /// while detached; attach it by passing it to a parent's `insert_text_container`, which
    /// consumes it and returns the attached handle (replaying any buffered ops).
    static std::shared_ptr<LoroText> init() {
        ::LoroContainer *c = loro_container_new(LORO_CONTAINER_TEXT);
        if (!c) throw LoroError("loro_container_new(Text) returned null");
        ::LoroText *t = loro_container_get_text(c);  // typed handle sharing detached state
        if (!t) {
            loro_container_free(c);
            throw LoroError("loro_container_get_text returned null");
        }
        return std::shared_ptr<LoroText>(new LoroText(c, t));
    }

    void insert(uint32_t pos, const std::string &s) {
        detail::check(loro_text_insert(raw_, pos, s.data(), s.size()));
    }
    /// Like `insert`, but `pos` is a UTF-8 **byte** offset (not a codepoint index).
    void insert_utf8(uint32_t pos, const std::string &s) {
        detail::check(loro_text_insert_utf8(raw_, pos, s.data(), s.size()));
    }
    void push_str(const std::string &s) {
        detail::check(loro_text_push_str(raw_, s.data(), s.size()));
    }
    void delete_(uint32_t pos, uint32_t len) {
        detail::check(loro_text_delete(raw_, pos, len));
    }
    /// Like `delete_`, but `pos`/`len` are UTF-8 **byte** offsets/lengths.
    void delete_utf8(uint32_t pos, uint32_t len) {
        detail::check(loro_text_delete_utf8(raw_, pos, len));
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
    /// Like `mark`, but `from`/`to` are UTF-8 **byte** indices.
    void mark_utf8(uint32_t from, uint32_t to, const std::string &key,
                   const std::shared_ptr<LoroValueLike> &value) {
        LoroValue v = value->as_loro_value();
        std::string json = detail::value_to_json(v);
        detail::check(loro_text_mark_utf8(raw_, from, to, key.data(), key.size(), json.data(),
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

    /// The rich-text value as a delta-with-attributes array `LoroValue` (a list of
    /// `{"insert": "...", "attributes": {...}}` maps). The C ABI emits this as JSON; parsed here
    /// via the same idiom as `to_delta` / `EphemeralStore::get_all_states`.
    LoroValue get_richtext_value() {
        detail::Bytes b;
        detail::check(loro_text_get_richtext_value(raw_, b.out()));
        std::string json = b.to_string();
        if (json.empty()) {
            return LoroValue(LoroValue::kList{std::make_shared<std::vector<LoroValue>>()});
        }
        return detail::json_to_value(detail::parse_json(json));
    }

    std::shared_ptr<Cursor> get_cursor(uint32_t pos, const Side &side) {
        ::LoroCursor *c = loro_text_get_cursor(raw_, pos, detail::to_c_side(side));
        if (!c) return nullptr;
        return std::shared_ptr<Cursor>(new Cursor(c));
    }

    /// Converts `pos` from the `from` coordinate system to `to`. Returns nullopt if the
    /// position is out of bounds or the conversion is unsupported.
    std::optional<uint32_t> convert_pos(uint32_t pos, PosType from, PosType to) {
        uintptr_t out = 0;
        LoroStatus s = loro_text_convert_pos(raw_, static_cast<uintptr_t>(pos),
                                             detail::to_c_pos_type(from),
                                             detail::to_c_pos_type(to), &out);
        if (s == LORO_ERR_INVALID_ARG) return std::nullopt;
        detail::check(s);
        return static_cast<uint32_t>(out);
    }

    bool is_attached() { return raw_ ? loro_text_is_attached(raw_) : false; }
    bool is_empty() { return loro_text_is_empty(raw_); }

    std::shared_ptr<Subscription> subscribe(
        const std::shared_ptr<Subscriber> &subscriber);  // out-of-line

    ~LoroText() {
        if (raw_) loro_text_free(raw_);
        if (container_) loro_container_free(container_);
    }
    LoroText(const LoroText &) = delete;
    LoroText &operator=(const LoroText &) = delete;

private:
    explicit LoroText(::LoroText *raw) : raw_(raw) {}  // attached
    // detached (from init()): a typed editing handle plus the type-erased container it shares
    // state with; the container is consumed on attach, the typed handle frees independently.
    LoroText(::LoroContainer *c, ::LoroText *raw) : raw_(raw), container_(c) {}

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
        ::LoroMap *m = loro_container_get_map(c);  // typed handle sharing detached state
        if (!m) {
            loro_container_free(c);
            throw LoroError("loro_container_get_map returned null");
        }
        return std::shared_ptr<LoroMap>(new LoroMap(c, m));
    }

    void insert(const std::string &key, const std::shared_ptr<LoroValueLike> &value) {
        LoroValue v = value->as_loro_value();
        detail::CValue cv(detail::to_c_value(v));
        detail::check(loro_map_insert_value(map(), key.data(), key.size(), cv.get()));
    }

    std::shared_ptr<ValueOrContainer> get(const std::string &key);  // out-of-line

    /// Returns whether `key` is present (loro-cpp parity helper). Non-throwing.
    bool contains(const std::string &key);  // out-of-line

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

    std::shared_ptr<Subscription> subscribe(
        const std::shared_ptr<Subscriber> &subscriber);  // out-of-line

    ~LoroMap() {
        if (raw_) loro_map_free(raw_);
        if (container_) loro_container_free(container_);
    }
    LoroMap(const LoroMap &) = delete;
    LoroMap &operator=(const LoroMap &) = delete;

private:
    explicit LoroMap(::LoroMap *raw) : raw_(raw) {}  // attached
    // detached (from init()): typed editing handle + the type-erased container it shares state with.
    LoroMap(::LoroContainer *c, ::LoroMap *raw) : raw_(raw), container_(c) {}

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
        ::LoroList *l = loro_container_get_list(c);  // typed handle sharing detached state
        if (!l) {
            loro_container_free(c);
            throw LoroError("loro_container_get_list returned null");
        }
        return std::shared_ptr<LoroList>(new LoroList(c, l));
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

    std::shared_ptr<Cursor> get_cursor(uint32_t pos, const Side &side) {
        ::LoroCursor *c = loro_list_get_cursor(list(), pos, detail::to_c_side(side));
        if (!c) return nullptr;
        return std::shared_ptr<Cursor>(new Cursor(c));
    }

    std::vector<LoroValue> to_vec() {
        ::LoroValue *cv = loro_list_get_value(list());
        if (!cv) throw LoroError(detail::last_error_message());
        detail::CValue g(cv);
        LoroValue v = detail::from_c_value(cv);
        if (auto *l = std::get_if<LoroValue::kList>(&v.get_variant())) return *l->value;
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

    std::shared_ptr<Subscription> subscribe(
        const std::shared_ptr<Subscriber> &subscriber);  // out-of-line

    ~LoroList() {
        if (raw_) loro_list_free(raw_);
        if (container_) loro_container_free(container_);
    }
    LoroList(const LoroList &) = delete;
    LoroList &operator=(const LoroList &) = delete;

private:
    explicit LoroList(::LoroList *raw) : raw_(raw) {}  // attached
    // detached (from init()): typed editing handle + the type-erased container it shares state with.
    LoroList(::LoroContainer *c, ::LoroList *raw) : raw_(raw), container_(c) {}

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
        ::LoroMovableList *l = loro_container_get_movable_list(c);  // shares detached state
        if (!l) {
            loro_container_free(c);
            throw LoroError("loro_container_get_movable_list returned null");
        }
        return std::shared_ptr<LoroMovableList>(new LoroMovableList(c, l));
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

    std::shared_ptr<Cursor> get_cursor(uint32_t pos, const Side &side) {
        ::LoroCursor *c = loro_movable_list_get_cursor(list(), pos, detail::to_c_side(side));
        if (!c) return nullptr;
        return std::shared_ptr<Cursor>(new Cursor(c));
    }

    std::vector<LoroValue> to_vec() {
        ::LoroValue *cv = loro_movable_list_get_value(list());
        if (!cv) throw LoroError(detail::last_error_message());
        detail::CValue g(cv);
        LoroValue v = detail::from_c_value(cv);
        if (auto *l = std::get_if<LoroValue::kList>(&v.get_variant())) return *l->value;
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

    std::shared_ptr<Subscription> subscribe(
        const std::shared_ptr<Subscriber> &subscriber);  // out-of-line

    ~LoroMovableList() {
        if (raw_) loro_movable_list_free(raw_);
        if (container_) loro_container_free(container_);
    }
    LoroMovableList(const LoroMovableList &) = delete;
    LoroMovableList &operator=(const LoroMovableList &) = delete;

private:
    explicit LoroMovableList(::LoroMovableList *raw) : raw_(raw) {}  // attached
    // detached (from init()): typed editing handle + the type-erased container it shares state with.
    LoroMovableList(::LoroContainer *c, ::LoroMovableList *raw) : raw_(raw), container_(c) {}

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
        ::LoroTree *t = loro_container_get_tree(c);  // typed handle sharing detached state
        if (!t) {
            loro_container_free(c);
            throw LoroError("loro_container_get_tree returned null");
        }
        return std::shared_ptr<LoroTree>(new LoroTree(c, t));
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

    std::shared_ptr<Subscription> subscribe(
        const std::shared_ptr<Subscriber> &subscriber);  // out-of-line

    ~LoroTree() {
        if (raw_) loro_tree_free(raw_);
        if (container_) loro_container_free(container_);
    }
    LoroTree(const LoroTree &) = delete;
    LoroTree &operator=(const LoroTree &) = delete;

private:
    explicit LoroTree(::LoroTree *raw) : raw_(raw) {}  // attached
    // detached (from init()): typed editing handle + the type-erased container it shares state with.
    LoroTree(::LoroContainer *c, ::LoroTree *raw) : raw_(raw), container_(c) {}

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
        ::LoroCounter *cn = loro_container_get_counter(c);  // typed handle sharing detached state
        if (!cn) {
            loro_container_free(c);
            throw LoroError("loro_container_get_counter returned null");
        }
        return std::shared_ptr<LoroCounter>(new LoroCounter(c, cn));
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

    std::shared_ptr<Subscription> subscribe(
        const std::shared_ptr<Subscriber> &subscriber);  // out-of-line

    ~LoroCounter() {
        if (raw_) loro_counter_free(raw_);
        if (container_) loro_container_free(container_);
    }
    LoroCounter(const LoroCounter &) = delete;
    LoroCounter &operator=(const LoroCounter &) = delete;

private:
    explicit LoroCounter(::LoroCounter *raw) : raw_(raw) {}  // attached
    // detached (from init()): typed editing handle + the type-erased container it shares state with.
    LoroCounter(::LoroContainer *c, ::LoroCounter *raw) : raw_(raw), container_(c) {}

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
    bool is_value() { return !is_container(); }

    /// The kind of the live child container, or nullopt when this holds a plain value.
    std::optional<ContainerType> container_type() {
        if (!is_container()) return std::nullopt;
        return detail::container_type_from_c(loro_value_or_container_container_type(raw_));
    }

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

    /// Adopts an owned `LoroCursor*` as a `Cursor` wrapper (used by the undo on_pop trampoline).
    static std::shared_ptr<Cursor> wrap_cursor(::LoroCursor *r) {
        return std::shared_ptr<Cursor>(new Cursor(r));
    }
    /// Borrows the raw `LoroCursor*` backing a `Cursor` (used by the undo on_push trampoline).
    static ::LoroCursor *cursor_raw(const std::shared_ptr<Cursor> &c) { return c->raw_; }

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

/// Resolves a `ContainerIdLike` to a typed container handle for `LoroDoc::get_*`. Root
/// (by-name) ids take the get-or-create `by_name` path (`loro_doc_get_*`); normal/nested ids
/// take the non-creating, typed `try_by_cid` path (`loro_doc_try_get_*`), which returns null if
/// the container does not exist (or is not that type). Defined after `Factory` so its `wrap`
/// overloads are visible.
template <typename CType>
auto get_container_typed(::LoroDoc *doc, const std::shared_ptr<ContainerIdLike> &id,
                         ContainerType ty,
                         CType *(*by_name)(const ::LoroDoc *, const char *, uintptr_t),
                         CType *(*try_by_cid)(const ::LoroDoc *, const char *, uintptr_t),
                         const char *what) -> decltype(Factory::wrap(std::declval<CType *>())) {
    ContainerId cid = id->as_container_id(std::move(ty));
    CType *h = nullptr;
    if (auto *root = std::get_if<ContainerId::kRoot>(&cid.get_variant())) {
        h = by_name(doc, root->name.data(), root->name.size());  // get-or-create
    } else {
        std::string s = container_id_to_cid_string(cid);
        h = try_by_cid(doc, s.data(), s.size());  // non-creating; null if absent / wrong type
    }
    if (!h) {
        std::string msg = last_error_message();
        throw LoroError(msg.empty() ? std::string(what) + " returned null" : msg);
    }
    return Factory::wrap(h);
}

}  // namespace detail

// --------------------------------------------- out-of-line wrapper definitions

inline std::shared_ptr<ValueOrContainer> LoroMap::get(const std::string &key) {
    ::LoroValueOrContainer *voc =
        loro_map_get_value_or_container(map(), key.data(), key.size());
    if (!voc) return nullptr;  // absent key → null, matching loro-cpp
    return detail::Factory::voc(voc);
}

inline bool LoroMap::contains(const std::string &key) { return get(key) != nullptr; }

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
    if (!voc) return nullptr;  // out-of-range index → null, matching loro-cpp
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
    if (!voc) return nullptr;  // out-of-range index → null, matching loro-cpp
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

    std::optional<int32_t> get_last(uint64_t peer) {
        int32_t out = 0;
        LoroStatus s = loro_version_vector_get_last(raw_, peer, &out);
        if (s == LORO_ERR_NOT_FOUND) return std::nullopt;
        detail::check(s);
        return out;
    }

    /// Causal partial order. Returns `std::nullopt` when the two are concurrent (incomparable).
    std::optional<Ordering> partial_cmp(const std::shared_ptr<VersionVector> &other) {
        int32_t cmp = 0;
        LoroStatus s = loro_version_vector_compare(raw_, other->raw_, &cmp);
        if (s == LORO_ERR_NOT_FOUND) return std::nullopt;
        detail::check(s);
        if (cmp < 0) return Ordering::kLess;
        if (cmp > 0) return Ordering::kGreater;
        return Ordering::kEqual;
    }

    bool includes_id(const Id &id) {
        return loro_version_vector_includes_id(raw_, detail::to_c_id(id));
    }
    bool includes_vv(const std::shared_ptr<VersionVector> &other) {
        return loro_version_vector_includes_vv(raw_, other->raw_);
    }
    void merge(const std::shared_ptr<VersionVector> &other) {
        detail::check(loro_version_vector_merge(raw_, other->raw_));
    }
    void extend_to_include_vv(const std::shared_ptr<VersionVector> &other) {
        detail::check(loro_version_vector_extend_to_include_vv(raw_, other->raw_));
    }
    void set_last(const Id &id) {
        detail::check(loro_version_vector_set_last(raw_, detail::to_c_id(id)));
    }
    void set_end(const Id &id) {
        detail::check(loro_version_vector_set_end(raw_, detail::to_c_id(id)));
    }
    bool try_update_last(const Id &id) {
        bool updated = false;
        detail::check(loro_version_vector_try_update_last(raw_, detail::to_c_id(id), &updated));
        return updated;
    }

    /// The spans in `target` this vector is missing. NOTE: peers cross as JSON numbers here, so
    /// very large peer ids beyond `int64_t` lose precision (a conformance-parser limit; the gate
    /// uses small peers).
    std::vector<IdSpan> get_missing_span(const std::shared_ptr<VersionVector> &target) {
        detail::Bytes b;
        detail::check(loro_version_vector_get_missing_span(raw_, target->raw_, b.out()));
        detail::JsonValue root = detail::parse_json(b.to_string());
        std::vector<IdSpan> out;
        for (const auto &e : root.arr) {
            const auto *peer = e.find("peer");
            const auto *cs = e.find("counter_start");
            const auto *ce = e.find("counter_end");
            if (!peer || !cs || !ce) continue;
            out.push_back(IdSpan{static_cast<uint64_t>(peer->i),
                                 CounterSpan{static_cast<int32_t>(cs->i),
                                             static_cast<int32_t>(ce->i)}});
        }
        return out;
    }

    std::optional<CounterSpan> intersect_span(const IdSpan &target) {
        ::LoroCounterSpan out;
        if (!loro_version_vector_intersect_span(raw_, detail::to_c_id_span(target), &out))
            return std::nullopt;
        return CounterSpan{out.start, out.end};
    }

    std::unordered_map<uint64_t, int32_t> to_hashmap() {
        detail::Bytes b;
        detail::check(loro_version_vector_to_json(raw_, b.out()));
        detail::JsonValue root = detail::parse_json(b.to_string());
        std::unordered_map<uint64_t, int32_t> out;
        for (const auto &kv : root.obj) {
            out[static_cast<uint64_t>(std::stoull(kv.first))] = static_cast<int32_t>(kv.second.i);
        }
        return out;
    }

    VersionVectorDiff diff(const std::shared_ptr<VersionVector> &rhs) {
        detail::Bytes b;
        detail::check(loro_version_vector_diff(raw_, rhs->raw_, b.out()));
        detail::JsonValue root = detail::parse_json(b.to_string());
        VersionVectorDiff out;
        auto fill = [](const detail::JsonValue *arr,
                       std::unordered_map<uint64_t, CounterSpan> &m) {
            if (!arr) return;
            for (const auto &e : arr->arr) {
                const auto *peer = e.find("peer");
                const auto *cs = e.find("counter_start");
                const auto *ce = e.find("counter_end");
                if (!peer || !cs || !ce) continue;
                m[static_cast<uint64_t>(peer->i)] =
                    CounterSpan{static_cast<int32_t>(cs->i), static_cast<int32_t>(ce->i)};
            }
        };
        fill(root.find("retreat"), out.retreat);
        fill(root.find("forward"), out.forward);
        return out;
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

    static std::shared_ptr<Frontiers> from_ids(const std::vector<Id> &ids) {
        std::vector<::LoroId> cids;
        cids.reserve(ids.size());
        for (const auto &id : ids) cids.push_back(detail::to_c_id(id));
        ::LoroFrontiers *f = loro_frontiers_from_ids(cids.data(), cids.size());
        if (!f) throw LoroError(detail::last_error_message());
        return std::shared_ptr<Frontiers>(new Frontiers(f));
    }
    static std::shared_ptr<Frontiers> from_id(const Id &id) {
        return from_ids(std::vector<Id>{id});
    }
    std::vector<Id> to_vec() {
        std::vector<Id> out;
        std::size_t n = loro_frontiers_len(raw_);
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            ::LoroId c;
            detail::check(loro_frontiers_get(raw_, i, &c));
            out.push_back(detail::from_c_id(c));
        }
        return out;
    }

    ~Frontiers() { loro_frontiers_free(raw_); }
    Frontiers(const Frontiers &) = delete;
    Frontiers &operator=(const Frontiers &) = delete;

private:
    explicit Frontiers(::LoroFrontiers *raw) : raw_(raw) {}
    ::LoroFrontiers *raw_;
    friend struct LoroDoc;
    friend struct detail::FrontiersFactory;
};

namespace detail {
/// Adopts an owned `::LoroFrontiers*` (e.g. from `loro_change_meta_deps`) as a [`Frontiers`].
struct FrontiersFactory {
    static std::shared_ptr<Frontiers> adopt(::LoroFrontiers *raw) {
        return std::shared_ptr<Frontiers>(new Frontiers(raw));
    }
};
}  // namespace detail

/// A position label for ordering tree siblings (loro-cpp `FractionalIndex`). Free with
/// `loro_fractional_index_free`. Obtain one from `LoroTree::fractional_index` (the hex string) +
/// `from_hex_string`, or directly from raw bytes.
struct FractionalIndex {
    static std::shared_ptr<FractionalIndex> from_bytes(const std::vector<uint8_t> &bytes) {
        ::LoroFractionalIndex *fi = loro_fractional_index_from_bytes(bytes.data(), bytes.size());
        if (!fi) throw LoroError(detail::last_error_message());
        return std::shared_ptr<FractionalIndex>(new FractionalIndex(fi));
    }
    static std::shared_ptr<FractionalIndex> from_hex_string(const std::string &str) {
        ::LoroFractionalIndex *fi = loro_fractional_index_from_string(str.data(), str.size());
        if (!fi) throw LoroError(detail::last_error_message());
        return std::shared_ptr<FractionalIndex>(new FractionalIndex(fi));
    }
    std::string to_string() const {
        detail::Bytes b;
        detail::check(loro_fractional_index_to_string(raw_, b.out()));
        return b.to_string();
    }

    ~FractionalIndex() { loro_fractional_index_free(raw_); }
    FractionalIndex(const FractionalIndex &) = delete;
    FractionalIndex &operator=(const FractionalIndex &) = delete;

private:
    explicit FractionalIndex(::LoroFractionalIndex *raw) : raw_(raw) {}
    ::LoroFractionalIndex *raw_;
};

// ------------------------------------------------------- change metadata / hooks

/// Metadata for one change (loro-cpp `ChangeMeta`). `deps` owns its [`Frontiers`].
struct ChangeMeta {
    uint32_t lamport;
    Id id;
    int64_t timestamp;
    std::optional<std::string> message;
    std::shared_ptr<Frontiers> deps;
    uint32_t len;
};

/// Rewrites the message / timestamp of the commit being processed (loro-cpp `ChangeModifier`).
///
/// **Lifetime:** borrows the callback-scoped `const LoroPreCommitPayload*` and is valid **only**
/// for the duration of `PreCommitCallback::on_pre_commit`. Do not store it and call it later.
struct ChangeModifier {
    void set_message(const std::string &msg) {
        detail::check(loro_pre_commit_payload_set_message(payload_, msg.data(), msg.size()));
    }
    void set_timestamp(int64_t timestamp) {
        detail::check(loro_pre_commit_payload_set_timestamp(payload_, timestamp));
    }

    ChangeModifier(const ChangeModifier &) = delete;
    ChangeModifier &operator=(const ChangeModifier &) = delete;

private:
    explicit ChangeModifier(const ::LoroPreCommitPayload *payload) : payload_(payload) {}
    const ::LoroPreCommitPayload *payload_;
    friend struct detail::PreCommitPayloadBuilder;
};

namespace detail {
/// Builds a callback-scoped [`ChangeModifier`] over a borrowed pre-commit payload.
struct PreCommitPayloadBuilder {
    static std::shared_ptr<ChangeModifier> modifier(const ::LoroPreCommitPayload *payload) {
        return std::shared_ptr<ChangeModifier>(new ChangeModifier(payload));
    }
};
}  // namespace detail

/// Payload of a pre-commit hook (loro-cpp `PreCommitCallbackPayload`).
struct PreCommitCallbackPayload {
    ChangeMeta change_meta;
    std::string origin;
    std::shared_ptr<ChangeModifier> modifier;
};

namespace detail {
/// Copies a callback-scoped `const LoroChangeMeta*` into an owned [`ChangeMeta`]. `deps` is
/// rebuilt from the change's own owned `loro_change_meta_deps`, so the result outlives the C
/// handle. An empty C message becomes `std::nullopt` (the C ABI gives no Some("") signal here).
inline ChangeMeta change_meta_from_c(const ::LoroChangeMeta *cm) {
    ChangeMeta m;
    ::LoroId cid = loro_change_meta_id(cm);
    m.id = Id{cid.peer, cid.counter};
    m.lamport = loro_change_meta_lamport(cm);
    m.timestamp = loro_change_meta_timestamp(cm);
    m.len = static_cast<uint32_t>(loro_change_meta_len(cm));
    Bytes msg;
    if (loro_change_meta_message(cm, msg.out()) == LORO_OK) {
        std::string s = msg.to_string();
        m.message = s.empty() ? std::nullopt : std::optional<std::string>(std::move(s));
    } else {
        m.message = std::nullopt;
    }
    ::LoroFrontiers *deps = loro_change_meta_deps(cm);  // owned
    m.deps = deps ? FrontiersFactory::adopt(deps) : Frontiers::init();
    return m;
}
}  // namespace detail

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

    // Resolves a root (by-name) OR a normal/nested (`{counter}@{peer}`) ContainerId. Roots take
    // the get-or-create `loro_doc_get_*` path; nested ids take the non-creating typed
    // `loro_doc_try_get_*` path (see detail::get_container_typed).
    std::shared_ptr<LoroText> get_text(const std::shared_ptr<ContainerIdLike> &id) {
        return detail::get_container_typed<::LoroText>(
            raw_, id, ContainerType(ContainerType::kText{}), loro_doc_get_text,
            loro_doc_try_get_text, "loro_doc_get_text");
    }

    std::shared_ptr<LoroMap> get_map(const std::shared_ptr<ContainerIdLike> &id) {
        return detail::get_container_typed<::LoroMap>(
            raw_, id, ContainerType(ContainerType::kMap{}), loro_doc_get_map,
            loro_doc_try_get_map, "loro_doc_get_map");
    }

    std::shared_ptr<LoroList> get_list(const std::shared_ptr<ContainerIdLike> &id) {
        return detail::get_container_typed<::LoroList>(
            raw_, id, ContainerType(ContainerType::kList{}), loro_doc_get_list,
            loro_doc_try_get_list, "loro_doc_get_list");
    }

    std::shared_ptr<LoroMovableList> get_movable_list(
        const std::shared_ptr<ContainerIdLike> &id) {
        return detail::get_container_typed<::LoroMovableList>(
            raw_, id, ContainerType(ContainerType::kMovableList{}), loro_doc_get_movable_list,
            loro_doc_try_get_movable_list, "loro_doc_get_movable_list");
    }

    std::shared_ptr<LoroTree> get_tree(const std::shared_ptr<ContainerIdLike> &id) {
        return detail::get_container_typed<::LoroTree>(
            raw_, id, ContainerType(ContainerType::kTree{}), loro_doc_get_tree,
            loro_doc_try_get_tree, "loro_doc_get_tree");
    }

    std::shared_ptr<LoroCounter> get_counter(const std::shared_ptr<ContainerIdLike> &id) {
        return detail::get_container_typed<::LoroCounter>(
            raw_, id, ContainerType(ContainerType::kCounter{}), loro_doc_get_counter,
            loro_doc_try_get_counter, "loro_doc_get_counter");
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
    std::shared_ptr<VersionVector> oplog_vv() {
        ::LoroVersionVector *v = loro_doc_oplog_vv(raw_);
        if (!v) throw LoroError(detail::last_error_message());
        return std::shared_ptr<VersionVector>(new VersionVector(v));
    }
    std::shared_ptr<Frontiers> state_frontiers() {
        ::LoroFrontiers *f = loro_doc_state_frontiers(raw_);
        if (!f) throw LoroError(detail::last_error_message());
        return std::shared_ptr<Frontiers>(new Frontiers(f));
    }
    std::shared_ptr<Frontiers> oplog_frontiers() {
        ::LoroFrontiers *f = loro_doc_oplog_frontiers(raw_);
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
        ::LoroImportStatus *st = nullptr;
        detail::check(loro_doc_import_json_updates_status(raw_, json.data(), json.size(), &st));
        return detail::import_status_from_c(st);
    }

    ImportStatus import(const std::vector<uint8_t> &bytes) {
        ::LoroImportStatus *st = nullptr;
        detail::check(loro_doc_import_status(raw_, bytes.data(), bytes.size(), &st));
        return detail::import_status_from_c(st);
    }

    ImportStatus import_with(const std::vector<uint8_t> &bytes, const std::string &origin) {
        ::LoroImportStatus *st = nullptr;
        detail::check(loro_doc_import_with_status(raw_, bytes.data(), bytes.size(), origin.data(),
                                                  origin.size(), &st));
        return detail::import_status_from_c(st);
    }

    ImportStatus import_batch(const std::vector<std::vector<uint8_t>> &bytes) {
        std::vector<const uint8_t *> datas;
        std::vector<uintptr_t> lens;
        datas.reserve(bytes.size());
        lens.reserve(bytes.size());
        for (const auto &b : bytes) {
            datas.push_back(b.data());
            lens.push_back(b.size());
        }
        ::LoroImportStatus *st = nullptr;
        detail::check(
            loro_doc_import_batch_status(raw_, datas.data(), lens.data(), datas.size(), &st));
        return detail::import_status_from_c(st);
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
        ::LoroCursor *update = nullptr;
        detail::check(loro_doc_get_cursor_pos_full(raw_, cursor->raw_, &out, &update));
        PosQueryResult r;
        r.update = update ? std::shared_ptr<Cursor>(new Cursor(update)) : nullptr;
        r.current =
            AbsolutePosition{static_cast<uint32_t>(out.abs_pos), detail::from_c_side(out.side)};
        return r;
    }

    // ---- path / jsonpath queries (RESHAPE Phase 5) ----
    // Defined inline: this LoroDoc body sits after detail::Factory, so Factory::voc is visible.

    std::shared_ptr<ValueOrContainer> get_by_str_path(const std::string &path) {
        ::LoroValueOrContainer *voc = loro_doc_get_by_str_path(raw_, path.data(), path.size());
        if (!voc) throw LoroError(detail::last_error_message());
        return detail::Factory::voc(voc);
    }

    std::shared_ptr<ValueOrContainer> get_by_path(const std::vector<Index> &path) {
        // Own the key strings: LoroPathComponent.key is borrowed for the duration of the call.
        std::vector<std::string> keys;
        keys.reserve(path.size());
        for (const auto &step : path) {
            if (auto *k = std::get_if<Index::kKey>(&step.get_variant()))
                keys.push_back(k->key);
            else
                keys.emplace_back();
        }
        std::vector<::LoroPathComponent> comps;
        comps.reserve(path.size());
        for (std::size_t i = 0; i < path.size(); ++i) {
            const auto &v = path[i].get_variant();
            ::LoroPathComponent c{};
            if (std::holds_alternative<Index::kKey>(v)) {
                c.kind = LORO_PATH_KEY;
                c.key = keys[i].data();
                c.key_len = keys[i].size();
            } else if (auto *s = std::get_if<Index::kSeq>(&v)) {
                c.kind = LORO_PATH_SEQ;
                c.seq = s->index;
            } else {
                c.kind = LORO_PATH_NODE;
                c.node = detail::to_c_tree_id(std::get<Index::kNode>(v).target);
            }
            comps.push_back(c);
        }
        ::LoroValueOrContainer *voc = loro_doc_get_by_path(raw_, comps.data(), comps.size());
        if (!voc) throw LoroError(detail::last_error_message());
        return detail::Factory::voc(voc);
    }

    std::vector<std::shared_ptr<ValueOrContainer>> jsonpath(const std::string &path) {
        ::LoroJsonPathResults *r = loro_doc_jsonpath(raw_, path.data(), path.size());
        if (!r) throw JsonPathError(detail::last_error_message());
        std::vector<std::shared_ptr<ValueOrContainer>> out;
        try {
            std::size_t n = loro_jsonpath_results_len(r);
            out.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                ::LoroValueOrContainer *voc = loro_jsonpath_results_get_value_or_container(r, i);
                if (!voc) throw LoroError(detail::last_error_message());
                out.push_back(detail::Factory::voc(voc));
            }
        } catch (...) {
            loro_jsonpath_results_free(r);
            throw;
        }
        loro_jsonpath_results_free(r);
        return out;
    }

    // ---- change introspection / commit config (RESHAPE Phase 3) ----

    void set_record_timestamp(bool record) { loro_doc_set_record_timestamp(raw_, record); }

    /// Attaches `origin` to the next commit (reported to subscribers, not persisted).
    void set_next_commit_origin(const std::string &origin) {
        detail::check(loro_doc_set_next_commit_origin(raw_, origin.data(), origin.size()));
    }

    uint64_t len_changes() { return static_cast<uint64_t>(loro_doc_len_changes(raw_)); }
    uintptr_t len_ops() { return loro_doc_len_ops(raw_); }

    /// True when the doc is checked out to a non-latest version (history browsing mode).
    bool is_detached() { return loro_doc_is_detached(raw_); }

    std::optional<ChangeMeta> get_change(const Id &id) {
        ::LoroId cid{id.peer, id.counter};
        ::LoroChangeMetaOwned *owned = nullptr;
        LoroStatus s = loro_doc_get_change(raw_, cid, &owned);
        if (s == LORO_ERR_NOT_FOUND) return std::nullopt;
        detail::check(s);
        if (!owned) return std::nullopt;
        ChangeMeta m = detail::change_meta_from_c(loro_change_meta_owned_as_ref(owned));
        loro_change_meta_owned_free(owned);
        return m;
    }

    std::vector<ContainerId> get_changed_containers_in(const Id &id, uint32_t len) {
        ::LoroId cid{id.peer, id.counter};
        detail::Bytes b;
        detail::check(loro_doc_get_changed_containers_in(raw_, cid, len, b.out()));
        std::vector<ContainerId> out;
        for (const auto &s : detail::parse_string_array(b.to_string())) {
            out.push_back(detail::cid_string_to_container_id(s));
        }
        return out;
    }

    // ---- subscriptions (RESHAPE Phase 3) — defined out-of-line after Subscription ----

    std::shared_ptr<Subscription> subscribe(const ContainerId &cid,
                                            const std::shared_ptr<Subscriber> &subscriber);
    std::shared_ptr<Subscription> subscribe_root(const std::shared_ptr<Subscriber> &subscriber);
    std::shared_ptr<Subscription> subscribe_local_update(
        const std::shared_ptr<LocalUpdateCallback> &callback);
    std::shared_ptr<Subscription> subscribe_first_commit_from_peer(
        const std::shared_ptr<FirstCommitFromPeerCallback> &callback);
    std::shared_ptr<Subscription> subscribe_pre_commit(
        const std::shared_ptr<PreCommitCallback> &callback);

    ~LoroDoc() { loro_doc_free(raw_); }
    LoroDoc(const LoroDoc &) = delete;
    LoroDoc &operator=(const LoroDoc &) = delete;

private:
    explicit LoroDoc(::LoroDoc *raw) : raw_(raw) {}
    ::LoroDoc *raw_;
    friend struct UndoManager;  // UndoManager::init needs the raw doc handle (Phase 4).
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
    friend struct detail::SubscriptionFactory;
};

namespace detail {
/// Wraps a `::LoroSubscription*` (from any `*_subscribe`) as a [`Subscription`].
struct SubscriptionFactory {
    static std::shared_ptr<Subscription> make(::LoroSubscription *raw) {
        return std::shared_ptr<Subscription>(new Subscription(raw));
    }
};
}  // namespace detail

// ----------------------------------------------- DiffEvent envelope construction

namespace detail {

inline EventTriggerKind trigger_from_c(LoroEventTriggerKind k) {
    switch (k) {
        case LORO_EVENT_TRIGGER_LOCAL: return EventTriggerKind::kLocal;
        case LORO_EVENT_TRIGGER_IMPORT: return EventTriggerKind::kImport;
        case LORO_EVENT_TRIGGER_CHECKOUT: return EventTriggerKind::kCheckout;
    }
    return EventTriggerKind::kLocal;
}

inline DiffKind diff_kind_from_c(LoroDiffKind k) {
    switch (k) {
        case LORO_DIFF_LIST: return DiffKind::kList;
        case LORO_DIFF_TEXT: return DiffKind::kText;
        case LORO_DIFF_MAP: return DiffKind::kMap;
        case LORO_DIFF_TREE: return DiffKind::kTree;
        case LORO_DIFF_COUNTER: return DiffKind::kCounter;
        case LORO_DIFF_UNKNOWN: return DiffKind::kUnknown;
    }
    return DiffKind::kUnknown;
}

/// Parses a path JSON array (`[{"cid":..,"index":{"key"|"seq"|"node":..}}]`, as produced by
/// `loro_container_diff_path_json`) into a vector of [`PathItem`].
inline std::vector<PathItem> path_items_from_json(const std::string &json) {
    std::vector<PathItem> out;
    if (json.empty()) return out;
    JsonValue root = parse_json(json);
    if (root.kind != JsonValue::Kind::Array) return out;
    for (const auto &step : root.arr) {
        if (step.kind != JsonValue::Kind::Object) continue;
        const JsonValue *cidv = step.find("cid");
        if (!cidv || cidv->kind != JsonValue::Kind::String) continue;
        ContainerId container = cid_string_to_container_id(cidv->s);
        const JsonValue *idxv = step.find("index");
        const JsonValue *k = idxv ? idxv->find("key") : nullptr;
        const JsonValue *s = idxv ? idxv->find("seq") : nullptr;
        const JsonValue *n = idxv ? idxv->find("node") : nullptr;
        if (k && k->kind == JsonValue::Kind::String) {
            out.push_back(PathItem{std::move(container), Index(Index::kKey{k->s})});
        } else if (s) {
            uint32_t idx = static_cast<uint32_t>(
                s->kind == JsonValue::Kind::Int ? s->i : static_cast<int64_t>(s->d));
            out.push_back(PathItem{std::move(container), Index(Index::kSeq{idx})});
        } else if (n && n->kind == JsonValue::Kind::Object) {
            const JsonValue *peer = n->find("peer");
            const JsonValue *counter = n->find("counter");
            TreeId tid{peer ? static_cast<uint64_t>(peer->i) : 0,
                       counter ? static_cast<int32_t>(counter->i) : 0};
            out.push_back(PathItem{std::move(container), Index(Index::kNode{tid})});
        }
    }
    return out;
}

/// Builds an owned [`DiffEvent`] from a callback-scoped `const LoroDiffEvent*`. The result owns
/// all of its data (value types only — no live handles), so a subscriber may stash it.
inline DiffEvent diff_event_from_c(const ::LoroDiffEvent *ev) {
    DiffEvent e;
    e.triggered_by = trigger_from_c(loro_diff_event_triggered_by(ev));
    Bytes origin;
    if (loro_diff_event_origin(ev, origin.out()) == LORO_OK) e.origin = origin.to_string();
    Bytes target;
    // NOT_FOUND (e.g. a root subscription) leaves current_target as std::nullopt — not an error.
    if (loro_diff_event_current_target(ev, target.out()) == LORO_OK) {
        e.current_target = cid_string_to_container_id(target.to_string());
    }
    uintptr_t count = loro_diff_event_count(ev);
    e.events.reserve(count);
    for (uintptr_t i = 0; i < count; ++i) {
        const ::LoroContainerDiff *cd = loro_diff_event_get(ev, i);
        if (!cd) continue;
        Bytes ctgt;
        if (loro_container_diff_target(cd, ctgt.out()) != LORO_OK) continue;
        ContainerId ctarget = cid_string_to_container_id(ctgt.to_string());
        bool unknown = loro_container_diff_is_unknown(cd);
        DiffKind kind = diff_kind_from_c(loro_container_diff_kind(cd));
        ContainerDiff d{std::move(ctarget), {}, unknown, kind};
        Bytes pj;
        if (loro_container_diff_path_json(cd, pj.out()) == LORO_OK) {
            d.path = path_items_from_json(pj.to_string());
        }
        e.events.push_back(std::move(d));
    }
    return e;
}

// Heap-held owners of a C++ subscriber, passed as `user_data` to the C callback structs.
struct SubscriberHolder {
    std::shared_ptr<Subscriber> sub;
};
struct LocalUpdateHolder {
    std::shared_ptr<LocalUpdateCallback> cb;
};
struct PreCommitHolder {
    std::shared_ptr<PreCommitCallback> cb;
};
struct FirstCommitHolder {
    std::shared_ptr<FirstCommitFromPeerCallback> cb;
};

}  // namespace detail

// ------------------------------------------------------- subscription trampolines
//
// One invoke/free pair per callback kind, mirroring the EphemeralStore pattern. Invoke builds
// the owned C++ payload from the callback-scoped C view and dispatches; it never lets a C++
// exception unwind across the C ABI. The bool-returning trampolines return `true` (stay
// subscribed) even after a caught exception — `false` would auto-unsubscribe.

extern "C" inline void loro_conf_subscriber_invoke(const ::LoroDiffEvent *ev, void *user_data) {
    auto *holder = static_cast<detail::SubscriberHolder *>(user_data);
    if (!holder || !holder->sub) return;
    try {
        holder->sub->on_diff(detail::diff_event_from_c(ev));
    } catch (...) {
    }
}
extern "C" inline void loro_conf_subscriber_free(void *user_data) {
    delete static_cast<detail::SubscriberHolder *>(user_data);
}

extern "C" inline bool loro_conf_local_update_invoke(const uint8_t *data, uintptr_t len,
                                                     void *user_data) {
    auto *holder = static_cast<detail::LocalUpdateHolder *>(user_data);
    if (!holder || !holder->cb) return true;
    try {
        std::vector<uint8_t> bytes(data, data + len);
        holder->cb->on_local_update(bytes);
    } catch (...) {
    }
    return true;
}
extern "C" inline void loro_conf_local_update_free(void *user_data) {
    delete static_cast<detail::LocalUpdateHolder *>(user_data);
}

extern "C" inline bool loro_conf_pre_commit_invoke(const ::LoroPreCommitPayload *payload,
                                                   void *user_data) {
    auto *holder = static_cast<detail::PreCommitHolder *>(user_data);
    if (!holder || !holder->cb) return true;
    try {
        PreCommitCallbackPayload pl{
            detail::change_meta_from_c(loro_pre_commit_payload_change_meta(payload)),
            std::string(),
            detail::PreCommitPayloadBuilder::modifier(payload),
        };
        detail::Bytes origin;
        if (loro_pre_commit_payload_origin(payload, origin.out()) == LORO_OK) {
            pl.origin = origin.to_string();
        }
        holder->cb->on_pre_commit(pl);
    } catch (...) {
    }
    return true;
}
extern "C" inline void loro_conf_pre_commit_free(void *user_data) {
    delete static_cast<detail::PreCommitHolder *>(user_data);
}

extern "C" inline bool loro_conf_first_commit_invoke(uint64_t peer, void *user_data) {
    auto *holder = static_cast<detail::FirstCommitHolder *>(user_data);
    if (!holder || !holder->cb) return true;
    try {
        holder->cb->on_first_commit_from_peer(FirstCommitFromPeerPayload{peer});
    } catch (...) {
    }
    return true;
}
extern "C" inline void loro_conf_first_commit_free(void *user_data) {
    delete static_cast<detail::FirstCommitHolder *>(user_data);
}

// ----------------------------------------------- LoroDoc subscription definitions

inline std::shared_ptr<Subscription> LoroDoc::subscribe(
    const ContainerId &cid, const std::shared_ptr<Subscriber> &subscriber) {
    std::string cs = detail::container_id_to_cid_string(cid);
    auto *holder = new detail::SubscriberHolder{subscriber};
    ::LoroSubscriber cb;
    cb.invoke = &loro_conf_subscriber_invoke;
    cb.user_data = holder;
    cb.free_user_data = &loro_conf_subscriber_free;
    // On a null return loro-c has already run free_user_data (freeing `holder`); don't free again.
    ::LoroSubscription *s = loro_doc_subscribe(raw_, cs.data(), cs.size(), cb);
    if (!s) throw LoroError("loro_doc_subscribe returned null");
    return detail::SubscriptionFactory::make(s);
}

inline std::shared_ptr<Subscription> LoroDoc::subscribe_root(
    const std::shared_ptr<Subscriber> &subscriber) {
    auto *holder = new detail::SubscriberHolder{subscriber};
    ::LoroSubscriber cb;
    cb.invoke = &loro_conf_subscriber_invoke;
    cb.user_data = holder;
    cb.free_user_data = &loro_conf_subscriber_free;
    ::LoroSubscription *s = loro_doc_subscribe_root(raw_, cb);
    if (!s) throw LoroError("loro_doc_subscribe_root returned null");
    return detail::SubscriptionFactory::make(s);
}

inline std::shared_ptr<Subscription> LoroDoc::subscribe_local_update(
    const std::shared_ptr<LocalUpdateCallback> &callback) {
    auto *holder = new detail::LocalUpdateHolder{callback};
    ::LoroLocalUpdateCallback cb;
    cb.invoke = &loro_conf_local_update_invoke;
    cb.user_data = holder;
    cb.free_user_data = &loro_conf_local_update_free;
    ::LoroSubscription *s = loro_doc_subscribe_local_update(raw_, cb);
    if (!s) throw LoroError("loro_doc_subscribe_local_update returned null");
    return detail::SubscriptionFactory::make(s);
}

inline std::shared_ptr<Subscription> LoroDoc::subscribe_first_commit_from_peer(
    const std::shared_ptr<FirstCommitFromPeerCallback> &callback) {
    auto *holder = new detail::FirstCommitHolder{callback};
    ::LoroFirstCommitFromPeerCallback cb;
    cb.invoke = &loro_conf_first_commit_invoke;
    cb.user_data = holder;
    cb.free_user_data = &loro_conf_first_commit_free;
    ::LoroSubscription *s = loro_doc_subscribe_first_commit_from_peer(raw_, cb);
    if (!s) throw LoroError("loro_doc_subscribe_first_commit_from_peer returned null");
    return detail::SubscriptionFactory::make(s);
}

inline std::shared_ptr<Subscription> LoroDoc::subscribe_pre_commit(
    const std::shared_ptr<PreCommitCallback> &callback) {
    auto *holder = new detail::PreCommitHolder{callback};
    ::LoroPreCommitCallback cb;
    cb.invoke = &loro_conf_pre_commit_invoke;
    cb.user_data = holder;
    cb.free_user_data = &loro_conf_pre_commit_free;
    ::LoroSubscription *s = loro_doc_subscribe_pre_commit(raw_, cb);
    if (!s) throw LoroError("loro_doc_subscribe_pre_commit returned null");
    return detail::SubscriptionFactory::make(s);
}

// ----------------------------------------- per-container subscription definitions

inline std::shared_ptr<Subscription> LoroText::subscribe(
    const std::shared_ptr<Subscriber> &subscriber) {
    auto *holder = new detail::SubscriberHolder{subscriber};
    ::LoroSubscriber cb;
    cb.invoke = &loro_conf_subscriber_invoke;
    cb.user_data = holder;
    cb.free_user_data = &loro_conf_subscriber_free;
    ::LoroSubscription *s = loro_text_subscribe(raw_, cb);
    if (!s) throw LoroError("loro_text_subscribe returned null");
    return detail::SubscriptionFactory::make(s);
}

inline std::shared_ptr<Subscription> LoroMap::subscribe(
    const std::shared_ptr<Subscriber> &subscriber) {
    auto *holder = new detail::SubscriberHolder{subscriber};
    ::LoroSubscriber cb;
    cb.invoke = &loro_conf_subscriber_invoke;
    cb.user_data = holder;
    cb.free_user_data = &loro_conf_subscriber_free;
    ::LoroSubscription *s = loro_map_subscribe(map(), cb);
    if (!s) throw LoroError("loro_map_subscribe returned null");
    return detail::SubscriptionFactory::make(s);
}

inline std::shared_ptr<Subscription> LoroList::subscribe(
    const std::shared_ptr<Subscriber> &subscriber) {
    auto *holder = new detail::SubscriberHolder{subscriber};
    ::LoroSubscriber cb;
    cb.invoke = &loro_conf_subscriber_invoke;
    cb.user_data = holder;
    cb.free_user_data = &loro_conf_subscriber_free;
    ::LoroSubscription *s = loro_list_subscribe(list(), cb);
    if (!s) throw LoroError("loro_list_subscribe returned null");
    return detail::SubscriptionFactory::make(s);
}

inline std::shared_ptr<Subscription> LoroMovableList::subscribe(
    const std::shared_ptr<Subscriber> &subscriber) {
    auto *holder = new detail::SubscriberHolder{subscriber};
    ::LoroSubscriber cb;
    cb.invoke = &loro_conf_subscriber_invoke;
    cb.user_data = holder;
    cb.free_user_data = &loro_conf_subscriber_free;
    ::LoroSubscription *s = loro_movable_list_subscribe(list(), cb);
    if (!s) throw LoroError("loro_movable_list_subscribe returned null");
    return detail::SubscriptionFactory::make(s);
}

inline std::shared_ptr<Subscription> LoroTree::subscribe(
    const std::shared_ptr<Subscriber> &subscriber) {
    auto *holder = new detail::SubscriberHolder{subscriber};
    ::LoroSubscriber cb;
    cb.invoke = &loro_conf_subscriber_invoke;
    cb.user_data = holder;
    cb.free_user_data = &loro_conf_subscriber_free;
    ::LoroSubscription *s = loro_tree_subscribe(tree(), cb);
    if (!s) throw LoroError("loro_tree_subscribe returned null");
    return detail::SubscriptionFactory::make(s);
}

inline std::shared_ptr<Subscription> LoroCounter::subscribe(
    const std::shared_ptr<Subscriber> &subscriber) {
    auto *holder = new detail::SubscriberHolder{subscriber};
    ::LoroSubscriber cb;
    cb.invoke = &loro_conf_subscriber_invoke;
    cb.user_data = holder;
    cb.free_user_data = &loro_conf_subscriber_free;
    ::LoroSubscription *s = loro_counter_subscribe(counter(), cb);
    if (!s) throw LoroError("loro_counter_subscribe returned null");
    return detail::SubscriptionFactory::make(s);
}

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

    /// Removes entries whose last update is older than the store's timeout (emitting a
    /// `Timeout` event to subscribers if any are removed).
    void remove_outdated() {
        detail::check(loro_ephemeral_store_remove_outdated(raw_));
    }

    /// Returns every live entry as a `key -> LoroValue` map. The C ABI emits a JSON object
    /// `{"<key>": <value>, ...}` (values are serialized `LoroValue`s), parsed here via the same
    /// idiom as `LoroText::to_delta`.
    std::unordered_map<std::string, LoroValue> get_all_states() {
        detail::Bytes b;
        detail::check(loro_ephemeral_store_get_all_states(raw_, b.out()));
        std::unordered_map<std::string, LoroValue> out;
        std::string json = b.to_string();
        if (json.empty()) return out;
        detail::JsonValue root = detail::parse_json(json);
        if (root.kind != detail::JsonValue::Kind::Object) return out;
        for (const auto &kv : root.obj) {
            out.emplace(kv.first, detail::json_to_value(kv.second));
        }
        return out;
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

    /// Subscribes to local updates: the callback receives the encoded bytes of each local change
    /// (`set` / `delete` / `apply`) to broadcast to peers. Mirrors `LoroDoc::subscribe_local_update`.
    std::shared_ptr<Subscription> subscribe_local_updates(
        const std::shared_ptr<LocalUpdateCallback> &callback) {
        auto *holder = new detail::LocalUpdateHolder{callback};
        ::LoroLocalUpdateCallback cb;
        cb.invoke = &loro_conf_local_update_invoke;
        cb.user_data = holder;
        cb.free_user_data = &loro_conf_local_update_free;
        // On error loro-c drops the callback (running free_user_data, freeing `holder`);
        // do NOT free it again here.
        ::LoroSubscription *s = loro_ephemeral_store_subscribe_local_updates(raw_, cb);
        if (!s) throw LoroError("loro_ephemeral_store_subscribe_local_updates returned null");
        return std::shared_ptr<Subscription>(new Subscription(s));
    }

    ~EphemeralStore() { loro_ephemeral_store_free(raw_); }
    EphemeralStore(const EphemeralStore &) = delete;
    EphemeralStore &operator=(const EphemeralStore &) = delete;

private:
    explicit EphemeralStore(::LoroEphemeralStore *raw) : raw_(raw) {}
    ::LoroEphemeralStore *raw_;
};

// --------------------------------------------------------- awareness (Phase 6, legacy)
//
// Per-peer presence (loro-cpp `Awareness`), deprecated upstream in favour of `EphemeralStore`
// but reproduced for faithful drop-in parity. Values are carried typed (no JSON) via the
// `loro_awareness_*_value` C ABI; peer-id lists cross as raw little-endian `u64` buffers.

namespace detail {
/// Decodes a little-endian `u64` buffer (as produced by `loro_awareness_peer_ids` /
/// `loro_awareness_apply_with_changes`) into a vector of peer ids.
inline std::vector<uint64_t> u64s_from_bytes(const Bytes &b) {
    std::vector<uint8_t> raw = b.to_vector();
    std::vector<uint64_t> out;
    out.reserve(raw.size() / 8);
    for (size_t i = 0; i + 8 <= raw.size(); i += 8) {
        uint64_t v = 0;
        for (size_t j = 0; j < 8; ++j) v |= static_cast<uint64_t>(raw[i + j]) << (8 * j);
        out.push_back(v);
    }
    return out;
}
}  // namespace detail

/// A peer's awareness entry (loro-cpp `PeerInfo`): typed `state` plus its logical `counter` and
/// local `timestamp`.
struct PeerInfo {
    LoroValue state;
    int32_t counter;
    int64_t timestamp;
};

/// The peers whose state changed in an [`Awareness::apply`] (loro-cpp `AwarenessPeerUpdate`).
struct AwarenessPeerUpdate {
    std::vector<uint64_t> updated;
    std::vector<uint64_t> added;
};

/// Out-of-document per-peer presence with timeout-based expiry (loro-cpp `Awareness`).
struct Awareness {
    static std::shared_ptr<Awareness> init(uint64_t peer, int64_t timeout) {
        ::LoroAwareness *a = loro_awareness_new(peer, timeout);
        if (!a) throw LoroError("loro_awareness_new returned null");
        return std::shared_ptr<Awareness>(new Awareness(a));
    }

    uint64_t peer() { return loro_awareness_peer(raw_); }

    void set_local_state(const std::shared_ptr<LoroValueLike> &value) {
        LoroValue v = value->as_loro_value();
        detail::CValue cv(detail::to_c_value(v));
        detail::check(loro_awareness_set_local_state_value(raw_, cv.get()));
    }

    std::optional<LoroValue> get_local_state() {
        ::LoroValue *cv = loro_awareness_get_local_state_value(raw_);
        if (!cv) return std::nullopt;
        detail::CValue g(cv);
        return detail::from_c_value(cv);
    }

    std::vector<uint8_t> encode(const std::vector<uint64_t> &peers) {
        detail::Bytes b;
        detail::check(loro_awareness_encode(raw_, peers.data(), peers.size(), b.out()));
        return b.to_vector();
    }

    std::vector<uint8_t> encode_all() {
        detail::Bytes b;
        detail::check(loro_awareness_encode_all(raw_, b.out()));
        return b.to_vector();
    }

    AwarenessPeerUpdate apply(const std::vector<uint8_t> &encoded_peers_info) {
        detail::Bytes updated;
        detail::Bytes added;
        detail::check(loro_awareness_apply_with_changes(raw_, encoded_peers_info.data(),
                                                        encoded_peers_info.size(), updated.out(),
                                                        added.out()));
        return AwarenessPeerUpdate{detail::u64s_from_bytes(updated),
                                   detail::u64s_from_bytes(added)};
    }

    std::unordered_map<uint64_t, PeerInfo> get_all_states() {
        detail::Bytes ids_bytes;
        detail::check(loro_awareness_peer_ids(raw_, ids_bytes.out()));
        std::unordered_map<uint64_t, PeerInfo> states;
        for (uint64_t peer : detail::u64s_from_bytes(ids_bytes)) {
            ::LoroValue *cv = loro_awareness_get_peer_state_value(raw_, peer);
            if (!cv) continue;  // raced out between peer_ids and the read
            detail::CValue g(cv);
            LoroValue state = detail::from_c_value(cv);
            int32_t counter = 0;
            int64_t timestamp = 0;
            if (!loro_awareness_get_peer_info(raw_, peer, &counter, &timestamp)) continue;
            states.emplace(peer, PeerInfo{std::move(state), counter, timestamp});
        }
        return states;
    }

    std::vector<uint64_t> remove_outdated() {
        detail::Bytes b;
        detail::check(loro_awareness_remove_outdated_ids(raw_, b.out()));
        return detail::u64s_from_bytes(b);
    }

    ~Awareness() { loro_awareness_free(raw_); }
    Awareness(const Awareness &) = delete;
    Awareness &operator=(const Awareness &) = delete;

private:
    explicit Awareness(::LoroAwareness *raw) : raw_(raw) {}
    ::LoroAwareness *raw_;
};

// ------------------------------------------------------------- undo manager (Phase 4)

namespace detail {
// Heap-held owners of a C++ undo listener, passed as `user_data` to the C callback structs.
// On install error (or when replaced / the manager is freed), loro-c runs `free_user_data`,
// which deletes the holder — so the C side owns it after a successful `set_on_*`.
struct OnPushHolder {
    std::shared_ptr<OnPush> cb;
};
struct OnPopHolder {
    std::shared_ptr<OnPop> cb;
};
}  // namespace detail

// Trampolines mirror the subscription ones: build the owned C++ payload from the callback-scoped
// C view and dispatch, never letting a C++ exception unwind across the C ABI. on_push writes the
// returned meta's value (typed, no JSON) and its cursors back into the writable `LoroUndoMeta`;
// on_pop reads both back out. loro transforms the stored cursors by intervening ops between push
// and pop, so a popped cursor reports its replayed position.
extern "C" inline void loro_conf_on_push_invoke(::LoroUndoOrRedo kind, ::LoroCounterSpan span,
                                                const ::LoroDiffEvent *event,
                                                ::LoroUndoMeta *meta, void *user_data) {
    auto *holder = static_cast<detail::OnPushHolder *>(user_data);
    if (!holder || !holder->cb) return;
    try {
        std::optional<DiffEvent> ev;
        if (event) ev = detail::diff_event_from_c(event);
        UndoItemMeta r = holder->cb->on_push(detail::from_c_undo_or_redo(kind),
                                             CounterSpan{span.start, span.end}, std::move(ev));
        detail::CValue cv(detail::to_c_value(r.value));
        loro_undo_meta_set_value(meta, cv.get());
        for (const auto &cwp : r.cursors) {
            if (cwp.cursor) loro_undo_meta_add_cursor(meta, detail::Factory::cursor_raw(cwp.cursor));
        }
    } catch (...) {
    }
}
extern "C" inline void loro_conf_on_push_free(void *user_data) {
    delete static_cast<detail::OnPushHolder *>(user_data);
}

extern "C" inline void loro_conf_on_pop_invoke(::LoroUndoOrRedo kind, ::LoroCounterSpan span,
                                               const ::LoroUndoMeta *meta, void *user_data) {
    auto *holder = static_cast<detail::OnPopHolder *>(user_data);
    if (!holder || !holder->cb) return;
    try {
        LoroValue value{LoroValue::kNull{}};
        if (::LoroValue *v = loro_undo_meta_get_value(meta)) {
            detail::CValue g(v);
            value = detail::from_c_value(v);
        }
        std::vector<CursorWithPos> cursors;
        for (size_t i = 0, n = loro_undo_meta_cursors_len(meta); i < n; ++i) {
            uintptr_t pos = 0;
            ::LoroSide side = LORO_SIDE_MIDDLE;
            if (::LoroCursor *c = loro_undo_meta_get_cursor(meta, i, &pos, &side)) {
                cursors.push_back(CursorWithPos{
                    detail::Factory::wrap_cursor(c),
                    AbsolutePosition{static_cast<uint32_t>(pos), detail::from_c_side(side)}});
            }
        }
        holder->cb->on_pop(detail::from_c_undo_or_redo(kind), CounterSpan{span.start, span.end},
                           UndoItemMeta{std::move(value), std::move(cursors)});
    } catch (...) {
    }
}
extern "C" inline void loro_conf_on_pop_free(void *user_data) {
    delete static_cast<detail::OnPopHolder *>(user_data);
}

/// Local undo/redo bound to a document's peer (loro-cpp `UndoManager`). `shared_ptr`-held,
/// `init(doc)`-constructed. `on_push`/`on_pop` listeners observe stack changes; the attached
/// [`UndoItemMeta`] carries a typed value (cursors are empty in this phase).
struct UndoManager {
    static std::shared_ptr<UndoManager> init(const std::shared_ptr<LoroDoc> &doc) {
        ::LoroUndoManager *um = loro_undo_manager_new(doc->raw_);
        if (!um) throw LoroError("loro_undo_manager_new returned null");
        return std::shared_ptr<UndoManager>(new UndoManager(um));
    }

    void add_exclude_origin_prefix(const std::string &prefix) {
        detail::check(
            loro_undo_manager_add_exclude_origin_prefix(raw_, prefix.data(), prefix.size()));
    }
    void set_max_undo_steps(uint32_t size) {
        detail::check(loro_undo_manager_set_max_undo_steps(raw_, static_cast<uintptr_t>(size)));
    }
    void set_merge_interval(int64_t interval) {
        detail::check(loro_undo_manager_set_merge_interval(raw_, interval));
    }

    void set_on_push(std::shared_ptr<OnPush> on_push) {
        auto *holder = new detail::OnPushHolder{std::move(on_push)};
        ::LoroUndoOnPush cb;
        cb.invoke = &loro_conf_on_push_invoke;
        cb.user_data = holder;
        cb.free_user_data = &loro_conf_on_push_free;
        // On error loro-c drops the callback (running free_user_data, freeing `holder`); the
        // check below just surfaces it — don't free `holder` again.
        detail::check(loro_undo_manager_set_on_push(raw_, cb));
    }
    void set_on_pop(std::shared_ptr<OnPop> on_pop) {
        auto *holder = new detail::OnPopHolder{std::move(on_pop)};
        ::LoroUndoOnPop cb;
        cb.invoke = &loro_conf_on_pop_invoke;
        cb.user_data = holder;
        cb.free_user_data = &loro_conf_on_pop_free;
        detail::check(loro_undo_manager_set_on_pop(raw_, cb));
    }

    bool can_undo() { return loro_undo_manager_can_undo(raw_); }
    bool can_redo() { return loro_undo_manager_can_redo(raw_); }
    uint32_t undo_count() { return static_cast<uint32_t>(loro_undo_manager_undo_count(raw_)); }
    uint32_t redo_count() { return static_cast<uint32_t>(loro_undo_manager_redo_count(raw_)); }
    uint64_t peer() { return loro_undo_manager_peer(raw_); }

    bool undo() {
        bool applied = false;
        detail::check(loro_undo_manager_undo(raw_, &applied));
        return applied;
    }
    bool redo() {
        bool applied = false;
        detail::check(loro_undo_manager_redo(raw_, &applied));
        return applied;
    }
    void record_new_checkpoint() {
        detail::check(loro_undo_manager_record_new_checkpoint(raw_));
    }
    void group_start() { detail::check(loro_undo_manager_group_start(raw_)); }
    void group_end() { detail::check(loro_undo_manager_group_end(raw_)); }

    std::optional<LoroValue> top_undo_value() {
        ::LoroValue *cv = loro_undo_manager_top_undo_value(raw_);
        if (!cv) return std::nullopt;
        detail::CValue g(cv);
        return detail::from_c_value(cv);
    }
    std::optional<LoroValue> top_redo_value() {
        ::LoroValue *cv = loro_undo_manager_top_redo_value(raw_);
        if (!cv) return std::nullopt;
        detail::CValue g(cv);
        return detail::from_c_value(cv);
    }
    std::optional<UndoItemMeta> top_undo_meta() {
        auto v = top_undo_value();
        if (!v) return std::nullopt;
        return UndoItemMeta{std::move(*v), {}};
    }
    std::optional<UndoItemMeta> top_redo_meta() {
        auto v = top_redo_value();
        if (!v) return std::nullopt;
        return UndoItemMeta{std::move(*v), {}};
    }

    ~UndoManager() { loro_undo_manager_free(raw_); }
    UndoManager(const UndoManager &) = delete;
    UndoManager &operator=(const UndoManager &) = delete;

private:
    explicit UndoManager(::LoroUndoManager *raw) : raw_(raw) {}
    ::LoroUndoManager *raw_;
};

}  // namespace loro

#endif  // LORO_LORO_HPP
