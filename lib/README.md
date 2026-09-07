# tree-feller

Streaming LR parsing over tree-sitter parse tables, without building a tree.

Rust bindings for the [tree-feller](https://github.com/dekker1/tree-feller) C library.
Hand it whatever a grammar crate exports as `LANGUAGE` and it drives that grammar's
parse tables directly — no `Tree`, no `Node`, no tree-sitter runtime. Nodes are
reported as they complete, children before parents, and nothing is retained beyond the
current nesting depth.

```rust
use tree_feller::{Child, Language, Node};

let language = Language::new(tree_sitter_c::LANGUAGE)?;
let nodes: usize = language.parse(
    b"int main(void) { return 0; }",
    |_node: Node<'_>, children: &mut Vec<Child<usize>>| {
        children.drain(..).map(|c| c.value).sum::<usize>() + 1
    },
)?;
# Ok::<_, Box<dyn std::error::Error>>(())
```

The grammar must be ABI 15 and must not use an external scanner; both are checked when
it is loaded. `Options` and `Visit::hidden` control what is reported and whether long runs are
folded as they complete; see the top-level README for the limits.

Licensed under the MIT licence.
