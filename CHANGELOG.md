# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
