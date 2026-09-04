// `tf_utf8_next` against the ICU macro tree-sitter's lexer actually uses.
//
// The headers come from the fetched libtree-sitter, not from this repository:
// the point is to check against the real thing, not against a copy of it kept
// next to the code it is supposed to be checking.
//
// Exhaustive for every sequence of one, two and three bytes, and for every
// four-byte sequence whose lead byte could plausibly start one. Truncated
// buffers are covered too, since a sequence cut off by the end of the input has
// to be rejected rather than read past.
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define U_EXPORT
#define U_EXPORT2
#include "unicode/utf8.h"

#include "tf_utf8.h"

static unsigned long long checked;
static unsigned long long failures;

static void compare(const uint8_t *bytes, uint32_t available) {
  int32_t want;
  uint32_t want_length = 0;
  // The increment happens inside ICU's macro, which is the thing under test.
  // NOLINTNEXTLINE(bugprone-inc-dec-in-conditions)
  U8_NEXT(bytes, want_length, available, want);

  uint32_t got_length = 0;
  int32_t got = tf_utf8_next(bytes, available, &got_length);

  checked++;
  bool ok = (want < 0) ? (got == TF_DECODE_ERROR) : (got == want && got_length == want_length);
  if (ok) return;
  unsigned long long reported = failures++;
  if (reported < 20) {
    fprintf(stderr,
            "  %02x %02x %02x %02x (%u available): ICU %" PRId32 "/%u, ours %" PRId32 "/%u\n",
            bytes[0], available > 1 ? bytes[1] : 0, available > 2 ? bytes[2] : 0,
            available > 3 ? bytes[3] : 0, available, want, want_length, got, got_length);
  }
}

int main(void) {
  // A sequence is read with every buffer length that could truncate it, so
  // "ends before the sequence does" is covered at each position.
  uint8_t b[4] = {0, 0, 0, 0};

  for (unsigned x = 0; x < 256; x++) {
    b[0] = (uint8_t)x;
    compare(b, 1);
  }
  printf("  1 byte    %llu sequences\n", checked);

  unsigned long long mark = checked;
  for (unsigned x = 0; x < 256; x++) {
    for (unsigned y = 0; y < 256; y++) {
      b[0] = (uint8_t)x;
      b[1] = (uint8_t)y;
      compare(b, 2);
    }
  }
  printf("  2 bytes   %llu sequences\n", checked - mark);

  mark = checked;
  for (unsigned x = 0; x < 256; x++) {
    for (unsigned y = 0; y < 256; y++) {
      for (unsigned z = 0; z < 256; z++) {
        b[0] = (uint8_t)x;
        b[1] = (uint8_t)y;
        b[2] = (uint8_t)z;
        compare(b, 3);
      }
    }
  }
  printf("  3 bytes   %llu sequences\n", checked - mark);

  // Every lead that can begin a four-byte sequence, and every lead that looks
  // like one but must be rejected.
  mark = checked;
  for (unsigned x = 0xF0; x < 0x100; x++) {
    for (unsigned y = 0; y < 256; y++) {
      for (unsigned z = 0; z < 256; z++) {
        for (unsigned w = 0; w < 256; w++) {
          b[0] = (uint8_t)x;
          b[1] = (uint8_t)y;
          b[2] = (uint8_t)z;
          b[3] = (uint8_t)w;
          compare(b, 4);
        }
      }
    }
  }
  printf("  4 bytes   %llu sequences\n", checked - mark);

  if (failures != 0) {
    fprintf(stderr, "%llu of %llu sequences decoded differently\n", failures, checked);
    return 1;
  }
  printf("ok: %llu sequences, identical to ICU\n", checked);
  return 0;
}
