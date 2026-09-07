// tree-feller is a C library first, whatever the Rust crate does with it. This
// walks the whole public header from C so that a C consumer stays a tested
// configuration rather than an assumed one.
//
// Behaviour is covered in Rust (`lib/tests/`); what is checked here is that
// every entry point is reachable, composes, and does something recognisable.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tree_feller.h"

#include "grammars.h"

static unsigned failures;

#define CHECK(cond, ...)                      \
  do {                                        \
    if (!(cond)) {                            \
      fprintf(stderr, "  FAIL " __VA_ARGS__); \
      fprintf(stderr, "\n");                  \
      failures++;                             \
    }                                         \
  } while (0)

static unsigned raw_shifts, raw_reduces, visible_nodes, folded_runs;
static unsigned handed_out, handed_back;

static void *on_shift(void *payload, const TFToken *token, bool extra) {
  (void)payload;
  (void)extra;
  CHECK(token->end_byte >= token->start_byte, "token span is inverted");
  raw_shifts++;
  return NULL;
}

static void *on_reduce(void *payload, const TFReduction *reduction) {
  (void)payload;
  CHECK(reduction->node_count >= reduction->child_count, "fewer nodes than children");
  raw_reduces++;
  return NULL;
}

// The value a consumer builds is whatever it returns; here, a node count.
static void *on_node(void *payload, const TFVisibleNode *node) {
  (void)payload;
  size_t total = 1;
  for (uint32_t i = 0; i < node->child_count; i++) {
    total += (size_t)node->children[i].value;
  }
  visible_nodes++;
  return (void *)total;
}

// An allocating consumer, to check that a failed parse hands everything back
// rather than dropping it.
static void *on_node_alloc(void *payload, const TFVisibleNode *node) {
  (void)payload;
  for (uint32_t i = 0; i < node->child_count; i++) {
    if (node->children[i].value) {
      free(node->children[i].value);
      handed_back++;
    }
  }
  handed_out++;
  return malloc(1);
}

static void on_discard(void *payload, void *value) {
  (void)payload;
  free(value);
  handed_back++;
}

static void *on_hidden(void *payload, const TFVisibleNode *node) {
  (void)payload;
  size_t total = 0;
  for (uint32_t i = 0; i < node->child_count; i++) {
    total += (size_t)node->children[i].value;
  }
  folded_runs++;
  return (void *)(total + 1);  // never NULL, which would decline the fold
}

int main(void) {
  const char *source = "int a[] = {1, 2, 3};\nint f(int b) { return b + 1; }\n";
  uint32_t size = (uint32_t)strlen(source);

  const char *load_error = NULL;
  TFLanguage *lang = tf_language_load(tree_sitter_c(), &load_error);
  CHECK(lang != NULL, "could not load tables: %s", load_error ? load_error : "?");
  if (lang == NULL) return 1;

  CHECK(tf_language_symbol_name(lang, 1) != NULL, "symbol 1 has no name");
  CHECK(tf_language_symbol_name(lang, 0xFFFF) == NULL, "out-of-range symbol got a name");
  CHECK(tf_language_field_name(lang, 0xFFFF) == NULL, "out-of-range field got a name");

  TFError error;

  // The raw reduction stream.
  TFSink raw = {.on_shift = on_shift, .on_reduce = on_reduce};
  CHECK(tf_parse(lang, source, size, &raw, NULL, &error), "raw parse failed: %s", error.message);
  CHECK(raw_shifts > 0 && raw_reduces > 0, "raw parse reported nothing");

  // The visible view, and the value threaded back out through the root.
  void *root = NULL;
  TFVisibleSink visible = {.on_node = on_node};
  CHECK(tf_parse_visible(lang, source, size, &visible, &root, &error), "visible parse failed: %s",
        error.message);
  CHECK((size_t)root == visible_nodes, "root value %zu is not the node count %u", (size_t)root,
        visible_nodes);

  // Folding, and dropping punctuation.
  unsigned all_nodes = visible_nodes;
  visible_nodes = 0;
  TFVisibleSink folding = {.on_node = on_node, .on_hidden = on_hidden, .named_only = true};
  CHECK(tf_parse_visible(lang, source, size, &folding, NULL, &error), "folded parse failed: %s",
        error.message);
  CHECK(folded_runs > 0, "nothing was offered to fold");
  CHECK(visible_nodes < all_nodes, "named_only reported as many nodes as a full walk");

  // Nothing built before a failure is dropped on the floor.
  handed_out = handed_back = 0;
  TFVisibleSink allocating = {.on_node = on_node_alloc, .on_discard = on_discard};
  CHECK(!tf_parse_visible(lang, "int a = 1; int int b;", 21, &allocating, NULL, &error),
        "malformed input was accepted");
  CHECK(handed_out > 0, "nothing was built before the failure");
  CHECK(handed_out == handed_back, "%u values built, %u handed back", handed_out, handed_back);

  // A length that does not fit a byte offset is refused, not truncated.
  CHECK(!tf_parse(lang, source, (size_t)1 << 32, NULL, NULL, &error), "4 GiB input was accepted");
  CHECK(strstr(error.message, "4 GiB") != NULL, "unhelpful message: %s", error.message);

  // A sink is optional; parsing with none is how you measure the driver alone.
  CHECK(tf_parse(lang, source, size, NULL, NULL, &error), "sinkless parse failed");

  // Failure carries a position and a message.
  CHECK(!tf_parse(lang, "int int x;", 10, NULL, NULL, &error), "malformed input was accepted");
  CHECK(error.message[0] != '\0', "no message on failure");
  CHECK(error.byte <= 10, "error byte past the end");

  // Mapping a file, and the error path when there is none.
  TFFile file;
  CHECK(!tf_file_open(&file, "/nonexistent/tree-feller", &error), "opened a missing file");
  CHECK(error.message[0] != '\0', "no message for a missing file");

  tf_language_free(lang);

  // A rejected language is a NULL return and a reason, not a crash.
  load_error = NULL;
  CHECK(tf_language_load(NULL, &load_error) == NULL, "NULL language was accepted");
  CHECK(load_error != NULL, "no reason given for a rejected language");

  if (failures != 0) {
    fprintf(stderr, "%u checks failed\n", failures);
    return 1;
  }
  printf("ok: raw %u shifts / %u reduces, %u visible nodes, %u runs folded\n", raw_shifts,
         raw_reduces, all_nodes, folded_runs);
  return 0;
}
