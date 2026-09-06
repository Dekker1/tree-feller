# tree-feller

Fast single-pass parsing with tree-sitter grammars, for programs that read a file once.

tree-sitter's runtime is built for an editor: reparse after every keystroke, keep the
tree between edits, recover from a syntax error and carry on. That costs a ref-counted
subtree per node and memory proportional to the whole input, and it is the right trade
when a human is typing into the file.

It is the wrong trade when a program reads a file once — a build tool loading a
configuration, a batch job over a few hundred megabytes of records, a compiler front
end. None of them edit anything, none want the tree afterwards, and all would rather
fail immediately on bad input than guess.

tree-feller keeps the parts you already have — the generated parse tables and
lexer, which you already have — and replaces the runtime around them with a
one-pass LR driver. Reductions are reported as events and folded straight into
your own values. There is no `TSTree`, no `TSNode`, and nothing is retained
beyond the current nesting depth.

Over a 312 MB corpus of MiniZinc data files, against libtree-sitter building a CST that
nothing reads:

| | throughput | peak RSS |
|---|---:|---:|
| libtree-sitter, CST built and discarded | 6.6 MB/s | 97.7× input |
| **tree-feller, raw reduction stream** | **72.4 MB/s** | **1.2× input** |

63 MB peak on a 61.7 MB file — about 1.3 MB of actual parser state. How much that is
worth depends on the workload; [`crates/tf-bench/RESULTS.md`](crates/tf-bench/RESULTS.md) has the method,
the rest of the numbers, and the cases where tree-feller loses.

## Quick start

```c
#include <stdio.h>
#include <string.h>
#include "tree_feller.h"

const TSLanguage *tree_sitter_c(void);  /* from your grammar's parser.c */

static void *on_node(void *payload, const TFVisibleNode *node) {
  (void)node;
  (*(int *)payload)++;
  return NULL;  /* whatever you return becomes this node's value in its parent */
}

int main(void) {
  TFLanguage *lang = tf_language_load(tree_sitter_c(), NULL);
  const char *source = "int main(void) { return 0; }";

  int count = 0;
  TFVisibleSink sink = {.payload = &count, .on_node = on_node};
  TFError error;
  if (!tf_parse_visible(lang, source, (uint32_t)strlen(source), &sink, NULL, &error)) {
    fprintf(stderr, "%u:%u: %s\n", error.point.row + 1, error.point.column, error.message);
  }
  printf("%d nodes\n", count);  /* 17 */

  tf_language_free(lang);
}
```

Build the library, run the tests:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

The library itself needs nothing but a C11 compiler. Tests and benchmarks fetch grammars
and libtree-sitter at configure time, so the first configure needs network access;
`-DTF_BUILD_TESTS=OFF` skips that.

### Linking it

Installed, or embedded with `add_subdirectory` / `FetchContent` — tests, benchmarks and
install rules all default off when tree-feller is not the top-level project, so nothing
is fetched:

```cmake
# installed
find_package(tree_feller REQUIRED)

# or fetched
include(FetchContent)
FetchContent_Declare(tree_feller
  GIT_REPOSITORY https://github.com/Dekker1/tree-feller.git
  GIT_TAG v0.1.0)
FetchContent_MakeAvailable(tree_feller)

target_link_libraries(app PRIVATE tree_feller::tree_feller)
```

`pkg-config --cflags --libs tree_feller` also works.

Your grammar's generated `parser.c` includes `"tree_sitter/parser.h"`. Every grammar
repository ships that header, so usually you just compile `parser.c` into your own
target. If you do not have it, link `tree_feller::parser_header`, which puts this
repository's copy on the include path — it is installed under `tree_feller/` precisely
so it cannot shadow the one that came with your grammar.

### From Rust

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
taking a slice. The crate does not expose `on_hidden` or `named_only` yet.

## Two ways to read a parse

`tf_parse` gives the **raw reduction stream**: every reduction the grammar performs, in
post-order, children before parents. It is the leanest form and what a purpose-built
consumer wants — you switch on the grammar's own symbols.

`tf_parse_visible` applies tree-sitter's visibility, alias and field rules on top, so
you see the node sequence a CST walk would give you: supertypes and `aux_sym_*_repeat1`
nodes gone, aliases applied, fields resolved. Use it when you are replacing a
`TSTreeCursor` walk.

Both hand you a `void *` per node, which becomes that node's entry in its parent's child
list. There is no tree; the value you build *is* the result.

The visible sink has two options, both off by default. Leave them alone and you get
exactly what a CST walk gives:

- **`on_hidden`** — a hidden rule's children cannot be handed to anyone until the
  nearest *visible* ancestor reduces, because only then is it known what they are
  children of. For the `aux_sym_*_repeat1` behind a long list that ancestor is the whole
  list, so one array of a few million values keeps every member live at once. Set
  `on_hidden` to fold each run as it completes and memory stays proportional to nesting
  depth instead: 1042 MB → 63 MB on one 61.7 MB file.

- **`named_only`** — an anonymous *leaf* that fills no field is not reported. These are
  the punctuation tokens, about half the nodes in a list-heavy file. Anonymous tokens
  that do fill a field, such as an `operator`, are still reported.

## Errors

On a missing action the valid-token set is read off the state's table row. Parsing `a(b`
with the regex grammar:

```
1:3: expected one of {)}, found end of file
```

One error, then the parse stops. There is no recovery and no `ERROR` node: for a program
reading a file, a precise hard failure is more useful than a guess.

Line and column match tree-sitter exactly, which means **only `\n` advances the row** and
**`column` counts bytes, not code points**. Convert if your consumer needs code point
columns.

Where a grammar has a declared conflict, the tables cannot decide with one token of
lookahead, so the driver forks branches that carry no values and runs them until one
survives — the sink still sees exactly one, correct, sequence of events. The winner is
chosen by tree-sitter's own rule (`ts_parser__select_tree`). Splits are bounded; needing
more than 4096 live branches is reported as an error, never as a wrong parse. The
details are in `src/tf_parser.c`.

## Limits

- **Not incremental.** No reparsing, no tree, no `TSNode` API, no queries. One pass.
- **No external scanners.** A grammar with a `scanner.c` is rejected at load. This rules
  out a good number of published grammars — Python, Ruby, Rust, Bash and others.
- **ABI 15 only.** Not 14, not 16. See below.
- **No non-terminal extras.** A grammar with a `0xFFFF` lex state is rejected at load.
- **4 GiB limit.** Byte offsets are `uint32_t`, as tree-sitter's are.
- **In-memory only.** `tf_parse` takes a contiguous buffer; `tf_file_open` maps a file.
  A streaming pull source is designed for but not implemented (see `src/tf_lexer.h`);
  adding it later is a new entry point rather than a rewrite.
- **64-bit only.** The visible filter packs a per-node cell into a pointer and asserts
  `sizeof(void *) >= 8` at compile time.
- **The visible layer is O(widest sibling list)** unless you set `on_hidden`, which the
  Rust crate does not expose yet.
- **Not faster than a good hand-written parser.** Against a Bison parser for the same
  language, both building the same AST, tree-feller came out 1.24× slower. The gap is
  the lexer, not the driver: `ts_lex` is generated *code* whose interface costs two
  indirect calls per input byte, while flex walks a raw pointer held in a register.
  Closing it would mean not using `ts_lex` — a different project. Memory goes the other
  way: 1.2× the input against Bison's 16.8×.
- **`tf_language_load` is not thread-safe**; a loaded `TFLanguage` is read-only and safe
  to share.

## Which tree-sitter

**ABI 15**, as generated by tree-sitter CLI **0.26.x**. `tf_language_load` rejects
anything else rather than reading a struct laid out differently — `TSLanguage` gained and
reordered fields across ABI versions, and reading the wrong one silently yields nonsense.

This is the one coupling that matters, so it is checked three ways: the load-time
`abi_version` check; `lib/include/tree_feller/tree_sitter/parser.h`, which is the ABI-15
header vendored verbatim, so the struct layout compiled against is fixed and visible; and
`tests/test_tables.c`, which walks **every state × every symbol** of all seven test
grammars and asserts agreement with libtree-sitter's own `ts_language_*` accessors.

That vendored header is the only third-party file here, and it cannot reasonably be
removed: it *is* the ABI. It is also why coexistence works — your grammar ships the same
header, both copies share the `TREE_SITTER_PARSER_H_` guard, so whichever is included
first wins.

To move to a new ABI: regenerate the grammars with the matching CLI, update `parser.h`,
bump `TF_ABI_VERSION` in `lib/include/tree_feller.h`, update the pinned libtree-sitter in
`CMakeLists.txt`, and run `ctest`. The table test is what tells you whether the port is
right.

## Testing

`ctest` runs, in order of how much they would catch:

| test | what it covers |
|---|---|
| `tables` | every state × every symbol of seven grammars against libtree-sitter's accessors |
| `lexer` | token stream — symbol and byte span — against the leaves of a real tree |
| `utf8` | 285 million byte sequences against ICU's decoder |
| `corpus_*` | `tests/corpus/<grammar>/`, node for node against a real tree |
| `sources_c` | tree-sitter's own runtime sources, as real-world C |
| `fold` | folding a hidden run reports the same visible nodes as not folding |
| `errors` | malformed input fails, with a position |

`tools/tf_diff` is the acceptance harness and runs over any directory, comparing the node
stream against a post-order walk of the tree libtree-sitter builds — same symbols, byte
spans, field ids and production ids:

```sh
build/tf_diff --grammar c /path/to/some/c/project
build/tf_diff --grammar go /path/to/some/go/project
```

Files the grammar itself rejects are counted separately; that is not a result about
tree-feller. The largest run so far is 18,910 files in one invocation, with no mismatches.

No generated parser lives in this repository. `cmake/TreeFellerGrammars.cmake` fetches
seven at configure time, each pinned by version and checked by SHA-256, chosen for the
shapes they exercise rather than for being popular languages — `c` for the dense
`parse_table` path and 39 fields, `go` for the packed table and reserved words, `regex`
for `FIELD_COUNT 0`, `solidity` for contextual keywords, `minizinc` for the most
conflicts, `datazinc` for the smallest case where the speculative split runs at all, and
`eprime` as a control. They are cached in the build directory, so only the first
configure needs the network.

CI (`.github/workflows/ci.yml`) builds and tests on Linux, macOS and Windows, runs
`cargo test`, lints with `clang-format`/`clang-tidy`/`clippy`, repeats the suite under
AddressSanitizer and UndefinedBehaviorSanitizer, and checks the packaged crate. Windows
builds and tests the library but not `tf_diff` or `tf_bench`, which walk directories with
`dirent.h` and read peak RSS with `getrusage`; the library itself is portable, and
`tf_file.c` has a Windows mapping path.

## Benchmarks

```sh
cargo bench -p tf-bench
```

Deterministic generated inputs — a few megabytes of `.dzn` and JSON, and repeated
source for MiniZinc, C, Go and Solidity — parsed in the two configurations a consumer
would actually pick: the full CST-equivalent view, and named nodes with hidden runs
folded. They run under [CodSpeed](https://codspeed.io) in CI, so a throughput
regression shows up on the pull request.

Grammars come from crates: C, Go and Solidity from crates.io, DataZinc and MiniZinc as
git dependencies on the shackle repository. JSON is the exception — it is published at
ABI 14, which this library does not load, so `tf-bench` regenerates it at ABI 15 with
the pinned CLI. That needs `npx`; without it the JSON benchmarks are skipped rather than
failing the build. TOML, YAML and XML cannot be used at all — they have external
scanners, so no amount of regenerating helps.

## Licence

MIT. The only third-party file is `lib/include/tree_feller/tree_sitter/parser.h`, which is
tree-sitter's own header, also MIT.
