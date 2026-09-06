//! `Visit::hidden` and `Options::named_only`, the two knobs that change what a
//! parse reports.
//!
//! The invariant worth pinning is the same one `tests/test_fold.c` pins on the C
//! side: folding a hidden run changes what a *parent* is handed and nothing
//! else, so the set of visible nodes is identical either way. Declining a fold
//! has to leave the run exactly as it was, which is the fiddly path -- the
//! children have already been taken out of the value slab by then.
use tree_feller::{Child, Language, Node, Options, Visit};

/// Records every node it is shown, and folds hidden runs only when told to.
struct Recorder {
    seen: Vec<(u16, u32, u32)>,
    folds: usize,
    fold: bool,
}

impl Visit<usize> for Recorder {
    fn node(&mut self, node: Node<'_>, children: &mut Vec<Child<usize>>) -> usize {
        let range = node.byte_range();
        self.seen.push((node.symbol(), range.start, range.end));
        children.drain(..).map(|c| c.value).sum::<usize>() + 1
    }

    fn hidden(&mut self, _node: Node<'_>, children: &mut Vec<Child<usize>>) -> Option<usize> {
        if !self.fold {
            return None;
        }
        self.folds += 1;
        Some(children.drain(..).map(|c| c.value).sum())
    }
}

fn run(source: &[u8], fold: bool, named_only: bool) -> (Vec<(u16, u32, u32)>, usize, usize) {
    let language = Language::new(tree_sitter_c::LANGUAGE).unwrap();
    let mut recorder = Recorder {
        seen: Vec::new(),
        folds: 0,
        fold,
    };
    let total = language
        .parse_with(
            source,
            Options::default().named_only(named_only),
            &mut recorder,
        )
        .expect("parses");
    (recorder.seen, recorder.folds, total)
}

impl Visit<usize> for &mut Recorder {
    fn node(&mut self, node: Node<'_>, children: &mut Vec<Child<usize>>) -> usize {
        (**self).node(node, children)
    }
    fn hidden(&mut self, node: Node<'_>, children: &mut Vec<Child<usize>>) -> Option<usize> {
        (**self).hidden(node, children)
    }
}

const SOURCES: &[&str] = &[
    "int x = 1;\n",
    "int a[] = {1, 2, 3, 4, 5};\n",
    "int f(int a, int b, int c) { return a + b + c; }\n",
    "struct S { int a; char *b; double c; };\n",
];

#[test]
fn folding_reports_the_same_visible_nodes() {
    // Not every source has a hidden run of more than one child to fold, so the
    // offer is only required to happen somewhere in the set.
    let mut offered = 0;
    for source in SOURCES {
        let (plain, declined, _) = run(source.as_bytes(), false, false);
        let (folded, taken, _) = run(source.as_bytes(), true, false);
        assert_eq!(declined, 0, "{source:?}: a declined fold was counted");
        offered += taken;
        assert_eq!(plain, folded, "{source:?}: folding changed the node stream");
    }
    assert!(
        offered > 0,
        "nothing in the corpus was ever offered to fold"
    );
}

/// Declining leaves the run in place, so the parent still sees every member and
/// the totals match. This is what breaks if the value slab is not restored.
#[test]
fn declining_a_fold_keeps_every_child() {
    for source in SOURCES {
        let (_, _, plain) = run(source.as_bytes(), false, false);
        let (_, _, folded) = run(source.as_bytes(), true, false);
        assert_eq!(plain, folded, "{source:?}: a child went missing");
    }
}

#[test]
fn named_only_drops_unfielded_anonymous_leaves() {
    let source = b"int a[] = {1, 2, 3, 4, 5};\n";
    let (all, _, _) = run(source, false, false);
    let (named, _, _) = run(source, false, true);
    assert!(
        named.len() < all.len(),
        "named_only reported as many nodes as a full walk"
    );
    // Every node it does report was in the full walk, at the same span.
    for node in &named {
        assert!(all.contains(node), "named_only invented a node: {node:?}");
    }
}
