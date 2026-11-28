/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "GopDisplayLayout.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    uint64_t offset = UINT64_MAX;
    uint64_t remaining = 0;
    uint32_t iova = 0;

    assert(tb710fu_gop_plane_span(0, 12800, 2000, 25600000, &offset, &remaining));
    assert(offset == 0 && remaining == 25600000);
    assert(tb710fu_gop_plane_span(4096, 12800, 2000, 25604096, &offset, &remaining));
    assert(offset == 4096 && remaining == 25600000);
    assert(!tb710fu_gop_plane_span(4097, 12800, 2000, 25604096, &offset, &remaining));
    assert(!tb710fu_gop_plane_span(0, UINT64_MAX, 2, UINT64_MAX, &offset, &remaining));
    assert(!tb710fu_gop_plane_span(UINT64_MAX - 7, 8, 1, UINT64_MAX, &offset, &remaining));
    assert(!tb710fu_gop_plane_span(0, 12800, 0, 25600000, &offset, &remaining));

    assert(tb710fu_gop_plane_iova(0x80000000u, 4096, 8192, &iova));
    assert(iova == 0x80001000u);
    assert(!tb710fu_gop_plane_iova(0, 0, 8192, &iova));
    assert(!tb710fu_gop_plane_iova(0xFFFFFFF0u, 32, 8192, &iova));
    assert(!tb710fu_gop_plane_iova(0x80000000u, 8192, 8192, &iova));
    assert(!tb710fu_gop_plane_iova(0x100000000u, 0, 8192, &iova));

    puts("PASS plane bounds and IOVA cases=11");
    return 0;
}
