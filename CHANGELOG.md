# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
This project tracks the pinned upstream `loro` crate version with a fourth
component for binding-level releases (e.g. `1.13.1.2` = the second `loro-c`
release against `loro 1.13.1`).

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
