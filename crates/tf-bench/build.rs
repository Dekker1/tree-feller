//! The JSON grammar, regenerated at ABI 15.
//!
//! Every other grammar the benchmarks use is a crate: C, Go and Solidity from
//! crates.io, DataZinc and MiniZinc as git dependencies on the shackle
//! repository. JSON cannot be, because it is published at ABI 14 and this
//! library only loads 15 -- so its `grammar.js` is fetched and regenerated with
//! the pinned CLI.
//!
//! That needs `npx`. Without it the JSON benchmarks are skipped rather than
//! failing the build, so `cargo bench` still works offline.
use std::path::{Path, PathBuf};
use std::process::Command;

const GRAMMAR: &str =
    "https://raw.githubusercontent.com/tree-sitter/tree-sitter-json/v0.24.8/grammar.js";
const CLI: &str = "tree-sitter-cli@0.26.12";

/// ABI 15 needs a `tree-sitter.json` beside the grammar, which the fetched file
/// does not come with.
const MANIFEST: &str = r#"{
  "grammars": [{"name": "json", "camelcase": "Json", "scope": "source.json", "file-types": ["json"]}],
  "metadata": {"version": "0.24.8", "license": "MIT", "description": "JSON grammar", "authors": [{"name": "tree-sitter"}]}
}"#;

fn generate(dir: &Path) -> Option<PathBuf> {
    let parser = dir.join("src/parser.c");
    if parser.exists() {
        return Some(parser);
    }
    std::fs::create_dir_all(dir).ok()?;
    let ok = Command::new("curl")
        .args(["-sSL", "--fail", "-o"])
        .arg(dir.join("grammar.js"))
        .arg(GRAMMAR)
        .status()
        .map(|s| s.success())
        .unwrap_or(false);
    if !ok {
        return None;
    }
    std::fs::write(dir.join("tree-sitter.json"), MANIFEST).ok()?;
    let ok = Command::new("npx")
        .args(["--yes", CLI, "generate", "--abi", "15", "grammar.js"])
        .current_dir(dir)
        .status()
        .map(|s| s.success())
        .unwrap_or(false);
    (ok && parser.exists()).then_some(parser)
}

fn main() {
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    match generate(&out.join("json")) {
        Some(parser) => {
            cc::Build::new()
                .warnings(false)
                .include("../../lib/include/tree_feller")
                .file(parser)
                .compile("tf_bench_json");
            println!("cargo:rustc-cfg=have_json");
        }
        None => println!(
            "cargo:warning=could not generate the JSON grammar (needs npx and network); \
             its benchmarks are skipped"
        ),
    }
    println!("cargo:rustc-check-cfg=cfg(have_json)");
    println!("cargo:rerun-if-changed=build.rs");
}
