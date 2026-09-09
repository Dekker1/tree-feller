# Outlier grammar stress audit

Tested on macOS/arm64, September 8–9, 2026, against tree-sitter 0.27.0.
The initial audit exposed parser bugs. Those bugs are now fixed, and every
retained regression runs normally with zero expected failures.

## Grammar selection

Inspected the generated parsers at the revisions in
[nvim-treesitter's registry](https://github.com/nvim-treesitter/nvim-treesitter/blob/main/lua/nvim-treesitter/parsers.lua).
The revisions below make this audit independent of subsequent registry changes.

| Grammar | Revision | ABI | States | External tokens | Result |
| --- | --- | ---: | ---: | ---: | --- |
| [SystemVerilog](https://github.com/gmlarumbe/tree-sitter-systemverilog) | `4e7525a777290e341b8a5ad880bd20bb4f291845` | 15 | 20,731 | 0 | Differentially tested |
| [SQL](https://github.com/derekstride/tree-sitter-sql) | `b2bd686bb5f258506be69cddf76e49aa6b9de2c3` | 15 | 30,622 | 3 | Unsupported scanner |
| [Zsh](https://github.com/georgeharker/tree-sitter-zsh) | `7a593401efb5418ffdedbe3c0e4c61c6d240166d` | 15 | 28,645 | 52 | Unsupported scanner |
| [Nim](https://github.com/alaviss/tree-sitter-nim) | `ac72ba30d16edf0be021588a9301ede4accd6cf4` | 14 | 13,022 | 17 | Unsupported ABI and scanner |

SQL, Zsh, and Nim were inspected for compatibility, not parsed by tree-feller.
Removing their scanners or changing their ABI number would not test the actual
grammars. SystemVerilog 0.4.0's published crate has a byte-identical `parser.c`
to the pinned commit: SHA-256
`bbd79dce4576990b683173217d8a8b212b5dc0c931348784e0622316bc54c0eb`.
Rust and C therefore use the same grammar tables.

## Manual differential runs

Used the grammar's own
[test submodule](https://github.com/gmlarumbe/tree-sitter-systemverilog-test/tree/155dfcafd4d00c89930e777bf5275595d5f4fd43),
pinned at `155dfcafd4d00c89930e777bf5275595d5f4fd43`. Its sources include
sv-tests, UVM, CVA6, PULP AXI, BaseJump STL, and upstream regression examples.
Each file was compared against a fresh reference parse, including node symbols,
production IDs, fields, named/extra flags, child counts, byte spans, and points.

| Inputs | Files | Exact matches after fixes | Reference rejects | Failures after fixes |
| --- | ---: | ---: | ---: | ---: |
| `.sv` | 2,902 | 2,733 | 169 | 0 |
| `.svh` and `.v` | 262 | 254 | 8 | 0 |
| Total | 3,164 | 2,987 | 177 | 0 |

Before the fixes, 1,072 valid inputs produced different nodes and 51 failed to
parse. Two reference-invalid inputs were falsely accepted. Three additional
error-position disagreements came from the checker's EOF boundary. All 1,128
reported failures are resolved. Reference rejection itself is counted separately
and is not a result about tree-feller's handling of valid syntax.

Additional deterministic stress inputs:

- 72 generated cases: eight families at sizes 1, 2, 4, 8, 16, 32, 64, 128, 256.
  Families were nested parentheses, right-associated ternaries, nested blocks,
  dotted members, parameter lists, concatenations, repeated modules, and
  repeated indexed method calls. All 72 now match exactly; previously 27 differed.
- 503 mutations of the original 11 feature fixtures: every third byte prefix,
  plus deletion of each individual `(){}[];` delimiter. There are 24 exact
  matches and 479 reference rejects, with zero failures. The previous 38
  error-position disagreements were due to the same EOF-boundary issue.
  Prefixes can split UTF-8 sequences, intentionally exercising malformed bytes.
- Exhaustive table agreement passed for SystemVerilog: 7,403 dense states,
  1,353 symbols, 522 tokens, 20 fields, 61 productions, and 636 genuine conflict
  entries after filtering repetition shifts.
- Existing grammar checks over the local MiniZinc repositories: MiniZinc had
  9,694 exact matches and 82 reference rejects; DataZinc had 20,618 exact
  matches and 86 reference rejects. Both reported zero failures. After the final
  replay refinements, the full MiniZinc run again produced the same result, and
  DataZinc over `mzn-challenge` produced 1,569 exact matches and 35 reference
  rejects, with zero failures. The complete SystemVerilog and generated/mutation
  corpora were also rerun after those refinements.

The small examples in `corpus/systemverilog` and `fixtures/systemverilog` were
written during this audit; the upstream source collection is not vendored.
Scratch downloads, generated cases, traces, and logs are in the gitignored
`integration/stress/` and `integration/fix-stress/` directories of the audit checkout.

## Fixes

The previous speculative parser kept or discarded entire independent stacks.
This lost alternatives that tree-sitter retains through shared predecessors.
The replacement uses value-free graph-structured stacks, following upstream
link equivalence, reduction-path selection, head ordering, dynamic precedence,
pruning limits, and token reuse. Error-paused heads are distinguished from
merged heads when accounting for temporary reduction overflow.

Some structural ties reach inside a node completed before the conflict.
Its shape is recovered lazily by privately replaying the prefix to the exact
fork. Ordinary parsing still emits directly into the sink. Private collection
never invokes the consumer, and only the selected speculative tree is replayed
into consumer callbacks. Differential tests assert that every callback belongs
to the returned tree, catching duplicate or discarded-node emissions.

The differential checker's error bound now includes EOF after trailing whitespace
when no later visible token exists. This handles incomplete directives without
allowing arbitrary later tokens past the reference error region.

## Retained tests

`systemverilog_matches_tree_sitter` requires reference acceptance and complete
Rust-visible node equality for 13 fixtures: parameters, casts, generate blocks,
assertions, constraints, macros, streaming/assignment patterns, functions,
interfaces/clocking, fork/join, UTF-8 comments/strings with CRLF, a directive
after repeated declarations, and precedence pruning.
`systemverilog_deep_and_wide_matches_tree_sitter` checks another 32 generated
cases at depths/widths 1, 8, 64, and 256, including the formerly failing
parentheses, dotted members, and indexed method calls. CTest also compares the fixture corpus,
including production IDs unavailable through the Rust API.

Nine reduced fixtures guard the repaired behaviors and the paths that carry
them:

| Fixture | Regression prevented |
| --- | --- |
| `static_call.sv` | Scoped call loses a hierarchical-identifier node |
| `parenthesized_concat.sv` | Identifier in a parenthesized expression becomes `tf_call` |
| `indexed_method.sv` | Indexed method receiver chooses a different tree |
| `empty_port.sv` | Second instance is rejected after an empty module port list |
| `invalid_range.sv` | Accepts a part-select expression rejected by the reference |
| `invalid_scope_range.sv` | Accepts a scoped part-select expression rejected by the reference |
| `pragma_eof.sv` | Incorrect EOF error boundary after an incomplete pragma |
| `line_eof.sv` | Incorrect EOF error boundary after an incomplete line directive |
| `clocking_expect.sv` | Reaches a prefix cell unrolled after the private replay already ran -- the only input in the tree that does. Reduced from `core/assertions/expect_assertion.sv`. Removing the resolve leaves the cell's child count at its sentinel, which no available input turns into a different tree, so this pins coverage rather than a demonstrated difference |

All tests are active. Each CTest regression requires zero failures.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cargo test -p tree-feller
```

To repeat the upstream comparison, unpack the pinned test archive at `$SV_TESTS`:

```sh
build/tf_diff --grammar systemverilog "$SV_TESTS/files"
# Directory traversal selects .sv only; pass the other extensions explicitly.
find "$SV_TESTS/files" -type f \( -name '*.svh' -o -name '*.v' \) |
  build/tf_diff --grammar systemverilog
build/tf_diff --grammar minizinc ~/Code/github.com/minizinc
build/tf_diff --grammar datazinc ~/Code/github.com/minizinc
```

## Validation and performance

All 23 CTest entries pass in Release, macOS ASan and UBSan, and Linux/glibc
UBSan builds. Rust passes 22 integration tests and three doc tests, with no
ignored tests. Package verification and package Clippy with warnings denied
also pass. The C checks use clang-format and clang-tidy 21.1.6.

A back-to-back null-sink A/B microbenchmark, alternating five runs against the
pre-fix implementation, measured median CPU times of 0.265 s versus 0.261 s
for repeated large DataZinc arrays, 0.0621 s versus 0.0616 s for MiniZinc
comprehensions, and 0.0611 s versus 0.119 s for repeated simple SystemVerilog
modules. These synthetic workloads are not representative throughput claims.
Correctness is established by the differential runs, independently of timing.

A follow-up A/B check on September 9 covered the other shipped grammars using
10,000 null-sink parses of one corpus fixture, with five alternating runs:

| Grammar / fixture | Before median | After median | Change |
| --- | ---: | ---: | ---: |
| C / `declarations.c` | 0.147 s | 0.151 s | +2% |
| Go / `basics.go` | 0.129 s | 0.200 s | +55% |
| Regex / `groups.regex` | 0.00536 s | 0.00529 s | −1% |
| Solidity / `repeated_conflicts.sol` | 0.170 s | 0.282 s | +66% |
| EPrime / `model.eprime` | 0.0186 s | 0.0190 s | +2% |

Those are fixture microbenchmarks, not whole-project measurements, and they
turned out to be a poor guide: see the corpus figures below, where SystemVerilog
went the other way entirely. No parse-selection or token-reuse rules were
weakened for speed.

## Corpus throughput

Fixtures of a few hundred bytes do not reach the shapes that dominate real
input, so the follow-up work measured whole directories instead. Each figure is
the median of five alternating A/B runs on macOS/arm64 (Apple M4, clang 21,
`-O3 -DNDEBUG`), parsing every file in the directory once with a null sink.
Grammar loading is timed separately and excluded. The corpora are the pinned
SystemVerilog test collection, 1,200 files of the Go standard library, the
OpenZeppelin Solidity contracts, tree-sitter's own C plus Lua and git sources,
`libminizinc`'s models, and 122 MB of `mzn-challenge` data files. Files the
grammar itself rejects stay in the workload and so in the timings, but are
counted separately in the differential results below.

| Grammar / corpus | Files | Bytes | Fixes as landed | Optimized | Change | Pre-fix |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| SystemVerilog / test corpus | 2,902 | 11.5 MB | 0.468 s | **0.353 s** | −24.5% | 0.895 s |
| Go / stdlib subset | 1,200 | 13.1 MB | 0.274 s | **0.201 s** | −26.6% | 0.190 s |
| Solidity / OpenZeppelin | 240 | 0.83 MB | 0.0123 s | **0.0074 s** | −39.4% | 0.0084 s |
| C / mixed sources | 281 | 1.74 MB | 0.0452 s | **0.0423 s** | −6.3% | 0.0408 s |
| MiniZinc / libminizinc | 2,000 | 5.15 MB | 0.156 s | **0.128 s** | −18.2% | 0.101 s |
| MiniZinc, minus one file | 1,999 | 4.37 MB | 0.0565 s | **0.0511 s** | −9.4% | 0.0623 s |
| DataZinc / mzn-challenge | 400 | 122 MB | 1.538 s | **1.516 s** | −1.4% | 1.524 s |

Peak RSS on the same workloads: SystemVerilog 93 MB → 92 MB, Go 25.5 MB →
23.3 MB, MiniZinc 270 MB → 179 MB, DataZinc unchanged at 119 MB.

Against the pre-fix implementation the optimized parser is faster on
SystemVerilog (−60%), Solidity (−11%), MiniZinc excluding the one outlier file
(−17%) and level on DataZinc; it remains slower on Go (+6%) and C (+5%), where
splits are frequent and short, and on MiniZinc as a whole (+28%) because of that
outlier. The pre-fix numbers are a reference point only: on SystemVerilog it was
producing different nodes for 1,072 files, which is why it was also slower there.

The visible-node layer tracks the raw stream rather than hiding the difference.
Against pre-fix, with the consumer counting visible nodes, folding hidden runs,
or allocating a value per node, SystemVerilog is 55% to 58% faster, MiniZinc
(minus the outlier) 11% to 13% faster, Solidity 8% to 10% faster, C level to 2%
slower, and Go 4% to 7% slower. The more work the consumer does, the smaller the
remaining gap: Go is +6.4% with a null sink and +3.8% with a per-node
allocation.

## Scaling

Measured on generated inputs, current build, null sink:

| Shape | Sizes | Result |
| --- | --- | --- |
| Independent items (`.dzn`) | 26 KB → 1.78 MB | linear; 36 → 62 MB/s, 1.5 → 3.2 MB RSS |
| One repetition (`.dzn`) | 2.9 KB → 2.9 MB | linear; 67 → 70 MB/s, 1.5 → 4.3 MB RSS |
| Nesting depth (`.mzn`) | 100 → 50,000 deep | linear; 26 → 48 MB/s, 2.4 → 6.2 MB RSS |
| Repeated conflicts (`.sol`) | 11 KB → 1.1 MB | linear; 64 → 67 MB/s, 3.0 → 4.0 MB RSS |
| Unresolved ambiguity (`.mzn`) | 2 KB → 800 KB | linear but steep: 12.6 → 6.7 MB/s, 3.8 → 242 MB RSS |

The last row is the one that matters. A MiniZinc array literal forks at the `[`
and cannot be resolved until the `]`, so the whole literal is parsed
speculatively and then replayed, and speculative state grows with the literal
rather than with nesting depth — about 300 bytes per input byte. Nothing here is
quadratic. The same data as `.dzn`, whose grammar has no such conflict, parses at
70 MB/s in a couple of megabytes. On an 800 KB case the branch as the fixes
landed managed 5.4 MB/s and 480 bytes per byte; the pre-fix parser managed
12 MB/s and 226 bytes per byte. Closing that gap would mean handing the settled
part of the stack to the real parser part-way through a split, which moves the
parser depth out from under both the graph's inherited frontier and the private
prefix replay; it was not attempted rather than risk the selection rules.

`integration/perf/` in the audit checkout holds the harness, the corpora and the
generators for these shapes.

## Rejected optimizations

Measured and discarded, recorded so they are not re-derived:

| Experiment | Result |
| --- | --- |
| Field-wise `TFSpecNode` writes, leaving the seven unused link slots alone | −0.2% to +1.4%; clang already narrows the store |
| Packing the replay completion marker so a finished node is not re-read | −1% on three corpora, +0.4% on the case it targeted; the postorder stack doubles in width |
| Multi-entry lexer cache keyed on (byte, state) | exact repeats within a split are 0.0% to 0.3% — nothing to cache |
| `noinline` on `tf_parser__split` | ±0.3%, matching the earlier reading |

## Validation of the optimized parser

All 23 CTest entries pass in Release, under macOS ASan and UBSan, and under
Linux/glibc UBSan in a container; Linux/glibc ASan with `detect_leaks=1` passes
every entry but the exhaustive UTF-8 test, which these changes do
not touch and which passes under Linux UBSan and macOS ASan. `tf_diff` reports
zero failures over every corpus listed above, and separately under both macOS
sanitizers over SystemVerilog, Go, Solidity and MiniZinc, and under Linux
UBSan over SystemVerilog and Go. The pinned upstream comparison is unchanged:
2,733 and 254 SystemVerilog matches with 169 and 8 reference rejects, 9,694
MiniZinc matches with 82 rejects, 20,618 DataZinc matches with 86 rejects, zero
failures throughout. Rust passes 22 integration tests and three doc tests;
`cargo fmt`, `cargo clippy -D warnings`, `cargo package` and the packaged
crate's own tests pass. clang-format and clang-tidy 21.1.6 are clean.
