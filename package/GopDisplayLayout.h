/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef TB710FU_GOP_DISPLAY_LAYOUT_H
#define TB710FU_GOP_DISPLAY_LAYOUT_H

#include <stdint.h>

/* Resolve the byte range occupied by one IOSurface plane in its backing
 * descriptor. IOSurface plane base is relative to the descriptor mapping. */
static inline int tb710fu_gop_plane_span(uint64_t base_offset, uint64_t stride,
                                         uint32_t height, uint64_t descriptor_bytes,
                                         uint64_t *out_offset, uint64_t *out_bytes)
{
    uint64_t span;

    if (height == 0u || stride == 0u || stride > UINT64_MAX / height) return 0;
    span = stride * height;
    if (base_offset > descriptor_bytes || span > descriptor_bytes - base_offset) return 0;
    if (out_offset) *out_offset = base_offset;
    if (out_bytes) *out_bytes = descriptor_bytes - base_offset;
    return 1;
}

/* The direct display path maps the complete descriptor and then starts the
 * DPU at the plane base. Keep the narrow hardware address conversion checked. */
static inline int tb710fu_gop_plane_iova(uint64_t iova, uint64_t base_offset,
                                         uint64_t descriptor_bytes, uint32_t *out_address)
{
    if (!out_address || iova == 0u || iova > UINT32_MAX ||
        base_offset >= descriptor_bytes || base_offset > UINT32_MAX - iova) return 0;
    *out_address = (uint32_t)(iova + base_offset);
    return 1;
}

#endif /* TB710FU_GOP_DISPLAY_LAYOUT_H */
