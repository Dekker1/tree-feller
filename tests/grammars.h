// The grammars the tests and tools run against.
//
// None of these live in this repository; CMake fetches each generated parser.c
// at configure time (cmake/TreeFellerGrammars.cmake). They are chosen for the
// shapes they exercise, not for being popular languages -- see that file.
#ifndef TF_TEST_GRAMMARS_H
#define TF_TEST_GRAMMARS_H

#include <string.h>

#include "tree_feller.h"

const TSLanguage *tree_sitter_c(void);
const TSLanguage *tree_sitter_go(void);
const TSLanguage *tree_sitter_regex(void);
const TSLanguage *tree_sitter_datazinc(void);
const TSLanguage *tree_sitter_minizinc(void);
const TSLanguage *tree_sitter_eprime(void);

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
    {"datazinc", tree_sitter_datazinc, ".dzn"},
    {"minizinc", tree_sitter_minizinc, ".mzn"},
    {"eprime", tree_sitter_eprime, ".eprime"},
};
#define TF_GRAMMAR_COUNT (sizeof(TF_GRAMMARS) / sizeof(TF_GRAMMARS[0]))

static inline const TFGrammar *tf_grammar_named(const char *name) {
  for (size_t i = 0; i < TF_GRAMMAR_COUNT; i++) {
    if (strcmp(TF_GRAMMARS[i].name, name) == 0) return &TF_GRAMMARS[i];
  }
  return NULL;
}

#endif  // TF_TEST_GRAMMARS_H
