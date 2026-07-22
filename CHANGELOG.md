# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
This project tracks the pinned upstream `loro` crate version with a fourth
component for binding-level releases (e.g. `1.13.1.2` = the second `loro-c`
release against `loro 1.13.1`).

## [1.13.1.3] - 2026-07-22

- **Fix: `<loro.hpp>` failed to compile under clang + libstdc++** ([#3]) —
  `loro::detail::JsonValue`'s implicit destructor made instantiating
  `std::pair<std::string, JsonValue>` force a completeness check on the
  still-mid-instantiation pair (via the `explicit(...)` condition on
  libstdc++'s pair default constructor), which clang rejects. The destructor
  is now user-declared and defined (defaulted) after the class, breaking the
  cycle; copy/move members are explicitly defaulted so semantics are
  unchanged. GCC, clang + libc++, and MSVC builds were unaffected. CI now
  syntax-checks the public header with clang against libstdc++ to keep the
  combination covered.

[#3]: https://github.com/gsfjohnson/loro-c/issues/3
[1.13.1.3]: https://github.com/gsfjohnson/loro-c/compare/v1.13.1.2...v1.13.1.3

## [1.13.1.2] - 2026-06-22

- **C++ `LoroDoc::revert_to(frontiers)`** — exposes the existing C ABI
  `loro_doc_revert_to` through the C++ `LoroDoc` wrapper. It rewinds the
  document state back to a target `Frontiers` by recording the inverse
  operations as a new change; unlike `checkout()`, the document stays attached
  and the rewind becomes part of history. Throws `LoroError`
  (`LORO_ERR_NOT_FOUND`) for an unknown version.

[1.13.1.2]: https://github.com/gsfjohnson/loro-c/compare/v1.13.1.1...v1.13.1.2

## [1.13.1.1] - 2026-06-18

- **initial releae**
