# Phase 6 benchmarks

`Apple M4`, macOS 26.6.2, 24 GB. Release builds (`-O3`). libminizinc at `5aa4f78a4`, its
own Release build; the reference libtree-sitter is v0.26.12, unmodified. See "About the
reference" below -- unmodified is not the same as fastest.

Reproduce with `bench/run.sh <file>...`. One process per (mode, file), because
`ru_maxrss` is a whole-process high-water mark.

## Corpus

The 12 largest distinct `.dzn` files in `mzn-challenge` and `minizinc-benchmarks`,
deduplicated by content hash (the challenge repeats instances across year
directories): 311.9 MB total, largest 61.7 MB.

Two files that would otherwise be in the top 14 are excluded:
`crosswords/grid-05.04_dict-80.dzn` and `grid-19.05_dict-80.dzn`. They contain
array comprehensions, which the DataZinc grammar has no rule for, so they are not
DataZinc at all. `parser_ts.cpp` still handles them, by parsing the whole file a
second time with the MiniZinc grammar (`fall_back_to_model_grammar`) -- at
1.9 MB/s and 1880 MB for a 5.7 MB input. Timing that against parsers which are
not doing two parses would not have measured anything.

## Modes

| mode | what it does |
|---|---|
| `bison` | libminizinc's Bison parser, full parse into a `Model` |
| `mzn-ts` | `parser_ts.cpp` data path, full parse into a `Model`. Uses libminizinc's own vendored libtree-sitter, which carries the `repeat_depth` patch, so this row is unaffected by the note below |
| `ts-parse` | libtree-sitter builds the CST; nothing reads it |
| `ts-walk` | ...plus a full `TSTreeCursor` walk reading each node's kind, field and span |
| `feller` | tree-feller reporting the same visible nodes, no tree |
| `feller-fold` | ...with `on_hidden`, folding each hidden run as it completes |
| `feller-named` | ...and skipping punctuation a consumer does not read |
| `feller-raw` | the underlying reduction stream, without the visibility layer |
| `feller-null` | the same parse with no sink: driver and lexer alone |

`bison` and `mzn-ts` build an AST; **none of the others do** -- they count what
they are handed and build nothing, so every comparison against those two is
measuring less work on the tree-feller side. `feller` against
`ts-walk` is the like-for-like pair -- both hand over every visible node with its
field and span.

## Aggregate, 12 files, 311.9 MB

```
mode         files  total MB  total s    MB/s  peak RSS  RSS/input
bison           12     311.9     5.60    55.7      436M      16.8x
mzn-ts          12     311.9    70.62     4.4     4825M     110.5x
ts-parse        12     311.9    46.95     6.6     4493M      97.7x
ts-walk         12     311.9   327.57     1.0     4496M      97.7x
feller          12     311.9     6.50    48.0     1042M      18.2x
feller-fold     12     311.9     6.65    46.9       63M       1.2x
feller-raw      12     311.9     4.31    72.4       63M       1.2x
feller-null     12     311.9     4.18    74.6       63M       1.2x
feller-named    12     311.9     6.21    50.2       63M       1.2x
```

`RSS/input` is the worst per-file ratio of peak RSS to input size.

## Largest single file, 61.7 MB

| mode | time | throughput | peak RSS |
|---|---|---|---|
| `bison` | 1.058 s | 58.3 MB/s | 436.5 MB |
| `mzn-ts` | 15.621 s | 3.9 MB/s | 4825.3 MB |
| `ts-parse` | 11.467 s | 5.4 MB/s | 4555.2 MB |
| `ts-walk` | 15.747 s | 3.9 MB/s | 4537.3 MB |
| `feller` | 1.259 s | 49.0 MB/s | 1042.1 MB |
| `feller-fold` | 1.262 s | 48.9 MB/s | 63.1 MB |
| `feller-raw` | 0.846 s | 73.0 MB/s | 63.1 MB |
| `feller-null` | 0.828 s | 74.5 MB/s | 63.1 MB |
| `feller-named` | 1.187 s | 52.0 MB/s | 63.1 MB |

## About the reference

The reference is stock libtree-sitter v0.26.12. It used to be patched, and the patch is
worth knowing about because it moves `ts-walk` by a factor of five.

`Subtree.repeat_depth` is a `uint16_t`. It counts how deep a repetition has nested, and
the parser uses it to rebalance long repetitions into a shallow tree. Past 65536
elements it wraps, rebalancing stops, and the tree stays thousands of levels deep --
which makes every cursor field lookup walk thousands of ancestors. Widening it to
`uint32_t` is a one-line change, and libminizinc carries exactly that patch downstream.

Measured both ways over the same 12 files:

| | stock | `repeat_depth` widened |
|---|---:|---:|
| `ts-parse` | 6.6 MB/s | 5.8 MB/s |
| `ts-walk` | 1.0 MB/s | 4.5 MB/s |

Rebalancing costs work, so *parsing* is 14% faster without it; *walking* is 4.7× slower.
On the 61.7 MB file the walk goes from 14.7 s to 148 s, which is the quadratic blow-up.

The numbers above are the stock ones, because that is what this repository builds and
what anyone re-running gets. Two things follow. `ts-walk` at 1.0 MB/s is a property of
unpatched tree-sitter on inputs with very long repetitions, not of tree-sitter generally
-- a real consumer hitting this patches it. And the headline comparison against
`ts-parse` is now made against the *faster* reference, which is the direction an honest
comparison should err in.

## What the numbers say

**The premise holds.** The tree-sitter path is **12.7x slower than Bison**
(4.4 against 55.7 MB/s) and uses **11x the memory**, despite parsing the narrow
data-only grammar while Bison handles the whole language.

**The cost is subtree allocation, not the walk.** `ts-parse` alone runs at
6.6 MB/s and already holds 97.7x the input in memory -- so the tree is built and
paid for before anything has read a single node. Whether the walk on top is cheap
or ruinous depends on `repeat_depth` (see above), but it is the wrong question:
`parser_ts.cpp` runs at 4.4 MB/s against a *patched* reference, and even a walk
that cost nothing at all could not have closed the gap to Bison. The tree has to
stop being built.

**Not building it works.** `feller-raw` runs at 72.4 MB/s in 1.2x the input size:
63 MB peak on a 61.7 MB file, so about 1.3 MB of actual parser state. That is the
O(nesting depth) claim, measured. Against `ts-parse` -- the same job, a parse of
the same file with nothing read afterwards -- that is **11x the throughput in
1/81st of the memory**.

## Why the first attempt was slower than Bison

The driver initially ran at 29.3 MB/s, below Bison's 51.4. A sampling profile put
**38% of the time in `tf_actions`** -- the parse-table lookup -- against 23% in
the lexer.

tree-sitter stores a state's row one of two ways. States with many entries get a
dense row in `parse_table`, indexed directly. Everything else is packed into
`small_parse_table` as unsorted `(value, symbol...)` groups, which
`ts_language_lookup` (`language.h:78-92`) can only search by linear scan.

The generator decides per state, by size. **So the stricter the grammar, the worse
this gets**: DataZinc emits 2 dense rows out of 163, meaning essentially every
lookup was a scan, while MiniZinc gets 518 of 1025. Reusing tree-sitter's tables
unchanged meant inheriting a representation tuned for an editor's working set, on
the grammar least suited to it. Bison, by contrast, resolves a state with a
single indexed probe into `yytable`/`yycheck`.

The fix is to expand the packed table once at load: `state_count * symbol_count`
16-bit cells, 25 KB for DataZinc and 574 KB for MiniZinc, filled in one pass over
the packed form. The lookup becomes an indexed load. The packed scan is kept as
`tf_lookup_packed`, since it is the definition the expansion is built from and
what the exhaustive table test checks against.

Result: **29.3 -> 66.3 MB/s**, and the lookup fell from 38% of samples to 5%.
Lexing is now the dominant cost at 52%, which is the same generated DFA
tree-sitter runs.

## The visibility layer, and `on_hidden`

`feller` reports what a CST walk would: every visible node with its field and
span. That cost 30% throughput and **16x memory** (63 MB -> 1042 MB) over the raw
stream, which is worse than it sounds -- it is not a constant factor but a change
of shape.

The reason is structural. A hidden rule's children cannot be handed to anyone
until the nearest *visible* ancestor reduces, because only then is it known what
they are children of. For `aux_sym_array_literal_repeat1` that ancestor is the
whole array, so a `.dzn` holding one array of a few million values keeps every
member live at once: O(widest sibling list), not O(nesting depth).

`TFVisibleSink.on_hidden` lets a consumer fold such a run as it completes,
collapsing it to a single value. That is what a consumer building an array does
anyway. Folding is offered only for a run of more than one child whose symbol is
hidden *and* which no production can alias into visibility (`alias_map`), so a
node can never be folded away and then turn out to have been visible. Returning
NULL declines, per node; leaving the callback NULL reproduces a CST walk exactly.

`feller-fold` is the result: **1042 MB -> 63 MB**, and faster too (36.4 -> 41.9
MB/s) for not copying the runs around. `tests/test_fold.c` pins the invariant that
makes this safe -- the sequence of visible nodes, with spans, is identical folded
or not, across all three grammars including the two that use aliases.

## Second round: closing on Bison

The driver floor was 66.3 MB/s and `feller-fold` 41.9, against Bison's 55.7. Four
changes, all measured one at a time:

- **Flatten the field maps.** `tf_field_map` hands back a list per production and
  resolving one child meant scanning it -- once per value on a data file. Same
  medicine as the parse table: a `production x child_index` rectangle, 492 bytes
  for DataZinc.
- **Guard two `memcpy`s.** The trailing-extras copy in `tf_parser__reduce` and the
  scratch-to-arena copy in the filter both run once per reduction and are both
  almost always zero-length. The call was costing more than the copy.
- **`named_only`.** Half the nodes in an array literal are commas. A consumer
  reads a tree-sitter tree through named children and fields, so an anonymous
  leaf filling no field need not be reported at all. `feller-named` is this.
- **Pack the filter's stack cell into the pointer.** The filter allocated a
  48-byte cell per shift *and* per reduce -- four per array member -- to hold what
  `TFNode` already carries plus two fields. Those two fit in the `void *` the
  driver stores anyway, which removed the cell, the block allocator and the free
  list.

Floor 66.3 -> 74.6 MB/s, `feller-fold` 41.9 -> 46.9, and `feller-named` at 50.2.

## Why it stops there

The floor is now **59% lexing**, and that is close to irreducible without giving
up the premise of the project. `ts_lex` is generated *code*, not a table, and its
interface costs two indirect calls per input byte: `lexer->advance()` once per
byte, and `lexer->eof()` re-evaluated at every state transition, because
`START_LEXER` re-enters the switch through `next_state:`. Neither call can be
inlined, so the lexer's position has to be reloaded from memory across each one.
Flex, which is what Bison is paired with here, walks a raw pointer held in a
register.

So the remaining gap is not the driver -- table lookup is down to a few percent
and the filter is now thin -- it is that tree-sitter's lexer interface is
per-byte virtual. Fixing it means not using `ts_lex`, which means re-deriving the
tokenizer rather than reusing the generated artifact. That is a different project
and it should be a deliberate decision, not something snuck in for a benchmark.

## Where it stands

| | builds an AST | throughput | peak RSS |
|---|---|---|---|
| Bison | yes | 55.7 MB/s | 16.8x input |
| `parser_ts.cpp` | yes | 4.4 MB/s | 110.5x input |
| tree-feller, named nodes and fields, folding | **no** | 50.2 MB/s | 1.2x input |
| tree-feller, raw reductions | **no** | 72.4 MB/s | 1.2x input |

Against the implementation it replaces: **11x the throughput and 77x less memory**.
That comparison is sound in the direction it is used -- `parser_ts.cpp` is slower
*and* fatter while doing strictly more, so removing the tree is a clear win.

Against Bison it is not sound yet, and the difference is not small. Bison's
55.7 MB/s includes constructing a complete libminizinc AST: allocating nodes,
converting integer and float literals, interning identifiers. tree-feller's
50.2 MB/s includes none of that -- the benchmark sink adds symbol ids and field
ids into a checksum and returns. So tree-feller is **11% behind while doing
substantially less work**, and the real deficit is larger by however much a
loader costs.

The AST-to-AST number is the one that decides this, and it is not measured. It
needs the worked `.dzn` loader driven from reduce events, compared through
`tests/parse_diff.cpp`. Until then the only claim these numbers support is the one
about `parser_ts.cpp`, plus the memory result -- 1.2x the input against Bison's
16.8x -- which does hold regardless of what is built on top, because it is a
property of not retaining a tree.
