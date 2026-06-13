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
#include <memory>
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

/// RAII wrapper around a `LoroText*` container handle. Move-only.
class Text {
public:
    /// Takes ownership of a raw handle (e.g. returned by loro_doc_get_text).
    explicit Text(LoroText* raw) : handle_(raw) {
        if (!raw) throw Error(LORO_ERR_OTHER, detail::last_error_message());
    }

    /// The underlying C handle (non-owning).
    LoroText* raw() const noexcept { return handle_.get(); }

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

private:
    struct Deleter {
        void operator()(LoroText* p) const noexcept { loro_text_free(p); }
    };
    std::unique_ptr<LoroText, Deleter> handle_;
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

private:
    struct Deleter {
        void operator()(LoroDoc* p) const noexcept { loro_doc_free(p); }
    };
    std::unique_ptr<LoroDoc, Deleter> handle_;
};

/// Version of the underlying loro Rust crate.
inline std::string version() {
    const char* v = loro_version();
    return v ? std::string(v) : std::string();
}

}  // namespace loro

#endif  // LORO_LORO_HPP
