/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Read-only Linux persistent_ram console decoder. See docs/UEFI_LOG_VIEWER.md.
 */
#ifndef TB710FU_RAMOOPS_READER_H
#define TB710FU_RAMOOPS_READER_H
#ifdef TB710FU_RAMOOPS_HOST_TEST
#include <stdint.h>
typedef uint8_t TR_BYTE;
typedef uint32_t TR_WORD;
#else
#include <Uefi.h>
typedef UINT8 TR_BYTE;
typedef UINT32 TR_WORD;
#endif

#define TR_ZONE_BYTES 0x40000U
#define TR_HEADER_BYTES 12U
#define TR_SIGNATURE 0x43474244U

typedef struct {
  TR_WORD Signature;
  TR_WORD Start;
  TR_WORD Size;
  TR_WORD Capacity;
} TR_VIEW;

static inline TR_WORD
TrLe32 (const TR_BYTE *p)
{
  return (TR_WORD)p[0] | ((TR_WORD)p[1] << 8) |
         ((TR_WORD)p[2] << 16) | ((TR_WORD)p[3] << 24);
}

/* ECC is supplied explicitly: persistent_ram has no on-media ECC mode tag.
 * ECC=16 computes the writer's 128-byte block layout; it does NOT repair ECC.
 */
static inline int
TrParse (const TR_BYTE *zone, TR_WORD bytes, TR_WORD ecc, TR_VIEW *view)
{
  TR_WORD buf, blocks;
  if (zone == 0 || view == 0 || bytes != TR_ZONE_BYTES ||
      (ecc != 0 && ecc != 16)) {
    return 0;
  }
  view->Signature = TrLe32 (zone);
  view->Start = TrLe32 (zone + 4);
  view->Size = TrLe32 (zone + 8);
  buf = bytes - TR_HEADER_BYTES;
  blocks = ecc ? (buf - ecc + 128U + ecc - 1U) / (128U + ecc) : 0;
  view->Capacity = buf - (blocks + (ecc ? 1U : 0U)) * ecc;
  return view->Signature == TR_SIGNATURE &&
         view->Start <= view->Size && view->Size <= view->Capacity;
}

/* Logical order is data[start:size] followed by data[0:start], as in Linux
 * persistent_ram_save_old(). Call only after TrParse returned true.
 */
static inline TR_BYTE
TrByte (const TR_BYTE *zone, const TR_VIEW *view, TR_WORD index)
{
  TR_WORD tail;
  if (index >= view->Size) {
    return 0;
  }
  tail = view->Size - view->Start;
  return zone[TR_HEADER_BYTES + (index < tail ? view->Start + index : index - tail)];
}

/* One display line, with bounded wrapping. CR is ignored; control bytes are
 * rendered as '.', tabs as one space. Every non-empty call makes progress.
 */
static inline TR_WORD
TrLine (const TR_BYTE *zone, const TR_VIEW *view, TR_WORD offset,
        TR_WORD columns, TR_BYTE *text, TR_WORD *length)
{
  TR_WORD used = 0;
  if (columns == 0) {
    *length = 0;
    return offset;
  }
  while (offset < view->Size && used < columns) {
    TR_BYTE c = TrByte (zone, view, offset++);
    if (c == '\n') {
      break;
    }
    if (c == '\r') {
      continue;
    }
    if (text != 0) {
      text[used] = c == '\t' ? ' ' : (c >= 32 && c <= 126 ? c : '.');
    }
    used++;
  }
  /* Avoid an extra empty row when a newline immediately follows a full row. */
  if (used == columns && offset < view->Size && TrByte (zone, view, offset) == '\r') {
    offset++;
  }
  if (used == columns && offset < view->Size && TrByte (zone, view, offset) == '\n') {
    offset++;
  }
  *length = used;
  return offset;
}
#endif
