# tree-feller

`tree-sitter` builds a tree and then sits with it — keeping it alive, patching it as
the source is edited. `tree-feller` uses the same tables and the same lexer DFA, but no
tree survives the parse: reductions are emitted as events and folded straight into the
consumer's own values, and nothing is retained beyond the current nesting depth.

It is a C11 library with a Rust crate. Point it at any grammar's generated `parser.c`
and it will drive that grammar's parse tables directly — no `TSTree`, no `TSNode`, no
tree-sitter runtime.

```c
#include "tree_feller.h"

const TSLanguage *tree_sitter_json(void);   /* from your grammar's parser.c */

static void *on_node(void *payload, const TFVisibleNode *node) {
  (void)node;
  (*(int *)payload)++;
  return NULL;   /* whatever you return becomes this node's value in its parent */
}

int main(void) {
  TFLanguage *lang = tf_language_load(tree_sitter_json(), NULL);
  int count = 0;
  TFVisibleSink sink = {.payload = &count, .on_node = on_node};
  TFError error;
  if (!tf_parse_visible(lang, "[1, 2, 3]", 9, &sink, NULL, &error)) {
    fprintf(stderr, "%u:%u: %s\n", error.point.row + 1, error.point.column, error.message);
  }
  tf_language_free(lang);
}
```

## Why

tree-sitter's runtime is built for an editor: reparse after every keystroke, keep the
tree around between edits, recover from a syntax error and carry on. That buys a
ref-counted subtree per node and memory proportional to the whole input, and it is the
right trade when a human is typing into the file.

It is the wrong trade when a program reads a file once. A build tool loading a
configuration, a batch job reading a few hundred megabytes of records, a compiler front
end — none of them edit anything, none of them want the tree afterwards, and all of them
would rather fail precisely on bad input than guess. For those you want one pass, no
tree, memory proportional to nesting depth, and a hard failure with a good message.

tree-feller keeps the part of tree-sitter worth keeping — the generated tables and the
lexer, which are excellent and which you already have — and replaces the runtime around
them.

How much that is worth depends on the workload, and it is worth most where the tree
dwarfs the data. Over a 312 MB corpus, comparing libtree-sitter building a CST that
nothing reads against tree-feller reporting the same parse as events:

| | throughput | peak RSS |
|---|---:|---:|
| libtree-sitter, CST built and discarded | 6.6 MB/s | 97.7× input |
| **tree-feller, raw reduction stream** | **72.4 MB/s** | **1.2× input** |

63 MB peak on a 61.7 MB file — about 1.3 MB of actual parser state. See
[`bench/RESULTS.md`](bench/RESULTS.md) for the method, the rest of the numbers, and the
cases where tree-feller loses.

## Two ways to read a parse

`tf_parse` gives the **raw reduction stream**: every reduction the grammar performs, in
post-order, children before parents. It is the fastest and leanest form, and it is what
a purpose-built consumer wants — you switch on the grammar's own symbols.

`tf_parse_visible` applies tree-sitter's visibility, alias and field rules on top, so
what you see is the node sequence you would get from walking the CST — supertypes and
`aux_sym_*_repeat1` nodes gone, aliases applied, fields resolved. Use it when you are
replacing an existing `TSTreeCursor` walk.

Both hand you a `void *` per node, which becomes that node's entry in its parent's
child list. There is no tree; the value you build *is* the result.

### Two options on the visible sink, both off by default

Leave them alone and you get exactly what a CST walk gives.

- **`on_hidden`** — a hidden rule's children cannot be handed to anyone until the
  nearest *visible* ancestor reduces, because only then is it known what they are
  children of. For the `aux_sym_*_repeat1` behind a long list that ancestor is the whole
  list, so a file with one array of a few million values keeps every member live at
  once. Set `on_hidden` to fold each run as it completes and memory stays proportional
  to nesting depth. On one 61.7 MB file this is 1042 MB → 63 MB.

- **`named_only`** — an anonymous *leaf* that fills no field is not reported. These are
  the punctuation tokens, about half the nodes in a list-heavy file. Anonymous tokens
  that do fill a field, such as an `operator`, are still reported.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

The library itself has no dependencies beyond a C11 compiler. Tests and benchmarks
fetch grammars and libtree-sitter at configure time (see below), so they need network
access on the first configure; `-DTF_BUILD_TESTS=OFF -DTF_BUILD_BENCHMARKS=OFF` skips
that entirely.

### Using it from C

Installed:

```cmake
find_package(tree_feller REQUIRED)
target_link_libraries(app PRIVATE tree_feller::tree_feller)
```

Embedded — `add_subdirectory` or `FetchContent`. Tests, benchmarks and install rules
all default off when tree-feller is not the top-level project, so nothing is fetched:

```cmake
include(FetchContent)
FetchContent_Declare(tree_feller
  GIT_REPOSITORY https://github.com/dekker1/tree-feller.git
  GIT_TAG v0.1.0)
FetchContent_MakeAvailable(tree_feller)
target_link_libraries(app PRIVATE tree_feller::tree_feller)
```

Your grammar's generated `parser.c` includes `"tree_sitter/parser.h"`. If you have that
header already — every grammar repository ships one — just compile `parser.c` into your
own target. If you do not, link `tree_feller::parser_header`, which puts this
repository's copy on the include path. It is installed under `tree_feller/` precisely so
that it cannot shadow the one that came with your grammar.

`pkg-config --cflags --libs tree_feller` also works.

### Using it from Rust

```toml
[dependencies]
tree-feller = "0.1"
tree-sitter-c = "0.24"   # or whichever grammar
```

The crate takes whatever a grammar crate exports as `LANGUAGE` — the
`tree_sitter_language::LanguageFn` you would otherwise hand to `tree_sitter::Parser` —
so there is no `extern "C"` block, no `build.rs` of your own, and no `unsafe`:

```rust
use tree_feller::{Child, Language, Node};

let language = Language::new(tree_sitter_c::LANGUAGE)?;
let nodes: usize = language.parse(
    b"int main(void) { return 0; }",
    |_node: Node<'_>, children: &mut Vec<Child<usize>>| {
        children.drain(..).map(|c| c.value).sum::<usize>() + 1
    },
)?;
```

The visitor returns a value per node, which arrives in its parent's `children`. There is
no tree to walk afterwards and nothing to free. `parse_file` maps a file instead of
taking a slice.

The crate does not expose `on_hidden` or `named_only` yet, so it has the memory
behaviour described above for the visible view.

`Cargo.toml` is at the repository root rather than in `rust/` on purpose: `cargo
package` only collects files beneath the manifest, so a manifest inside `rust/` would
publish bindings with no library behind them. This is the layout the tree-sitter grammar
crates use, for the same reason.

## Which tree-sitter

**ABI 15**, as generated by tree-sitter CLI **0.26.x**. `tf_language_load` rejects
anything else rather than reading a struct laid out differently — `TSLanguage` gained
and reordered fields across ABI versions, and reading the wrong one silently yields
nonsense rather than an error.

This is the one coupling that matters, so it is checked three ways:

1. `tf_language_load` refuses a language whose `abi_version` is not 15.
2. `include/tree_feller/tree_sitter/parser.h` is the ABI-15 header, vendored verbatim,
   so the struct layout the library compiles against is fixed and visible.
3. `tests/test_tables.c` walks **every state × every symbol** of six grammars and
   asserts agreement with real libtree-sitter's own `ts_language_*` accessors. If the
   ABI moves under you, this fails loudly and immediately.

To move to a new ABI: regenerate the grammars with the matching CLI, update
`parser.h`, bump `TF_ABI_VERSION` in `include/tree_feller.h`, update the pinned
libtree-sitter in `CMakeLists.txt`, and run `ctest`. Test 3 is the one that tells you
whether the port is actually right.

### The one file this repository does not own

`include/tree_feller/tree_sitter/parser.h` is tree-sitter's, copied unmodified. It is
the only third-party file here, and it cannot reasonably be removed: it *is* the ABI —
the layout of `TSLanguage`, `TSParseAction` and `TSLexer` is what "reads tree-sitter's
tables" means. Generating it, fetching it at build time, or restating it in our own
words would each replace a checkable copy of the contract with a paraphrase of it.

It is also why coexistence works. Your grammar ships the same header, and both copies
share the `TREE_SITTER_PARSER_H_` guard, so whichever is included first wins and the
other is skipped. Ours lives under `tree_feller/` so it is not on the include path
unless you ask for it.

Everything else third-party is gone. UTF-8 decoding used to pull in five ICU headers
(92 KB) for one macro; `src/tf_utf8.h` now spells out the same rules in forty lines,
and `tests/test_utf8.c` checks it against the real ICU macro — inside the fetched
libtree-sitter, not a copy kept here — over all 285 million sequences of up to four
bytes. Generated parsers are fetched, not vendored.

## Details worth knowing

### `SHIFT_REPEAT` actions are dropped

Grammars carry table entries with two actions where one of them is a `SHIFT_REPEAT`.
Those set `shift.repetition`, which exists only for incremental subtree reuse; upstream
skips them outright (`if (action.shift.repetition) break;` in `ts_parser__advance`).
tree-feller filters them out **once at load time**, so the hot path never sees them and
those entries become single-action.

This matters more than it sounds. In one of the test grammars, 67 of 309 action entries
carry two actions — but 56 of those are a `SHIFT_REPEAT` pair, leaving only 11 that are
ambiguous at all. Treating the rest as conflicts would mean speculating constantly for
no reason.

### Genuine conflicts

What survives that filtering is real ambiguity. tree-feller runs a bounded speculative
split at exactly those states, and lets the branches run until one of them dies or they
converge.

Nothing is decided at the conflict itself. Resolving greedily there — taking the higher
dynamic precedence and dropping the other action — is a coin flip whenever the two carry
equal precedence, which for a declared conflict they usually do. So the choice is made
when two branches converge, by the same rule tree-sitter uses in
`ts_parser__select_tree`: higher dynamic precedence, then a structural comparison of
what each branch built, then the one that got there first.

Splits are bounded and must reconverge, because naive GLR is exponential here — in one
grammar, nested live splits give 2, then 4, then 8 heads. Nothing in `src/` knows about
any particular grammar; this is table-driven like everything else.

A worked example, from the grammar this was first written against: its only declared
conflict is whether `[| expr :` begins a column-index header or a row index.

```
[| a: b: | 1, 2 |]     column header
[| 1: 2, 3 |]          row index
```

Both reach the same 11 table entries, all in a single state, and the answer is decided
by the one token *after* the next expression — so no fixed lookahead settles it. That is
exactly the case a bounded split handles and a lookahead hack does not.

### Errors

On a missing action the valid-token set is read off the state's table row and reported
as `expected one of {…}, found <token>`, with byte offset and line/column. One error,
then the parse stops. There is no recovery and no `ERROR` node: for a program reading a
file, a precise hard failure is more useful than a guess.

Line and column match tree-sitter exactly, which means **only `\n` advances the row**
and **`column` counts bytes, not code points**. Convert if your consumer needs code
point columns.

## Caveats

Real limits, not aspirations:

- **Not incremental.** No reparsing, no tree, no `TSNode` API, no queries. One pass.
- **No external scanners.** A grammar with a `scanner.c` is rejected at load. This rules
  out a good number of published grammars — Python, Ruby, Rust, Bash and others.
- **ABI 15 only.** Not 14, not 16.
- **No non-terminal extras.** A grammar with a `0xFFFF` lex state is rejected at load.
- **4 GiB limit.** Byte offsets are `uint32_t`, as tree-sitter's are.
- **In-memory only.** `tf_parse` takes a contiguous buffer; `tf_file_open` maps a file.
  A streaming pull source is designed for but not implemented — the API takes a buffer,
  not a reader, so adding it later is a new entry point rather than a rewrite.
- **64-bit only.** The visible filter packs a per-node cell into a pointer and asserts
  `sizeof(void *) >= 8` at compile time.
- **The visible layer is O(widest sibling list)** unless you set `on_hidden`, and the
  Rust crate does not expose that option yet.
- **Not faster than a good hand-written parser.** Against a Bison parser for the same
  language, both building the same AST, tree-feller came out 1.24× slower. The gap is
  the lexer, not the driver: `ts_lex` is generated *code* whose interface costs two
  indirect calls per input byte (`advance` per byte, `eof` re-evaluated at every state
  transition), while flex walks a raw pointer held in a register. Closing it would mean
  not using `ts_lex` — a different project. Memory goes the other way: 1.2× the input
  against Bison's 16.8×.
- **`tf_language_load` is not thread-safe**; a loaded `TFLanguage` is read-only and safe
  to share.
- **One known wrong-tree bug.** On a Solidity member expression such as `a.b`, the
  visible layer reports an extra `expression` node wrapping the object and puts the
  `object` field on that rather than on the identifier. The production id matches
  tree-sitter's on both sides, so the parse is right and the visibility filter is not.
  No other grammar tested reaches it. `corpus_solidity` is pinned to that one failure so
  it stays visible; the raw reduction stream is unaffected.

## Testing

`ctest` runs, in order of how much they would catch:

| test | what it covers |
|---|---|
| `tables` | every state × every symbol of six grammars against libtree-sitter's accessors |
| `lexer` | token stream — symbol and byte span — against the leaves of a real tree |
| `utf8` | 285 million byte sequences against ICU's decoder |
| `corpus_*` | `tests/corpus/<grammar>/`, node for node against a real tree |
| `sources_c` | tree-sitter's own runtime sources, as real-world C |
| `fold` | folding a hidden run reports the same visible nodes as not folding |
| `errors` | malformed input fails, with a position |

`tools/tf_diff` is the acceptance harness and runs over any directory:

```sh
build/tf_diff --grammar c /path/to/some/c/project
build/tf_diff --grammar go /path/to/some/go/project
```

It compares tree-feller's node stream against a post-order walk of the tree
libtree-sitter builds — same symbols, byte spans, field ids and production ids. Files
the grammar itself rejects are counted separately; that is not a result about
tree-feller. The largest run so far is 18,910 files in one invocation, with no
mismatches.

### Grammars are fetched, not vendored

No generated parser lives in this repository. `cmake/TreeFellerGrammars.cmake` fetches
six at configure time, each pinned by version and checked by SHA-256:

| grammar | states | dense | what it is here for |
|---|---:|---:|---|
| `c` | 2015 | 455 | the direct `parse_table` path, aliases, 39 fields |
| `go` | 1442 | 29 | almost entirely the packed table; ABI 15 reserved words |
| `regex` | 137 | 13 | `FIELD_COUNT 0`, the branch `tf_field_map` short-circuits |
| `solidity` | 977 | 368 | contextual keywords — words that are a keyword in one position and an identifier in another |
| `minizinc` | 1025 | 518 | the largest, and the most conflicts |
| `eprime` | 284 | 2 | another grammar from the same generator, as a control |
| `datazinc` | 163 | 2 | small, and the declared conflict described above |

They are cached in the build directory, so only the first configure needs the network.

## What CI checks

`.github/workflows/ci.yml`, all of it runnable locally:

| job | what it does |
|---|---|
| `c` | configure, build and `ctest` on Linux, macOS and Windows, then install and check the package landed |
| `rust` | `cargo test` on the same three |
| `lint` | `cargo fmt --check`, `cargo clippy -D warnings`, `clang-format --dry-run -Werror`, `clang-tidy` |
| `sanitize` | the whole test suite again under AddressSanitizer and UndefinedBehaviorSanitizer |
| `package` | `cargo package`, then `cargo test` inside the packaged crate |

`.clang-format` and `.clang-tidy` are Google style at 100 columns with
`WarningsAsErrors: '*'`, minus the C++-only checks and with the naming rules restated
for C. `tests/`, `tools/` and `bench/` carry a `.clang-tidy` that relaxes three checks,
because a test may encode a handle in a pointer and may treat running out of memory as
fatal, where the library may not.

Windows builds and tests the library, but not `tf_diff` or `tf_bench`: those walk
directories with `dirent.h` and read peak RSS with `getrusage`. The library itself is
portable — `tf_file.c` has a Windows mapping path.

## Benchmarks

```sh
bench/run.sh file ...
```

One process per (mode, file), because peak RSS is a whole-process high-water mark. The
modes separate parsing from delivering nodes from building values;
[`bench/RESULTS.md`](bench/RESULTS.md) explains what each one measures and what the
numbers mean.

## Layout

```
include/tree_feller.h      the public API, and the only header you include
include/tree_feller/       tree-sitter's ABI-15 parser.h, verbatim
src/tf_language.c          table access: the ports of tree-sitter's accessors
src/tf_lexer.c             TSLexer over a buffer, including the keyword re-lex
src/tf_parser.c            the LR driver
src/tf_visible.c           visibility, alias and field rules over the raw stream
src/tf_file.c              mapping a file
src/tf_utf8.h              UTF-8 decoding, to ICU's rules, checked against them
rust/                      the Rust crate's sources
Cargo.toml                 at the root, so the crate can ship the C library
```

## Licence

MIT. The only third-party file is `include/tree_feller/tree_sitter/parser.h`, which is
tree-sitter's own header, also MIT.
