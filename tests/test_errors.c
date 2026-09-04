// Phase 3: malformed input must be refused, at a sensible place, without
// wandering past the damage.
#include <stdio.h>
#include <string.h>

#include "tree_feller.h"

const TSLanguage *tree_sitter_datazinc(void);

static unsigned failures;

static void expect_error(const char *source, uint32_t at_or_before) {
  const char *load_error = NULL;
  TFLanguage *lang = tf_language_load(tree_sitter_datazinc(), &load_error);
  TFError error;
  bool ok = tf_parse(lang, source, (uint32_t)strlen(source), NULL, NULL, &error);
  if (ok) {
    fprintf(stderr, "  FAIL %-32s accepted, should not have been\n", source);
    failures++;
  } else if (error.byte > at_or_before) {
    fprintf(stderr, "  FAIL %-32s failed at byte %u, expected at or before %u (%s)\n", source,
            error.byte, at_or_before, error.message);
    failures++;
  } else {
    printf("  %-34s %u:%u %.60s\n", source, error.point.row + 1, error.point.column, error.message);
  }
  tf_language_free(lang);
}

static void expect_ok(const char *source) {
  const char *load_error = NULL;
  TFLanguage *lang = tf_language_load(tree_sitter_datazinc(), &load_error);
  TFError error;
  if (!tf_parse(lang, source, (uint32_t)strlen(source), NULL, NULL, &error)) {
    fprintf(stderr, "  FAIL %-32s rejected: %s\n", source, error.message);
    failures++;
  }
  tf_language_free(lang);
}

int main(void) {
  printf("malformed input:\n");
  expect_error("x = ", 4);                  // truncated
  expect_error("x = [1, 2", 9);             // unterminated array
  expect_error("x = \"unterminated", 17);   // unterminated string
  expect_error("x = |];", 4);               // stray closer
  expect_error("x = [| 1, 2 |;", 13);       // truncated 2d literal
  expect_error("x = 1 y = 2;", 6);          // missing separator
  expect_error("= 1;", 0);                  // no name
  expect_error("x = 1; }", 7);              // stray brace
  expect_error("x = /* unterminated", 19);  // runs to EOF, so that is where it fails
  expect_error("x = 1 @ 2;", 6);            // unlexable character
  expect_error("x = {1, 2;", 9);            // unterminated set
  expect_error("x = (a: 1;", 9);            // unterminated record

  // The empty file and a file of only extras are valid.
  expect_ok("");
  expect_ok("% just a comment\n");
  expect_ok("\n\n  \t\n");

  if (failures) {
    fprintf(stderr, "%u failures\n", failures);
    return 1;
  }
  printf("ok\n");
  return 0;
}
