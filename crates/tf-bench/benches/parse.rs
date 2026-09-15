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
use tf_bench::{Count, CASES};
use tree_feller::{Language, Options};

fn main() {
    divan::main();
}

const SIZE: usize = 512 * 1024;

/// The tables and the input for one case, or `None` if that grammar is not
/// available in this build.
fn case(name: &str) -> Option<(Language, String)> {
    let language = Language::new(tf_bench::find(name)?).expect("tables load");
    Some((language, tf_bench::inputs::of(name, SIZE)))
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
            // A benchmark that does not parse is broken, not slow.
            language
                .parse(source.as_bytes(), Count { fold: false })
                .unwrap_or_else(|e| panic!("{name}: benchmark input does not parse: {e}"))
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
                .unwrap_or_else(|e| panic!("{name}: benchmark input does not parse: {e}"))
        });
}
