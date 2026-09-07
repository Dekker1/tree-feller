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
taking a slice. `Options` and `Visit::hidden` give you the two knobs described below.

## Two ways to read a parse

`tf_parse` gives the **raw reduction stream**: every reduction the grammar performs, in
post-order, children before parents. It is the leanest form, and what a purpose-built
consumer wants — you switch on the grammar's own symbols.

`tf_parse_visible` applies tree-sitter's visibility, alias and field rules on top, so you
see the node sequence a CST walk would give you. Use it when you are replacing a
`TSTreeCursor` walk.

Both hand you a `void *` per node, which becomes that node's entry in its parent's child
list. There is no tree; the value you build *is* the result. You own a child's value the
moment you are handed it. If the parse fails there is no root to hand back, so set
`on_discard` and the sink is given every value no parent consumed.

Two options on the visible sink, both off by default:

- **`on_hidden`** — a hidden rule's children cannot be attributed until the nearest
  *visible* ancestor reduces, which for a long list is the whole list. Fold each run as
  it completes and memory stays proportional to nesting depth: 1042 MB → 63 MB on one
  61.7 MB file. In Rust this is `Visit::hidden`.

- **`named_only`** — an anonymous *leaf* that fills no field is not reported: the
  punctuation, about half the nodes in a list-heavy file. Anonymous tokens that do fill a
  field, such as an `operator`, still are.

## Errors

On a missing action the valid-token set is read off the state's table row. Parsing `a(b`
with the regex grammar:

```
1:3: expected one of {)}, found end of file
```

One error, then the parse stops — no recovery, no `ERROR` node. Line and column match
tree-sitter exactly, which means **only `\n` advances the row** and **`column` counts
bytes, not code points**.

Where a grammar has a declared conflict, the driver forks branches that carry no values
and runs them until one survives; the sink still sees exactly one correct sequence of
events, and the winner is chosen by tree-sitter's own rule. Splits are bounded — needing
more than 4096 live branches is an error, never a wrong parse.

## Limits

- **Not incremental.** No reparsing, no tree, no `TSNode` API, no queries. One pass.
- **No external scanners.** A grammar with a `scanner.c` is rejected at load — which
  rules out Python, Ruby, Rust, Bash and many others.
- **ABI 15 only**, and no non-terminal extras. Both are rejected at load.
- **4 GiB limit**, because byte offsets are `uint32_t`, as tree-sitter's are. A longer
  input is refused, not truncated.
- **In-memory only.** A streaming pull source is designed for but not built; see
  `lib/src/tf_lexer.h`.
- **64-bit only**, asserted at compile time.
- **The visible layer is O(widest sibling list)** unless you set `on_hidden`.
- **Not faster than a good hand-written parser.** Against Bison for the same language,
  both building the same AST, 1.17× slower on a 29 MB data file. What is left is the
  lexer: `ts_lex` is generated *code* whose interface costs two indirect calls per input
  byte, where flex walks a pointer in a register. Building with PGO closes it. Memory
  goes the other way — 1.2× the input against Bison's 16.8×.
- **`tf_language_load` is not thread-safe**; a loaded `TFLanguage` is read-only and safe
  to share.

## Which tree-sitter

**ABI 15**, from tree-sitter CLI **0.26.x**. `TSLanguage` gained and reordered fields
across ABI versions, so reading the wrong one yields nonsense rather than an error —
`tf_language_load` refuses anything else. `tests/test_tables.c` walks every state × every
symbol of all seven test grammars against libtree-sitter's own accessors, which is what
would catch a bad port.

`lib/include/tree_feller/tree_sitter/parser.h` is the ABI-15 header, vendored verbatim.
It is the only third-party file here and cannot be removed: it *is* the ABI. Your grammar
ships the same header and both share the `TREE_SITTER_PARSER_H_` guard, so whichever is
included first wins.

To move ABI: regenerate the grammars, update `parser.h`, bump `TF_ABI_VERSION`, update the
pinned libtree-sitter in `CMakeLists.txt`, run `ctest`.

## Testing

`ctest` covers the table accessors against libtree-sitter, the token stream against a real
tree's leaves, UTF-8 against ICU over 285 million sequences, per-grammar corpora node for
node, folding equivalence, and error positions. `cargo test` covers the Rust surface.

`tools/tf_diff` is the acceptance harness and runs over any directory, comparing the node
stream against a post-order walk of the tree libtree-sitter builds — same symbols, byte
spans, field ids and production ids:

```sh
build/tf_diff --grammar c /path/to/some/c/project
```

Files the grammar itself rejects are counted separately. The largest run so far is 18,910
files in one invocation with no mismatches; ten grammars have been checked this way.

No generated parser lives in this repository — `cmake/TreeFellerGrammars.cmake` fetches
seven at configure time, hash-pinned, chosen for the shapes they exercise rather than for
being popular. CI builds and tests on Linux, macOS and Windows, lints, repeats the suite
under ASan and UBSan, and checks the packaged crate.

## Benchmarks

```sh
cargo bench -p tf-bench
```

Generated inputs — `.dzn`, JSON, MiniZinc, C, Go, Solidity — parsed both as a full CST
walk and as named nodes with runs folded. They run under [CodSpeed](https://codspeed.io)
in CI, so a regression shows up on the pull request.
[`crates/tf-bench/RESULTS.md`](crates/tf-bench/RESULTS.md) has the method and the numbers.

JSON is regenerated at ABI 15 by `tf-bench` because it ships at ABI 14; that needs `npx`,
and without it those benchmarks are skipped. TOML, YAML and XML have external scanners, so
no amount of regenerating helps.

## Licence

MIT. `lib/include/tree_feller/tree_sitter/parser.h` is tree-sitter's, also MIT.
