//! tree-feller against libtree-sitter on the same input.
//!
//! This is the comparison the library exists to make, so it is kept runnable
//! rather than left as numbers in a document. libtree-sitter builds a CST and
//! nothing reads it -- the cheapest thing it can be asked to do -- against
//! tree-feller reporting every node.
//!
//! Peak RSS is the other half of the story and is not measured here; a
//! whole-process high-water mark needs one process per measurement. See
//! `RESULTS.md`.
use divan::{counter::BytesCount, Bencher};
use tree_feller::{Child, Language, Node};

fn main() {
    divan::main();
}

const CASES: &[&str] = &["datazinc", "json", "minizinc", "c", "go", "solidity"];

fn source_for(name: &str) -> String {
    // Deliberately smaller than in `parse.rs`: libtree-sitter allocates a
    // subtree per node, and this runs it many times.
    tf_bench::inputs::of(name, 128 * 1024)
}

#[divan::bench(args = CASES)]
fn tree_feller(bencher: Bencher, name: &str) {
    let Some(grammar) = tf_bench::find(name) else {
        return;
    };
    let language = Language::new(grammar).expect("tables load");
    let source = source_for(name);
    bencher
        .counter(BytesCount::of_slice(source.as_bytes()))
        .bench_local(|| {
            language
                .parse(
                    source.as_bytes(),
                    |_: Node<'_>, c: &mut Vec<Child<usize>>| {
                        c.drain(..).map(|k| k.value).sum::<usize>() + 1
                    },
                )
                .unwrap()
        });
}

#[divan::bench(args = CASES)]
fn libtree_sitter(bencher: Bencher, name: &str) {
    let Some(grammar) = tf_bench::find(name) else {
        return;
    };
    let language: tree_sitter::Language = grammar.into();
    let source = source_for(name);
    let mut parser = tree_sitter::Parser::new();
    parser.set_language(&language).expect("language loads");
    bencher
        .counter(BytesCount::of_slice(source.as_bytes()))
        .bench_local(|| parser.parse(source.as_bytes(), None).expect("parses"));
}
