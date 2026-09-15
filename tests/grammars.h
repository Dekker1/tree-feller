// The grammars the tests and tools run against.
//
// None of these live in this repository; CMake fetches each generated parser.c
// at configure time (cmake/TreeFellerGrammars.cmake). They are chosen for the
// shapes they exercise, not for being popular languages -- see that file.
#ifndef TF_TEST_GRAMMARS_H
#define TF_TEST_GRAMMARS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tree_feller.h"

const TSLanguage *tree_sitter_c(void);
const TSLanguage *tree_sitter_go(void);
const TSLanguage *tree_sitter_regex(void);
const TSLanguage *tree_sitter_solidity(void);
const TSLanguage *tree_sitter_datazinc(void);
const TSLanguage *tree_sitter_minizinc(void);
const TSLanguage *tree_sitter_eprime(void);
const TSLanguage *tree_sitter_systemverilog(void);

typedef struct {
  const char *name;
  const TSLanguage *(*language)(void);
  // What a file in this language is called, for the corpus tools. Empty when
  // the language has no file form worth walking a directory for.
  const char *extension;
} TFGrammar;

static const TFGrammar TF_GRAMMARS[] = {
    {"c", tree_sitter_c, ".c"},
    {"go", tree_sitter_go, ".go"},
    {"regex", tree_sitter_regex, ".regex"},
    {"solidity", tree_sitter_solidity, ".sol"},
    {"datazinc", tree_sitter_datazinc, ".dzn"},
    {"minizinc", tree_sitter_minizinc, ".mzn"},
    {"eprime", tree_sitter_eprime, ".eprime"},
    {"systemverilog", tree_sitter_systemverilog, ".sv"},
};
#define TF_GRAMMAR_COUNT (sizeof(TF_GRAMMARS) / sizeof(TF_GRAMMARS[0]))

static inline const TFGrammar *tf_grammar_named(const char *name) {
  for (size_t i = 0; i < TF_GRAMMAR_COUNT; i++) {
    if (strcmp(TF_GRAMMARS[i].name, name) == 0) return &TF_GRAMMARS[i];
  }
  return NULL;
}

// Running out of memory is fatal in a test, but `p = realloc(p, n)` still loses
// the original buffer when it fails, so the result goes through here instead.
static inline void *tf_xrealloc(void *ptr, size_t size) {
  void *grown = realloc(ptr, size);
  if (grown == NULL) {
    free(ptr);
    fprintf(stderr, "out of memory\n");
    exit(2);
  }
  return grown;
}

// The next non-empty line of stdin, without its newline, so a tool can be fed a
// find(1) pipeline.
static inline bool tf_next_stdin_path(char *path, int size) {
  while (fgets(path, size, stdin)) {
    path[strcspn(path, "\n")] = '\0';
    if (*path) return true;
  }
  return false;
}

#endif  // TF_TEST_GRAMMARS_H
