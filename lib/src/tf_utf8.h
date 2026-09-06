// UTF-8 decoding, to the same rules as ICU's `U8_NEXT` -- which is what
// tree-sitter's lexer uses, so the token boundaries this produces are the same.
//
// tree-sitter vendors five ICU headers to get one macro. This library needs the
// same decision (code point, or malformed) in two places, so it spells the rules
// out instead of carrying 92 KB of somebody else's headers and a second licence.
// `tests/test_utf8.c` checks that against the real macro, exhaustively for every
// sequence up to three bytes and for every four-byte sequence with a plausible
// lead, so "same rules" is a tested claim rather than a hopeful comment.
#ifndef TF_UTF8_H
#define TF_UTF8_H

#include <stdint.h>

// ICU's U_SENTINEL: what `U8_NEXT` substitutes for a malformed sequence.
#define TF_DECODE_ERROR (-1)

// Decodes the sequence at `s`, of which `available` bytes are readable.
//
// On success returns the code point and stores the number of bytes it spans.
// On a malformed sequence -- a stray continuation byte, an overlong encoding, a
// surrogate half, anything above U+10FFFF, or a sequence cut short by the end of
// the buffer -- returns TF_DECODE_ERROR and leaves `*length` alone. tree-sitter
// then advances a single byte, so how far ICU would have skipped does not matter.
static inline int32_t tf_utf8_next(const uint8_t *s, uint32_t available, uint32_t *length) {
  uint8_t lead = s[0];
  if (lead < 0x80) {
    *length = 1;
    return lead;
  }
  // 0x80-0xBF is a continuation byte with nothing to continue; 0xC0 and 0xC1
  // could only ever encode a value that fits in one byte.
  if (lead < 0xC2) return TF_DECODE_ERROR;

  if (lead < 0xE0) {
    if (available < 2 || (s[1] & 0xC0) != 0x80) return TF_DECODE_ERROR;
    *length = 2;
    return (int32_t)(((uint32_t)(lead & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F));
  }

  if (lead < 0xF0) {
    if (available < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return TF_DECODE_ERROR;
    // E0 80..9F would be overlong; ED A0..BF is a surrogate half.
    if (lead == 0xE0 ? s[1] < 0xA0 : (lead == 0xED && s[1] > 0x9F)) return TF_DECODE_ERROR;
    *length = 3;
    return (int32_t)(((uint32_t)(lead & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) |
                     (uint32_t)(s[2] & 0x3F));
  }

  // F5 and above would start a code point past U+10FFFF.
  if (lead < 0xF5) {
    if (available < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) {
      return TF_DECODE_ERROR;
    }
    // F0 80..8F would be overlong; F4 90..BF is past U+10FFFF.
    if (lead == 0xF0 ? s[1] < 0x90 : (lead == 0xF4 && s[1] > 0x8F)) return TF_DECODE_ERROR;
    *length = 4;
    return (int32_t)(((uint32_t)(lead & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
                     ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F));
  }

  return TF_DECODE_ERROR;
}

#endif  // TF_UTF8_H
