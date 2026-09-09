//! The same comparison the C harness makes, from the Rust side: tree-feller's
//! node stream against a cursor walk of the tree libtree-sitter builds.
//!
//! Production ids are not compared -- the `tree-sitter` crate does not expose
//! them -- so `tools/tf_diff` remains the stricter of the two. Everything else a
//! consumer can observe is here.
use tree_feller::{Child, Language, LanguageFn, Node};
use tree_sitter::{Parser, TreeCursor};

#[derive(Clone, Debug, PartialEq, Eq)]
struct Record {
    symbol: u16,
    field_id: u16,
    named: bool,
    extra: bool,
    start_byte: u32,
    end_byte: u32,
    start_row: u32,
    start_column: u32,
    end_row: u32,
    end_column: u32,
    child_count: usize,
}

/// Published grammar crates, taken exactly as a consumer would take them.
/// Between them they cover the shapes the driver has to handle: C leans on the
/// dense parse table, Go on the small one and on ABI 15 reserved words, and
/// regex has no fields at all.
#[derive(Clone, Copy)]
enum Grammar {
    C,
    Go,
    Regex,
    SystemVerilog,
}

impl Grammar {
    /// The one constant both parsers are built from, so neither can be pointed
    /// at a different grammar than the other.
    fn language_fn(self) -> LanguageFn {
        match self {
            Grammar::C => tree_sitter_c::LANGUAGE,
            Grammar::Go => tree_sitter_go::LANGUAGE,
            Grammar::Regex => tree_sitter_regex::LANGUAGE,
            Grammar::SystemVerilog => tree_sitter_systemverilog::LANGUAGE,
        }
    }

    fn tree_sitter(self) -> tree_sitter::Language {
        tree_sitter::Language::new(self.language_fn())
    }

    fn tree_feller(self) -> Language {
        Language::new(self.language_fn()).expect("tables should load")
    }
}

/// The reference: every visible node, in post-order.
fn walk(cursor: &mut TreeCursor<'_>, out: &mut Vec<Record>) {
    let node = cursor.node();
    let child_count = node.child_count() as usize;
    if cursor.goto_first_child() {
        loop {
            walk(cursor, out);
            if !cursor.goto_next_sibling() {
                break;
            }
        }
        cursor.goto_parent();
    }
    let start = node.start_position();
    let end = node.end_position();
    out.push(Record {
        symbol: node.kind_id(),
        field_id: cursor.field_id().map_or(0, |id| id.get()),
        named: node.is_named(),
        extra: node.is_extra(),
        start_byte: node.start_byte() as u32,
        end_byte: node.end_byte() as u32,
        start_row: start.row as u32,
        start_column: start.column as u32,
        end_row: end.row as u32,
        end_column: end.column as u32,
        child_count,
    });
}

/// tree-feller reports a node when its parent reduces, so a node under a hidden
/// one arrives before an earlier sibling that is not. Children always precede
/// parents, which is enough to reassemble the tree and walk it in post-order.
fn subject(language: &Language, source: &[u8]) -> Result<Vec<Record>, tree_feller::ParseError> {
    let mut records: Vec<Record> = Vec::new();
    let mut links: Vec<Vec<usize>> = Vec::new();

    let root = language.parse(
        source,
        |node: Node<'_>, children: &mut Vec<Child<usize>>| {
            let mut mine = Vec::with_capacity(children.len());
            for child in children.drain(..) {
                // A node's field belongs to the edge from its parent, so it is only
                // known now.
                records[child.value].field_id = child.field_id;
                mine.push(child.value);
            }
            records.push(Record {
                symbol: node.symbol(),
                field_id: 0,
                named: node.is_named(),
                extra: node.is_extra(),
                start_byte: node.byte_range().start,
                end_byte: node.byte_range().end,
                start_row: node.start_point().row,
                start_column: node.start_point().column,
                end_row: node.end_point().row,
                end_column: node.end_point().column,
                child_count: node.child_count(),
            });
            links.push(mine);
            records.len() - 1
        },
    )?;

    fn flatten(index: usize, records: &[Record], links: &[Vec<usize>], out: &mut Vec<Record>) {
        for &child in &links[index] {
            flatten(child, records, links, out);
        }
        out.push(records[index].clone());
    }
    let mut out = Vec::with_capacity(records.len());
    flatten(root, &records, &links, &mut out);
    assert_eq!(
        records.len(),
        out.len(),
        "visitor emitted nodes outside the selected tree"
    );
    Ok(out)
}

/// Returns false when the reference parser could not parse it either, which is
/// not a result about tree-feller.
fn compare(grammar: Grammar, label: &str, source: &[u8]) -> bool {
    let ts = grammar.tree_sitter();
    let mut parser = Parser::new();
    parser.set_language(&ts).unwrap();
    let tree = parser.parse(source, None).expect("reference parse");

    let language = grammar.tree_feller();
    if tree.root_node().has_error() {
        assert!(
            language
                .parse(source, |_: Node<'_>, c: &mut Vec<Child<()>>| c.clear())
                .is_err(),
            "{label}: accepted input the reference parser could not parse",
        );
        return false;
    }

    let mut expected = Vec::new();
    walk(&mut tree.walk(), &mut expected);
    let actual = subject(&language, source).unwrap_or_else(|e| panic!("{label}: {e}"));

    for (i, (want, got)) in expected.iter().zip(actual.iter()).enumerate() {
        assert_eq!(
            want,
            got,
            "{label}: node {i} ({} vs {})",
            language.symbol_name(want.symbol).unwrap_or("?"),
            language.symbol_name(got.symbol).unwrap_or("?"),
        );
    }
    assert_eq!(expected.len(), actual.len(), "{label}: node count");
    true
}

const C: &[&str] = &[
    "",
    "int x = 1;\n",
    "/* lead */ int main(void) { return 0; }\n",
    "// line comment\n#define FOO 1\n",
    "struct S { int a; char *b; };\n",
    "typedef struct { int x, y; } Point;\n",
    "int a[] = {1, 2, 3};\n",
    "void f(int *restrict p, const char s[static 4]);\n",
    "int g(void) { for (int i = 0; i < 3; i++) { if (i) continue; else break; } return 0; }\n",
    "enum E { A = 1, B, C };\nunion U { int i; float f; };\n",
    "char *s = \"escapes \\\" and \\n\";\n",
    "\u{feff}int bom = 1;\n",
    "int (*fp)(int, int) = 0;\n",
    "int h(int a, int b, int c) { int d = a ? b : c; d = (int)d, d += sizeof(int); return d; }\n",
];

/// Go is the reserved-word case: `MAX_RESERVED_WORD_SET_SIZE` is 25, so the
/// keyword re-lex has to consult the state's reserved set and not just the
/// action table.
const GO: &[&str] = &[
    "package main\n",
    "package main\n\nfunc main() {}\n",
    "package main\n\nimport \"fmt\"\n\nfunc main() { fmt.Println(\"hi\") }\n",
    "package main\n\ntype T struct { A int; B string }\n",
    "package main\n\ntype I interface { M() error }\n",
    "package main\n\nfunc f() (int, error) { return 0, nil }\n",
    "package main\n\nfunc f(x any) { switch v := x.(type) { case int: _ = v; default: } }\n",
    "package main\n\nvar m = map[string][]int{\"a\": {1, 2}}\n",
    "package main\n\nfunc f() { for i := range 3 { go func() { _ = i }() } }\n",
    "package main\n\nconst (\n\tA = iota\n\tB\n\tC\n)\n",
    "package main\n\nfunc f[T comparable](a, b T) bool { return a == b }\n",
    "package main\n\n// comment\nfunc f() { defer func() { recover() }() }\n",
];

const REGEX: &[&str] = &[
    "a",
    "[a-z]+",
    "(foo|bar)?",
    "^\\d{2,4}$",
    "(?i)abc",
    "(?:non)(capturing)",
    "[^\\]]*",
    "a{1,}b*c+",
    "\\p{L}+",
    "(?<name>x)\\k<name>",
];

fn check(grammar: Grammar, label: &str, sources: &[&str]) {
    for (i, source) in sources.iter().enumerate() {
        assert!(
            compare(grammar, &format!("{label} case {i}"), source.as_bytes()),
            "{label} case {i}: the reference parser rejected {source:?}",
        );
    }
}

#[test]
fn c_matches_tree_sitter() {
    check(Grammar::C, "c", C);
}

#[test]
fn go_matches_tree_sitter() {
    check(Grammar::Go, "go", GO);
}

#[test]
fn regex_matches_tree_sitter() {
    check(Grammar::Regex, "regex", REGEX);
}

/// These exercise the large grammar's casts, contextual keywords, assertions,
/// constraints, macros, and visibility/field rules. Require reference acceptance:
/// a fixture becoming invalid must not silently reduce the coverage.
#[test]
fn systemverilog_matches_tree_sitter() {
    check(
        Grammar::SystemVerilog,
        "systemverilog",
        &[
            include_str!("corpus/systemverilog/parameters.sv"),
            include_str!("corpus/systemverilog/casts.sv"),
            include_str!("corpus/systemverilog/generate.sv"),
            include_str!("corpus/systemverilog/assertions.sv"),
            include_str!("corpus/systemverilog/constraints.sv"),
            include_str!("corpus/systemverilog/macro.sv"),
            include_str!("corpus/systemverilog/stream.sv"),
            include_str!("corpus/systemverilog/function.sv"),
            include_str!("corpus/systemverilog/interface.sv"),
            include_str!("corpus/systemverilog/fork.sv"),
            include_str!("corpus/systemverilog/unicode.sv"),
            include_str!("corpus/systemverilog/directive_after_repetition.sv"),
            include_str!("corpus/systemverilog/precedence_pruning.sv"),
        ],
    );
}

#[test]
fn systemverilog_deep_and_wide_matches_tree_sitter() {
    for n in [1, 8, 64, 256] {
        let parameters = (0..n)
            .map(|i| format!("P{i}={i}"))
            .collect::<Vec<_>>()
            .join(", ");
        let cases = [
            format!(
                "module m; initial {}a=1; {}endmodule",
                "begin ".repeat(n),
                "end ".repeat(n)
            ),
            format!("module m; initial a = {}d; endmodule", "b ? c : ".repeat(n)),
            format!("module m #(parameter {parameters})(); endmodule"),
            format!(
                "module m; initial a = {{{}}}; endmodule",
                vec!["b"; n].join(", ")
            ),
            "module m; endmodule\n".repeat(n),
            format!(
                "module m; initial a = {}b{}; endmodule",
                "(".repeat(n),
                ")".repeat(n)
            ),
            format!("module m; initial a = b{}; endmodule", ".c".repeat(n)),
            format!(
                "function void f(); {}endfunction",
                "this.a[i+1].b(); ".repeat(n)
            ),
        ];
        for (i, source) in cases.iter().enumerate() {
            assert!(compare(
                Grammar::SystemVerilog,
                &format!("systemverilog stress {i}, size {n}"),
                source.as_bytes(),
            ));
        }
    }
}

#[test]
fn systemverilog_incomplete_directives_fail_at_eof() {
    for source in [
        include_bytes!("fixtures/systemverilog/pragma_eof.sv").as_slice(),
        include_bytes!("fixtures/systemverilog/line_eof.sv").as_slice(),
    ] {
        assert!(!compare(
            Grammar::SystemVerilog,
            "incomplete directive",
            source
        ));
        let error = Grammar::SystemVerilog
            .tree_feller()
            .parse(source, |_: Node<'_>, children: &mut Vec<Child<()>>| {
                children.clear()
            })
            .expect_err("missing directive argument");
        assert_eq!(error.byte as usize, source.len());
    }
}

// Reduced from the outlier-grammar audit. These assert reference acceptance
// and tree equality for valid inputs, and rejection for malformed inputs.
macro_rules! systemverilog_regression {
    ($name:ident, $fixture:literal, $reason:literal, $valid:expr) => {
        #[test]
        #[doc = $reason]
        fn $name() {
            assert_eq!(
                compare(
                    Grammar::SystemVerilog,
                    stringify!($name),
                    include_bytes!($fixture)
                ),
                $valid,
            );
        }
    };
}

systemverilog_regression!(
    systemverilog_static_call,
    "fixtures/systemverilog/static_call.sv",
    "Regression: scoped call loses hierarchical_identifier",
    true
);
systemverilog_regression!(
    systemverilog_parenthesized_concat,
    "fixtures/systemverilog/parenthesized_concat.sv",
    "Regression: identifier becomes tf_call in parenthesized expression",
    true
);
systemverilog_regression!(
    systemverilog_indexed_method,
    "fixtures/systemverilog/indexed_method.sv",
    "Regression: indexed method receiver chooses a different tree",
    true
);
systemverilog_regression!(
    systemverilog_empty_port,
    "fixtures/systemverilog/empty_port.sv",
    "Regression: second instance after an empty module port list",
    true
);
systemverilog_regression!(
    systemverilog_invalid_range,
    "fixtures/systemverilog/invalid_range.sv",
    "Regression: call expression in part-select",
    false
);
systemverilog_regression!(
    systemverilog_invalid_scope_range,
    "fixtures/systemverilog/invalid_scope_range.sv",
    "Regression: scope expression in part-select",
    false
);

/// Point `TF_CORPUS` at a directory to run the same comparison over every `.c`
/// file in it. Left out of the default run so `cargo test` stays hermetic.
#[test]
fn corpus_matches_tree_sitter() {
    let Some(root) = std::env::var_os("TF_CORPUS") else {
        return;
    };
    let mut checked = 0;
    let mut skipped = 0;
    let mut stack = vec![std::path::PathBuf::from(root)];
    while let Some(path) = stack.pop() {
        let Ok(entries) = std::fs::read_dir(&path) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
            } else if path.extension().is_some_and(|e| e == "c") {
                let Ok(source) = std::fs::read(&path) else {
                    continue;
                };
                if compare(Grammar::C, &path.display().to_string(), &source) {
                    checked += 1;
                } else {
                    skipped += 1;
                }
            }
        }
    }
    println!("{checked} matched, {skipped} not parseable by this grammar");
    assert!(checked > 0, "TF_CORPUS held no parseable .c files");
}

/// Errors must be reported, not silently tolerated, and must carry a position.
#[test]
fn malformed_input_is_rejected() {
    let language = Grammar::C.tree_feller();
    for source in [
        "int x = ",
        "int a[] = {1, 2",
        "char *s = \"unterminated",
        "struct { int a; ",
        "int 1x = 2;",
    ] {
        let result = language.parse(source.as_bytes(), |_: Node<'_>, c: &mut Vec<Child<()>>| {
            c.clear()
        });
        let error = result.expect_err(&format!("{source:?} should not parse"));
        assert!(error.byte as usize <= source.len(), "{source:?}: {error}");
        assert!(!error.message.is_empty(), "{source:?}: empty message");
    }
}

/// A panic in the visitor must not unwind through C.
#[test]
#[should_panic(expected = "visitor exploded")]
fn visitor_panic_is_contained() {
    let language = Grammar::C.tree_feller();
    let _ = language.parse(b"int x = 1;", |_: Node<'_>, _: &mut Vec<Child<()>>| {
        panic!("visitor exploded");
    });
}
