//! tree-feller is C, and so is every grammar it reads, so a C compiler is needed
//! either way; this crate binds the library rather than reimplementing it.
use std::path::PathBuf;

fn main() {
    // The manifest sits beside the C library, in `lib/`.
    let root = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let src = root.join("src");

    let mut build = cc::Build::new();
    build
        .include(root.join("include"))
        .include(root.join("include/tree_feller"))
        .flag_if_supported("-std=c11")
        .warnings(true);
    for name in [
        "tf_file.c",
        "tf_language.c",
        "tf_lexer.c",
        "tf_parser.c",
        "tf_visible.c",
    ] {
        let path = src.join(name);
        println!("cargo:rerun-if-changed={}", path.display());
        build.file(path);
    }
    println!(
        "cargo:rerun-if-changed={}",
        root.join("include/tree_feller.h").display()
    );
    build.compile("tree_feller");
}
