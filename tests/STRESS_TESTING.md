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

## Historical: measurements taken while the fixes landed

Everything in this section is kept for the record and has been superseded. The
final verified numbers are further down, under **Verified results**.

A back-to-back null-sink A/B microbenchmark, alternating five runs against the
pre-fix implementation, measured median CPU times of 0.265 s versus 0.261 s
for repeated large DataZinc arrays, 0.0621 s versus 0.0616 s for MiniZinc
comprehensions, and 0.0611 s versus 0.119 s for repeated simple SystemVerilog
modules.

A follow-up A/B check on September 9 covered the other shipped grammars using
10,000 null-sink parses of one corpus fixture, with five alternating runs:

| Grammar / fixture | Before median | After median | Change |
| --- | ---: | ---: | ---: |
| C / `declarations.c` | 0.147 s | 0.151 s | +2% |
| Go / `basics.go` | 0.129 s | 0.200 s | +55% |
| Regex / `groups.regex` | 0.00536 s | 0.00529 s | −1% |
| Solidity / `repeated_conflicts.sol` | 0.170 s | 0.282 s | +66% |
| EPrime / `model.eprime` | 0.0186 s | 0.0190 s | +2% |

Those are fixture microbenchmarks of a few hundred bytes, and they turned out to
be a poor guide: on directory-sized corpora SystemVerilog went the other way
entirely. They are not comparable to anything below.

One further historical claim was **wrong and is retracted**: a September 10 note
recorded that Linux/glibc ASan failed the exhaustive UTF-8 test. It does not.
That entry came from a run that was killed by hand after 27 minutes and then
written up as a failure. See **The Linux ASan investigation** below.

## Verified results

Measured on macOS/arm64 (Apple M4, Apple clang 21.0.0, `-O3 -DNDEBUG`), each
figure the median of five alternating A/B runs of the same binaries, taken with
the machine's load average below 3 and the per-side spread reported by the
harness under 4%. Every file in the directory is parsed once with a null sink;
grammar loading is timed separately and excluded. Files the grammar itself
rejects stay in the workload and so in the timings, and are counted separately
in the differential results.

Corpora: the pinned SystemVerilog test collection, 1,200 files of the Go
standard library, the OpenZeppelin Solidity contracts, tree-sitter's own C plus
Lua and git sources, `libminizinc`'s models, and 122 MB of `mzn-challenge` data
files.

Against the tree as the correctness fixes landed (`23c9ebe`):

| Grammar / corpus | Files | Bytes | Fixes as landed | Optimized | Change |
| --- | ---: | ---: | ---: | ---: | ---: |
| SystemVerilog / test corpus | 2,902 | 11.5 MB | 0.481 s | **0.324 s** | −32.7% |
| Go / stdlib subset | 1,200 | 13.1 MB | 0.280 s | **0.198 s** | −29.4% |
| Solidity / OpenZeppelin | 240 | 0.83 MB | 0.0126 s | **0.0073 s** | −41.8% |
| C / mixed sources | 281 | 1.74 MB | 0.0464 s | **0.0415 s** | −10.5% |
| MiniZinc / libminizinc | 2,000 | 5.15 MB | 0.158 s | **0.119 s** | −24.5% |
| MiniZinc, minus one file | 1,999 | 4.37 MB | 0.0568 s | **0.0511 s** | −10.2% |
| DataZinc / mzn-challenge | 400 | 122 MB | 1.544 s | **1.544 s** | +0.0% |

The visible-node layer and a consumer that allocates a value per node track the
raw stream rather than hiding the difference. Against the same baseline:
SystemVerilog −29% visible and −25% with the allocating consumer, Go −25% and
−21%, MiniZinc −21% and −18%, C −1% and −7%. Those were taken one revision
before the `tf_visible.c` fix below, whose own cost measures between −1.1% and
+2.0% across raw, visible and consumer modes -- inside this harness's
resolution, and it moves raw SystemVerilog too, where the visible layer is not
used at all.

Against the pre-fix implementation (`0d2de2a`), which produced different nodes
for 1,072 SystemVerilog files and is a **performance reference only**:

| Grammar / corpus | Pre-fix | Optimized | Change |
| --- | ---: | ---: | ---: |
| SystemVerilog / test corpus | 0.911 s | 0.323 s | −64.6% |
| MiniZinc, minus one file | 0.0626 s | 0.0511 s | −18.4% |
| Solidity / OpenZeppelin | 0.0085 s | 0.0072 s | −14.5% |
| C / mixed sources | 0.0409 s | 0.0412 s | +0.8% |
| DataZinc / mzn-challenge | 1.530 s | 1.548 s | +1.2% |
| Go / stdlib subset | 0.192 s | 0.199 s | +3.7% |
| MiniZinc / libminizinc | 0.102 s | 0.120 s | +18.1% |
| MiniZinc / `648.mzn` alone | 0.0404 s | 0.0700 s | +73.1% |

Peak RSS on the same workloads:

| Workload | Pre-fix | Fixes as landed | Optimized |
| --- | ---: | ---: | ---: |
| SystemVerilog corpus | 205 MB | 93 MB | 92 MB |
| Go corpus | 24 MB | 25 MB | 23 MB |
| MiniZinc corpus | 141 MB | 270 MB | 179 MB |
| `648.mzn` alone | 128 MB | 265 MB | 173 MB |
| 800 KB generated array | 181 MB | 382 MB | 241 MB |
| DataZinc corpus | 119 MB | 119 MB | 119 MB |

### What is still slower than the pre-fix parser

Go +3.7%, C +0.8% and DataZinc +1.2% -- the last is at the edge of what this
harness resolves, and DataZinc runs 25 splits in 122 MB, so no change to the
speculative path can genuinely cost it that much. Measured, the new parser
performs **fewer** speculative steps than the old one, not more: 1.25 M advances
against 1.61 M branch steps for Go, 1.42 M against 2.27 M for SystemVerilog,
0.59 M against 1.18 M for MiniZinc. What is left is cost per step. The old
branch representation appended one ~40-byte log entry per step; the graph
appends a 56-byte subtree, a 32-byte node and an 8-byte link, because
`ts_subtree_compare` and `stack__subtree_is_equivalent` need the structure that
the log could not provide. That is the price of the correctness fix, and it is
about 9 ns per speculative advance on this machine.

The one large remaining cost is a grammar conflict that cannot be resolved until
the end of a construct; see **Scaling**.

## The Linux ASan investigation

The exhaustive UTF-8 test does **not** fail under Linux/glibc ASan. It is
extremely slow there, under one specific toolchain, and an earlier note recorded
a hand-killed run as a failure.

Reproduced in `ubuntu:24.04` on aarch64 (gcc 13.3.0, clang 18.1.3, glibc 2.39,
CMake 3.28.3), each run under `timeout` with `/usr/bin/time -v`, no pipeline
around the test, exit status captured:

| Toolchain | Optimization | User time | Exit | Result |
| --- | --- | ---: | ---: | --- |
| gcc 13.3, UBSan | `-O0` | 1.64 s | 0 | 285,278,464 sequences, identical to ICU |
| gcc 13.3, ASan | `-O1` | 0.89 s | 0 | identical to ICU |
| clang 18.1, ASan | `-O1` | 2.45 s | 0 | identical to ICU |
| clang 18.1, ASan | `-O0` | 3.64 s | 0 | identical to ICU |
| Apple clang 21, ASan | `-O0` | 3.26 s | 0 | identical to ICU |
| **gcc 13.3, ASan** | **`-O0`** | **2561.79 s** | **0** | identical to ICU |

Root cause: at `-O0` gcc instruments every memory access with an out-of-line
call through the PLT into the shared `libasan.so.8`, and inlines neither
`tf_utf8_next` nor ICU's `U8_NEXT` temporaries, so a test that performs a fixed
285 million decodes pays roughly 9 µs each. clang inlines the shadow checks even
at `-O0` and links its runtime statically. Raising gcc to `-O1` removes the
cliff entirely. Nothing in the test or in the parser is implicated: the two
files involved, `tests/test_utf8.c` and `lib/src/tf_utf8.h`, are byte-identical
between the pre-optimization baseline and the optimized revision, and both
revisions measured the same (clang `-O0` 3.64 s versus 3.71 s; gcc `-O1` 0.89 s
versus 0.90 s).

Two changes follow from this, and neither suppresses a diagnostic:

* The CI `Sanitize` job now sets `CC: clang` and keeps `-O0`, so the address
  sanitizer runs with its redzones around every local rather than only around
  the ones the optimizer did not promote. Linux gcc still builds and runs the
  whole suite unsanitized in the `C / ubuntu-latest` job. Verified in the
  container: `CC=clang`, `-fsanitize=address`, `-O0`, 23/23 CTest entries pass,
  the UTF-8 test in 3.66 s, the suite in 11.06 s, and `tf_diff` under ASan gives
  2,733 SystemVerilog matches with 169 reference rejects and 1,200 Go matches,
  zero failures.
* `utf8` now carries `TIMEOUT 600` in CTest. The work it does is a fixed 285
  million sequences, so its runtime is predictable; the bound turns a 43-minute
  configuration into a visible failure instead of an invisible cost.

One trap found while reproducing this, recorded so it is not rediscovered:
`stdbuf` works by `LD_PRELOAD`, and gcc's shared ASan runtime refuses to start
when it is not first in the initial library list -- "ASan runtime does not come
first in initial library list". Wrapping a sanitized binary in `stdbuf` to get
line-buffered progress produces an immediate spurious failure. Use a pty
(`script -qec ...`) instead.

## A null-pointer bug the compiler change found

Switching the sanitizer job to clang immediately reported undefined behaviour
that had been present all along:

```
lib/src/tf_visible.c:203:24: runtime error: applying zero offset to null pointer
    #0 tf_filter__on_reduce  lib/src/tf_visible.c:203
    #1 tf_parser__reduce     lib/src/tf_parser.c:227
    #2 tf_parser__run        lib/src/tf_parser.c:345
    #3 tf_parse              lib/src/tf_parser.c:475
    #4 tf_parse_visible      lib/src/tf_visible.c:300
    #5 main                  tests/test_c_api.c:108
```

`tf_filter__on_reduce` handed the sink `&self->arena[position]` for a node's
children. The arena is allocated on demand, so the very first reduction of a
parse -- one whose children are all hidden, which is most of them -- evaluates
`&arena[0]` with `arena` still null. That is undefined in C even though the
result is never dereferenced.

It is **not** a consequence of the optimization work. `lib/src/tf_visible.c` is
untouched by it, and the same clang 18 UBSan build reports the same line on
every revision:

| Revision | CTest under clang 18 UBSan |
| --- | --- |
| `23c9ebe`, correctness fixes as landed | 15 of 22 failed |
| `bc27dee`, optimizations as committed | 16 of 23 failed |
| working tree, after the fix | **23 of 23 passed, no diagnostic** |

Apple clang 21 and gcc 13.3 do not report it, which is why it survived the
audit: every earlier sanitizer run used one of those two. It is the same family
as the glibc `memcpy` non-null trap already recorded in `CLAUDE.md`.

The fix substitutes a valid empty object rather than a null pointer, because
`children` is documented as a pointer to `child_count` entries: handing back a
null would make `memcpy(dst, node->children, 0)` in a consumer undefined in
turn, and Rust's `slice::from_raw_parts` requires a non-null pointer even for an
empty slice -- the binding calls it directly. Nothing about the emitted node
stream changes; `tf_diff` is byte-for-byte identical before and after.

The durable guard is the CI change: the `Sanitize` job now runs clang, which
reports this class of defect.

## Scaling

Measured on generated inputs, current build, null sink:

| Shape | Sizes | Result |
| --- | --- | --- |
| Independent items (`.dzn`) | 26 KB → 1.78 MB | linear; 36 → 62 MB/s, 1.5 → 3.2 MB RSS |
| One repetition (`.dzn`) | 2.9 KB → 2.9 MB | linear; 67 → 70 MB/s, 1.5 → 4.3 MB RSS |
| Nesting depth (`.mzn`) | 100 → 50,000 deep | linear; 26 → 48 MB/s, 2.4 → 6.2 MB RSS |
| Repeated conflicts (`.sol`) | 11 KB → 1.1 MB | linear; 64 → 67 MB/s, 3.0 → 4.0 MB RSS |
| Unresolved ambiguity (`.mzn`) | 200 KB → 800 KB | linear but steep: 8.6 → 7.7 MB/s, 66 → 242 MB RSS |

The last row is the one that matters. A MiniZinc array literal forks at the `[`
and cannot be resolved until the `]`, so the whole literal is parsed
speculatively and then replayed, and speculative state grows with the literal
rather than with nesting depth — about 300 bytes per input byte. Nothing here is
quadratic. The same data as `.dzn`, whose grammar has no such conflict, parses at
70 MB/s in a couple of megabytes. On an 800 KB case the branch as the fixes
landed managed 5.4 MB/s and 480 bytes per byte; the pre-fix parser managed
12 MB/s and 226 bytes per byte. The optimized parser is at 7.7 MB/s and 300
bytes per byte -- better than the fixes as landed, still short of the parser it
replaced. Closing that gap would mean handing the settled
part of the stack to the real parser part-way through a split, which moves the
parser depth out from under both the graph's inherited frontier and the private
prefix replay; it was not attempted rather than risk the selection rules.

`integration/perf/` in the audit checkout holds the harness, the corpora and the
generators for these shapes.

## Rejected optimizations

Measured and discarded, recorded so they are not re-derived. Percentages are
medians of alternating A/B runs; anything inside +/-2% on this machine is at the
edge of what the harness resolves, and the harness now reports the per-side
spread and the load average it ran under.

| Experiment | Result |
| --- | --- |
| Field-wise `TFSpecNode` writes, leaving the seven unused link slots alone | −0.2% to +1.4%; clang already narrows the store |
| Packing the replay completion marker so a finished node is not re-read | −1% on three corpora, +0.4% on the case it targeted; the postorder stack doubles in width |
| Passing arena entries to `tf_spec__tree` by pointer instead of by value | no consistent effect; the callee is inlined, so there was no copy to remove |
| Multi-entry lexer cache keyed on (byte, state) | exact repeats within a split are 0.0% to 0.3% -- nothing to cache |
| `noinline` on `tf_parser__split` | ±0.3%; `noinline` on `tf_spec__replay` alone is the placement that pays |
| Ending a split early when one head remains but a fork survives below it | not attempted: it needs the settled stack handed to the real parser mid-split, which moves the parser depth out from under both the graph's inherited frontier and the private prefix replay |

## Validation of the optimized parser

Verified against the working tree at the time of writing, on macOS/arm64 with
Apple clang 21.0.0 and in `ubuntu:24.04` on aarch64 with clang 18.1.3 and gcc
13.3.0.

| Check | Result |
| --- | --- |
| CTest, Release | 23/23 |
| CTest, macOS ASan and UBSan | 23/23 each |
| CTest, Linux/glibc `CC=clang` ASan | 23/23, UTF-8 test 3.81 s, suite 11.86 s |
| CTest, Linux/glibc `CC=clang` UBSan | 23/23, UTF-8 test 3.71 s, suite 14.12 s; 16 of 23 failed before the `tf_visible.c` fix |
| `tf_diff`, SystemVerilog `.sv` | 2,733 matched, 169 reference rejects, 0 failed |
| `tf_diff`, SystemVerilog `.svh`/`.v` | 254 matched, 8 reference rejects, 0 failed |
| `tf_diff`, MiniZinc repositories | 9,694 matched, 82 reference rejects, 0 failed |
| `tf_diff`, DataZinc repositories | 20,618 matched, 86 reference rejects, 0 failed |
| `tf_diff`, Go / Solidity / C corpora | 1,200 / 240 / 281 matched, 0 failed |
| `tf_diff` under macOS ASan and UBSan | SystemVerilog, Go, Solidity, MiniZinc: 0 failed |
| `tf_diff` under Linux/glibc ASan and UBSan | SystemVerilog, Go: 0 failed each |
| Rust | 22 integration tests, 3 doc tests, none ignored |
| `cargo fmt`, `cargo clippy -D warnings` | clean |
| `cargo package` and the packaged crate's tests | pass |
| clang-format 21.1.6, clang-tidy 21.1.6 | clean |

Reference-invalid inputs are counted separately throughout and are never folded
into the failure count. `differential.rs` asserts that every value the consumer
is handed belongs to the tree that is returned, which is what would catch a
discarded alternative or a duplicated node reaching a callback; it passes.

### Known issues and unverified claims

* **Not fixed.** A grammar conflict that cannot be resolved before the end of a
  construct still costs about 1.8x the pre-fix parser in time and 1.3x in peak
  memory on that construct -- `648.mzn` is 0.0700 s against 0.0404 s, and
  173 MB against 128 MB. The measured bottleneck and the reason it was not
  pursued are in **Scaling** and in the rejected-optimizations table.
* **Not fixed.** Go is 3.7% slower than the pre-fix parser on the corpora above;
  C is within measurement of it at +0.8%. The cause is measured -- more bytes
  written per speculative step, not more speculative steps -- and is the cost of
  the structure the selection rules need.
* **At the edge of measurement.** DataZinc reads 0.0% against the
  correctness-fixed baseline and +1.2% against the pre-fix parser. DataZinc
  performs 25 splits in 122 MB, so no change to the speculative path can
  genuinely account for that; comment-only rebuilds of the same source move it
  by up to 1%. Treat it as codegen layout, not as a cost of these changes.
* **Not measured here.** Windows, and any 32-bit target. CI covers the Windows
  build and Release test run; nothing in this report was measured there.
* **Not measured here.** The CodSpeed benchmarks in `crates/tf-bench`. They run
  in CI and were not re-run for this work.
* **Environment-limited.** The Linux runs are in a `podman` VM on this machine,
  not on CI hardware. The gcc `-O0` ASan figure of 2561.79 s is that VM's; the
  ratio to clang, not the absolute number, is the finding.
* **Not re-run under gcc.** Now that CI's sanitizer job uses clang, no automated
  job runs the sanitizers under gcc. gcc still builds and runs the whole suite
  unsanitized on Linux. A gcc `-O0` ASan run of the full suite was not repeated
  after the `tf_visible.c` fix: it costs 43 minutes for the UTF-8 test alone,
  and the defect it would exercise is reported by clang in 12 s.
* **Cost of the `tf_visible.c` fix is inside the noise.** It measures between
  −1.1% and +2.0% across raw, visible and consumer modes, including +2.0% on raw
  SystemVerilog where the visible layer is not used at all, so the spread is
  codegen layout rather than the branch it adds.
