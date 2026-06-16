/*
 * loro.hpp — hand-written C++20 RAII wrapper over the generated C ABI in <loro/loro.h>.
 *
 * Design:
 *   - Every Loro handle (LoroDoc*, LoroText*, ...) is owned by a move-only RAII class
 *     that frees it in its destructor.
 *   - Fallible C functions return a LoroStatus; this layer throws loro::Error on any
 *     non-OK status, attaching the thread-local last-error message.
 *   - Binary/string outputs (LoroBytes) are converted to std::string / std::vector and
 *     freed automatically.
 *
 * Lifetime (important): a container handle such as loro::Text is a *strong co-owner* of
 * the underlying document state. It stays valid even after its parent loro::Doc is
 * destroyed, and the document's state is released only once the Doc and every container
 * obtained from it have been destroyed. In other words, holding (or leaking) a Text keeps
 * the whole document alive. Handles may be destroyed in any order — there is no
 * use-after-free hazard from destruction order. (Verified against loro-internal 1.13.1.)
 *
 * Threading: Loro is Send + Sync. These wrappers add no locking. A subscription callback
 * (added in M3) may fire from any thread that mutates the document, so such callbacks
 * must be reentrant.
 */
#ifndef LORO_LORO_HPP
#define LORO_LORO_HPP

#include <loro/loro.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace loro {

/// Exception thrown when a C ABI call returns a non-OK status. Carries both the status
/// code and the detail message recorded by the library.
class Error : public std::runtime_error {
public:
    Error(LoroStatus status, std::string message)
        : std::runtime_error(message.empty() ? default_message(status) : message),
          status_(status) {}

    /// The status code that triggered the exception.
    LoroStatus status() const noexcept { return status_; }

private:
    LoroStatus status_;

    static std::string default_message(LoroStatus s) {
        return "loro error (status " + std::to_string(static_cast<int>(s)) + ")";
    }
};

namespace detail {

/// Returns the current thread's last-error message, or an empty string.
inline std::string last_error_message() {
    const char* m = loro_last_error_message();
    return m ? std::string(m) : std::string();
}

/// Throws loro::Error if `s` is not LORO_OK.
inline void check(LoroStatus s) {
    if (s != LORO_OK) {
        throw Error(s, last_error_message());
    }
}

/// Returns true if `s` is LORO_OK, false if LORO_ERR_NOT_FOUND, and throws otherwise.
/// Used by lookups that map "absent" to an empty std::optional.
inline bool check_found(LoroStatus s) {
    if (s == LORO_OK) return true;
    if (s == LORO_ERR_NOT_FOUND) return false;
    throw Error(s, last_error_message());
}

/// RAII holder for a LoroBytes buffer returned across the FFI boundary.
class Bytes {
public:
    Bytes() : raw_{nullptr, 0, 0} {}
    ~Bytes() { loro_bytes_free(raw_); }

    Bytes(const Bytes&) = delete;
    Bytes& operator=(const Bytes&) = delete;
    Bytes(Bytes&&) = delete;
    Bytes& operator=(Bytes&&) = delete;

    /// Address of the underlying struct, to pass as the `out` parameter.
    LoroBytes* out() { return &raw_; }

    std::string to_string() const {
        if (raw_.data == nullptr || raw_.len == 0) return {};
        return std::string(reinterpret_cast<const char*>(raw_.data), raw_.len);
    }

    std::vector<std::uint8_t> to_vector() const {
        if (raw_.data == nullptr || raw_.len == 0) return {};
        return std::vector<std::uint8_t>(raw_.data, raw_.data + raw_.len);
    }

private:
    LoroBytes raw_;
};

}  // namespace detail

// Forward declarations: the typed container wrappers and the type-erased Container are
// mutually referential (e.g. Map::insert_container takes/returns a Container, while
// Container::as_map returns a Map). Methods that couple a typed wrapper to Container are
// declared in-class and defined out-of-line once every class is complete (see below).
class Text;
class Map;
class List;
class MovableList;
class Counter;
class Tree;
class Container;
class Cursor;
// G6.4: the per-container doc()/subscribe() accessors couple every container wrapper to
// Doc / Subscription / DiffEvent, which are defined further below. Forward-declare them so
// the in-class declarations compile; the definitions are out-of-line once Doc is complete.
class Doc;
class DiffEvent;
class Subscription;

/// The kind of a type-erased loro::Container. Alias of the C ABI enum.
using ContainerType = ::LoroContainerType;

/// Identifies a tree node (`peer`, `counter`). Alias of the C ABI struct.
using TreeId = ::LoroTreeID;

/// How a tree node's parent is classified (root / deleted / node / unexist). Alias of the C
/// ABI enum.
using ParentKind = ::LoroTreeParentKind;

/// Text position coordinate system (bytes / Unicode / UTF-16). Alias of the C ABI enum.
using PosType = ::LoroPosType;

/// How a text mark expands at its boundaries. Alias of the C ABI enum.
using ExpandType = ::LoroExpandType;

/// Which side of an element a cursor is anchored to. Alias of the C ABI enum.
using Side = ::LoroSide;

/// A cursor's resolved absolute position (`abs_pos`) and side. Alias of the C ABI struct.
using PosQueryResult = ::LoroPosQueryResult;

/// RAII wrapper around a `LoroText*` container handle. Move-only.
class Text {
public:
    /// Takes ownership of a raw handle (e.g. returned by loro_doc_get_text).
    explicit Text(LoroText* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    /// The underlying C handle (non-owning).
    LoroText* raw() const noexcept { return handle_.get(); }

    /// This container's id string (e.g. "cid:root-name:Text"); pass to Doc::subscribe.
    std::string id() const {
        detail::Bytes b;
        detail::check(loro_text_id(handle_.get(), b.out()));
        return b.to_string();
    }

    /// Inserts `s` (UTF-8) at Unicode codepoint index `pos`.
    void insert(std::size_t pos, std::string_view s) {
        detail::check(loro_text_insert(handle_.get(), pos, s.data(), s.size()));
    }

    /// Inserts `s` (UTF-8) at UTF-8 byte index `pos`.
    void insert_utf8(std::size_t pos, std::string_view s) {
        detail::check(loro_text_insert_utf8(handle_.get(), pos, s.data(), s.size()));
    }

    /// Deletes `len` Unicode codepoints starting at codepoint index `pos`.
    void remove(std::size_t pos, std::size_t len) {
        detail::check(loro_text_delete(handle_.get(), pos, len));
    }

    /// Deletes `len` UTF-8 bytes starting at byte index `pos`.
    void remove_utf8(std::size_t pos, std::size_t len) {
        detail::check(loro_text_delete_utf8(handle_.get(), pos, len));
    }

    /// Length in Unicode codepoints.
    std::size_t len_unicode() const { return loro_text_len_unicode(handle_.get()); }

    /// Length in UTF-8 bytes.
    std::size_t len_utf8() const { return loro_text_len_utf8(handle_.get()); }

    /// Whether the text is empty.
    bool empty() const { return loro_text_is_empty(handle_.get()); }

    /// The current text content as a UTF-8 std::string.
    std::string to_string() const {
        detail::Bytes b;
        detail::check(loro_text_to_string(handle_.get(), b.out()));
        return b.to_string();
    }

    // --- G1: rich text ---

    /// Marks the range `[from, to)` (Unicode codepoint indices) with `key` = `value`,
    /// where `value` is a JSON-encoded value (e.g. "true" for bold, "\"https://…\"" for a
    /// link).
    void mark(std::size_t from, std::size_t to, std::string_view key, std::string_view value) {
        detail::check(loro_text_mark(handle_.get(), from, to, key.data(), key.size(),
                                     value.data(), value.size()));
    }

    /// Like `mark`, but `from`/`to` are UTF-8 byte indices.
    void mark_utf8(std::size_t from, std::size_t to, std::string_view key,
                   std::string_view value) {
        detail::check(loro_text_mark_utf8(handle_.get(), from, to, key.data(), key.size(),
                                          value.data(), value.size()));
    }

    /// Like `mark`, but `from`/`to` are UTF-16 code unit indices.
    void mark_utf16(std::size_t from, std::size_t to, std::string_view key,
                    std::string_view value) {
        detail::check(loro_text_mark_utf16(handle_.get(), from, to, key.data(), key.size(),
                                           value.data(), value.size()));
    }

    /// Removes the mark `key` over the range `[from, to)` (Unicode codepoint indices).
    void unmark(std::size_t from, std::size_t to, std::string_view key) {
        detail::check(loro_text_unmark(handle_.get(), from, to, key.data(), key.size()));
    }

    /// Like `unmark`, but `from`/`to` are UTF-16 code unit indices.
    void unmark_utf16(std::size_t from, std::size_t to, std::string_view key) {
        detail::check(loro_text_unmark_utf16(handle_.get(), from, to, key.data(), key.size()));
    }

    /// The rich-text value as a JSON delta-with-attributes array.
    std::string get_richtext_value() const {
        detail::Bytes b;
        detail::check(loro_text_get_richtext_value(handle_.get(), b.out()));
        return b.to_string();
    }

    /// The text in Quill Delta format as a JSON array string.
    std::string to_delta() const {
        detail::Bytes b;
        detail::check(loro_text_to_delta(handle_.get(), b.out()));
        return b.to_string();
    }

    /// Applies a Quill Delta (a JSON array string) to the text.
    void apply_delta(std::string_view delta) {
        detail::check(loro_text_apply_delta(handle_.get(), delta.data(), delta.size()));
    }

    /// Replaces the whole content with `s` by diffing. `timeout_ms < 0` means no timeout.
    void update(std::string_view s, double timeout_ms = -1.0, bool use_refined_diff = true) {
        detail::check(
            loro_text_update(handle_.get(), s.data(), s.size(), timeout_ms, use_refined_diff));
    }

    /// Like `update`, but uses a faster, line-based diff.
    void update_by_line(std::string_view s, double timeout_ms = -1.0,
                        bool use_refined_diff = true) {
        detail::check(loro_text_update_by_line(handle_.get(), s.data(), s.size(), timeout_ms,
                                               use_refined_diff));
    }

    /// Deletes `len` codepoints at `pos`, inserts `s` there; returns the removed text.
    std::string splice(std::size_t pos, std::size_t len, std::string_view s) {
        detail::Bytes b;
        detail::check(loro_text_splice(handle_.get(), pos, len, s.data(), s.size(), b.out()));
        return b.to_string();
    }

    /// The substring over the Unicode codepoint range `[start, end)`.
    std::string slice(std::size_t start, std::size_t end) const {
        detail::Bytes b;
        detail::check(loro_text_slice(handle_.get(), start, end, b.out()));
        return b.to_string();
    }

    /// The single Unicode character at codepoint index `pos` (as a UTF-8 std::string).
    std::string char_at(std::size_t pos) const {
        detail::Bytes b;
        detail::check(loro_text_char_at(handle_.get(), pos, b.out()));
        return b.to_string();
    }

    /// Inserts `s` (UTF-8) at UTF-16 code unit index `pos`.
    void insert_utf16(std::size_t pos, std::string_view s) {
        detail::check(loro_text_insert_utf16(handle_.get(), pos, s.data(), s.size()));
    }

    /// Deletes `len` UTF-16 code units starting at UTF-16 code unit index `pos`.
    void remove_utf16(std::size_t pos, std::size_t len) {
        detail::check(loro_text_delete_utf16(handle_.get(), pos, len));
    }

    /// Length in UTF-16 code units.
    std::size_t len_utf16() const { return loro_text_len_utf16(handle_.get()); }

    /// Converts `index` from the `from` coordinate system to the `to` coordinate system.
    std::size_t convert_pos(std::size_t index, PosType from, PosType to) const {
        std::size_t out = 0;
        detail::check(loro_text_convert_pos(handle_.get(), index, from, to, &out));
        return out;
    }

    /// Appends `s` (UTF-8) to the end of the text.
    void push_str(std::string_view s) {
        detail::check(loro_text_push_str(handle_.get(), s.data(), s.size()));
    }

    // --- G2: cursors ---

    /// A stable cursor anchored at codepoint index `pos` (on `side`), or std::nullopt if the
    /// position cannot be anchored. Resolve it later with Doc::get_cursor_pos.
    std::optional<Cursor> get_cursor(std::size_t pos, Side side = LORO_SIDE_MIDDLE) const;

    // --- G6.4: container introspection & attribution ---

    /// Whether this container has been deleted from its document.
    bool is_deleted() const { return loro_text_is_deleted(handle_.get()); }

    /// Whether this container is attached to a document.
    bool is_attached() const { return loro_text_is_attached(handle_.get()); }

    /// The attached counterpart of this detached container, or std::nullopt if none.
    std::optional<Text> get_attached() const {
        LoroText* t = loro_text_get_attached(handle_.get());
        if (!t) return std::nullopt;
        return Text(t);
    }

    /// The document this container belongs to, or std::nullopt if detached.
    std::optional<Doc> doc() const;

    /// Subscribes to this container's changes; std::nullopt if the container is detached.
    std::optional<Subscription> subscribe(std::function<void(const DiffEvent&)> cb) const;

    /// The peer id of the last editor at Unicode codepoint `pos`, or std::nullopt.
    std::optional<std::uint64_t> get_editor_at_unicode_pos(std::size_t pos) const {
        std::uint64_t out = 0;
        if (!loro_text_get_editor_at_unicode_pos(handle_.get(), pos, &out)) return std::nullopt;
        return out;
    }

private:
    struct Deleter {
        void operator()(LoroText* p) const noexcept { loro_text_free(p); }
    };
    std::unique_ptr<LoroText, Deleter> handle_;
};

/// RAII wrapper around a `LoroMap*` container handle. Move-only. Values are exchanged as
/// JSON strings; nested containers via the type-erased Container.
class Map {
public:
    /// Takes ownership of a raw handle (e.g. returned by loro_doc_get_map).
    explicit Map(LoroMap* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    /// The underlying C handle (non-owning).
    LoroMap* raw() const noexcept { return handle_.get(); }

    /// This container's id string (e.g. "cid:root-name:Map"); pass to Doc::subscribe.
    std::string id() const {
        detail::Bytes b;
        detail::check(loro_map_id(handle_.get(), b.out()));
        return b.to_string();
    }

    /// Inserts the JSON-encoded value `json` under `key`.
    void insert(std::string_view key, std::string_view json) {
        detail::check(
            loro_map_insert(handle_.get(), key.data(), key.size(), json.data(), json.size()));
    }

    /// The value at `key` as a JSON string (containers resolved to their deep value), or
    /// std::nullopt if absent. For a child container handle, use get_container.
    std::optional<std::string> get(std::string_view key) const {
        detail::Bytes b;
        if (!detail::check_found(loro_map_get(handle_.get(), key.data(), key.size(), b.out())))
            return std::nullopt;
        return b.to_string();
    }

    /// The child container at `key`, or std::nullopt if absent or a plain value.
    std::optional<Container> get_container(std::string_view key) const;

    /// Attaches `child` under `key`, returning the attached handle. Consumes `child`.
    Container insert_container(std::string_view key, Container&& child);

    /// Deletes the entry at `key` (no error if absent).
    void remove(std::string_view key) {
        detail::check(loro_map_delete(handle_.get(), key.data(), key.size()));
    }

    /// The map's keys as a JSON array string.
    std::string keys() const {
        detail::Bytes b;
        detail::check(loro_map_keys(handle_.get(), b.out()));
        return b.to_string();
    }

    /// The whole map as a JSON object string.
    std::string to_json() const {
        detail::Bytes b;
        detail::check(loro_map_to_json(handle_.get(), b.out()));
        return b.to_string();
    }

    std::size_t size() const { return loro_map_len(handle_.get()); }
    bool empty() const { return loro_map_is_empty(handle_.get()); }
    void clear() { detail::check(loro_map_clear(handle_.get())); }

    // --- G6.4: container introspection & attribution ---

    /// Whether this container has been deleted from its document.
    bool is_deleted() const { return loro_map_is_deleted(handle_.get()); }

    /// Whether this container is attached to a document.
    bool is_attached() const { return loro_map_is_attached(handle_.get()); }

    /// The attached counterpart of this detached container, or std::nullopt if none.
    std::optional<Map> get_attached() const {
        LoroMap* m = loro_map_get_attached(handle_.get());
        if (!m) return std::nullopt;
        return Map(m);
    }

    /// The document this container belongs to, or std::nullopt if detached.
    std::optional<Doc> doc() const;

    /// Subscribes to this container's changes; std::nullopt if the container is detached.
    std::optional<Subscription> subscribe(std::function<void(const DiffEvent&)> cb) const;

    /// The peer id of the last editor of `key`, or std::nullopt if there is none.
    std::optional<std::uint64_t> get_last_editor(std::string_view key) const {
        std::uint64_t out = 0;
        if (!loro_map_get_last_editor(handle_.get(), key.data(), key.size(), &out))
            return std::nullopt;
        return out;
    }

    // --- G6.5: mergeable child containers ---
    //
    // Get-or-create a child container at `key` whose concurrent creations at the same key
    // *merge* into one container (unlike insert_container, which conflicts). Throws on error
    // (e.g. the key already holds a non-mergeable value).

    /// Get-or-create a mergeable Text child at `key`.
    Text ensure_mergeable_text(std::string_view key);
    /// Get-or-create a mergeable Map child at `key`.
    Map ensure_mergeable_map(std::string_view key);
    /// Get-or-create a mergeable List child at `key`.
    List ensure_mergeable_list(std::string_view key);
    /// Get-or-create a mergeable MovableList child at `key`.
    MovableList ensure_mergeable_movable_list(std::string_view key);
    /// Get-or-create a mergeable Tree child at `key`.
    Tree ensure_mergeable_tree(std::string_view key);
    /// Get-or-create a mergeable Counter child at `key`.
    Counter ensure_mergeable_counter(std::string_view key);

private:
    struct Deleter {
        void operator()(LoroMap* p) const noexcept { loro_map_free(p); }
    };
    std::unique_ptr<LoroMap, Deleter> handle_;
};

/// RAII wrapper around a `LoroList*` container handle. Move-only.
class List {
public:
    explicit List(LoroList* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    LoroList* raw() const noexcept { return handle_.get(); }

    /// This container's id string (e.g. "cid:root-name:List"); pass to Doc::subscribe.
    std::string id() const {
        detail::Bytes b;
        detail::check(loro_list_id(handle_.get(), b.out()));
        return b.to_string();
    }

    /// Inserts the JSON-encoded value `json` at `pos`.
    void insert(std::size_t pos, std::string_view json) {
        detail::check(loro_list_insert(handle_.get(), pos, json.data(), json.size()));
    }

    /// Appends the JSON-encoded value `json`.
    void push(std::string_view json) {
        detail::check(loro_list_push(handle_.get(), json.data(), json.size()));
    }

    /// The value at `index` as a JSON string, or std::nullopt if out of range.
    std::optional<std::string> get(std::size_t index) const {
        detail::Bytes b;
        if (!detail::check_found(loro_list_get(handle_.get(), index, b.out())))
            return std::nullopt;
        return b.to_string();
    }

    /// The child container at `index`, or std::nullopt if out of range or a plain value.
    std::optional<Container> get_container(std::size_t index) const;

    /// Inserts `child` at `pos`, returning the attached handle. Consumes `child`.
    Container insert_container(std::size_t pos, Container&& child);

    /// Appends `child`, returning the attached handle. Consumes `child`.
    Container push_container(Container&& child);

    /// Deletes `len` elements starting at `pos`.
    void remove(std::size_t pos, std::size_t len) {
        detail::check(loro_list_delete(handle_.get(), pos, len));
    }

    /// Pops the last element as a JSON string, or std::nullopt if the list is empty.
    std::optional<std::string> pop() {
        detail::Bytes b;
        bool present = false;
        detail::check(loro_list_pop(handle_.get(), b.out(), &present));
        if (!present) return std::nullopt;
        return b.to_string();
    }

    /// The whole list as a JSON array string.
    std::string to_json() const {
        detail::Bytes b;
        detail::check(loro_list_to_json(handle_.get(), b.out()));
        return b.to_string();
    }

    std::size_t size() const { return loro_list_len(handle_.get()); }
    bool empty() const { return loro_list_is_empty(handle_.get()); }
    void clear() { detail::check(loro_list_clear(handle_.get())); }

    /// A stable cursor anchored at index `pos` (on `side`), or std::nullopt if the position
    /// cannot be anchored. Resolve it later with Doc::get_cursor_pos.
    std::optional<Cursor> get_cursor(std::size_t pos, Side side = LORO_SIDE_MIDDLE) const;

    // --- G6.4: container introspection ---

    /// Whether this container has been deleted from its document.
    bool is_deleted() const { return loro_list_is_deleted(handle_.get()); }

    /// Whether this container is attached to a document.
    bool is_attached() const { return loro_list_is_attached(handle_.get()); }

    /// The attached counterpart of this detached container, or std::nullopt if none.
    std::optional<List> get_attached() const {
        LoroList* l = loro_list_get_attached(handle_.get());
        if (!l) return std::nullopt;
        return List(l);
    }

    /// The document this container belongs to, or std::nullopt if detached.
    std::optional<Doc> doc() const;

    /// Subscribes to this container's changes; std::nullopt if the container is detached.
    std::optional<Subscription> subscribe(std::function<void(const DiffEvent&)> cb) const;

private:
    struct Deleter {
        void operator()(LoroList* p) const noexcept { loro_list_free(p); }
    };
    std::unique_ptr<LoroList, Deleter> handle_;
};

/// RAII wrapper around a `LoroMovableList*` container handle. Move-only. Adds in-place
/// `set` and element `move` over the plain List surface.
class MovableList {
public:
    explicit MovableList(LoroMovableList* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    LoroMovableList* raw() const noexcept { return handle_.get(); }

    /// This container's id string (e.g. "cid:root-name:MovableList"); pass to Doc::subscribe.
    std::string id() const {
        detail::Bytes b;
        detail::check(loro_movable_list_id(handle_.get(), b.out()));
        return b.to_string();
    }

    void insert(std::size_t pos, std::string_view json) {
        detail::check(loro_movable_list_insert(handle_.get(), pos, json.data(), json.size()));
    }

    void push(std::string_view json) {
        detail::check(loro_movable_list_push(handle_.get(), json.data(), json.size()));
    }

    /// Replaces the value at `pos` in place with the JSON-encoded value `json`.
    void set(std::size_t pos, std::string_view json) {
        detail::check(loro_movable_list_set(handle_.get(), pos, json.data(), json.size()));
    }

    /// Moves the element at `from` to `to`, preserving its identity.
    void move(std::size_t from, std::size_t to) {
        detail::check(loro_movable_list_mov(handle_.get(), from, to));
    }

    std::optional<std::string> get(std::size_t index) const {
        detail::Bytes b;
        if (!detail::check_found(loro_movable_list_get(handle_.get(), index, b.out())))
            return std::nullopt;
        return b.to_string();
    }

    std::optional<Container> get_container(std::size_t index) const;
    Container insert_container(std::size_t pos, Container&& child);
    Container push_container(Container&& child);

    /// Replaces the element at `pos` in place with `child`, returning the attached handle.
    /// Consumes `child`.
    Container set_container(std::size_t pos, Container&& child);

    void remove(std::size_t pos, std::size_t len) {
        detail::check(loro_movable_list_delete(handle_.get(), pos, len));
    }

    std::optional<std::string> pop() {
        detail::Bytes b;
        bool present = false;
        detail::check(loro_movable_list_pop(handle_.get(), b.out(), &present));
        if (!present) return std::nullopt;
        return b.to_string();
    }

    std::string to_json() const {
        detail::Bytes b;
        detail::check(loro_movable_list_to_json(handle_.get(), b.out()));
        return b.to_string();
    }

    std::size_t size() const { return loro_movable_list_len(handle_.get()); }
    bool empty() const { return loro_movable_list_is_empty(handle_.get()); }
    void clear() { detail::check(loro_movable_list_clear(handle_.get())); }

    /// A stable cursor anchored at index `pos` (on `side`), or std::nullopt if the position
    /// cannot be anchored. Resolve it later with Doc::get_cursor_pos.
    std::optional<Cursor> get_cursor(std::size_t pos, Side side = LORO_SIDE_MIDDLE) const;

    // --- G6.4: container introspection & attribution ---

    /// Whether this container has been deleted from its document.
    bool is_deleted() const { return loro_movable_list_is_deleted(handle_.get()); }

    /// Whether this container is attached to a document.
    bool is_attached() const { return loro_movable_list_is_attached(handle_.get()); }

    /// The attached counterpart of this detached container, or std::nullopt if none.
    std::optional<MovableList> get_attached() const {
        LoroMovableList* l = loro_movable_list_get_attached(handle_.get());
        if (!l) return std::nullopt;
        return MovableList(l);
    }

    /// The document this container belongs to, or std::nullopt if detached.
    std::optional<Doc> doc() const;

    /// Subscribes to this container's changes; std::nullopt if the container is detached.
    std::optional<Subscription> subscribe(std::function<void(const DiffEvent&)> cb) const;

    /// The peer id that created the element at `pos`, or std::nullopt.
    std::optional<std::uint64_t> get_creator_at(std::size_t pos) const {
        std::uint64_t out = 0;
        if (!loro_movable_list_get_creator_at(handle_.get(), pos, &out)) return std::nullopt;
        return out;
    }

    /// The peer id that last moved the element at `pos`, or std::nullopt.
    std::optional<std::uint64_t> get_last_mover_at(std::size_t pos) const {
        std::uint64_t out = 0;
        if (!loro_movable_list_get_last_mover_at(handle_.get(), pos, &out)) return std::nullopt;
        return out;
    }

    /// The peer id that last edited the value at `pos`, or std::nullopt.
    std::optional<std::uint64_t> get_last_editor_at(std::size_t pos) const {
        std::uint64_t out = 0;
        if (!loro_movable_list_get_last_editor_at(handle_.get(), pos, &out)) return std::nullopt;
        return out;
    }

private:
    struct Deleter {
        void operator()(LoroMovableList* p) const noexcept { loro_movable_list_free(p); }
    };
    std::unique_ptr<LoroMovableList, Deleter> handle_;
};

/// RAII wrapper around a `LoroCounter*` container handle. Move-only.
class Counter {
public:
    explicit Counter(LoroCounter* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    LoroCounter* raw() const noexcept { return handle_.get(); }

    /// This container's id string (e.g. "cid:root-name:Counter"); pass to Doc::subscribe.
    std::string id() const {
        detail::Bytes b;
        detail::check(loro_counter_id(handle_.get(), b.out()));
        return b.to_string();
    }

    /// Increments the counter by `value` (may be negative).
    void increment(double value) { detail::check(loro_counter_increment(handle_.get(), value)); }

    /// Decrements the counter by `value` (may be negative).
    void decrement(double value) { detail::check(loro_counter_decrement(handle_.get(), value)); }

    /// The counter's current value.
    double value() const { return loro_counter_get_value(handle_.get()); }

    // --- G6.4: container introspection ---

    /// Whether this container has been deleted from its document.
    bool is_deleted() const { return loro_counter_is_deleted(handle_.get()); }

    /// Whether this container is attached to a document.
    bool is_attached() const { return loro_counter_is_attached(handle_.get()); }

    /// The attached counterpart of this detached container, or std::nullopt if none.
    std::optional<Counter> get_attached() const {
        LoroCounter* c = loro_counter_get_attached(handle_.get());
        if (!c) return std::nullopt;
        return Counter(c);
    }

    /// The document this container belongs to, or std::nullopt if detached.
    std::optional<Doc> doc() const;

    /// Subscribes to this container's changes; std::nullopt if the container is detached.
    std::optional<Subscription> subscribe(std::function<void(const DiffEvent&)> cb) const;

private:
    struct Deleter {
        void operator()(LoroCounter* p) const noexcept { loro_counter_free(p); }
    };
    std::unique_ptr<LoroCounter, Deleter> handle_;
};

/// RAII wrapper around a `LoroTree*` container handle. Move-only. A node is a TreeId; a
/// null/absent parent means a root.
class Tree {
public:
    explicit Tree(LoroTree* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    LoroTree* raw() const noexcept { return handle_.get(); }

    /// This container's id string (e.g. "cid:root-name:Tree"); pass to Doc::subscribe.
    std::string id() const {
        detail::Bytes b;
        detail::check(loro_tree_id(handle_.get(), b.out()));
        return b.to_string();
    }

    /// Creates a node under `parent` (nullopt = root).
    TreeId create(std::optional<TreeId> parent = std::nullopt) {
        TreeId out{};
        const LoroTreeID* p = parent ? &parent.value() : nullptr;
        detail::check(loro_tree_create(handle_.get(), p, &out));
        return out;
    }

    /// Creates a node under `parent` (nullopt = root) at child position `index`. Requires
    /// the fractional index to be enabled.
    TreeId create_at(std::size_t index, std::optional<TreeId> parent = std::nullopt) {
        TreeId out{};
        const LoroTreeID* p = parent ? &parent.value() : nullptr;
        detail::check(loro_tree_create_at(handle_.get(), p, index, &out));
        return out;
    }

    /// Moves `target` to be a child of `parent` (nullopt = root).
    void move_to(TreeId target, std::optional<TreeId> parent = std::nullopt) {
        const LoroTreeID* p = parent ? &parent.value() : nullptr;
        detail::check(loro_tree_mov(handle_.get(), target, p));
    }

    /// Moves `target` to child position `index` under `parent` (nullopt = root). Requires
    /// the fractional index to be enabled.
    void move_to_index(TreeId target, std::size_t index,
                       std::optional<TreeId> parent = std::nullopt) {
        const LoroTreeID* p = parent ? &parent.value() : nullptr;
        detail::check(loro_tree_mov_to(handle_.get(), target, p, index));
    }

    /// Moves `target` to be the sibling immediately after `after`. Requires the fractional
    /// index to be enabled.
    void move_after(TreeId target, TreeId after) {
        detail::check(loro_tree_mov_after(handle_.get(), target, after));
    }

    /// Moves `target` to be the sibling immediately before `before`. Requires the
    /// fractional index to be enabled.
    void move_before(TreeId target, TreeId before) {
        detail::check(loro_tree_mov_before(handle_.get(), target, before));
    }

    /// Deletes `target` and its subtree.
    void erase(TreeId target) { detail::check(loro_tree_delete(handle_.get(), target)); }

    /// The metadata map of `target`.
    Map get_meta(TreeId target) const {
        LoroMap* m = loro_tree_get_meta(handle_.get(), target);
        if (!m) throw Error(LORO_ERR_OTHER, detail::last_error_message());
        return Map(m);
    }

    bool contains(TreeId target) const { return loro_tree_contains(handle_.get(), target); }

    bool is_node_deleted(TreeId target) const {
        bool out = false;
        detail::check(loro_tree_is_node_deleted(handle_.get(), target, &out));
        return out;
    }

    /// `target`'s fractional index, or std::nullopt if it has none.
    std::optional<std::string> fractional_index(TreeId target) const {
        detail::Bytes b;
        if (!detail::check_found(loro_tree_fractional_index(handle_.get(), target, b.out())))
            return std::nullopt;
        return b.to_string();
    }

    /// All root nodes.
    std::vector<TreeId> roots() const {
        const std::size_t n = loro_tree_roots_len(handle_.get());
        std::vector<TreeId> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            TreeId id{};
            detail::check(loro_tree_root_at(handle_.get(), i, &id));
            out.push_back(id);
        }
        return out;
    }

    /// The children of `parent` (nullopt = root), or std::nullopt if `parent` does not
    /// exist.
    std::optional<std::vector<TreeId>> children(std::optional<TreeId> parent = std::nullopt) const {
        const LoroTreeID* p = parent ? &parent.value() : nullptr;
        std::size_t n = 0;
        if (!detail::check_found(loro_tree_children_len(handle_.get(), p, &n)))
            return std::nullopt;
        std::vector<TreeId> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            TreeId id{};
            detail::check(loro_tree_child_at(handle_.get(), p, i, &id));
            out.push_back(id);
        }
        return out;
    }

    /// Enables the fractional index (required for positional moves).
    void enable_fractional_index(std::uint8_t jitter = 0) {
        detail::check(loro_tree_enable_fractional_index(handle_.get(), jitter));
    }

    bool is_fractional_index_enabled() const {
        return loro_tree_is_fractional_index_enabled(handle_.get());
    }

    bool empty() const { return loro_tree_is_empty(handle_.get()); }

    /// The whole tree as a JSON string.
    std::string to_json() const {
        detail::Bytes b;
        detail::check(loro_tree_to_json(handle_.get(), b.out()));
        return b.to_string();
    }

    // --- G6.4: container introspection & attribution ---

    /// Whether this container has been deleted from its document.
    bool is_deleted() const { return loro_tree_is_deleted(handle_.get()); }

    /// Whether this container is attached to a document.
    bool is_attached() const { return loro_tree_is_attached(handle_.get()); }

    /// The attached counterpart of this detached container, or std::nullopt if none.
    std::optional<Tree> get_attached() const {
        LoroTree* t = loro_tree_get_attached(handle_.get());
        if (!t) return std::nullopt;
        return Tree(t);
    }

    /// The document this container belongs to, or std::nullopt if detached.
    std::optional<Doc> doc() const;

    /// Subscribes to this container's changes; std::nullopt if the container is detached.
    std::optional<Subscription> subscribe(std::function<void(const DiffEvent&)> cb) const;

    /// The op id of the last move of node `target`, or std::nullopt if it never moved.
    std::optional<::LoroId> get_last_move_id(TreeId target) const {
        ::LoroId out{};
        if (!loro_tree_get_last_move_id(handle_.get(), target, &out)) return std::nullopt;
        return out;
    }

    // --- G6.5: tree extras ---

    /// A node's parent classification (`kind`); `node` carries the parent id only when
    /// `kind == LORO_TREE_PARENT_NODE`.
    struct Parent {
        ParentKind kind;
        std::optional<TreeId> node;
    };

    /// The parent classification of `target`, or std::nullopt if `target` does not exist.
    std::optional<Parent> parent(TreeId target) const {
        ParentKind kind{};
        TreeId node{};
        if (!loro_tree_parent(handle_.get(), target, &kind, &node)) return std::nullopt;
        Parent out{kind, std::nullopt};
        if (kind == LORO_TREE_PARENT_NODE) out.node = node;
        return out;
    }

    /// All root nodes as a JSON array string of `{peer,counter}` objects.
    std::string roots_json() const {
        detail::Bytes b;
        detail::check(loro_tree_roots_json(handle_.get(), b.out()));
        return b.to_string();
    }

    /// All nodes (including deleted) as a JSON array string of `{peer,counter}` objects.
    std::string nodes_json() const {
        detail::Bytes b;
        detail::check(loro_tree_nodes_json(handle_.get(), b.out()));
        return b.to_string();
    }

    /// The children of `parent` (nullopt = root) as a JSON array string of `{peer,counter}`
    /// objects. Throws if `parent` does not exist.
    std::string children_json(std::optional<TreeId> parent = std::nullopt) const {
        const LoroTreeID* p = parent ? &parent.value() : nullptr;
        detail::Bytes b;
        detail::check(loro_tree_children_json(handle_.get(), p, b.out()));
        return b.to_string();
    }

    /// The tree's hierarchy with each node's metadata resolved, as a JSON string.
    std::string get_value_with_meta_json() const {
        detail::Bytes b;
        detail::check(loro_tree_get_value_with_meta_json(handle_.get(), b.out()));
        return b.to_string();
    }

    /// Disables the fractional index (positional moves become unavailable afterwards).
    void disable_fractional_index() {
        detail::check(loro_tree_disable_fractional_index(handle_.get()));
    }

private:
    struct Deleter {
        void operator()(LoroTree* p) const noexcept { loro_tree_free(p); }
    };
    std::unique_ptr<LoroTree, Deleter> handle_;
};

/// RAII wrapper around a type-erased `LoroContainer*`. Move-only. Created detached with a
/// factory (e.g. Container::map()), attached via a typed wrapper's *_container method, and
/// downcast to a typed wrapper with as_*().
class Container {
public:
    explicit Container(LoroContainer* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    /// Creates a new detached container of `type`.
    static Container create(ContainerType type) { return Container(loro_container_new(type)); }
    static Container map() { return create(LORO_CONTAINER_MAP); }
    static Container list() { return create(LORO_CONTAINER_LIST); }
    static Container text() { return create(LORO_CONTAINER_TEXT); }
    static Container movable_list() { return create(LORO_CONTAINER_MOVABLE_LIST); }
    static Container tree() { return create(LORO_CONTAINER_TREE); }
    static Container counter() { return create(LORO_CONTAINER_COUNTER); }

    LoroContainer* raw() const noexcept { return handle_.get(); }

    /// This container's id string (e.g. "cid:root-name:Map"); pass to Doc::subscribe.
    /// Only meaningful once the container is attached to a document.
    std::string id() const {
        detail::Bytes b;
        detail::check(loro_container_id(handle_.get(), b.out()));
        return b.to_string();
    }

    /// Relinquishes ownership of the raw handle. Used internally when passing a container
    /// into a *_insert_container function, which consumes it.
    LoroContainer* release() noexcept { return handle_.release(); }

    /// The kind of container.
    ContainerType type() const { return loro_container_type(handle_.get()); }

    Map as_map() const {
        LoroMap* m = loro_container_get_map(handle_.get());
        if (!m) throw Error(LORO_ERR_OTHER, detail::last_error_message());
        return Map(m);
    }
    List as_list() const {
        LoroList* l = loro_container_get_list(handle_.get());
        if (!l) throw Error(LORO_ERR_OTHER, detail::last_error_message());
        return List(l);
    }
    Text as_text() const {
        LoroText* t = loro_container_get_text(handle_.get());
        if (!t) throw Error(LORO_ERR_OTHER, detail::last_error_message());
        return Text(t);
    }
    MovableList as_movable_list() const {
        LoroMovableList* l = loro_container_get_movable_list(handle_.get());
        if (!l) throw Error(LORO_ERR_OTHER, detail::last_error_message());
        return MovableList(l);
    }
    Tree as_tree() const {
        LoroTree* t = loro_container_get_tree(handle_.get());
        if (!t) throw Error(LORO_ERR_OTHER, detail::last_error_message());
        return Tree(t);
    }
    Counter as_counter() const {
        LoroCounter* c = loro_container_get_counter(handle_.get());
        if (!c) throw Error(LORO_ERR_OTHER, detail::last_error_message());
        return Counter(c);
    }

private:
    struct Deleter {
        void operator()(LoroContainer* p) const noexcept { loro_container_free(p); }
    };
    std::unique_ptr<LoroContainer, Deleter> handle_;
};

// Out-of-line definitions of the typed wrappers' Container-coupled methods (Container is
// now complete).

inline std::optional<Container> Map::get_container(std::string_view key) const {
    LoroContainer* c = loro_map_get_container(handle_.get(), key.data(), key.size());
    if (!c) return std::nullopt;
    return Container(c);
}

inline Container Map::insert_container(std::string_view key, Container&& child) {
    LoroContainer* a =
        loro_map_insert_container(handle_.get(), key.data(), key.size(), child.release());
    if (!a) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    return Container(a);
}

// G6.5: Map mergeable child containers. Defined out-of-line because List/MovableList/Tree/
// Counter are declared after Map.
inline Text Map::ensure_mergeable_text(std::string_view key) {
    LoroText* c = loro_map_ensure_mergeable_text(handle_.get(), key.data(), key.size());
    if (!c) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    return Text(c);
}

inline Map Map::ensure_mergeable_map(std::string_view key) {
    LoroMap* c = loro_map_ensure_mergeable_map(handle_.get(), key.data(), key.size());
    if (!c) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    return Map(c);
}

inline List Map::ensure_mergeable_list(std::string_view key) {
    LoroList* c = loro_map_ensure_mergeable_list(handle_.get(), key.data(), key.size());
    if (!c) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    return List(c);
}

inline MovableList Map::ensure_mergeable_movable_list(std::string_view key) {
    LoroMovableList* c =
        loro_map_ensure_mergeable_movable_list(handle_.get(), key.data(), key.size());
    if (!c) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    return MovableList(c);
}

inline Tree Map::ensure_mergeable_tree(std::string_view key) {
    LoroTree* c = loro_map_ensure_mergeable_tree(handle_.get(), key.data(), key.size());
    if (!c) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    return Tree(c);
}

inline Counter Map::ensure_mergeable_counter(std::string_view key) {
    LoroCounter* c = loro_map_ensure_mergeable_counter(handle_.get(), key.data(), key.size());
    if (!c) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    return Counter(c);
}

inline std::optional<Container> List::get_container(std::size_t index) const {
    LoroContainer* c = loro_list_get_container(handle_.get(), index);
    if (!c) return std::nullopt;
    return Container(c);
}

inline Container List::insert_container(std::size_t pos, Container&& child) {
    LoroContainer* a = loro_list_insert_container(handle_.get(), pos, child.release());
    if (!a) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    return Container(a);
}

inline Container List::push_container(Container&& child) {
    LoroContainer* a = loro_list_push_container(handle_.get(), child.release());
    if (!a) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    return Container(a);
}

inline std::optional<Container> MovableList::get_container(std::size_t index) const {
    LoroContainer* c = loro_movable_list_get_container(handle_.get(), index);
    if (!c) return std::nullopt;
    return Container(c);
}

inline Container MovableList::insert_container(std::size_t pos, Container&& child) {
    LoroContainer* a = loro_movable_list_insert_container(handle_.get(), pos, child.release());
    if (!a) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    return Container(a);
}

inline Container MovableList::push_container(Container&& child) {
    LoroContainer* a = loro_movable_list_push_container(handle_.get(), child.release());
    if (!a) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    return Container(a);
}

inline Container MovableList::set_container(std::size_t pos, Container&& child) {
    LoroContainer* a = loro_movable_list_set_container(handle_.get(), pos, child.release());
    if (!a) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    return Container(a);
}

/// RAII wrapper around a `LoroCursor*` — a stable position that survives concurrent edits.
/// Move-only. Obtain one from Text/List/MovableList::get_cursor, transport it via
/// encode()/decode(), and resolve it back to an absolute position with Doc::get_cursor_pos.
/// Unlike the container handles, a Cursor does not co-own the document state.
class Cursor {
public:
    explicit Cursor(LoroCursor* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    /// Decodes a cursor previously produced by encode().
    static Cursor decode(const std::uint8_t* data, std::size_t len) {
        LoroCursor* c = loro_cursor_decode(data, len);
        if (!c) throw Error(LORO_ERR_DECODE, detail::last_error_message());
        return Cursor(c);
    }
    static Cursor decode(const std::vector<std::uint8_t>& data) {
        return decode(data.data(), data.size());
    }

    LoroCursor* raw() const noexcept { return handle_.get(); }

    /// Encodes the cursor into a compact, transportable byte buffer.
    std::vector<std::uint8_t> encode() const {
        detail::Bytes b;
        detail::check(loro_cursor_encode(handle_.get(), b.out()));
        return b.to_vector();
    }

private:
    struct Deleter {
        void operator()(LoroCursor* p) const noexcept { loro_cursor_free(p); }
    };
    std::unique_ptr<LoroCursor, Deleter> handle_;
};

// Out-of-line definitions of the containers' get_cursor methods (Cursor is now complete).

inline std::optional<Cursor> Text::get_cursor(std::size_t pos, Side side) const {
    LoroCursor* c = loro_text_get_cursor(handle_.get(), pos, side);
    if (!c) return std::nullopt;
    return Cursor(c);
}

inline std::optional<Cursor> List::get_cursor(std::size_t pos, Side side) const {
    LoroCursor* c = loro_list_get_cursor(handle_.get(), pos, side);
    if (!c) return std::nullopt;
    return Cursor(c);
}

inline std::optional<Cursor> MovableList::get_cursor(std::size_t pos, Side side) const {
    LoroCursor* c = loro_movable_list_get_cursor(handle_.get(), pos, side);
    if (!c) return std::nullopt;
    return Cursor(c);
}

/// How a diff event was triggered. Alias of the C ABI enum.
using EventTriggerKind = ::LoroEventTriggerKind;

/// The kind of a container diff (selects how to read ContainerDiff::to_json). Alias of the
/// C ABI enum.
using DiffKind = ::LoroDiffKind;

/// Non-owning, **callback-scoped** view of one container's diff within a DiffEvent. Valid
/// only for the duration of the subscriber callback; never store it or any string obtained
/// from it beyond the call.
class ContainerDiff {
public:
    explicit ContainerDiff(const LoroContainerDiff* raw) noexcept : raw_(raw) {}

    const LoroContainerDiff* raw() const noexcept { return raw_; }

    /// The target container's id string (matches e.g. Text::id()).
    std::string target() const {
        detail::Bytes b;
        detail::check(loro_container_diff_target(raw_, b.out()));
        return b.to_string();
    }

    /// Whether the diff is from an unknown container type.
    bool is_unknown() const { return loro_container_diff_is_unknown(raw_); }

    /// The kind of this diff; selects how to interpret to_json().
    DiffKind kind() const { return loro_container_diff_kind(raw_); }

    /// The path from the document root to this container, as a JSON array string.
    std::string path_json() const {
        detail::Bytes b;
        detail::check(loro_container_diff_path_json(raw_, b.out()));
        return b.to_string();
    }

    /// This container's delta payload as a JSON string (schema depends on kind()).
    std::string to_json() const {
        detail::Bytes b;
        detail::check(loro_container_diff_to_json(raw_, b.out()));
        return b.to_string();
    }

private:
    const LoroContainerDiff* raw_;
};

/// Non-owning, **callback-scoped** view of a diff event passed to a subscriber. Valid only
/// for the duration of the callback; never store it (or any ContainerDiff / string obtained
/// from it) beyond the call.
class DiffEvent {
public:
    explicit DiffEvent(const LoroDiffEvent* raw) noexcept : raw_(raw) {}

    const LoroDiffEvent* raw() const noexcept { return raw_; }

    /// How the event was triggered.
    EventTriggerKind triggered_by() const { return loro_diff_event_triggered_by(raw_); }

    /// The event origin string (possibly empty).
    std::string origin() const {
        detail::Bytes b;
        detail::check(loro_diff_event_origin(raw_, b.out()));
        return b.to_string();
    }

    /// The current target container id, or std::nullopt (e.g. for a root subscription).
    std::optional<std::string> current_target() const {
        detail::Bytes b;
        if (!detail::check_found(loro_diff_event_current_target(raw_, b.out())))
            return std::nullopt;
        return b.to_string();
    }

    /// The number of per-container diffs.
    std::size_t size() const { return loro_diff_event_count(raw_); }

    /// The container diff at `index`. Throws loro::Error if out of range.
    ContainerDiff operator[](std::size_t index) const {
        const LoroContainerDiff* cd = loro_diff_event_get(raw_, index);
        if (!cd) throw Error(LORO_ERR_NOT_FOUND, detail::last_error_message());
        return ContainerDiff(cd);
    }

private:
    const LoroDiffEvent* raw_;
};

/// RAII handle for a subscription. Destroying it unsubscribes and runs the callback's
/// user-data destructor. Move-only.
class Subscription {
public:
    explicit Subscription(LoroSubscription* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    LoroSubscription* raw() const noexcept { return handle_.get(); }

    /// Detaches: stops managing the subscription here, leaving the callback firing until the
    /// document is dropped. The handle becomes empty.
    void detach() { loro_subscription_detach(handle_.release()); }

private:
    struct Deleter {
        void operator()(LoroSubscription* p) const noexcept { loro_subscription_free(p); }
    };
    std::unique_ptr<LoroSubscription, Deleter> handle_;
};

/// A document/container subscriber: invoked with a callback-scoped DiffEvent on each change.
/// May fire from any thread that mutates the document, so it must be reentrant.
using SubscriberFn = std::function<void(const DiffEvent&)>;

/// A local-update subscriber: invoked with the update bytes of each local commit; return
/// false to auto-unsubscribe. May fire from any thread that mutates the document.
using LocalUpdateFn = std::function<bool(const std::uint8_t*, std::size_t)>;

namespace detail {

// `extern "C"` trampolines bridging the type-erased C callback triple to the stored
// std::function. Uniquely named to avoid clashing with other C symbols.
extern "C" inline void loro_hpp_subscriber_invoke(const LoroDiffEvent* ev, void* ud) {
    DiffEvent event(ev);
    (*static_cast<SubscriberFn*>(ud))(event);
}
extern "C" inline void loro_hpp_subscriber_free(void* ud) {
    delete static_cast<SubscriberFn*>(ud);
}
extern "C" inline bool loro_hpp_local_update_invoke(const std::uint8_t* data, std::uintptr_t len,
                                                    void* ud) {
    return (*static_cast<LocalUpdateFn*>(ud))(data, len);
}
extern "C" inline void loro_hpp_local_update_free(void* ud) {
    delete static_cast<LocalUpdateFn*>(ud);
}

}  // namespace detail

// ===========================================================================
// M4: versions / frontiers / fractional index / change metadata
// ===========================================================================

/// A single operation id `(peer, counter)`. Alias of the C ABI struct.
using Id = ::LoroId;
/// A half-open op-counter span `[start, end)`. Alias of the C ABI struct.
using CounterSpan = ::LoroCounterSpan;
/// Whether an undo item is on the undo or redo stack. Alias of the C ABI enum.
using UndoOrRedo = ::LoroUndoOrRedo;
/// How an ephemeral-store event was triggered. Alias of the C ABI enum.
using EphemeralEventTrigger = ::LoroEphemeralEventTrigger;

class VersionVector;

/// RAII wrapper around a `LoroFrontiers*` (a document version expressed as a set of op ids).
/// Move-only.
class Frontiers {
public:
    explicit Frontiers(LoroFrontiers* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    /// An empty frontiers.
    static Frontiers create() { return Frontiers(loro_frontiers_new()); }

    /// Builds a frontiers from a list of ids.
    static Frontiers from_ids(const std::vector<Id>& ids) {
        return Frontiers(loro_frontiers_from_ids(ids.data(), ids.size()));
    }

    /// Decodes a frontiers previously produced by encode().
    static Frontiers decode(const std::uint8_t* data, std::size_t len) {
        LoroFrontiers* f = loro_frontiers_decode(data, len);
        if (!f) throw Error(LORO_ERR_DECODE, detail::last_error_message());
        return Frontiers(f);
    }
    static Frontiers decode(const std::vector<std::uint8_t>& data) {
        return decode(data.data(), data.size());
    }

    LoroFrontiers* raw() const noexcept { return handle_.get(); }

    std::vector<std::uint8_t> encode() const {
        detail::Bytes b;
        detail::check(loro_frontiers_encode(handle_.get(), b.out()));
        return b.to_vector();
    }

    std::size_t size() const { return loro_frontiers_len(handle_.get()); }
    bool empty() const { return loro_frontiers_is_empty(handle_.get()); }
    bool contains(Id id) const { return loro_frontiers_contains(handle_.get(), id); }
    void push(Id id) { detail::check(loro_frontiers_push(handle_.get(), id)); }

    /// All ids in the frontiers.
    std::vector<Id> to_vec() const {
        const std::size_t n = size();
        std::vector<Id> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            Id id{};
            detail::check(loro_frontiers_get(handle_.get(), i, &id));
            out.push_back(id);
        }
        return out;
    }

private:
    struct Deleter {
        void operator()(LoroFrontiers* p) const noexcept { loro_frontiers_free(p); }
    };
    std::unique_ptr<LoroFrontiers, Deleter> handle_;
};

/// RAII wrapper around a `LoroVersionVector*` (a `peer -> counter` view of seen history).
/// Move-only.
class VersionVector {
public:
    explicit VersionVector(LoroVersionVector* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    /// An empty version vector.
    static VersionVector create() { return VersionVector(loro_version_vector_new()); }

    /// Decodes a version vector previously produced by encode().
    static VersionVector decode(const std::uint8_t* data, std::size_t len) {
        LoroVersionVector* v = loro_version_vector_decode(data, len);
        if (!v) throw Error(LORO_ERR_DECODE, detail::last_error_message());
        return VersionVector(v);
    }
    static VersionVector decode(const std::vector<std::uint8_t>& data) {
        return decode(data.data(), data.size());
    }

    LoroVersionVector* raw() const noexcept { return handle_.get(); }

    std::vector<std::uint8_t> encode() const {
        detail::Bytes b;
        detail::check(loro_version_vector_encode(handle_.get(), b.out()));
        return b.to_vector();
    }

    /// The last counter seen for `peer`, or std::nullopt if the vector has no entry for it.
    std::optional<std::int32_t> get_last(std::uint64_t peer) const {
        std::int32_t out = 0;
        if (!detail::check_found(loro_version_vector_get_last(handle_.get(), peer, &out)))
            return std::nullopt;
        return out;
    }

    /// Records `id` as the last op seen from its peer.
    void set_last(Id id) { detail::check(loro_version_vector_set_last(handle_.get(), id)); }

    /// Whether this vector has seen op `id`.
    bool includes(Id id) const { return loro_version_vector_includes_id(handle_.get(), id); }
    /// Whether this vector includes everything in `other`.
    bool includes(const VersionVector& other) const {
        return loro_version_vector_includes_vv(handle_.get(), other.raw());
    }

    /// Causal comparison: -1 (this < other), 0 (equal), 1 (this > other), or std::nullopt if
    /// the two are concurrent (incomparable).
    std::optional<int> compare(const VersionVector& other) const {
        std::int32_t out = 0;
        if (!detail::check_found(
                loro_version_vector_compare(handle_.get(), other.raw(), &out)))
            return std::nullopt;
        return out;
    }

    /// The frontiers corresponding to this version vector.
    Frontiers to_frontiers() const {
        LoroFrontiers* f = loro_version_vector_to_frontiers(handle_.get());
        if (!f) throw Error(LORO_ERR_OTHER, detail::last_error_message());
        return Frontiers(f);
    }

    /// The vector as a JSON object string `{"<peer>": <counter>, ...}`.
    std::string to_json() const {
        detail::Bytes b;
        detail::check(loro_version_vector_to_json(handle_.get(), b.out()));
        return b.to_string();
    }

private:
    struct Deleter {
        void operator()(LoroVersionVector* p) const noexcept { loro_version_vector_free(p); }
    };
    std::unique_ptr<LoroVersionVector, Deleter> handle_;
};

/// RAII wrapper around a `LoroFractionalIndex*` (a comparable position key). Move-only.
class FractionalIndex {
public:
    explicit FractionalIndex(LoroFractionalIndex* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    /// The default (smallest) index.
    static FractionalIndex create() {
        return FractionalIndex(loro_fractional_index_default());
    }

    /// An index strictly between `lower` and `upper` (either may be null for "unbounded"),
    /// or std::nullopt if none exists.
    static std::optional<FractionalIndex> between(const FractionalIndex* lower,
                                                  const FractionalIndex* upper) {
        LoroFractionalIndex* fi = loro_fractional_index_between(
            lower ? lower->raw() : nullptr, upper ? upper->raw() : nullptr);
        if (!fi) return std::nullopt;
        return FractionalIndex(fi);
    }

    static FractionalIndex from_bytes(const std::uint8_t* data, std::size_t len) {
        return FractionalIndex(loro_fractional_index_from_bytes(data, len));
    }
    static FractionalIndex from_bytes(const std::vector<std::uint8_t>& data) {
        return from_bytes(data.data(), data.size());
    }
    static FractionalIndex from_string(std::string_view hex) {
        return FractionalIndex(loro_fractional_index_from_string(hex.data(), hex.size()));
    }

    LoroFractionalIndex* raw() const noexcept { return handle_.get(); }

    std::vector<std::uint8_t> to_bytes() const {
        detail::Bytes b;
        detail::check(loro_fractional_index_to_bytes(handle_.get(), b.out()));
        return b.to_vector();
    }
    std::string to_string() const {
        detail::Bytes b;
        detail::check(loro_fractional_index_to_string(handle_.get(), b.out()));
        return b.to_string();
    }

    /// -1 (this < other), 0 (equal), or 1 (this > other).
    int compare(const FractionalIndex& other) const {
        std::int32_t out = 0;
        detail::check(loro_fractional_index_compare(handle_.get(), other.raw(), &out));
        return out;
    }

private:
    struct Deleter {
        void operator()(LoroFractionalIndex* p) const noexcept {
            loro_fractional_index_free(p);
        }
    };
    std::unique_ptr<LoroFractionalIndex, Deleter> handle_;
};

/// Non-owning, **callback-scoped** view of one change's metadata (passed to a change-ancestor
/// traveler or a pre-commit hook). Valid only for the duration of the callback.
class ChangeMeta {
public:
    explicit ChangeMeta(const LoroChangeMeta* raw) noexcept : raw_(raw) {}

    const LoroChangeMeta* raw() const noexcept { return raw_; }

    /// The change's first-op id.
    Id id() const { return loro_change_meta_id(raw_); }
    /// The change's Lamport timestamp.
    std::uint32_t lamport() const { return loro_change_meta_lamport(raw_); }
    /// The change's wall-clock timestamp (seconds since the Unix epoch; 0 if unset).
    std::int64_t timestamp() const { return loro_change_meta_timestamp(raw_); }
    /// The number of ops in the change.
    std::size_t size() const { return loro_change_meta_len(raw_); }
    /// The change's commit message (possibly empty).
    std::string message() const {
        detail::Bytes b;
        detail::check(loro_change_meta_message(raw_, b.out()));
        return b.to_string();
    }
    /// An owned copy of the change's dependency frontiers.
    Frontiers deps() const {
        LoroFrontiers* f = loro_change_meta_deps(raw_);
        if (!f) throw Error(LORO_ERR_OTHER, detail::last_error_message());
        return Frontiers(f);
    }

private:
    const LoroChangeMeta* raw_;
};

/// Owned metadata for one change, returned by `Doc::get_change`. Unlike `ChangeMeta` (a
/// callback-scoped view), this owns its handle and may be held for any lifetime.
class ChangeMetaOwned {
public:
    explicit ChangeMetaOwned(LoroChangeMetaOwned* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    LoroChangeMetaOwned* raw() const noexcept { return handle_.get(); }

    /// The change's first-op id.
    Id id() const { return loro_change_meta_id(as_ref()); }
    /// The change's Lamport timestamp.
    std::uint32_t lamport() const { return loro_change_meta_lamport(as_ref()); }
    /// The change's wall-clock timestamp (seconds since the Unix epoch; 0 if unset).
    std::int64_t timestamp() const { return loro_change_meta_timestamp(as_ref()); }
    /// The number of ops in the change.
    std::size_t size() const { return loro_change_meta_len(as_ref()); }
    /// The change's commit message (possibly empty).
    std::string message() const {
        detail::Bytes b;
        detail::check(loro_change_meta_message(as_ref(), b.out()));
        return b.to_string();
    }
    /// An owned copy of the change's dependency frontiers.
    Frontiers deps() const {
        LoroFrontiers* f = loro_change_meta_deps(as_ref());
        if (!f) throw Error(LORO_ERR_OTHER, detail::last_error_message());
        return Frontiers(f);
    }

private:
    const LoroChangeMeta* as_ref() const {
        return loro_change_meta_owned_as_ref(handle_.get());
    }

    struct Deleter {
        void operator()(LoroChangeMetaOwned* p) const noexcept { loro_change_meta_owned_free(p); }
    };
    std::unique_ptr<LoroChangeMetaOwned, Deleter> handle_;
};

// ===========================================================================
// M4: awareness / ephemeral store
// ===========================================================================

/// RAII wrapper around a `LoroAwareness*` (legacy per-peer presence state). Move-only.
/// Prefer EphemeralStore for new code.
class Awareness {
public:
    explicit Awareness(LoroAwareness* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    /// Creates an awareness for local `peer` with an inactivity `timeout` (milliseconds).
    Awareness(std::uint64_t peer, std::int64_t timeout)
        : handle_(loro_awareness_new(peer, timeout)) {
        if (!handle_) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    LoroAwareness* raw() const noexcept { return handle_.get(); }

    std::uint64_t peer() const { return loro_awareness_peer(handle_.get()); }

    /// Sets the local peer's state from a JSON value string.
    void set_local_state(std::string_view json) {
        detail::check(loro_awareness_set_local_state(handle_.get(), json.data(), json.size()));
    }

    /// The local peer's state as a JSON string, or std::nullopt if unset.
    std::optional<std::string> get_local_state() const {
        detail::Bytes b;
        if (!detail::check_found(loro_awareness_get_local_state(handle_.get(), b.out())))
            return std::nullopt;
        return b.to_string();
    }

    std::vector<std::uint8_t> encode_all() const {
        detail::Bytes b;
        detail::check(loro_awareness_encode_all(handle_.get(), b.out()));
        return b.to_vector();
    }

    std::vector<std::uint8_t> encode(const std::vector<std::uint64_t>& peers) const {
        detail::Bytes b;
        detail::check(loro_awareness_encode(handle_.get(), peers.data(), peers.size(), b.out()));
        return b.to_vector();
    }

    void apply(const std::uint8_t* data, std::size_t len) {
        detail::check(loro_awareness_apply(handle_.get(), data, len));
    }
    void apply(const std::vector<std::uint8_t>& data) { apply(data.data(), data.size()); }

    void remove_outdated() { detail::check(loro_awareness_remove_outdated(handle_.get())); }

    /// All peers' state as a JSON object string.
    std::string get_all_states() const {
        detail::Bytes b;
        detail::check(loro_awareness_get_all_states(handle_.get(), b.out()));
        return b.to_string();
    }

private:
    struct Deleter {
        void operator()(LoroAwareness* p) const noexcept { loro_awareness_free(p); }
    };
    std::unique_ptr<LoroAwareness, Deleter> handle_;
};

/// Non-owning, **callback-scoped** view of an ephemeral-store change event. Valid only for
/// the duration of the subscriber callback.
class EphemeralStoreEvent {
public:
    explicit EphemeralStoreEvent(const LoroEphemeralStoreEvent* raw) noexcept : raw_(raw) {}

    const LoroEphemeralStoreEvent* raw() const noexcept { return raw_; }

    /// How the event was triggered.
    EphemeralEventTrigger by() const { return loro_ephemeral_event_by(raw_); }
    /// The added keys, as a JSON array string.
    std::string added() const {
        detail::Bytes b;
        detail::check(loro_ephemeral_event_added(raw_, b.out()));
        return b.to_string();
    }
    /// The updated keys, as a JSON array string.
    std::string updated() const {
        detail::Bytes b;
        detail::check(loro_ephemeral_event_updated(raw_, b.out()));
        return b.to_string();
    }
    /// The removed keys, as a JSON array string.
    std::string removed() const {
        detail::Bytes b;
        detail::check(loro_ephemeral_event_removed(raw_, b.out()));
        return b.to_string();
    }

private:
    const LoroEphemeralStoreEvent* raw_;
};

/// An ephemeral-store subscriber: invoked with a callback-scoped event on each change.
using EphemeralSubscriberFn = std::function<void(const EphemeralStoreEvent&)>;

// ===========================================================================
// M4: undo manager metadata view
// ===========================================================================

/// Callback-scoped view of an undo item's metadata. In an on_push listener it is writable
/// (set_value); in an on_pop listener it is read-only (value).
class UndoMeta {
public:
    explicit UndoMeta(LoroUndoMeta* raw) noexcept : mut_(raw), const_(raw) {}
    explicit UndoMeta(const LoroUndoMeta* raw) noexcept : mut_(nullptr), const_(raw) {}

    /// Attaches a JSON metadata value to the item (on_push only).
    void set_value(std::string_view json) {
        detail::check(loro_undo_meta_set_value_json(mut_, json.data(), json.size()));
    }
    /// The item's metadata value as a JSON string.
    std::string value() const {
        detail::Bytes b;
        detail::check(loro_undo_meta_get_value_json(const_, b.out()));
        return b.to_string();
    }

private:
    LoroUndoMeta* mut_;
    const LoroUndoMeta* const_;
};

/// on_push listener: (stack kind, change span, originating event-or-null, writable meta).
using UndoOnPushFn = std::function<void(UndoOrRedo, CounterSpan, const DiffEvent*, UndoMeta&)>;
/// on_pop listener: (stack kind, change span, read-only meta).
using UndoOnPopFn = std::function<void(UndoOrRedo, CounterSpan, const UndoMeta&)>;

// ===========================================================================
// M4: jsonpath results
// ===========================================================================

/// RAII wrapper around a `LoroJsonPathResults*` collection. Move-only.
class JsonPathResults {
public:
    explicit JsonPathResults(LoroJsonPathResults* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    LoroJsonPathResults* raw() const noexcept { return handle_.get(); }

    std::size_t size() const { return loro_jsonpath_results_len(handle_.get()); }

    /// Whether the result at `index` is a container (vs. a plain value).
    bool is_container(std::size_t index) const {
        bool out = false;
        detail::check(loro_jsonpath_results_is_container(handle_.get(), index, &out));
        return out;
    }

    /// The result at `index` as JSON (a container renders as its deep value).
    std::string value_json(std::size_t index) const {
        detail::Bytes b;
        detail::check(loro_jsonpath_results_get_value_json(handle_.get(), index, b.out()));
        return b.to_string();
    }

    /// The result at `index` as a container, or std::nullopt if it is a plain value.
    std::optional<Container> container(std::size_t index) const {
        LoroContainer* c = loro_jsonpath_results_get_container(handle_.get(), index);
        if (!c) return std::nullopt;
        return Container(c);
    }

private:
    struct Deleter {
        void operator()(LoroJsonPathResults* p) const noexcept {
            loro_jsonpath_results_free(p);
        }
    };
    std::unique_ptr<LoroJsonPathResults, Deleter> handle_;
};

// ===========================================================================
// G5: value navigation (get_by_path / get_by_str_path)

/// One step of a structured path for Doc::get_by_path. Build with the static makers. Owns its
/// map key, so the LoroPathComponent it lowers to (via lower()) borrows a pointer that stays
/// valid as long as this object lives.
class PathComponent {
public:
    /// A map key step.
    static PathComponent key(std::string k) {
        PathComponent p;
        p.kind_ = LORO_PATH_KEY;
        p.key_ = std::move(k);
        return p;
    }
    /// A list / movable-list index step.
    static PathComponent seq(std::size_t index) {
        PathComponent p;
        p.kind_ = LORO_PATH_SEQ;
        p.seq_ = index;
        return p;
    }
    /// A tree node-id step.
    static PathComponent node(TreeId id) {
        PathComponent p;
        p.kind_ = LORO_PATH_NODE;
        p.node_ = id;
        return p;
    }

    /// Lowers to the C struct, borrowing this object's key string.
    LoroPathComponent lower() const {
        LoroPathComponent c{};
        c.kind = kind_;
        c.key = key_.data();
        c.key_len = key_.size();
        c.seq = seq_;
        c.node = node_;
        return c;
    }

private:
    LoroPathComponentKind kind_ = LORO_PATH_KEY;
    std::string key_;
    std::size_t seq_ = 0;
    TreeId node_{};
};

/// RAII wrapper around a `LoroValueOrContainer*` — the result of Doc::get_by_path /
/// get_by_str_path. Either a plain value (read with value_json()) or a live container (recovered
/// with container()). Move-only. Like DiffBatch, it can hand back a live nested container, which
/// a flat JSON dump cannot represent.
class ValueOrContainer {
public:
    explicit ValueOrContainer(LoroValueOrContainer* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    LoroValueOrContainer* raw() const noexcept { return handle_.get(); }

    /// Whether the result is a live container (vs. a plain value).
    bool is_container() const { return loro_value_or_container_is_container(handle_.get()); }

    /// The container kind; meaningful only when is_container() is true.
    ContainerType container_type() const {
        return loro_value_or_container_container_type(handle_.get());
    }

    /// The result as a live container, or std::nullopt if it is a plain value.
    std::optional<Container> container() const {
        LoroContainer* c = loro_value_or_container_get_container(handle_.get());
        if (!c) return std::nullopt;
        return Container(c);
    }

    /// The result as JSON (a container renders as its deep value).
    std::string value_json() const {
        detail::Bytes b;
        detail::check(loro_value_or_container_get_value_json(handle_.get(), b.out()));
        return b.to_string();
    }

private:
    struct Deleter {
        void operator()(LoroValueOrContainer* p) const noexcept {
            loro_value_or_container_free(p);
        }
    };
    std::unique_ptr<LoroValueOrContainer, Deleter> handle_;
};

// ===========================================================================
// M4: commit hooks
// ===========================================================================

/// Non-owning, **callback-scoped** view of a pre-commit payload. Valid only for the duration
/// of the pre-commit callback.
class PreCommitPayload {
public:
    explicit PreCommitPayload(const LoroPreCommitPayload* raw) noexcept : raw_(raw) {}

    const LoroPreCommitPayload* raw() const noexcept { return raw_; }

    /// The change's metadata (callback-scoped).
    ChangeMeta change_meta() const {
        return ChangeMeta(loro_pre_commit_payload_change_meta(raw_));
    }
    /// The commit's origin string (possibly empty).
    std::string origin() const {
        detail::Bytes b;
        detail::check(loro_pre_commit_payload_origin(raw_, b.out()));
        return b.to_string();
    }
    /// Rewrites the message for the commit being processed.
    void set_message(std::string_view msg) const {
        detail::check(loro_pre_commit_payload_set_message(raw_, msg.data(), msg.size()));
    }
    /// Rewrites the timestamp (seconds since the Unix epoch) for the commit.
    void set_timestamp(std::int64_t timestamp) const {
        detail::check(loro_pre_commit_payload_set_timestamp(raw_, timestamp));
    }

private:
    const LoroPreCommitPayload* raw_;
};

/// A pre-commit hook: returns true to stay subscribed, false to auto-unsubscribe.
using PreCommitFn = std::function<bool(const PreCommitPayload&)>;
/// A first-commit-from-peer hook (receives the peer id): returns true to stay subscribed.
using FirstCommitFromPeerFn = std::function<bool(std::uint64_t)>;
/// A change-ancestor traveler: returns true to continue, false to stop the traversal.
using ChangeAncestorsFn = std::function<bool(const ChangeMeta&)>;
/// A JSONPath notification: a payload-free signal that the path's matches may have changed.
using JsonPathNotifyFn = std::function<void()>;

namespace detail {

// `extern "C"` trampolines bridging the C callback triples to the stored std::functions.
extern "C" inline void loro_hpp_ephemeral_invoke(const LoroEphemeralStoreEvent* ev, void* ud) {
    EphemeralStoreEvent e(ev);
    (*static_cast<EphemeralSubscriberFn*>(ud))(e);
}
extern "C" inline void loro_hpp_ephemeral_free(void* ud) {
    delete static_cast<EphemeralSubscriberFn*>(ud);
}

extern "C" inline void loro_hpp_undo_on_push(LoroUndoOrRedo kind, LoroCounterSpan span,
                                             const LoroDiffEvent* ev, LoroUndoMeta* meta,
                                             void* ud) {
    UndoMeta m(meta);
    if (ev) {
        DiffEvent d(ev);
        (*static_cast<UndoOnPushFn*>(ud))(kind, span, &d, m);
    } else {
        (*static_cast<UndoOnPushFn*>(ud))(kind, span, nullptr, m);
    }
}
extern "C" inline void loro_hpp_undo_on_push_free(void* ud) {
    delete static_cast<UndoOnPushFn*>(ud);
}
extern "C" inline void loro_hpp_undo_on_pop(LoroUndoOrRedo kind, LoroCounterSpan span,
                                            const LoroUndoMeta* meta, void* ud) {
    UndoMeta m(meta);
    (*static_cast<UndoOnPopFn*>(ud))(kind, span, m);
}
extern "C" inline void loro_hpp_undo_on_pop_free(void* ud) {
    delete static_cast<UndoOnPopFn*>(ud);
}

extern "C" inline bool loro_hpp_pre_commit_invoke(const LoroPreCommitPayload* p, void* ud) {
    PreCommitPayload payload(p);
    return (*static_cast<PreCommitFn*>(ud))(payload);
}
extern "C" inline void loro_hpp_pre_commit_free(void* ud) {
    delete static_cast<PreCommitFn*>(ud);
}
extern "C" inline bool loro_hpp_first_commit_invoke(std::uint64_t peer, void* ud) {
    return (*static_cast<FirstCommitFromPeerFn*>(ud))(peer);
}
extern "C" inline void loro_hpp_first_commit_free(void* ud) {
    delete static_cast<FirstCommitFromPeerFn*>(ud);
}

extern "C" inline bool loro_hpp_travel_invoke(const LoroChangeMeta* m, void* ud) {
    ChangeMeta cm(m);
    return (*static_cast<ChangeAncestorsFn*>(ud))(cm);
}
extern "C" inline void loro_hpp_travel_free(void* ud) {
    delete static_cast<ChangeAncestorsFn*>(ud);
}

extern "C" inline void loro_hpp_jsonpath_invoke(void* ud) {
    (*static_cast<JsonPathNotifyFn*>(ud))();
}
extern "C" inline void loro_hpp_jsonpath_free(void* ud) {
    delete static_cast<JsonPathNotifyFn*>(ud);
}

}  // namespace detail

/// RAII wrapper around a `LoroEphemeralStore*` (keyed last-write-wins presence state).
/// Move-only.
class EphemeralStore {
public:
    explicit EphemeralStore(LoroEphemeralStore* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    /// Creates an ephemeral store with an inactivity `timeout` (milliseconds).
    explicit EphemeralStore(std::int64_t timeout)
        : handle_(loro_ephemeral_store_new(timeout)) {
        if (!handle_) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    LoroEphemeralStore* raw() const noexcept { return handle_.get(); }

    /// Sets `key` to the JSON value string `json`.
    void set(std::string_view key, std::string_view json) {
        detail::check(loro_ephemeral_store_set(handle_.get(), key.data(), key.size(),
                                               json.data(), json.size()));
    }
    /// The value at `key` as a JSON string, or std::nullopt if absent/expired.
    std::optional<std::string> get(std::string_view key) const {
        detail::Bytes b;
        if (!detail::check_found(
                loro_ephemeral_store_get(handle_.get(), key.data(), key.size(), b.out())))
            return std::nullopt;
        return b.to_string();
    }
    /// Deletes `key`.
    void remove(std::string_view key) {
        detail::check(loro_ephemeral_store_delete(handle_.get(), key.data(), key.size()));
    }

    std::vector<std::uint8_t> encode(std::string_view key) const {
        detail::Bytes b;
        detail::check(
            loro_ephemeral_store_encode(handle_.get(), key.data(), key.size(), b.out()));
        return b.to_vector();
    }
    std::vector<std::uint8_t> encode_all() const {
        detail::Bytes b;
        detail::check(loro_ephemeral_store_encode_all(handle_.get(), b.out()));
        return b.to_vector();
    }
    void apply(const std::uint8_t* data, std::size_t len) {
        detail::check(loro_ephemeral_store_apply(handle_.get(), data, len));
    }
    void apply(const std::vector<std::uint8_t>& data) { apply(data.data(), data.size()); }

    void remove_outdated() {
        detail::check(loro_ephemeral_store_remove_outdated(handle_.get()));
    }
    /// The store's keys as a JSON array string.
    std::string keys() const {
        detail::Bytes b;
        detail::check(loro_ephemeral_store_keys(handle_.get(), b.out()));
        return b.to_string();
    }
    /// All entries as a JSON object string.
    std::string get_all_states() const {
        detail::Bytes b;
        detail::check(loro_ephemeral_store_get_all_states(handle_.get(), b.out()));
        return b.to_string();
    }

    /// Subscribes to change events. Destroy the returned Subscription to unsubscribe.
    Subscription subscribe(EphemeralSubscriberFn cb) {
        auto* fn = new EphemeralSubscriberFn(std::move(cb));
        LoroEphemeralSubscriber c{detail::loro_hpp_ephemeral_invoke, fn,
                                  detail::loro_hpp_ephemeral_free};
        LoroSubscription* s = loro_ephemeral_store_subscribe(handle_.get(), c);
        if (!s) {
            delete fn;
            throw Error(LORO_ERR_OTHER, detail::last_error_message());
        }
        return Subscription(s);
    }

    /// Subscribes to local updates (encoded bytes to broadcast). The callback returns true to
    /// stay subscribed (false auto-unsubscribes). Destroy the Subscription to unsubscribe.
    Subscription subscribe_local_updates(LocalUpdateFn cb) {
        auto* fn = new LocalUpdateFn(std::move(cb));
        LoroLocalUpdateCallback c{detail::loro_hpp_local_update_invoke, fn,
                                  detail::loro_hpp_local_update_free};
        LoroSubscription* s = loro_ephemeral_store_subscribe_local_updates(handle_.get(), c);
        if (!s) {
            delete fn;
            throw Error(LORO_ERR_OTHER, detail::last_error_message());
        }
        return Subscription(s);
    }

private:
    struct Deleter {
        void operator()(LoroEphemeralStore* p) const noexcept {
            loro_ephemeral_store_free(p);
        }
    };
    std::unique_ptr<LoroEphemeralStore, Deleter> handle_;
};

/// RAII wrapper around a `LoroUndoManager*` (local undo/redo). Move-only. Create one with
/// Doc::undo_manager().
class UndoManager {
public:
    explicit UndoManager(LoroUndoManager* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    LoroUndoManager* raw() const noexcept { return handle_.get(); }

    /// Undoes the last recorded change; returns whether an undo actually happened.
    bool undo() {
        bool applied = false;
        detail::check(loro_undo_manager_undo(handle_.get(), &applied));
        return applied;
    }
    /// Redoes the last undone change; returns whether a redo actually happened.
    bool redo() {
        bool applied = false;
        detail::check(loro_undo_manager_redo(handle_.get(), &applied));
        return applied;
    }

    bool can_undo() const { return loro_undo_manager_can_undo(handle_.get()); }
    bool can_redo() const { return loro_undo_manager_can_redo(handle_.get()); }
    std::size_t undo_count() const { return loro_undo_manager_undo_count(handle_.get()); }
    std::size_t redo_count() const { return loro_undo_manager_redo_count(handle_.get()); }

    void record_new_checkpoint() {
        detail::check(loro_undo_manager_record_new_checkpoint(handle_.get()));
    }
    void set_merge_interval(std::int64_t interval_ms) {
        detail::check(loro_undo_manager_set_merge_interval(handle_.get(), interval_ms));
    }
    void set_max_undo_steps(std::size_t steps) {
        detail::check(loro_undo_manager_set_max_undo_steps(handle_.get(), steps));
    }
    void add_exclude_origin_prefix(std::string_view prefix) {
        detail::check(loro_undo_manager_add_exclude_origin_prefix(handle_.get(), prefix.data(),
                                                                  prefix.size()));
    }
    void clear() { detail::check(loro_undo_manager_clear(handle_.get())); }
    void group_start() { detail::check(loro_undo_manager_group_start(handle_.get())); }
    void group_end() { detail::check(loro_undo_manager_group_end(handle_.get())); }

    /// Installs (or replaces) the on_push listener. The previous listener (and its captured
    /// state) is released when replaced or when the manager is destroyed.
    void set_on_push(UndoOnPushFn cb) {
        auto* fn = new UndoOnPushFn(std::move(cb));
        LoroUndoOnPush c{detail::loro_hpp_undo_on_push, fn, detail::loro_hpp_undo_on_push_free};
        detail::check(loro_undo_manager_set_on_push(handle_.get(), c));
    }
    /// Installs (or replaces) the on_pop listener.
    void set_on_pop(UndoOnPopFn cb) {
        auto* fn = new UndoOnPopFn(std::move(cb));
        LoroUndoOnPop c{detail::loro_hpp_undo_on_pop, fn, detail::loro_hpp_undo_on_pop_free};
        detail::check(loro_undo_manager_set_on_pop(handle_.get(), c));
    }

private:
    struct Deleter {
        void operator()(LoroUndoManager* p) const noexcept { loro_undo_manager_free(p); }
    };
    std::unique_ptr<LoroUndoManager, Deleter> handle_;
};

/// RAII builder mapping text-style keys to their expand behaviour. Pass to
/// `Doc::config_text_style`. Move-only.
class StyleConfigMap {
public:
    StyleConfigMap() : handle_(loro_style_config_map_new()) {
        if (!handle_) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    /// The underlying C handle (non-owning).
    LoroStyleConfigMap* raw() const noexcept { return handle_.get(); }

    /// Sets the expand behaviour for marks created with style `key` (must not contain `:`).
    void insert(std::string_view key, ExpandType expand) {
        LoroStyleConfig config{expand};
        detail::check(
            loro_style_config_map_insert(handle_.get(), key.data(), key.size(), config));
    }

private:
    struct Deleter {
        void operator()(LoroStyleConfigMap* p) const noexcept { loro_style_config_map_free(p); }
    };
    std::unique_ptr<LoroStyleConfigMap, Deleter> handle_;
};

/// RAII wrapper around a `LoroDiffBatch*` — a collection of per-container diffs produced by
/// Doc::diff and replayed with Doc::apply_diff. Move-only. Unlike a JSON dump, it losslessly
/// carries live nested containers; use to_json() only for inspection.
class DiffBatch {
public:
    explicit DiffBatch(LoroDiffBatch* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    LoroDiffBatch* raw() const noexcept { return handle_.get(); }

    /// Renders the batch as a JSON string (an object keyed by container-id), for inspection.
    std::string to_json() const {
        detail::Bytes b;
        detail::check(loro_diff_batch_to_json(handle_.get(), b.out()));
        return b.to_string();
    }

private:
    struct Deleter {
        void operator()(LoroDiffBatch* p) const noexcept { loro_diff_batch_free(p); }
    };
    std::unique_ptr<LoroDiffBatch, Deleter> handle_;
};

/// RAII wrapper around a `LoroConfigure*` — a document's live configuration (whether commits
/// record a wall-clock timestamp, and the change-merge interval). Obtained from `Doc::config`.
/// Move-only. The handle shares state with the document it came from: changes made through it
/// affect the document, and vice-versa.
class Configure {
public:
    explicit Configure(LoroConfigure* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    LoroConfigure* raw() const noexcept { return handle_.get(); }

    /// Whether commits record a wall-clock timestamp.
    bool record_timestamp() const { return loro_configure_record_timestamp(handle_.get()); }

    /// Sets whether commits record a wall-clock timestamp.
    void set_record_timestamp(bool record) {
        loro_configure_set_record_timestamp(handle_.get(), record);
    }

    /// The change-merge interval, in seconds.
    std::int64_t merge_interval() const { return loro_configure_merge_interval(handle_.get()); }

    /// Sets the change-merge interval, in seconds.
    void set_merge_interval(std::int64_t interval) {
        loro_configure_set_merge_interval(handle_.get(), interval);
    }

private:
    struct Deleter {
        void operator()(LoroConfigure* p) const noexcept { loro_configure_free(p); }
    };
    std::unique_ptr<LoroConfigure, Deleter> handle_;
};

/// Builder for the options passed to `Doc::commit_with` / `Doc::set_next_commit_options`. Each
/// field is optional; unset fields fall back to loro's defaults (current time for the timestamp,
/// immediate-renew on). `origin` is reported to subscribers but not persisted; `message` persists.
class CommitOptions {
public:
    CommitOptions& origin(std::string_view o) {
        origin_ = std::string(o);
        has_origin_ = true;
        return *this;
    }
    CommitOptions& message(std::string_view m) {
        message_ = std::string(m);
        has_message_ = true;
        return *this;
    }
    CommitOptions& timestamp(std::int64_t ts) {
        timestamp_ = ts;
        has_timestamp_ = true;
        return *this;
    }
    CommitOptions& immediate_renew(bool renew) {
        immediate_renew_ = renew;
        return *this;
    }

    /// Builds the C POD. The returned struct borrows this object's string storage, so keep the
    /// CommitOptions alive until the FFI call returns.
    LoroCommitOptions to_c() const {
        LoroCommitOptions o{};
        o.origin = has_origin_ ? origin_.data() : nullptr;
        o.origin_len = has_origin_ ? origin_.size() : 0;
        o.message = has_message_ ? message_.data() : nullptr;
        o.message_len = has_message_ ? message_.size() : 0;
        o.timestamp = timestamp_;
        o.has_timestamp = has_timestamp_;
        o.immediate_renew = immediate_renew_;
        return o;
    }

private:
    std::string origin_;
    std::string message_;
    std::int64_t timestamp_ = 0;
    bool has_origin_ = false;
    bool has_message_ = false;
    bool has_timestamp_ = false;
    bool immediate_renew_ = true;
};

/// RAII wrapper around a `LoroDoc*`. Move-only.
class Doc {
public:
    /// Creates a new, empty document.
    Doc() : handle_(loro_doc_new()) {
        if (!handle_) throw Error(LORO_ERR_OTHER, "loro_doc_new returned null");
    }

    /// Adopts an existing raw handle (takes ownership).
    explicit Doc(LoroDoc* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    /// The underlying C handle (non-owning).
    LoroDoc* raw() const noexcept { return handle_.get(); }

    /// Returns the root text container `id`, creating it if absent.
    Text get_text(std::string_view id) {
        return Text(loro_doc_get_text(handle_.get(), id.data(), id.size()));
    }

    /// Returns the root map container `id`, creating it if absent.
    Map get_map(std::string_view id) {
        return Map(loro_doc_get_map(handle_.get(), id.data(), id.size()));
    }

    /// Returns the root list container `id`, creating it if absent.
    List get_list(std::string_view id) {
        return List(loro_doc_get_list(handle_.get(), id.data(), id.size()));
    }

    /// Returns the root movable-list container `id`, creating it if absent.
    MovableList get_movable_list(std::string_view id) {
        return MovableList(loro_doc_get_movable_list(handle_.get(), id.data(), id.size()));
    }

    /// Returns the root tree container `id`, creating it if absent.
    Tree get_tree(std::string_view id) {
        return Tree(loro_doc_get_tree(handle_.get(), id.data(), id.size()));
    }

    /// Returns the root counter container `id`, creating it if absent.
    Counter get_counter(std::string_view id) {
        return Counter(loro_doc_get_counter(handle_.get(), id.data(), id.size()));
    }

    /// Applies a text-style configuration (controls mark expand semantics). The map is
    /// copied; the caller still owns it.
    void config_text_style(const StyleConfigMap& styles) {
        detail::check(loro_doc_config_text_style(handle_.get(), styles.raw()));
    }

    /// Sets the default text style used for any key without an explicit entry.
    void config_default_text_style(ExpandType expand) {
        LoroStyleConfig config{expand};
        detail::check(loro_doc_config_default_text_style(handle_.get(), &config));
    }

    /// Resets the default text style.
    void reset_default_text_style() {
        detail::check(loro_doc_config_default_text_style(handle_.get(), nullptr));
    }

    /// Returns a handle to this document's live configuration. Mutations through it affect
    /// the document (and vice-versa); see Configure.
    Configure config() const {
        LoroConfigure* raw = loro_doc_config(handle_.get());
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
        return Configure(raw);
    }

    /// Shortcut for `config().set_record_timestamp(record)`.
    void set_record_timestamp(bool record) {
        loro_doc_set_record_timestamp(handle_.get(), record);
    }

    /// Shortcut for `config().set_merge_interval(interval)` (interval in seconds).
    void set_change_merge_interval(std::int64_t interval) {
        loro_doc_set_change_merge_interval(handle_.get(), interval);
    }

    // ---- G6.2: history & introspection ----

    /// The total number of operations in the document's OpLog.
    std::size_t len_ops() const { return loro_doc_len_ops(handle_.get()); }

    /// The total number of changes in the document's OpLog.
    std::size_t len_changes() const { return loro_doc_len_changes(handle_.get()); }

    /// The number of operations in the pending (uncommitted) transaction.
    std::size_t pending_txn_len() const { return loro_doc_get_pending_txn_len(handle_.get()); }

    /// Whether the history cache (used to speed up checkout) is currently built.
    bool has_history_cache() const { return loro_doc_has_history_cache(handle_.get()); }

    /// Frees the history cache (rebuilt automatically on demand).
    void free_history_cache() { detail::check(loro_doc_free_history_cache(handle_.get())); }

    /// Frees the cached diff calculator (rebuilt automatically on demand).
    void free_diff_calculator() { detail::check(loro_doc_free_diff_calculator(handle_.get())); }

    /// Encodes all ops and the history cache into the kv store, freeing parsed-op memory.
    void compact_change_store() { detail::check(loro_doc_compact_change_store(handle_.get())); }

    /// Sets whether empty root containers are hidden from deep values and snapshots.
    void set_hide_empty_root_containers(bool hide) {
        detail::check(loro_doc_set_hide_empty_root_containers(handle_.get(), hide));
    }

    /// Whether the document contains the container with id `cid` (a container-id string).
    bool has_container(std::string_view cid) const {
        return loro_doc_has_container(handle_.get(), cid.data(), cid.size());
    }

    /// Deletes all content from the root container `cid` and hides it. Only affects root
    /// containers (those without a parent).
    void delete_root_container(std::string_view cid) {
        detail::check(loro_doc_delete_root_container(handle_.get(), cid.data(), cid.size()));
    }

    /// The path from the document root to container `cid`, as a JSON array of
    /// `{"cid":…,"index":…}` steps. Throws LORO_ERR_NOT_FOUND if `cid` does not resolve.
    std::string get_path_to_container(std::string_view cid) const {
        detail::Bytes b;
        detail::check(
            loro_doc_get_path_to_container(handle_.get(), cid.data(), cid.size(), b.out()));
        return b.to_string();
    }

    /// The container ids modified in the change range `[id, id+len)`, as a sorted JSON array of
    /// container-id strings. Implicitly commits the current transaction.
    std::string get_changed_containers_in(Id id, std::size_t len) const {
        detail::Bytes b;
        detail::check(loro_doc_get_changed_containers_in(handle_.get(), id, len, b.out()));
        return b.to_string();
    }

    /// Compares `frontiers` with the document's current version: -1 (doc behind / `frontiers`
    /// not fully contained), 0 (equal), or 1 (doc ahead).
    int cmp_with_frontiers(const Frontiers& frontiers) const {
        std::int32_t out = 0;
        detail::check(loro_doc_cmp_with_frontiers(handle_.get(), frontiers.raw(), &out));
        return out;
    }

    /// A minimized equivalent of `frontiers` (the smallest set marking the same version), or
    /// std::nullopt if a frontier id is not in this document's history.
    std::optional<Frontiers> minimize_frontiers(const Frontiers& frontiers) const {
        LoroFrontiers* f = loro_doc_minimize_frontiers(handle_.get(), frontiers.raw());
        if (!f) return std::nullopt;
        return Frontiers(f);
    }

    /// A fork of the document containing only history up to `frontiers`, or std::nullopt on
    /// error (e.g. an unknown frontier).
    std::optional<Doc> fork_at(const Frontiers& frontiers) const {
        LoroDoc* d = loro_doc_fork_at(handle_.get(), frontiers.raw());
        if (!d) return std::nullopt;
        return Doc(d);
    }

    /// Looks up an existing text container by container-id string (std::nullopt if absent).
    std::optional<Text> try_get_text(std::string_view cid) const {
        LoroText* t = loro_doc_try_get_text(handle_.get(), cid.data(), cid.size());
        if (!t) return std::nullopt;
        return Text(t);
    }
    /// Looks up an existing map container by container-id string (std::nullopt if absent).
    std::optional<Map> try_get_map(std::string_view cid) const {
        LoroMap* m = loro_doc_try_get_map(handle_.get(), cid.data(), cid.size());
        if (!m) return std::nullopt;
        return Map(m);
    }
    /// Looks up an existing list container by container-id string (std::nullopt if absent).
    std::optional<List> try_get_list(std::string_view cid) const {
        LoroList* l = loro_doc_try_get_list(handle_.get(), cid.data(), cid.size());
        if (!l) return std::nullopt;
        return List(l);
    }
    /// Looks up an existing movable-list container by container-id string (std::nullopt if absent).
    std::optional<MovableList> try_get_movable_list(std::string_view cid) const {
        LoroMovableList* l = loro_doc_try_get_movable_list(handle_.get(), cid.data(), cid.size());
        if (!l) return std::nullopt;
        return MovableList(l);
    }
    /// Looks up an existing tree container by container-id string (std::nullopt if absent).
    std::optional<Tree> try_get_tree(std::string_view cid) const {
        LoroTree* t = loro_doc_try_get_tree(handle_.get(), cid.data(), cid.size());
        if (!t) return std::nullopt;
        return Tree(t);
    }
    /// Looks up an existing counter container by container-id string (std::nullopt if absent).
    std::optional<Counter> try_get_counter(std::string_view cid) const {
        LoroCounter* c = loro_doc_try_get_counter(handle_.get(), cid.data(), cid.size());
        if (!c) return std::nullopt;
        return Counter(c);
    }

    /// The change containing operation `id`, or std::nullopt if no change contains it.
    std::optional<ChangeMetaOwned> get_change(Id id) const {
        LoroChangeMetaOwned* m = nullptr;
        if (loro_doc_get_change(handle_.get(), id, &m) != LORO_OK) return std::nullopt;
        return ChangeMetaOwned(m);
    }

    /// Commits pending operations into the oplog.
    void commit() { detail::check(loro_doc_commit(handle_.get())); }

    /// The document's current peer id.
    std::uint64_t peer_id() const { return loro_doc_peer_id(handle_.get()); }

    /// Sets the document's peer id.
    void set_peer_id(std::uint64_t peer) {
        detail::check(loro_doc_set_peer_id(handle_.get(), peer));
    }

    /// Exports a full snapshot (history + state).
    std::vector<std::uint8_t> export_snapshot() const {
        detail::Bytes b;
        detail::check(loro_doc_export_snapshot(handle_.get(), b.out()));
        return b.to_vector();
    }

    /// Exports all updates (operation history only).
    std::vector<std::uint8_t> export_updates() const {
        detail::Bytes b;
        detail::check(loro_doc_export_updates(handle_.get(), b.out()));
        return b.to_vector();
    }

    /// Imports a snapshot or updates blob, merging it into this document.
    void import(const std::uint8_t* data, std::size_t len) {
        detail::check(loro_doc_import(handle_.get(), data, len));
    }

    /// Imports a snapshot or updates blob, merging it into this document.
    void import(const std::vector<std::uint8_t>& data) {
        import(data.data(), data.size());
    }

    /// The whole document state serialized to a JSON string.
    std::string to_json() const {
        detail::Bytes b;
        detail::check(loro_doc_get_deep_value_json(handle_.get(), b.out()));
        return b.to_string();
    }

    /// A deep fork of the document at its current version.
    Doc fork() const { return Doc(loro_doc_fork(handle_.get())); }

    /// Subscribes to changes of the container with id `cid` (e.g. `text.id()`). The callback
    /// fires after each relevant commit/import with a callback-scoped DiffEvent. Destroy the
    /// returned Subscription to unsubscribe. The callback must be reentrant (it may fire from
    /// any thread that mutates the document).
    Subscription subscribe(std::string_view cid, SubscriberFn cb) {
        auto* fn = new SubscriberFn(std::move(cb));
        LoroSubscriber c{detail::loro_hpp_subscriber_invoke, fn, detail::loro_hpp_subscriber_free};
        LoroSubscription* s = loro_doc_subscribe(handle_.get(), cid.data(), cid.size(), c);
        if (!s) {
            delete fn;
            throw Error(LORO_ERR_OTHER, detail::last_error_message());
        }
        return Subscription(s);
    }

    /// Subscribes to all changes of the whole document. Destroy the returned Subscription to
    /// unsubscribe.
    Subscription subscribe_root(SubscriberFn cb) {
        auto* fn = new SubscriberFn(std::move(cb));
        LoroSubscriber c{detail::loro_hpp_subscriber_invoke, fn, detail::loro_hpp_subscriber_free};
        LoroSubscription* s = loro_doc_subscribe_root(handle_.get(), c);
        if (!s) {
            delete fn;
            throw Error(LORO_ERR_OTHER, detail::last_error_message());
        }
        return Subscription(s);
    }

    /// Subscribes to the raw update bytes of each local commit. The callback returns true to
    /// stay subscribed (false auto-unsubscribes). Destroy the returned Subscription to
    /// unsubscribe.
    Subscription subscribe_local_update(LocalUpdateFn cb) {
        auto* fn = new LocalUpdateFn(std::move(cb));
        LoroLocalUpdateCallback c{detail::loro_hpp_local_update_invoke, fn,
                                  detail::loro_hpp_local_update_free};
        LoroSubscription* s = loro_doc_subscribe_local_update(handle_.get(), c);
        if (!s) {
            delete fn;
            throw Error(LORO_ERR_OTHER, detail::last_error_message());
        }
        return Subscription(s);
    }

    // ---- M4: versions & time travel ----

    /// The oplog version vector (everything in the document's history).
    VersionVector oplog_vv() const {
        LoroVersionVector* v = loro_doc_oplog_vv(handle_.get());
        if (!v) throw Error(LORO_ERR_OTHER, detail::last_error_message());
        return VersionVector(v);
    }
    /// The current state version vector.
    VersionVector state_vv() const {
        LoroVersionVector* v = loro_doc_state_vv(handle_.get());
        if (!v) throw Error(LORO_ERR_OTHER, detail::last_error_message());
        return VersionVector(v);
    }
    /// The oplog frontiers.
    Frontiers oplog_frontiers() const {
        LoroFrontiers* f = loro_doc_oplog_frontiers(handle_.get());
        if (!f) throw Error(LORO_ERR_OTHER, detail::last_error_message());
        return Frontiers(f);
    }
    /// The current state frontiers.
    Frontiers state_frontiers() const {
        LoroFrontiers* f = loro_doc_state_frontiers(handle_.get());
        if (!f) throw Error(LORO_ERR_OTHER, detail::last_error_message());
        return Frontiers(f);
    }
    /// Converts frontiers to a version vector against this document. Throws if the frontiers
    /// are not contained in the document's history.
    VersionVector frontiers_to_vv(const Frontiers& f) const {
        LoroVersionVector* v = loro_doc_frontiers_to_vv(handle_.get(), f.raw());
        if (!v) throw Error(LORO_ERR_NOT_FOUND, detail::last_error_message());
        return VersionVector(v);
    }
    /// Converts a version vector to frontiers against this document.
    Frontiers vv_to_frontiers(const VersionVector& vv) const {
        LoroFrontiers* f = loro_doc_vv_to_frontiers(handle_.get(), vv.raw());
        if (!f) throw Error(LORO_ERR_OTHER, detail::last_error_message());
        return Frontiers(f);
    }
    /// Time-travels the document state to `frontiers` (detaching from the latest version).
    void checkout(const Frontiers& frontiers) {
        detail::check(loro_doc_checkout(handle_.get(), frontiers.raw()));
    }
    /// Re-attaches the document state to the latest version.
    void checkout_to_latest() { detail::check(loro_doc_checkout_to_latest(handle_.get())); }
    /// Whether the document is detached (checked out to a non-latest version).
    bool is_detached() const { return loro_doc_is_detached(handle_.get()); }

    /// Computes the diff that turns the state at `from` into the state at `to`. The returned
    /// batch can be replayed onto another document with apply_diff(). Throws LORO_ERR_NOT_FOUND
    /// if either frontiers references a version this document does not contain.
    DiffBatch diff(const Frontiers& from, const Frontiers& to) const {
        LoroDiffBatch* raw = nullptr;
        detail::check(loro_doc_diff(handle_.get(), from.raw(), to.raw(), &raw));
        return DiffBatch(raw);
    }
    /// Applies `batch` to this document, mutating its state. The batch is not consumed.
    void apply_diff(const DiffBatch& batch) {
        detail::check(loro_doc_apply_diff(handle_.get(), batch.raw()));
    }
    /// Reverts this document's state back to `frontiers`, recording the inverse as new ops (so,
    /// unlike checkout(), the document stays attached and the rewind is part of history).
    void revert_to(const Frontiers& frontiers) {
        detail::check(loro_doc_revert_to(handle_.get(), frontiers.raw()));
    }

    /// Exports only the updates after `from` (the delta to bring a peer at `from` up to date).
    std::vector<std::uint8_t> export_updates_from(const VersionVector& from) const {
        detail::Bytes b;
        detail::check(loro_doc_export_updates_from(handle_.get(), from.raw(), b.out()));
        return b.to_vector();
    }

    // ---- G3: JSON-update sync + export modes ----

    /// Imports JSON-format updates (loro's update schema) into this document. Use to interop
    /// with peers that exchange the human-readable JSON format rather than the binary one.
    void import_json_updates(std::string_view json) {
        detail::check(loro_doc_import_json_updates(handle_.get(), json.data(), json.size()));
    }

    /// Exports the ops in the range `(from, to]` as a JSON string. Pass an empty `from`
    /// (VersionVector::create()) to export the full history.
    std::string export_json_updates(const VersionVector& from, const VersionVector& to) const {
        detail::Bytes b;
        detail::check(
            loro_doc_export_json_updates(handle_.get(), from.raw(), to.raw(), b.out()));
        return b.to_string();
    }

    /// Like export_json_updates but without peer-id compression (full peer ids in the JSON).
    std::string export_json_updates_without_peer_compression(const VersionVector& from,
                                                             const VersionVector& to) const {
        detail::Bytes b;
        detail::check(loro_doc_export_json_updates_without_peer_compression(
            handle_.get(), from.raw(), to.raw(), b.out()));
        return b.to_string();
    }

    /// Exports the changes within a single id span as a JSON array string. Deterministic output
    /// (suitable for hashing); may include pending uncommitted changes.
    std::string export_json_in_id_span(const LoroIdSpan& span) const {
        detail::Bytes b;
        detail::check(loro_doc_export_json_in_id_span(handle_.get(), span, b.out()));
        return b.to_string();
    }
    std::string export_json_in_id_span(std::uint64_t peer, std::int32_t counter_start,
                                       std::int32_t counter_end) const {
        return export_json_in_id_span(LoroIdSpan{peer, counter_start, counter_end});
    }

    /// Exports a shallow snapshot whose retained history starts at `frontiers` (older ops
    /// trimmed).
    std::vector<std::uint8_t> export_shallow_snapshot(const Frontiers& frontiers) const {
        detail::Bytes b;
        detail::check(
            loro_doc_export_shallow_snapshot(handle_.get(), frontiers.raw(), b.out()));
        return b.to_vector();
    }

    /// Exports a snapshot at `frontiers` (full history up to that version plus the state there).
    std::vector<std::uint8_t> export_snapshot_at(const Frontiers& frontiers) const {
        detail::Bytes b;
        detail::check(loro_doc_export_snapshot_at(handle_.get(), frontiers.raw(), b.out()));
        return b.to_vector();
    }

    /// Exports a state-only snapshot (state plus a minimal set of history). Pass `nullptr` to
    /// use the latest version.
    std::vector<std::uint8_t> export_state_only(const Frontiers* frontiers = nullptr) const {
        detail::Bytes b;
        detail::check(loro_doc_export_state_only(
            handle_.get(), frontiers ? frontiers->raw() : nullptr, b.out()));
        return b.to_vector();
    }

    /// Exports only the ops in the given id spans.
    std::vector<std::uint8_t> export_updates_in_range(
        const std::vector<LoroIdSpan>& spans) const {
        detail::Bytes b;
        detail::check(loro_doc_export_updates_in_range(handle_.get(), spans.data(),
                                                       spans.size(), b.out()));
        return b.to_vector();
    }

    /// Imports a batch of snapshot/update blobs at once; loro applies them in dependency order
    /// regardless of the order given.
    void import_batch(const std::vector<std::vector<std::uint8_t>>& blobs) {
        std::vector<const std::uint8_t*> ptrs(blobs.size());
        std::vector<std::size_t> lens(blobs.size());
        for (std::size_t i = 0; i < blobs.size(); ++i) {
            ptrs[i] = blobs[i].data();
            lens[i] = blobs[i].size();
        }
        detail::check(
            loro_doc_import_batch(handle_.get(), ptrs.data(), lens.data(), blobs.size()));
    }

    /// Imports a blob while attaching `origin` to the resulting change event (for telemetry /
    /// event filtering).
    void import_with(const std::uint8_t* data, std::size_t len, std::string_view origin) {
        detail::check(
            loro_doc_import_with(handle_.get(), data, len, origin.data(), origin.size()));
    }
    void import_with(const std::vector<std::uint8_t>& data, std::string_view origin) {
        import_with(data.data(), data.size(), origin);
    }

    /// Whether the document is shallow (history trimmed; only retains ops since
    /// shallow_since_vv()).
    bool is_shallow() const { return loro_doc_is_shallow(handle_.get()); }

    /// The start version of a shallow document's retained history (empty when not shallow).
    VersionVector shallow_since_vv() const {
        LoroVersionVector* v = loro_doc_shallow_since_vv(handle_.get());
        if (!v) throw Error(LORO_ERR_OTHER, detail::last_error_message());
        return VersionVector(v);
    }

    /// Traverses the ancestor changes of `ids` (latest to oldest). The callback returns true
    /// to continue or false to stop early.
    void travel_change_ancestors(const std::vector<Id>& ids, ChangeAncestorsFn cb) {
        auto* fn = new ChangeAncestorsFn(std::move(cb));
        LoroChangeAncestorsTraveler t{detail::loro_hpp_travel_invoke, fn,
                                      detail::loro_hpp_travel_free};
        detail::check(
            loro_doc_travel_change_ancestors(handle_.get(), ids.data(), ids.size(), t));
    }

    // ---- G2: cursors ----

    /// Resolves `cursor` against the document's current state, returning its absolute position
    /// and resolved side. Throws loro::Error (LORO_ERR_NOT_FOUND) if the position cannot be
    /// located (the container was deleted, the id is unknown, or history was cleared).
    PosQueryResult get_cursor_pos(const Cursor& cursor) const {
        PosQueryResult out{};
        detail::check(loro_doc_get_cursor_pos(handle_.get(), cursor.raw(), &out));
        return out;
    }

    // ---- M4: undo manager ----

    /// Creates an undo manager bound to this document's current peer.
    UndoManager undo_manager() {
        return UndoManager(loro_undo_manager_new(handle_.get()));
    }

    // ---- M4: jsonpath ----

    /// Runs a JSONPath query against the whole document.
    JsonPathResults jsonpath(std::string_view path) const {
        LoroJsonPathResults* r = loro_doc_jsonpath(handle_.get(), path.data(), path.size());
        if (!r) throw Error(LORO_ERR_INVALID_ARG, detail::last_error_message());
        return JsonPathResults(r);
    }

    /// Subscribes to updates that might affect a JSONPath query (lightweight notification, may
    /// fire false positives). Destroy the returned Subscription to unsubscribe.
    Subscription subscribe_jsonpath(std::string_view path, JsonPathNotifyFn cb) {
        auto* fn = new JsonPathNotifyFn(std::move(cb));
        LoroJsonPathSubscriber c{detail::loro_hpp_jsonpath_invoke, fn,
                                 detail::loro_hpp_jsonpath_free};
        LoroSubscription* s =
            loro_doc_subscribe_jsonpath(handle_.get(), path.data(), path.size(), c);
        if (!s) {
            delete fn;
            throw Error(LORO_ERR_OTHER, detail::last_error_message());
        }
        return Subscription(s);
    }

    // ---- G5: value navigation ----

    /// Resolves a string path (e.g. "map/key", "list/0", "tree/0/prop") to a value or live
    /// container, or std::nullopt if the path does not resolve.
    std::optional<ValueOrContainer> get_by_str_path(std::string_view path) const {
        LoroValueOrContainer* v =
            loro_doc_get_by_str_path(handle_.get(), path.data(), path.size());
        if (!v) return std::nullopt;
        return ValueOrContainer(v);
    }

    /// Resolves a structured path (a sequence of PathComponent steps) to a value or live
    /// container, or std::nullopt if the path does not resolve.
    std::optional<ValueOrContainer> get_by_path(const std::vector<PathComponent>& path) const {
        std::vector<LoroPathComponent> raw;
        raw.reserve(path.size());
        for (const auto& c : path) raw.push_back(c.lower());
        LoroValueOrContainer* v = loro_doc_get_by_path(handle_.get(), raw.data(), raw.size());
        if (!v) return std::nullopt;
        return ValueOrContainer(v);
    }

    // ---- M4: commit hooks ----

    /// Sets the commit message for the next commit (persisted, replicates to peers).
    void set_next_commit_message(std::string_view msg) {
        detail::check(loro_doc_set_next_commit_message(handle_.get(), msg.data(), msg.size()));
    }
    /// Sets the timestamp (seconds since the Unix epoch) for the next commit.
    void set_next_commit_timestamp(std::int64_t timestamp) {
        detail::check(loro_doc_set_next_commit_timestamp(handle_.get(), timestamp));
    }
    /// Sets the origin for the next commit (reported to subscribers; not persisted).
    void set_next_commit_origin(std::string_view origin) {
        detail::check(
            loro_doc_set_next_commit_origin(handle_.get(), origin.data(), origin.size()));
    }
    /// Sets the full options (origin / message / timestamp / immediate_renew) for the next commit.
    void set_next_commit_options(const CommitOptions& opts) {
        LoroCommitOptions c = opts.to_c();
        detail::check(loro_doc_set_next_commit_options(handle_.get(), c));
    }
    /// Clears any options previously set for the next commit.
    void clear_next_commit_options() {
        detail::check(loro_doc_clear_next_commit_options(handle_.get()));
    }
    /// Commits the pending operations using the given options.
    void commit_with(const CommitOptions& opts) {
        LoroCommitOptions c = opts.to_c();
        detail::check(loro_doc_commit_with(handle_.get(), c));
    }

    // ---- G6.3: doc method tail ----

    /// Forces the document into attached mode, re-syncing the state to the latest version.
    void attach() { detail::check(loro_doc_attach(handle_.get())); }
    /// Forces the document into detached mode (imports recorded in the OpLog only until reattach).
    void detach() { detail::check(loro_doc_detach(handle_.get())); }

    /// Looks up any container by container-id string as a type-erased Container (std::nullopt if
    /// absent or `cid` is unparseable).
    std::optional<Container> get_container(std::string_view cid) const {
        LoroContainer* c = loro_doc_get_container(handle_.get(), cid.data(), cid.size());
        if (!c) return std::nullopt;
        return Container(c);
    }

    /// The document's deep value including container ids, as a JSON string.
    std::string deep_value_with_id() const {
        detail::Bytes b;
        detail::check(loro_doc_get_deep_value_with_id_json(handle_.get(), b.out()));
        return b.to_string();
    }

    /// The operation id spans between versions `from` and `to`, as a JSON string
    /// `{"retreat":{…},"forward":{…}}` keyed by peer.
    std::string find_id_spans_between(const Frontiers& from, const Frontiers& to) const {
        detail::Bytes b;
        detail::check(
            loro_doc_find_id_spans_between(handle_.get(), from.raw(), to.raw(), b.out()));
        return b.to_string();
    }

    /// Subscribes to pre-commit events. The callback returns true to stay subscribed (false
    /// auto-unsubscribes). Destroy the returned Subscription to unsubscribe.
    Subscription subscribe_pre_commit(PreCommitFn cb) {
        auto* fn = new PreCommitFn(std::move(cb));
        LoroPreCommitCallback c{detail::loro_hpp_pre_commit_invoke, fn,
                                detail::loro_hpp_pre_commit_free};
        LoroSubscription* s = loro_doc_subscribe_pre_commit(handle_.get(), c);
        if (!s) {
            delete fn;
            throw Error(LORO_ERR_OTHER, detail::last_error_message());
        }
        return Subscription(s);
    }

    /// Subscribes to the first commit from each peer. The callback returns true to stay
    /// subscribed (false auto-unsubscribes). Destroy the returned Subscription to unsubscribe.
    Subscription subscribe_first_commit_from_peer(FirstCommitFromPeerFn cb) {
        auto* fn = new FirstCommitFromPeerFn(std::move(cb));
        LoroFirstCommitFromPeerCallback c{detail::loro_hpp_first_commit_invoke, fn,
                                          detail::loro_hpp_first_commit_free};
        LoroSubscription* s = loro_doc_subscribe_first_commit_from_peer(handle_.get(), c);
        if (!s) {
            delete fn;
            throw Error(LORO_ERR_OTHER, detail::last_error_message());
        }
        return Subscription(s);
    }

private:
    struct Deleter {
        void operator()(LoroDoc* p) const noexcept { loro_doc_free(p); }
    };
    std::unique_ptr<LoroDoc, Deleter> handle_;
};

// --- G6.4: per-container doc() / subscribe() (need the complete Doc & Subscription types) ---

namespace detail {
// Wraps a container subscribe: builds the C callback triple, returns std::nullopt (deleting
// the heap callback) when the container is detached, else an owning Subscription.
template <typename SubFn>
inline std::optional<Subscription> container_subscribe(SubscriberFn cb, SubFn sub_fn) {
    auto* fn = new SubscriberFn(std::move(cb));
    LoroSubscriber c{loro_hpp_subscriber_invoke, fn, loro_hpp_subscriber_free};
    LoroSubscription* s = sub_fn(c);
    if (!s) {
        delete fn;
        return std::nullopt;
    }
    return Subscription(s);
}
}  // namespace detail

inline std::optional<Doc> Text::doc() const {
    LoroDoc* d = loro_text_doc(handle_.get());
    if (!d) return std::nullopt;
    return Doc(d);
}
inline std::optional<Subscription> Text::subscribe(SubscriberFn cb) const {
    return detail::container_subscribe(
        std::move(cb), [this](LoroSubscriber c) { return loro_text_subscribe(handle_.get(), c); });
}

inline std::optional<Doc> Map::doc() const {
    LoroDoc* d = loro_map_doc(handle_.get());
    if (!d) return std::nullopt;
    return Doc(d);
}
inline std::optional<Subscription> Map::subscribe(SubscriberFn cb) const {
    return detail::container_subscribe(
        std::move(cb), [this](LoroSubscriber c) { return loro_map_subscribe(handle_.get(), c); });
}

inline std::optional<Doc> List::doc() const {
    LoroDoc* d = loro_list_doc(handle_.get());
    if (!d) return std::nullopt;
    return Doc(d);
}
inline std::optional<Subscription> List::subscribe(SubscriberFn cb) const {
    return detail::container_subscribe(
        std::move(cb), [this](LoroSubscriber c) { return loro_list_subscribe(handle_.get(), c); });
}

inline std::optional<Doc> MovableList::doc() const {
    LoroDoc* d = loro_movable_list_doc(handle_.get());
    if (!d) return std::nullopt;
    return Doc(d);
}
inline std::optional<Subscription> MovableList::subscribe(SubscriberFn cb) const {
    return detail::container_subscribe(std::move(cb), [this](LoroSubscriber c) {
        return loro_movable_list_subscribe(handle_.get(), c);
    });
}

inline std::optional<Doc> Counter::doc() const {
    LoroDoc* d = loro_counter_doc(handle_.get());
    if (!d) return std::nullopt;
    return Doc(d);
}
inline std::optional<Subscription> Counter::subscribe(SubscriberFn cb) const {
    return detail::container_subscribe(std::move(cb), [this](LoroSubscriber c) {
        return loro_counter_subscribe(handle_.get(), c);
    });
}

inline std::optional<Doc> Tree::doc() const {
    LoroDoc* d = loro_tree_doc(handle_.get());
    if (!d) return std::nullopt;
    return Doc(d);
}
inline std::optional<Subscription> Tree::subscribe(SubscriberFn cb) const {
    return detail::container_subscribe(
        std::move(cb), [this](LoroSubscriber c) { return loro_tree_subscribe(handle_.get(), c); });
}

/// Version of the underlying loro Rust crate.
inline std::string version() {
    const char* v = loro_version();
    return v ? std::string(v) : std::string();
}

}  // namespace loro

#endif  // LORO_LORO_HPP
