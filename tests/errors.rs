//! Malformed input must fail, with a position and a message. There is no
//! recovery: one error, then the parse stops.
//!
//! That the failure lands at or before the byte where the grammar itself first
//! goes wrong is checked against libtree-sitter in `differential.rs`, which has
//! a reference tree to compare against; here the concern is only that a failure
//! is reported at all and is usable.
use tree_feller::{Child, Language, Node, ParseError};

fn parse(source: &str) -> Result<usize, ParseError> {
    let language = Language::new(tree_sitter_c::LANGUAGE).expect("tables load");
    language.parse(
        source.as_bytes(),
        |_: Node<'_>, c: &mut Vec<Child<usize>>| c.drain(..).map(|k| k.value).sum::<usize>() + 1,
    )
}

const MALFORMED: &[&str] = &[
    "int x = ",                    // truncated
    "int a[] = {1, 2",             // unterminated initialiser
    "char *s = \"unterminated",    // unterminated string
    "struct S { int a; ",          // unterminated struct
    "int f(void) { return 0;",     // unterminated body
    "int f(void) { return 0; } }", // stray brace
    "int int x;",                  // a token that lexes but has no action here
    "@",                           // unlexable
];

const WELL_FORMED: &[&str] = &[
    "",
    "// just a comment\n",
    "int x = 1;\n",
    "int a[] = {1, 2, 3};\nchar *s = \"text\";\n",
    "struct S { int a; };\nint f(int b) { return b + 1; }\n",
];

#[test]
fn malformed_input_fails_with_a_position() {
    for source in MALFORMED {
        let error = parse(source).expect_err(&format!("{source:?} was accepted"));
        assert!(
            error.byte as usize <= source.len(),
            "{source:?}: byte {} is past the end",
            error.byte
        );
        assert!(!error.message.is_empty(), "{source:?}: empty message");
        assert!(
            (error.point.row as usize) <= source.matches('\n').count(),
            "{source:?}: row {} is past the end",
            error.point.row
        );
        // `Display` is what a consumer will actually print.
        assert!(!error.to_string().is_empty());
    }
}

#[test]
fn well_formed_input_is_accepted() {
    for source in WELL_FORMED {
        parse(source).unwrap_or_else(|e| panic!("{source:?} rejected: {e}"));
    }
}

#[test]
fn the_error_names_what_was_expected() {
    // On a missing action the valid-token set is read off the state's table row,
    // which is the difference between a usable message and "syntax error".
    let error = parse("int int x;").unwrap_err();
    assert!(
        error.message.contains("expected one of"),
        "unhelpful message: {}",
        error.message
    );
}
