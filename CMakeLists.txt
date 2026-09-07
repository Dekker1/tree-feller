cmake_minimum_required(VERSION 3.20)
project(tree_feller
  VERSION 0.1.0
  DESCRIPTION "Streaming LR parsing over tree-sitter parse tables, without building a tree"
  HOMEPAGE_URL "https://github.com/dekker1/tree-feller"
  LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Off by default when embedded, so `add_subdirectory` or FetchContent does not
# drag in libtree-sitter or reach for the network.
if(PROJECT_IS_TOP_LEVEL)
  set(tf_default ON)
else()
  set(tf_default OFF)
endif()
option(TF_BUILD_TESTS "Build the tests (fetches grammars and libtree-sitter)" ${tf_default})
option(TF_INSTALL "Generate install rules" ${tf_default})

include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# The library
#
# No dependencies beyond a C11 compiler. A consumer compiles its own generated
# parser.c and links it against this.

add_library(tree_feller
  lib/src/tf_file.c
  lib/src/tf_language.c
  lib/src/tf_lexer.c
  lib/src/tf_parser.c
  lib/src/tf_visible.c
)
add_library(tree_feller::tree_feller ALIAS tree_feller)

target_include_directories(tree_feller
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/lib/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>

  PRIVATE
    lib/src
    lib/include/tree_feller)
# For compiling a consumer's own generated parser.c, which includes
# "tree_sitter/parser.h". That header is installed under `tree_feller/` so that
# it cannot shadow the copy shipping with a grammar, which means it is not on
# the include path by default -- link this as well if you need it.
add_library(tree_feller_parser_header INTERFACE)
add_library(tree_feller::parser_header ALIAS tree_feller_parser_header)
target_include_directories(tree_feller_parser_header INTERFACE
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/lib/include/tree_feller>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/tree_feller>)
set_target_properties(tree_feller_parser_header PROPERTIES EXPORT_NAME parser_header)

set_target_properties(tree_feller PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  C_VISIBILITY_PRESET hidden
  EXPORT_NAME tree_feller)

# ---------------------------------------------------------------------------
# Install
#
# `include/tree_feller/tree_sitter/parser.h` goes with it: it is the ABI this
# library is written against, and the public header needs it. It is kept under
# `tree_feller/` so including it cannot shadow the copy that ships with a
# consumer's own generated parser.

if(TF_INSTALL)
  include(CMakePackageConfigHelpers)

  install(TARGETS tree_feller tree_feller_parser_header EXPORT tree_fellerTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES lib/include/tree_feller.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(DIRECTORY lib/include/tree_feller DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  install(EXPORT tree_fellerTargets
    FILE tree_fellerTargets.cmake
    NAMESPACE tree_feller::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tree_feller)

  configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/cmake/tree_feller-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/tree_feller-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tree_feller)
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/tree_feller-config-version.cmake
    COMPATIBILITY SameMajorVersion)
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/tree_feller-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/tree_feller-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tree_feller)

  install(FILES LICENSE README.md
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/tree_feller)

  configure_file(cmake/tree_feller.pc.in tree_feller.pc @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/tree_feller.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()

# ---------------------------------------------------------------------------
# Tests
#
# Benchmarks live in `crates/tf-bench` and are run with cargo; what is left
# here is what cannot be done from Rust. Everything below needs grammars, and the differential tests need libtree-sitter
# to compare against. Both are fetched; neither is required to use the library.

if(TF_BUILD_TESTS)
  include(${PROJECT_SOURCE_DIR}/cmake/TreeFellerGrammars.cmake)
  tf_add_grammars()

  include(FetchContent)
  set(BUILD_SHARED_LIBS OFF)
  # Pinned: the reference has to be the runtime whose tables and behaviour this
  # library is written against. See README, "Grammar compatibility".
  FetchContent_Declare(tree-sitter
    URL https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.27.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    URL_HASH SHA256=d35c96e68736bd9569d2757c3cc71052485f33082c3825f1aed9d0e86013a159)
  # Added with EXCLUDE_FROM_ALL so its install rules stay out of ours: the
  # reference runtime is a test dependency, and installing tree-feller must not
  # drop a copy of libtree-sitter next to it.
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    FetchContent_MakeAvailable(tree-sitter)
    set_property(DIRECTORY ${tree-sitter_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)
  else()
    FetchContent_GetProperties(tree-sitter)
    if(NOT tree-sitter_POPULATED)
      FetchContent_Populate(tree-sitter)
      add_subdirectory(${tree-sitter_SOURCE_DIR} ${tree-sitter_BINARY_DIR} EXCLUDE_FROM_ALL)
    endif()
  endif()

  # The reference accessors are in tree-sitter's private headers, so the table
  # test reaches into its source tree rather than using the public API.
  add_library(ts_internal INTERFACE)
  target_link_libraries(ts_internal INTERFACE tree-sitter)
  target_include_directories(ts_internal INTERFACE ${tree-sitter_SOURCE_DIR}/lib/src)

  add_library(tf_testing INTERFACE)
  target_link_libraries(tf_testing INTERFACE tree_feller tf_grammars)
  target_include_directories(tf_testing INTERFACE tests)
endif()

if(TF_BUILD_TESTS)
  enable_testing()

  # Every state x every symbol against libtree-sitter's own accessors, for every
  # grammar. Exhaustive, and the only test that can catch an ABI drift.
  add_executable(test_tables tests/test_tables.c)
  target_link_libraries(test_tables PRIVATE tf_testing ts_internal)
  target_include_directories(test_tables PRIVATE lib/src)
  add_test(NAME tables COMMAND test_tables)

  add_executable(test_lexer tests/test_lexer.c)
  target_link_libraries(test_lexer PRIVATE tf_testing ts_internal)
  target_include_directories(test_lexer PRIVATE lib/src)
  add_test(NAME lexer COMMAND test_lexer)

  add_executable(test_utf8 tests/test_utf8.c)
  target_link_libraries(test_utf8 PRIVATE tree_feller)
  target_include_directories(test_utf8 PRIVATE lib/src ${tree-sitter_SOURCE_DIR}/lib/src)
  add_test(NAME utf8 COMMAND test_utf8)

  # The public C surface, walked from C. Behaviour lives in the Rust tests; this
  # is here so that using the library from C stays a tested configuration.
  add_executable(test_c_api tests/test_c_api.c)
  target_link_libraries(test_c_api PRIVATE tf_testing)
  add_test(NAME c_api COMMAND test_c_api)

  # The acceptance test: the node stream against a walk of the tree real
  # libtree-sitter builds. Also runnable over a directory -- see the README.
  #
  # It walks directories with `dirent.h`, so it is POSIX-only. The library is
  # not: `tf_file.c` has a Windows mapping path, and everything above builds and
  # runs there. Only the corpus tests, which need this tool, are missing.
  if(NOT WIN32)
    add_executable(tf_diff tools/tf_diff.c)
    target_link_libraries(tf_diff PRIVATE tf_testing ts_internal)

    # A corpus per grammar, in this repository, compared node for node against a
    # walk of the tree libtree-sitter builds. Pinned to an exact count rather than
    # "no failures", so a regression shows up and a fix does too.
    #
    foreach(grammar c go regex solidity datazinc minizinc eprime)
      if(NOT DEFINED corpus_failures_${grammar})
        set(corpus_failures_${grammar} 0)
      endif()
      add_test(NAME corpus_${grammar}
               COMMAND tf_diff --grammar ${grammar}
                       --expect-failures ${corpus_failures_${grammar}}
                       "${PROJECT_SOURCE_DIR}/tests/corpus/${grammar}")
    endforeach()

    # And real C, rather than only what was written to be a test: tree-sitter's
    # own runtime, which has been fetched anyway.
    add_test(NAME sources_c
             COMMAND tf_diff --grammar c --expect-failures 0 "${tree-sitter_SOURCE_DIR}/lib/src")

  # The grammars' own tree-sitter test corpora, if a checkout is around. Not
  # fetched: unlike parser.c they are not what tree-feller is coupled to, and
  # they are only published in the grammar repositories, not in the crates.
  set(TF_GRAMMAR_REPO "" CACHE PATH
      "Checkout of shackle-rs/shackle/parsers, for the grammars' own corpora")
  foreach(grammar datazinc eprime minizinc)
      if(TF_GRAMMAR_REPO AND EXISTS "${TF_GRAMMAR_REPO}/tree-sitter-${grammar}/test/corpus")
        add_test(NAME upstream_corpus_${grammar}
                 COMMAND tf_diff --grammar ${grammar} --corpus --expect-failures 0
                         "${TF_GRAMMAR_REPO}/tree-sitter-${grammar}/test/corpus")
      endif()
    endforeach()
  endif()
endif()
