# Notes for agents

The README covers what this is and how to build it. This is the rest.

## Invariants

- **`lib/src/` knows about no grammar.** Everything is driven from the tables. If a
  fix needs to name a rule, it is the wrong fix.
- **`lib/src/tf_language.*` and `lib/src/tf_lexer.c` are transcriptions** of tree-sitter's
  runtime, and the `file.c:line` references in their comments are the spec. The
  fetched source is at `build/_deps/tree-sitter-src/lib/src/` — read it rather
  than reasoning about what it probably does. Don't tidy these into something
  that no longer matches upstream.
- **`TFToken` and the sink structs are public.** Internal per-token state goes on
  `TFLexer`, which is already copied per branch during a split.

## Verifying a change

`ctest` is necessary and not sufficient. The acceptance test is `tools/tf_diff`,
which compares the node stream against a real libtree-sitter walk:

```sh
build/tf_diff --grammar c ~/some/c/project
build/tf_diff --grammar datazinc ~/Code/github.com/minizinc/mzn-challenge
```

Any change to the parser, lexer or visibility layer gets run over a large corpus
before you believe it. `mzn-challenge` and `minizinc-benchmarks` under
`~/Code/github.com/minizinc/` are ~19k files and take a few minutes.

Files a grammar itself rejects are counted separately and are not a result about
tree-feller.

## Traps that have cost real time

- **Stale probe binaries.** A scratch program linked against
  `build/libtree_feller.a` does *not* pick up `cmake --build`. Relink it. Two
  wrong diagnoses in this repo's history came from a probe reporting the previous
  build's behaviour.
- **`ts_parser_set_logger` is public.** For "why does tree-sitter do X here",
  dumping its parse trace and diffing against ours beats reading tables.
- **Benchmark numbers drift with machine state.** Only compare A/B measured
  back-to-back in the same session; a number from an hour ago is not a baseline.
- **Linux finds things macOS does not.** glibc declares `memcpy` non-null, so a
  zero-length copy from a null pointer passes on macOS and fails on Linux — under
  UBSan *and* under clang-tidy, whose `NonNullParamChecker` only fires there.
  Pinning the tool version does not make the platforms agree. Sanitizers also run
  separately, ASan and UBSan, as CI does. To check a lint or sanitizer result the
  way CI will see it, run it in a container rather than pushing to find out:

  ```sh
  podman run --rm -v "$PWD":/w:ro ubuntu:24.04 bash -c '
    apt-get update -qq && apt-get install -y -qq python3-venv cmake ninja-build gcc git
    cp -r /w /tmp/repo && cd /tmp/repo && python3 -m venv /tmp/v
    /tmp/v/bin/pip install -q clang-tidy==21.1.6
    cmake -S . -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    git ls-files "src/*.c" "tools/*.c" "tests/test_*.c" "bench/*.c" \
      | xargs /tmp/v/bin/clang-tidy -p build --warnings-as-errors="*"'
  ```
- **Field order in `TFLexer` is load-bearing** for the hot loop. Add to the end.
- **Offering a fold is O(run).** `on_hidden` hands over every child in the run,
  and a repetition's run grows by one each time it reduces, so re-offering a
  symbol after it declined is quadratic across the list. The filter caches the
  refusal per symbol; do not "simplify" that away. It was worth 515x on a data
  file, and the benchmarks are what found it.

## Benchmarks

`cargo bench -p tf-bench`, or `cargo codspeed run` as CI does. Inputs are
generated, not checked in, so numbers do not move because someone edited a
fixture. Grammars that are not published as crates are fetched in `build.rs`;
JSON is regenerated at ABI 15 there because it ships at ABI 14, which needs
`npx` -- without it those benchmarks are skipped rather than failing.

## Performance

The dispatch loop in `tf_parse` and `tf_lexer_next` are the hot path; ~60% is
lexing, most of that inside the generated `ts_lex`, which is not ours to speed
up. Anything on the `count == 0` path costs nothing. Table lookups go through
the parse table expanded at load (`tf_lookup`), never the packed scan — that
expansion was a 2.5x win and should not be undone.

## Source map

Beyond what the names give away: `tf_language.*` is table access, `tf_lexer.c` is
a `TSLexer` over a buffer including the keyword re-lex, `tf_parser.c` is the LR
driver and the branch machinery, `tf_visible.c` applies tree-sitter's visibility,
alias and field rules to the raw reduction stream, `tf_utf8.h` decodes UTF-8 to
ICU's rules — `tests/test_utf8.c` holds it to that over 285M sequences, so do not
"simplify" it.

No generated `parser.c` lives in this repository, deliberately. Don't vendor one.

## Conflicts

`SHIFT_REPEAT` actions are filtered out **once at load**, so the hot path never
sees them and most apparently-ambiguous entries turn out to be single-action.
Multi-action entries fork branches that run until one survives; the winner is
picked by `tf_branch__prefer_second`, a port of `ts_parser__select_tree`. Nothing
is decided at the conflict itself: resolving greedily on dynamic precedence is a
coin flip when the actions carry equal precedence, which for a declared conflict
they usually do.
Branches merge only on identical stacks, deliberately: tree-sitter merges heads
on top state and position because a GSS lets one version stand for many stacks,
while a branch here is kept or discarded whole. Loosening it breaks grammars.
The comparison must treat a cell inherited from before the fork as comparable,
not skippable — skipping it silently handed ties to array order.

## Adding a grammar

Hash-pinned in `cmake/TreeFellerGrammars.cmake`; also needs an entry in
`tests/grammars.h`, a `tests/corpus/<name>/` directory and a line in the corpus
loop in `CMakeLists.txt`. It must be ABI 15 with no external scanner, or
`tf_language_load` refuses it — which rules out most published grammars.

## CI

Jobs: `c` and `rust` on Linux, macOS and Windows; `lint`; `sanitize` (ASan and
UBSan separately); `package`, which builds and tests the crate as published.
Windows builds the library but not `tf_diff` or `tf_bench`, which need `dirent.h`
and `getrusage`.

## Lint

Pinned to clang-format and clang-tidy **21.1.6**, installed from PyPI. To
reproduce CI exactly, `pip install clang-format==21.1.6 clang-tidy==21.1.6` in a
venv; a distro clang-tidy will report different things. `tests/`, `tools/` and
`bench/` have their own `.clang-tidy` relaxing three checks.

## Not in the repository

`integration/` is gitignored. It can be used to place projects to test possible
integrations.
