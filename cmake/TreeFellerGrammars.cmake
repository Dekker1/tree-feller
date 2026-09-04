# Grammars for the tests and benchmarks, fetched at configure time.
#
# Generated parsers are not kept in this repository: they belong to their
# grammars, they are large, and vendoring them would make it look as though
# tree-feller is coupled to a particular set. It is coupled to the ABI, not to
# any grammar. Each is pinned by version and checked by hash, so a build is
# reproducible, and the download is cached in the build directory.
#
# Nothing here is needed to *use* tree-feller. A consumer compiles its own
# generated parser.c and links it against the library.

set(TF_GRAMMAR_CACHE "${CMAKE_BINARY_DIR}/_grammars" CACHE PATH
    "Where fetched grammars are unpacked")

# A grammar published on crates.io. The `.crate` archive is a tarball of the
# grammar repository, so the parser is at <name>-<version>/src/parser.c.
function(tf_fetch_crate_grammar name version sha256 out_var)
  set(dir "${TF_GRAMMAR_CACHE}/${name}-${version}")
  set(parser "${dir}/src/parser.c")
  if(NOT EXISTS "${parser}")
    set(archive "${TF_GRAMMAR_CACHE}/${name}-${version}.tar.gz")
    message(STATUS "tree-feller: fetching ${name} ${version}")
    file(DOWNLOAD
      "https://static.crates.io/crates/${name}/${name}-${version}.crate"
      "${archive}"
      EXPECTED_HASH SHA256=${sha256}
      TLS_VERIFY ON
      STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
      list(GET status 1 reason)
      message(FATAL_ERROR "could not fetch ${name} ${version}: ${reason}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${TF_GRAMMAR_CACHE}")
  endif()
  if(NOT EXISTS "${parser}")
    message(FATAL_ERROR "${name} ${version} has no src/parser.c")
  endif()
  set(${out_var} "${parser}" PARENT_SCOPE)
endfunction()

# A grammar taken straight from a repository, by commit. Only the one generated
# file is fetched; the rest of the repository is not wanted.
function(tf_fetch_raw_grammar name url sha256 out_var)
  set(parser "${TF_GRAMMAR_CACHE}/${name}/parser.c")
  if(NOT EXISTS "${parser}")
    message(STATUS "tree-feller: fetching ${name}")
    file(DOWNLOAD "${url}" "${parser}"
      EXPECTED_HASH SHA256=${sha256}
      TLS_VERIFY ON
      STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
      list(GET status 1 reason)
      file(REMOVE "${parser}")
      message(FATAL_ERROR "could not fetch ${name}: ${reason}")
    endif()
  endif()
  set(${out_var} "${parser}" PARENT_SCOPE)
endfunction()

# Builds one static library per grammar, plus `tf_grammars` linking them all.
#
# The set is chosen to cover what the driver has to get right, not to be a
# sample of popular languages:
#
#   c         2015 states, 455 of them dense -- the direct `parse_table` path,
#             plus aliases and 39 fields.
#   go        1442 states but only 29 dense, so almost entirely the packed
#             table; the one grammar here that uses ABI 15 reserved words.
#   regex     137 states and no fields at all, which is the branch
#             `tf_field_map` short-circuits.
#   minizinc  1025 states, 518 dense: the largest, and the one with the most
#             conflicts (12).
#   datazinc  163 states, 2 dense, and exactly one declared conflict -- the
#             smallest case where the speculative split runs at all.
#   eprime    a third grammar from the same generator, as a control.
#
# All are ABI 15 with no external scanner, which is what the driver accepts.
function(tf_add_grammars)
  set(SHACKLE "https://raw.githubusercontent.com/shackle-rs/shackle/03b6430/parsers")

  tf_fetch_crate_grammar(tree-sitter-c 0.24.2
    a9b2eb57a55fed6b00812912e730b7a275cf4fe98bfd6a5d76263d4438371728 c_parser)
  tf_fetch_crate_grammar(tree-sitter-go 0.25.0
    c8560a4d2f835cc0d4d2c2e03cbd0dde2f6114b43bc491164238d333e28b16ea go_parser)
  tf_fetch_crate_grammar(tree-sitter-regex 0.25.0
    bd8a59be9f0ac131fd8f062eaaba14882b2fa5a6a7882a20134cb1d60df2e625 regex_parser)
  tf_fetch_raw_grammar(datazinc "${SHACKLE}/tree-sitter-datazinc/src/parser.c"
    070ea374153f806433b42a43b82fd976a243534e189b5d4dd8126c7b8b0e209a datazinc_parser)
  tf_fetch_raw_grammar(minizinc "${SHACKLE}/tree-sitter-minizinc/src/parser.c"
    b2383631766367c40f63aff26ef62c0c18d38dec673246dfe749ec5662caf495 minizinc_parser)
  tf_fetch_raw_grammar(eprime "${SHACKLE}/tree-sitter-eprime/src/parser.c"
    eabd6e18fb9feb50b258bc5589012d795713fe4ef860a53297b39bbd991298d4 eprime_parser)

  add_library(tf_grammars STATIC
    "${c_parser}" "${go_parser}" "${regex_parser}"
    "${datazinc_parser}" "${minizinc_parser}" "${eprime_parser}")
  # The generated parsers include "tree_sitter/parser.h" and are not warning
  # clean; neither is ours to fix.
  target_include_directories(tf_grammars PRIVATE "${PROJECT_SOURCE_DIR}/include/tree_feller")
  set_target_properties(tf_grammars PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
  target_compile_options(tf_grammars PRIVATE -w)
endfunction()
