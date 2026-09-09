# tree-feller

Fast, single-pass parsing with tree-sitter grammars for programs that read a file once.

tree-sitter keeps a concrete syntax tree (CST) for incremental editing and error
recovery. tree-feller instead drives the same generated lexer and parse tables once,
reports reductions as they finish, and retains only live parser state. It builds no
`TSTree` or `TSNode` and stops at the first error.

On a 312 MB MiniZinc data corpus:

| | throughput | peak RSS |
|---|---:|---:|
| libtree-sitter, discarded CST | 6.6 MB/s | 97.7× input |
| **tree-feller, raw reductions** | **72.4 MB/s** | **1.2× input** |

That is 63 MB peak for a 61.7 MB file, including about 1.3 MB of parser state. See
[`crates/tf-bench/RESULTS.md`](crates/tf-bench/RESULTS.md) for the method, full results,
and cases where tree-feller loses.

## Quick start

```c
#include <stdio.h>
#include <string.h>
#include "tree_feller.h"

const TSLanguage *tree_sitter_c(void); /* From the grammar's parser.c. */

static void *on_node(void *payload, const TFVisibleNode *node) {
  (void)node;
  (*(int *)payload)++;
  return NULL; /* Becomes this node's value in its parent. */
}

int main(void) {
  TFLanguage *lang = tf_language_load(tree_sitter_c(), NULL);
  const char *source = "int main(void) { return 0; }";
  int count = 0;
  TFVisibleSink sink = {.payload = &count, .on_node = on_node};
  TFError error;

  if (!tf_parse_visible(lang, source, (uint32_t)strlen(source), &sink, NULL, &error))
    fprintf(stderr, "%u:%u: %s\n", error.point.row + 1, error.point.column, error.message);

  printf("%d nodes\n", count); /* 17 */
  tf_language_free(lang);
}
```

Build and test:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

The library needs only a C11 compiler. Tests and benchmarks fetch grammars and
libtree-sitter during the first configure; `-DTF_BUILD_TESTS=OFF` avoids that.

### Link from C or CMake

```cmake
find_package(tree_feller REQUIRED)

# Or embed it:
include(FetchContent)
FetchContent_Declare(tree_feller
  GIT_REPOSITORY https://github.com/Dekker1/tree-feller.git
  GIT_TAG v0.1.0)
FetchContent_MakeAvailable(tree_feller)

target_link_libraries(app PRIVATE tree_feller::tree_feller)
```

`pkg-config --cflags --libs tree_feller` also works. Embedded builds disable tests,
benchmarks, installation, and downloads by default.

Compile your grammar's generated `parser.c` into the application. It normally ships
with `tree_sitter/parser.h`; otherwise also link `tree_feller::parser_header`.

### Use from Rust

```toml
[dependencies]
tree-feller = "0.1"
tree-sitter-c = "0.24" # Or another ABI-15 grammar.
```

```rust
use tree_feller::{Child, Language, Node};

let language = Language::new(tree_sitter_c::LANGUAGE)?;
let nodes: usize = language.parse(
    b"int main(void) { return 0; }",
    |_node: Node<'_>, children: &mut Vec<Child<usize>>| {
        children.drain(..).map(|child| child.value).sum::<usize>() + 1
    },
)?;
```

The visitor's result for each node becomes a child value in its parent. `parse_file`
maps a file. `Options` and `Visit::hidden` configure the visible stream.

## Parse streams

`tf_parse` reports every grammar reduction in post-order. It is the leanest API and
suits consumers that understand the grammar's symbols.

`tf_parse_visible` applies tree-sitter's visibility, alias, and field rules, producing
the sequence seen in a CST walk. Use it to replace a `TSTreeCursor` walk.

Each callback returns a value for the parent. The consumer owns child values when they
arrive. On failure there is no root, so use `on_discard` to reclaim unconsumed values.

`TFVisibleSink` has two optional settings:

- `on_hidden` folds completed hidden runs, keeping memory proportional to nesting depth
  instead of the widest sibling list. In Rust, implement `Visit::hidden`.
- `named_only` omits anonymous leaves with no field, usually punctuation. Anonymous
  tokens that fill a field remain visible.

## Errors

Parsing stops at the first missing action and reports the valid tokens, for example:

```text
1:3: expected one of {)}, found end of file
```

There is no recovery or `ERROR` node. As in tree-sitter, only `\n` advances the row and
columns count bytes. Declared conflicts fork value-free branches until one survives;
more than 4096 live branches is an error.

## Limits

- No incremental parsing, tree, `TSNode` API, queries, or recovery.
- No external scanners; this excludes Python, Ruby, Rust, Bash, and many other grammars.
- ABI 15 only; non-terminal extras are unsupported.
- Inputs must fit in 4 GiB because byte offsets are `uint32_t`.
- Inputs must be contiguous in memory; `parse_file` memory-maps them.
- Without `on_hidden`, the visible layer uses memory proportional to the widest sibling list.
- A good hand-written parser can be faster. In an AST-for-AST test, tree-feller was
  1.17× slower than Bison, but used 1.2× rather than 16.8× the input memory. PGO closed
  the speed gap.
- `tf_language_load` is not thread-safe. A loaded `TFLanguage` is immutable and shareable.

## Grammar compatibility

Use ABI 15 grammars, generated by tree-sitter CLI 0.25 through 0.27.
`tf_language_load` rejects other ABIs because `TSLanguage` layouts differ.

The vendored `lib/include/tree_feller/tree_sitter/parser.h` is tree-sitter's ABI-15
header. A grammar normally supplies the same header; their shared include guard makes
the first one found win.

To update the ABI, regenerate grammars, replace `parser.h`, bump `TF_ABI_VERSION`, update
the pinned libtree-sitter, and run the tests.

## Testing and benchmarks

`ctest` checks table access, tokens, UTF-8, corpus nodes, folding, and errors against
libtree-sitter and ICU. `cargo test` checks the Rust API.

For parser, lexer, or visibility changes, compare any corpus with libtree-sitter:

```sh
build/tf_diff --grammar c /path/to/c/project
```

Files rejected by the grammar are counted separately. The repository fetches eight
hash-pinned ABI-15 grammars; no generated parser is committed. CI tests Linux, macOS,
Windows, sanitizers, lint, and the packaged crate.

Run throughput benchmarks with `cargo bench -p tf-bench`. CodSpeed tracks them in CI.
JSON needs `npx` for ABI-15 regeneration and is skipped when unavailable.

## Licence

MIT. The vendored tree-sitter header is also MIT licensed.
