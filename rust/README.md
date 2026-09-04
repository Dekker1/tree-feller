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
it is loaded. See the top-level README for the full list of caveats — in particular,
this crate does not yet expose the `on_hidden` and `named_only` options, so the node
view costs memory proportional to the widest sibling list rather than to nesting depth.

Licensed under the MIT licence.
