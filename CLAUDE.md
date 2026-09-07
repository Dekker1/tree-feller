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
with one buffer only `tf_lexer__goto` and `tf_lexer__do_advance` move the
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
trailing `tf_lexer__goto` when the position is already right, which it is for
79-100% of tokens and still measured neutral.

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
