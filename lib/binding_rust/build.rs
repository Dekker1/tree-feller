//! tree-feller is C, and so is every grammar it reads, so a C compiler is needed
//! either way; this crate binds the library rather than reimplementing it.
use std::path::PathBuf;

fn main() {
    // The manifest sits beside the C library, in `lib/`.
    let root = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let src = root.join("src");

    // Directories are scanned recursively, so this covers every source and
    // header, including the internal ones tf_parser.c includes.
    println!("cargo:rerun-if-changed={}", src.display());
    println!("cargo:rerun-if-changed={}", root.join("include").display());

    let mut build = cc::Build::new();
    build
        .include(root.join("include"))
        .flag_if_supported("-std=c11")
        .warnings(true);
    for name in [
        "tf_file.c",
        "tf_language.c",
        "tf_lexer.c",
        "tf_parser.c",
        "tf_visible.c",
    ] {
        build.file(src.join(name));
    }
    build.compile("tree_feller");
}
