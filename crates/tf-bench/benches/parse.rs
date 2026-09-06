//! Throughput benchmarks, run under CodSpeed in CI so a regression shows up as a
//! diff on the pull request rather than as a surprise later.
//!
//! Inputs are generated rather than checked in: a few megabytes of `.dzn` is not
//! something to put in a repository, and generating means the numbers do not
//! move because someone edited a fixture. Each generator is deterministic.
//!
//! The grammars span the two shapes that stress different things -- data files,
//! which are wide and shallow and dominated by the lexer, and programming
//! languages, which are deeper and spend more time in the driver.
//!
//! Building the input and loading the tables happen outside the timed closure,
//! so what is measured is the parse.
use divan::{counter::BytesCount, Bencher};
use tree_feller::{Child, Language, Node, Options, Visit};

fn main() {
    divan::main();
}

/// Counts nodes, and folds hidden runs when asked. Deliberately cheap: the
/// point is to measure the parser, not a consumer.
struct Count {
    fold: bool,
}

impl Visit<usize> for Count {
    fn node(&mut self, _node: Node<'_>, children: &mut Vec<Child<usize>>) -> usize {
        children.drain(..).map(|c| c.value).sum::<usize>() + 1
    }
    fn hidden(&mut self, _node: Node<'_>, children: &mut Vec<Child<usize>>) -> Option<usize> {
        self.fold.then(|| children.drain(..).map(|c| c.value).sum())
    }
}

/// Everything the benchmarks might run against. A grammar `build.rs` could not
/// fetch or generate is skipped rather than failing the run.
const CASES: &[&str] = &["datazinc", "json", "minizinc", "c", "go", "solidity"];

const SIZE: usize = 512 * 1024;

/// The tables and the input for one case, or `None` if that grammar is not
/// available in this build.
fn case(name: &str) -> Option<(Language, String)> {
    let language = Language::new(tf_bench::find(name)?).expect("tables load");
    let source = tf_bench::inputs::of(name, SIZE);
    // A benchmark that does not parse is broken, not slow.
    language
        .parse(source.as_bytes(), Count { fold: false })
        .unwrap_or_else(|e| panic!("{name}: benchmark input does not parse: {e}"));
    Some((language, source))
}

/// What a CST walk gives: every node, punctuation included, nothing folded.
#[divan::bench(args = CASES)]
fn visible(bencher: Bencher, name: &str) {
    let Some((language, source)) = case(name) else {
        return;
    };
    bencher
        .counter(BytesCount::of_slice(source.as_bytes()))
        .bench_local(|| {
            language
                .parse(source.as_bytes(), Count { fold: false })
                .unwrap()
        });
}

/// What a consumer actually asks for: named nodes and fields, with hidden runs
/// folded as they complete. This is the configuration that keeps memory flat.
#[divan::bench(args = CASES)]
fn named_folded(bencher: Bencher, name: &str) {
    let Some((language, source)) = case(name) else {
        return;
    };
    let options = Options::default().named_only(true);
    bencher
        .counter(BytesCount::of_slice(source.as_bytes()))
        .bench_local(|| {
            language
                .parse_with(source.as_bytes(), options, Count { fold: true })
                .unwrap()
        });
}
