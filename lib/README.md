# tree-feller

Rust bindings for [tree-feller](https://github.com/dekker1/tree-feller): LR parsing
with tree-sitter parse tables, without constructing a public syntax tree.
Conflicts retain private structural alternatives and can replay a prefix to
resolve ties; consumer callbacks receive only the selected result.

```rust
use tree_feller::{Child, Language, Node};

let language = Language::new(tree_sitter_c::LANGUAGE)?;
let nodes: usize = language.parse(
    b"int main(void) { return 0; }",
    |_node: Node<'_>, children: &mut Vec<Child<usize>>| {
        children.drain(..).map(|child| child.value).sum::<usize>() + 1
    },
)?;
# Ok::<_, Box<dyn std::error::Error>>(())
```

Nodes arrive after their children and only live parser state is retained. Grammars must
use ABI 15 and no external scanner. Use `Options` to filter nodes and `Visit::hidden` to
fold long runs. See the repository README for details and limits.

MIT licensed.
