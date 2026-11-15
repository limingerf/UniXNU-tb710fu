/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#define TB710FU_RAMOOPS_HOST_TEST
#include "../Ramoops.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(TR_BYTE *p, TR_WORD n)
{
  for (unsigned i = 0; i < 4; i++) { p[i] = (TR_BYTE)(n >> (8 * i)); }
}

static void header(TR_BYTE *p, TR_WORD start, TR_WORD size)
{
  put32(p, TR_SIGNATURE); put32(p + 4, start); put32(p + 8, size);
}

int main(void)
{
  TR_BYTE *zone = calloc(1, TR_ZONE_BYTES);
  TR_BYTE *before = malloc(TR_ZONE_BYTES);
  TR_BYTE line[32];
  TR_VIEW view;
  TR_WORD n, next;
  assert(zone && before);
  assert(!TrParse(zone, TR_ZONE_BYTES, 0, &view));
  assert(!TrParse(zone, 11, 0, &view));
  assert(!TrParse(zone, TR_ZONE_BYTES, 8, &view));
  assert(!TrParse(NULL, TR_ZONE_BYTES, 0, &view));
  assert(!TrParse(zone, TR_ZONE_BYTES, 0, NULL));
  header(zone, 0, 0);
  assert(TrParse(zone, TR_ZONE_BYTES, 0, &view));
  assert(view.Capacity == 262132 && TrByte(zone, &view, 0) == 0);
  assert(TrParse(zone, TR_ZONE_BYTES, 16, &view));
  assert(view.Capacity == 232980); /* Same integer formula as the XNU writer. */
  header(zone, 1, 0);
  assert(!TrParse(zone, TR_ZONE_BYTES, 0, &view));
  header(zone, 262133, 262133);
  assert(!TrParse(zone, TR_ZONE_BYTES, 0, &view));
  header(zone, 0xffffffffU, 0xffffffffU);
  assert(!TrParse(zone, TR_ZONE_BYTES, 0, &view));

  /* Ring model: fill/overwrite then compare the entire chronological stream. */
  for (TR_WORD ecc = 0; ecc <= 16; ecc += 16) {
    header(zone, 0, 0);
    assert(TrParse(zone, TR_ZONE_BYTES, ecc, &view));
    TR_WORD cap = view.Capacity;
    const TR_WORD lengths[] = {1, 17, cap - 1, cap, cap + 1, 2 * cap + 13};
    for (unsigned t = 0; t < sizeof(lengths) / sizeof(lengths[0]); t++) {
      TR_WORD count = lengths[t], start = 0, size = 0;
      for (TR_WORD i = 0; i < count; i++) {
        zone[TR_HEADER_BYTES + start] = (TR_BYTE)(i % 251);
        start = (start + 1) % cap;
        if (size < cap) { size++; }
      }
      header(zone, start, size);
      memcpy(before, zone, TR_ZONE_BYTES);
      assert(TrParse(zone, TR_ZONE_BYTES, ecc, &view));
      for (TR_WORD i = 0; i < size; i++) {
        assert(TrByte(zone, &view, i) == (TR_BYTE)((count - size + i) % 251));
      }
      assert(TrByte(zone, &view, size) == 0);
      assert(memcmp(before, zone, TR_ZONE_BYTES) == 0);
    }
  }

  /* Wrapping, CRLF, a tab, terminal escape, high byte and a final short row. */
  static const TR_BYTE text[] = "abcd\r\nef\t\033\xff\nxyz";
  memcpy(zone + TR_HEADER_BYTES, text, sizeof(text) - 1);
  header(zone, sizeof(text) - 1, sizeof(text) - 1);
  assert(TrParse(zone, TR_ZONE_BYTES, 0, &view));
  next = TrLine(zone, &view, 0, 4, line, &n);
  assert(n == 4 && next == 6 && memcmp(line, "abcd", 4) == 0);
  next = TrLine(zone, &view, next, 4, line, &n);
  assert(n == 4 && memcmp(line, "ef .", 4) == 0);
  next = TrLine(zone, &view, next, 4, line, &n);
  assert(n == 1 && line[0] == '.');
  next = TrLine(zone, &view, next, 4, line, &n);
  assert(n == 3 && memcmp(line, "xyz", 3) == 0 && next == view.Size);
  assert(TrLine(zone, &view, 0, 0, line, &n) == 0 && n == 0);

  /* Corrupted headers: never inspect text unless the full bounds check passed. */
  TR_WORD rng = 47;
  for (unsigned i = 0; i < 20000; i++) {
    rng = rng * 1664525U + 1013904223U;
    put32(zone + 4, rng);
    rng = rng * 1664525U + 1013904223U;
    put32(zone + 8, rng);
    if (TrParse(zone, TR_ZONE_BYTES, i & 1 ? 16 : 0, &view)) {
      assert(view.Start <= view.Size && view.Size <= view.Capacity);
      for (TR_WORD at = 0; at < view.Size;) {
        next = TrLine(zone, &view, at, 32, NULL, &n);
        assert(next > at && next <= view.Size);
        at = next;
      }
    }
  }
  free(before); free(zone);
  puts("PASS: ramoops headers, ECC layouts, ring wrap, pagination, sanitization, read-only decode");
  return 0;
}
