//! Malformed input must fail, with a position and a message, unless recovery
//! is asked for: then it parses, and the visitor hears about each error first.
//!
//! That the failure lands at or before the byte where the grammar itself first
//! goes wrong, and that a recovered tree is tree-sitter's, is checked against
//! libtree-sitter in `differential.rs`, which has a reference tree to compare
//! against; here the concern is that errors are reported at all and are usable.
use std::sync::OnceLock;
use tree_feller::{Child, ErrorEvent, Language, Node, Options, ParseError, Visit, ERROR_SYMBOL};

fn language() -> &'static Language {
    static LANGUAGE: OnceLock<Language> = OnceLock::new();
    LANGUAGE.get_or_init(|| Language::new(tree_sitter_c::LANGUAGE).expect("tables load"))
}

fn parse(source: &str) -> Result<usize, ParseError> {
    language().parse(
        source.as_bytes(),
        |_: Node<'_>, c: &mut Vec<Child<usize>>| c.drain(..).map(|k| k.value).sum::<usize>() + 1,
    )
}

const MALFORMED: &[&str] = &[
    "int x = ",                    // truncated
    "int a[] = {1, 2",             // unterminated initialiser
    "char *s = \"unterminated",    // unterminated string
    "struct S { int a; ",          // unterminated struct
    "struct { int a; ",            // unterminated anonymous struct
    "int f(void) { return 0;",     // unterminated body
    "int f(void) { return 0; } }", // stray brace
    "int int x;",                  // a token that lexes but has no action here
    "@",                           // unlexable
    "int 1x = 2;",                 // a number where a declarator belongs
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

/// Counts ERROR and MISSING nodes, and records every error event, answering
/// each with `recover`.
struct Recorder {
    recover: bool,
    events: Vec<(u16, std::ops::Range<u32>, Vec<u16>)>,
}

// By reference, so the events are still there after the parse.
impl Visit<usize> for &mut Recorder {
    fn node(&mut self, node: Node<'_>, children: &mut Vec<Child<usize>>) -> usize {
        let own = (node.is_error() || node.is_missing()) as usize;
        children.drain(..).map(|c| c.value).sum::<usize>() + own
    }
    fn error(&mut self, error: ErrorEvent<'_>) -> bool {
        self.events.push((
            error.symbol(),
            error.byte_range(),
            error.expected().to_vec(),
        ));
        self.recover
    }
}

fn recover(source: &str, answer: bool) -> (Result<usize, ParseError>, Recorder) {
    let mut recorder = Recorder {
        recover: answer,
        events: Vec::new(),
    };
    let result = language().parse_with(
        source.as_bytes(),
        Options::default().recover(true),
        &mut recorder,
    );
    (result, recorder)
}

#[test]
fn malformed_input_recovers() {
    for source in MALFORMED {
        let (result, recorder) = recover(source, true);
        let found = result.unwrap_or_else(|e| panic!("{source:?} did not recover: {e}"));
        assert!(found > 0, "{source:?}: no ERROR or MISSING node");
        assert!(!recorder.events.is_empty(), "{source:?}: no error event");
    }
}

#[test]
fn well_formed_input_reports_no_error() {
    for source in WELL_FORMED {
        let (result, recorder) = recover(source, true);
        assert_eq!(result.unwrap_or_else(|e| panic!("{source:?}: {e}")), 0);
        assert!(
            recorder.events.is_empty(),
            "{source:?}: {:?}",
            recorder.events
        );
    }
}

#[test]
fn the_event_names_the_token_and_what_was_expected() {
    // `int int` declares a variable named `int`; it is the `x` that is wrong.
    let (_, recorder) = recover("int int x;", true);
    let (symbol, bytes, expected) = &recorder.events[0];
    assert_eq!(language().symbol_name(*symbol), Some("identifier"));
    assert_eq!(*bytes, 8..9);
    assert_eq!(bytes.start, parse("int int x;").unwrap_err().byte);
    assert!(!expected.is_empty());
    assert!(expected.windows(2).all(|w| w[0] < w[1]), "not ascending");
    assert!(
        !expected.contains(symbol),
        "the rejected token is not expected"
    );
}

#[test]
fn unlexable_bytes_arrive_as_the_error_symbol() {
    let (_, recorder) = recover("@", true);
    assert_eq!(recorder.events[0].0, ERROR_SYMBOL);
}

#[test]
fn declining_stops_with_the_error_recovery_would_not_have_changed() {
    for source in MALFORMED {
        let (result, recorder) = recover(source, false);
        assert_eq!(
            result.unwrap_err(),
            parse(source).unwrap_err(),
            "{source:?}"
        );
        assert_eq!(recorder.events.len(), 1, "{source:?}");
    }
}

#[test]
fn a_panicking_error_callback_is_contained() {
    struct Panics;
    impl Visit<()> for Panics {
        fn node(&mut self, _: Node<'_>, children: &mut Vec<Child<()>>) {
            children.clear();
        }
        fn error(&mut self, _: ErrorEvent<'_>) -> bool {
            panic!("from the error callback")
        }
    }
    let caught = std::panic::catch_unwind(|| {
        language().parse_with(b"int int x;", Options::default().recover(true), Panics)
    });
    assert!(caught.is_err());
}
