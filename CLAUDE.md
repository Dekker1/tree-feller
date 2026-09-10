# Notes for agents

The README covers what this is and how to build it. This is the rest.

## Invariants

- **`lib/src/` knows about no grammar.** If a fix needs to name a rule, it is the
  wrong fix.
- **`tf_language.*` and `tf_lexer.c` are transcriptions** of tree-sitter's runtime,
  and the `file.c:line` references in their comments are the spec. Read the fetched
  source at `build/_deps/tree-sitter-src/lib/src/` rather than guessing. Don't tidy
  them into something that no longer matches upstream.
- **`TFToken` and the sink structs are public.** Internal per-token state goes on
  `TFLexer`, which is already copied per branch during a split.
- **No generated `parser.c` lives here.** Grammars are fetched, hash-pinned, in
  `cmake/TreeFellerGrammars.cmake`.

## Verifying a change

`ctest` is necessary and not sufficient. The acceptance test is `tools/tf_diff`,
which compares the node stream against a real libtree-sitter walk:

```sh
build/tf_diff --grammar datazinc ~/Code/github.com/minizinc/mzn-challenge
```

Any change to the parser, lexer or visibility layer gets run over a large corpus
before you believe it. The MiniZinc repos under `~/Code/github.com/minizinc/` are
~19k files and take a few minutes. Files a grammar itself rejects are counted
separately and are not a result about tree-feller.

## Traps that have cost real time

- **Stale probe binaries.** A scratch program linked against `build/libtree_feller.a`
  does *not* pick up `cmake --build`. Relink it. Two wrong diagnoses here came from
  a probe reporting the previous build's behaviour.
- **`ts_parser_set_logger` is public.** For "why does tree-sitter do X here",
  diffing its parse trace against ours beats reading tables.
- **Benchmark numbers drift with machine state.** Only trust A/B measured
  back-to-back in the same session.
- **Linux finds things macOS does not.** glibc declares `memcpy` non-null, so a
  zero-length copy from a null pointer passes here and fails there — under UBSan
  *and* under clang-tidy. Pinning the tool version does not make the platforms
  agree; run it in a container instead of pushing to find out.
- **The sanitizer compiler is load-bearing.** clang 18's UBSan reports
  `applying zero offset to null pointer`; Apple clang 21 and gcc 13.3 do not.
  A null-arena `&arena[0]` in `tf_visible.c` survived the whole stress audit
  because every sanitizer run used one of those two. CI's `Sanitize` job pins
  `CC: clang` for this reason.
- **gcc's address sanitizer at `-O0` is ~700x slower than clang's**, because it
  calls out through the PLT into the shared `libasan` for every access and
  inlines nothing. `tests/test_utf8.c` does a fixed 285 million decodes: 3 s
  under clang, 2,562 s under gcc, both passing. `-O1` removes the cliff. The
  `utf8` test carries a `TIMEOUT` so the cost cannot hide again.
- **`stdbuf` and a sanitized binary do not mix.** It works by `LD_PRELOAD`, and
  gcc's shared ASan runtime aborts with "ASan runtime does not come first in
  initial library list". Use a pty — `script -qec ...` — to get line-buffered
  progress out of a long sanitizer run.
- **Field order in `TFLexer` is load-bearing** for the hot loop. Add to the end.
- **Offering a fold is O(run).** A repetition's run grows by one each reduction, so
  re-offering a symbol after it declined is quadratic. The filter caches the refusal
  per symbol; do not "simplify" that away. It was worth 515x on a data file.

## Performance

The dispatch loop in `tf_parse` and `tf_lexer_next` are the hot path; ~52% is
lexing, under half of that inside the generated `ts_lex`, which is not ours to
speed up. Anything on the `count == 0` path costs nothing. Table lookups go
through the parse table expanded at load (`tf_lookup`), never the packed scan —
that was a 2.5x win.

`tf_lexer__get_lookahead` handles ASCII before it calls `tf_utf8_next`: the
decoder is too large for the compiler to inline there, so every input byte was
paying a call — 6.9% of a null-sink profile, in a frame of its own. Worth 14%
(datazinc) to 28% (c), and why lexing is 52% rather than 60%.

`tf_lexer__start` does not re-decode either. tree-sitter decodes there because
a move between chunks or included ranges can have invalidated the lookahead;
with one buffer only `tf_lexer_seek` and `tf_lexer__do_advance` move the
position and both refresh it. Worth ~1.5% across the benchmarks, being one
decode per token and another on every keyword re-lex.

The largest lexing cost left is not in `lib/src/` at all: for identifier-heavy
grammars, `set_contains` in `tree_sitter/parser.h` is ~15% of samples, binary
searching 678-802 ranges of which only 3-6 are ASCII. Scanning the leading
ranges for a `< 0x80` lookahead measures +15% (c) and +21% (minizinc) on a
visible-node parse, and is equivalent given the sortedness the binary search
already assumes (3.08M random-set comparisons, no mismatch). It is not shipped:
that header is vendored verbatim, every grammar crate bundles its own copy which
wins the include guard, so it reaches only consumers compiling `parser.c` against
ours -- and `cargo bench` cannot see it. Upstream is the right home for it.

Measured on the lexer and not worth doing, recorded so they do not get
re-derived: **LTO, or `parser.c` in the same TU as the lexer** — zero, both ways,
because clang will not devirtualize the `TSLexer` vtable without a profile;
**deferring row/column tracking to per-token** — +4% on datazinc and negative on
c with the arithmetic deleted outright, which is the ceiling; **direct-call
macros in our `tree_sitter/parser.h`**, which `-include` can put ahead of the
grammar's own copy — +1% (c) to +7% (minizinc), against making `TFLexer`'s layout
public, and useless to grammar crates that compile their own `parser.c`. What
does move it is **PGO**: +3% to +13% over the shipped build, because clang then
promotes the per-byte indirect calls. That is a consumer's build flag, and it
subsumes the ASCII path.

Also measured and reverted, all inside noise or worse: memoizing the keyword
re-lex on the token's bytes (the keyword DFA fails after ~2.6 bytes, so there is
nothing to memo -- 66% hit rate and 16% *slower*); dropping the
`if (self->lookahead_size)` guard in `tf_lexer__do_advance`, which is logically
dead here but cost 3-5% on solidity; keeping `TSLexerMode`/`reserved_word_set_id`
across the generated call; a branchless row/column update; and skipping the
trailing `tf_lexer_seek` when the position is already right, which it is for
79-100% of tokens and still measured neutral.

Conflict handling is the other hot path, and it is the one that scales badly.
Three things paid for themselves, all measured over directory-sized corpora
rather than fixtures, because a fixture microbenchmark does not reach the shapes
that matter:

* **The inherited prefix is built on demand.** A split used to turn every cell
  of the real stack below the fork into a graph node up front. Only 5.5% to 16%
  of those cells are ever reached by a reduction, and building the rest was 48%
  to 68% of every tree the split allocated. `tf_spec__extend` unrolls one cell at
  a time; a pop that is only collecting the speculative region stops at the
  frontier instead of walking to the bottom of the stack.
* **`tf_spec__pop` takes a direct path when the chain has no fork**, which is
  almost every pop. The breadth-first walk, its edge list and its iterator array
  only earn their keep at a real fork.
* **A graph node keeps its links in a shared arena.** An inline `links[8]` made
  `TFSpecNode` 96 bytes when a node almost always has exactly one link; it is now
  32. `TFSpecTree` was reordered to 56 bytes from 64 at the same time, worth
  under 1% on its own and kept for the memory: peak RSS on the worst MiniZinc
  file in the local corpora went 265 MB -> 173 MB.

Three more followed, all in `tf_spec__replay`, which is a third of the time on
an ambiguity-heavy input: read the arena entry through a pointer instead of
copying 56 bytes per visited node, descend into the leftmost child directly
rather than pushing it and popping it straight back, and keep the whole function
**out of line** — it runs once per split, so the call is free, and inlining it
grows the function that also holds the ordinary dispatch loop. That last one is
worth -11% on SystemVerilog on its own. Note that `noinline` on
`tf_parser__split`, which contains it, measures nothing: the placement matters.

Together, against the tree at the point the conflict fixes landed, null-sink
over directory-sized corpora: -32.7% (SystemVerilog, 2,902 files), -29.4% (Go,
1,200), -41.8% (Solidity, 240), -24.5% (MiniZinc, 2,000), -10.5% (C, 281), 0.0%
(DataZinc, 122 MB). Peak RSS on the MiniZinc corpus went 270 MB -> 179 MB.

DataZinc is the control: it performs 25 splits in 122 MB, so nothing on the
speculative path can move it. It reads +/-1% between builds of the same source,
which is the resolution of this harness for two separately linked binaries.

Also measured on the speculative path and reverted: **writing `TFSpecNode` field
by field** instead of assigning the struct, so the seven unused link slots are
not cleared — zero, clang already narrows the store, and it leaves the struct
partly uninitialised for nothing; **packing the replay completion marker** so a
finished node is not read out of the tree arena a second time — a wash on the
corpora and 0.4% *slower* on the case it was aimed at, because the postorder
stack doubles in width; **passing arena entries to `tf_spec__tree` by pointer** —
nothing, the callee is inlined so there was no copy to remove; **a multi-entry
lexer cache** keyed on (byte, state) — an exact repeat within one split happens
0.0% to 0.3% of the time, so there is nothing to cache, and reusing a token
across *different* states is exactly the predicate `tf_spec__lex` already ports
from `ts_parser__can_reuse_first_leaf`; **marking `tf_parser__split` noinline** —
±0.3%, where marking `tf_spec__replay` alone is worth -11%.

What is left is inherent to replaying a decision. A grammar conflict that cannot
be resolved until the end of a construct keeps the split alive for the whole
construct, and everything in it is built speculatively and then replayed: about
twice the work, and speculative state proportional to the construct rather than
to nesting depth. MiniZinc's array literal is the clearest case -- `v = [...]`
forks at the `[` and cannot settle until the `]` -- so an 800 KB `.mzn` array
runs at 7.7 MB/s and 300 bytes of parser state per input byte, against 70 MB/s
and a couple of megabytes for the same data as `.dzn`, which has no such
conflict. Both time and memory are linear in the construct, not quadratic, and
the branch that shipped the conflict fixes was 5.4 MB/s and 480 bytes per byte,
so this is better than it was and still worse than the incorrect parser it
replaced (12 MB/s, 226 bytes per byte). 99.998% of the speculative advances in
that split happen with a single live head: the alternatives merge almost
immediately and leave one unresolved fork near the bottom of the stack, which
`tf_spec__unique` will not accept and which nothing pops through until the
construct closes. Ending the split there would mean flushing the settled part of
the stack into the real parser mid-split, which moves `p->depth` under the
frontier and under the private-replay capture, so it was not attempted.
`integration/perf/` has a generator for the shape.

## Conflicts

`SHIFT_REPEAT` actions are filtered out once at load, so most apparently-ambiguous
entries turn out to be single-action. What is left forks branches that run until one
survives, picked by `tf_branch__prefer_second`, a port of `ts_parser__select_tree`.
Nothing is decided at the conflict itself: equal dynamic precedence, which is usual
for a declared conflict, makes greedy resolution a coin flip.

Branches merge only on identical stacks, deliberately — tree-sitter can merge on top
state alone because a GSS lets one version stand for many stacks, while a branch here
is kept or discarded whole. The comparison must treat a cell inherited from before
the fork as comparable, not skippable; skipping it handed ties to array order.

## Where tests go

One `tests/` directory, both languages. Rust unless it cannot be: `test_tables.c`
and `tf_diff.c` need tree-sitter's private headers for table entries and production
ids, `test_lexer.c` needs `ts_subtree_parse_state`, `test_utf8.c` needs ICU's
`U8_NEXT`. `test_c_api.c` exists so that using the library from C stays tested.

Adding a grammar also needs an entry in `tests/grammars.h`, a `tests/corpus/<name>/`
directory, and a line in the corpus loop in `CMakeLists.txt`. It must be ABI 15 with
no external scanner.

## Lint and CI

clang-format and clang-tidy pinned to **21.1.6** from PyPI; a distro clang-tidy
reports different things. `tests/` and `tools/` relax three checks in their own
`.clang-tidy`. CI runs C and Rust on three platforms, lint, ASan and UBSan
separately, the packaged crate, and the benchmarks under CodSpeed.

## Not in the repository

`integration/` is gitignored, for trying integrations against other projects.
