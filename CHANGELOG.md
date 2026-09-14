# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.1](https://github.com/Dekker1/tree-feller/compare/v0.1.0...v0.1.1) - 2026-09-14

### Fixed

- null-arena pointer arithmetic, and speed up speculative replay

### Other

- cut per-reduction work in the visible filter and the reduce path
- Reduce speculative parser allocation and traversal overhead
- Fix speculative parsing to preserve tree-sitter alternatives
- Add pinned SystemVerilog grammar and differential coverage
- Add 32-bit pointer support to visible-node filter
- tighten documentation
