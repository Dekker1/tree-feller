// Throughput and peak RSS for one parser over one file.
//
// Peak RSS is a high-water mark for the whole process, so each mode runs in its
// own invocation -- otherwise the largest mode's footprint would be reported for
// all of them. `bench/run.sh` drives it.
//
// The modes are chosen to attribute the cost of the current implementation:
//
//   ts-parse  libtree-sitter builds the CST and nothing reads it.
//   ts-walk   ...plus a full cursor walk reading each node's kind, field and
//             span. That pair is what parser_ts.cpp pays before it has built
//             any AST at all, split into subtree allocation and traversal.
//   feller    tree-feller reporting the same visible nodes, no tree.
//   feller-raw   the underlying reduction stream, without the visibility layer.
//   feller-fold  ...with `on_hidden`, folding each hidden run as it completes,
//             which is what a bulk consumer would do.
//   feller-named ...and skipping the punctuation a consumer does not read.
//             Still only counts what it is handed: no values are built, no
//             token text is read. It is the cost of *delivering* the nodes a
//             loader wants, not the cost of a loader.
//   feller-null  the same parse with no sink at all, which is the driver and
//             lexer alone -- the floor the others are measured against.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

// api.h first: tree_feller.h pulls in the generated parser.h, which defines
// TSLanguageMetadata itself unless api.h has already been seen.
#include "tree_sitter/api.h"

#include "tree_feller.h"

const TSLanguage *tree_sitter_datazinc(void);

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

static double peak_rss_mb(void) {
  struct rusage u;
  getrusage(RUSAGE_SELF, &u);
  // ru_maxrss is bytes on macOS, kilobytes on Linux.
#ifdef __APPLE__
  return (double)u.ru_maxrss / (1024.0 * 1024.0);
#else
  return (double)u.ru_maxrss / 1024.0;
#endif
}

// Something for each mode to accumulate, so no parse can be optimised away.
static uint64_t sink_checksum;

static void *count_visible(void *payload, const TFVisibleNode *node) {
  (void)payload;
  sink_checksum += node->symbol + node->end_byte;
  for (uint32_t i = 0; i < node->child_count; i++) sink_checksum += node->children[i].field_id;
  return NULL;
}

static void *count_reduce(void *payload, const TFReduction *r) {
  (void)payload;
  sink_checksum += r->symbol + r->end_byte + r->child_count;
  return NULL;
}

static void *count_shift(void *payload, const TFToken *t, bool extra) {
  (void)payload;
  (void)extra;
  sink_checksum += t->symbol + t->end_byte;
  return NULL;
}

// What a consumer folding a long repetition does: take the run, keep a count,
// hand back one value. Anything non-NULL collapses the run.
static void *fold_hidden(void *payload, const TFVisibleNode *node) {
  (void)payload;
  sink_checksum += node->symbol + node->child_count;
  for (uint32_t i = 0; i < node->child_count; i++) sink_checksum += node->children[i].field_id;
  return (void *)(uintptr_t)(node->child_count + 1);
}

// The traversal parser_ts.cpp performs: every node, its kind, its field and its
// span. Iterative, because the trees are deep enough to overflow a stack.
static void walk(TSTree *tree) {
  TSTreeCursor cursor = ts_tree_cursor_new(ts_tree_root_node(tree));
  bool descend = true;
  for (;;) {
    if (descend) {
      TSNode node = ts_tree_cursor_current_node(&cursor);
      sink_checksum += ts_node_symbol(node) + ts_node_end_byte(node);
      sink_checksum += ts_tree_cursor_current_field_id(&cursor);
      if (ts_tree_cursor_goto_first_child(&cursor)) continue;
    }
    if (ts_tree_cursor_goto_next_sibling(&cursor)) {
      descend = true;
      continue;
    }
    if (!ts_tree_cursor_goto_parent(&cursor)) break;
    descend = false;
  }
  ts_tree_cursor_delete(&cursor);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr,
            "usage: tf_bench "
            "<ts-parse|ts-walk|feller|feller-fold|feller-named|feller-raw|feller-null> <file>\n");
    return 2;
  }
  const char *mode = argv[1];
  const char *path = argv[2];

  TFFile file;
  TFError error;
  if (!tf_file_open(&file, path, &error)) {
    fprintf(stderr, "%s: %s\n", path, error.message);
    return 1;
  }

  // Fault the file in first: this measures parsers, not the page cache.
  {
    volatile uint64_t warm = 0;
    const unsigned char *p = file.data;
    for (uint32_t i = 0; i < file.size; i += 4096) warm += p[i];
    (void)warm;
  }

  double elapsed;
  if (strncmp(mode, "feller", 6) == 0) {
    const TFLanguage *lang = tf_language_load(tree_sitter_datazinc(), NULL);
    if (!lang) {
      fprintf(stderr, "could not load tables\n");
      return 1;
    }
    bool raw = strcmp(mode, "feller-raw") == 0;
    bool null_sink = strcmp(mode, "feller-null") == 0;
    bool named = strcmp(mode, "feller-named") == 0;
    bool fold = named || strcmp(mode, "feller-fold") == 0;
    TFVisibleSink visible = {
        .payload = NULL,
        .on_node = count_visible,
        .on_hidden = fold ? fold_hidden : NULL,
        .named_only = named,
    };
    TFSink rawsink = {.payload = NULL, .on_shift = count_shift, .on_reduce = count_reduce};
    TFSink empty = {.payload = NULL, .on_shift = NULL, .on_reduce = NULL};
    double start = now();
    bool ok = null_sink ? tf_parse(lang, file.data, file.size, &empty, NULL, &error)
              : raw     ? tf_parse(lang, file.data, file.size, &rawsink, NULL, &error)
                        : tf_parse_visible(lang, file.data, file.size, &visible, NULL, &error);
    elapsed = now() - start;
    if (!ok) {
      fprintf(stderr, "%s: %s\n", path, error.message);
      return 1;
    }
  } else {
    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_datazinc());
    double start = now();
    TSTree *tree = ts_parser_parse_string(parser, NULL, file.data, file.size);
    if (!tree) {
      fprintf(stderr, "%s: reference parse failed\n", path);
      return 1;
    }
    if (strcmp(mode, "ts-walk") == 0) walk(tree);
    elapsed = now() - start;
    ts_tree_delete(tree);
    ts_parser_delete(parser);
  }

  double mb = (double)file.size / (1024.0 * 1024.0);
  printf("%-11s %9.2f MB %8.3f s %9.1f MB/s %9.1f MB  %s\n", mode, mb, elapsed, mb / elapsed,
         peak_rss_mb(), path);
  tf_file_close(&file);
  return sink_checksum == 0xdeadbeef ? 3 : 0;  // keep the accumulator live
}
