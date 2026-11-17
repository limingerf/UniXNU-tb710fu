/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef TB710FU_LIVE_QR_H
#define TB710FU_LIVE_QR_H
#include <stddef.h>
#include <stdint.h>
/* Serialized by the adapter. Returned pixels survive until the next call. */
int tb_live_qr_encode(const uint8_t *frame, size_t bytes, const uint32_t **pixels);
#endif
