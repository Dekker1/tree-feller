# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.0](https://github.com/Dekker1/tree-feller/compare/v0.1.1...v0.2.0) - 2026-09-15

### Fixed

- stop capacity doubling from wrapping to 0 in the parser and visible filter

### Other

- unroll the speculative frontier through one helper
- emit the visible root inside its reduction
- [**breaking**] derive whether a TFFile is mapped instead of storing it
- move the streaming-input design note to CLAUDE.md
- drop the unused tf_lookup_packed
- emit the root reduction in place instead of holding it back
- brace every if, else, for and while body
- report out of memory from one label in the parse loop
- handle private replay capture inside the split hook
- move trailing extras into place instead of through a buffer
- deduplicate CMake, cargo build script and CI configuration
- deduplicate Rust tests and bench, drop per-child handles
- remove duplication and dead code in the parser core

## [0.1.1](https://github.com/Dekker1/tree-feller/compare/v0.1.0...v0.1.1) - 2026-09-14

### Added

- add pinned SystemVerilog grammar and differential coverage
- add 32-bit pointer support to visible-node filter

### Fixed

- null-arena pointer arithmetic, and speed up speculative replay
- speculative parsing to preserve tree-sitter alternatives

### Other

- cut per-reduction work in the visible filter and the reduce path
- reduce speculative parser allocation and traversal overhead
- tighten documentation
