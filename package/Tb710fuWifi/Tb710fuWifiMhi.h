/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* MHI 1.0 bring-up for the WCN7850, as a state machine the kernel thread drives plus pure builders for the context
 * array, command/event rings and the BHIe segment table. The state machine follows UniXNU-sheng's MhiBoot.c
 * (BSD-2-Clause-Patent, hardware-verified on the same WCN7850 IP); the context/ring layout follows ath12k's mhi.h and
 * the register writes sheng's WifiProbe.cpp uses. Everything here is MMIO-free: the caller supplies read/write. */
#ifndef TB710FU_WIFI_MHI_H
#define TB710FU_WIFI_MHI_H

#include <stdint.h>

#include "Tb710fuWifiRegs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;
    uint32_t (*read)(void *ctx, uint32_t reg);
    void (*write)(void *ctx, uint32_t reg, uint32_t value);
} Tb710fuWifiMhiIo;

enum {
    TB710FU_WIFI_MHI_IDLE = 0,
    TB710FU_WIFI_MHI_RESETTING,
    TB710FU_WIFI_MHI_SBL_LOADING,
    TB710FU_WIFI_MHI_WAIT_READY,
    TB710FU_WIFI_MHI_NEED_CONTEXTS,
    TB710FU_WIFI_MHI_WAIT_SBL,
    TB710FU_WIFI_MHI_IMAGE_LOADING,
    TB710FU_WIFI_MHI_WAIT_MISSION,
    TB710FU_WIFI_MHI_DONE,
    TB710FU_WIFI_MHI_FAILED
};

enum {
    TB710FU_WIFI_MHI_ERR_NONE = 0,
    TB710FU_WIFI_MHI_ERR_LAYOUT,
    TB710FU_WIFI_MHI_ERR_NOT_PBL,
    TB710FU_WIFI_MHI_ERR_BHI,
    TB710FU_WIFI_MHI_ERR_BHIE,
    TB710FU_WIFI_MHI_ERR_SYSERR,
    TB710FU_WIFI_MHI_ERR_TIMEOUT,
    TB710FU_WIFI_MHI_ERR_STATE,
    TB710FU_WIFI_MHI_ERR_RDDM
};

enum {
    TB710FU_WIFI_EE_PBL = 0,
    TB710FU_WIFI_EE_SBL = 1,
    TB710FU_WIFI_EE_AMSS = 2,
    TB710FU_WIFI_EE_RDDM = 3,
    TB710FU_WIFI_EE_WFW = 4,
    TB710FU_WIFI_EE_EDL = 6
};

enum {
    TB710FU_WIFI_MHI_STATE_RESET = 0,
    TB710FU_WIFI_MHI_STATE_READY = 1,
    TB710FU_WIFI_MHI_STATE_M0 = 2,
    TB710FU_WIFI_MHI_STATE_BHI = 7,
    TB710FU_WIFI_MHI_STATE_SYSERR = 0xff
};

typedef struct {
    uint64_t address, size;
} Tb710fuWifiBhieEntry;

typedef struct {
    Tb710fuWifiMhiIo io;
    uint32_t reglen, bhi, bhie;
    uint32_t msi_vector, seq;
    uint64_t sbl_addr, table_addr;
    uint32_t sbl_size, table_size;
    uint32_t ee, state, last_status, errcode;
    uint32_t phase, failed_phase, error;
} Tb710fuWifiMhi;

int tb710fu_wifi_mhi_open(Tb710fuWifiMhi *m, const Tb710fuWifiMhiIo *io);
uint32_t tb710fu_wifi_mhi_ee(Tb710fuWifiMhi *m);
uint32_t tb710fu_wifi_mhi_state(Tb710fuWifiMhi *m);
uint32_t tb710fu_wifi_mhi_begin(Tb710fuWifiMhi *m, uint32_t msi_vector, uint64_t sbl_addr, uint32_t sbl_size,
                                uint64_t table_addr, uint32_t table_size, uint32_t seq);
uint32_t tb710fu_wifi_mhi_step(Tb710fuWifiMhi *m);
uint32_t tb710fu_wifi_mhi_contexts_done(Tb710fuWifiMhi *m);
uint32_t tb710fu_wifi_mhi_timeout(Tb710fuWifiMhi *m);
uint32_t tb710fu_wifi_bhie_table(Tb710fuWifiBhieEntry *table, uint32_t count, const uint64_t *addresses,
                                 uint32_t image_size, uint32_t segment_size);

/* Host-memory plan: 128 channel contexts, two event contexts, one command context, then the rings, all inside one
 * contiguous DMA buffer whose device address is IOVA. Offsets are relative to that buffer. */
typedef struct {
    uint32_t ch_off, ev_off, cmd_off, ring_off;
    uint64_t ev_base[TB710FU_WIFI_MHI_EVENTS];
    uint32_t ev_bytes[TB710FU_WIFI_MHI_EVENTS];
    uint64_t cmd_base;
    uint32_t cmd_bytes;
    uint32_t total_bytes;
} Tb710fuWifiMhiPlan;

int tb710fu_wifi_mhi_plan_make(uint64_t iova, Tb710fuWifiMhiPlan *plan);
int tb710fu_wifi_mhi_contexts(uint8_t *buffer, uint32_t bytes, uint64_t iova, uint32_t msi_vector,
                              const Tb710fuWifiMhiPlan *plan);

/* The MHI register writes that point the device at the plan (and the MHICFG NER field). */
typedef struct {
    uint32_t words[7][2]; /* offset, value: CCABAP, ECABAP, CRCBAP, ctrl base, ctrl limit, data base, data limit */
    uint32_t cfg_mask, cfg_value;
} Tb710fuWifiMhiMmio;

int tb710fu_wifi_mhi_mmio(const Tb710fuWifiMhiPlan *plan, uint64_t iova, Tb710fuWifiMhiMmio *mmio);

/* Ring pointer arithmetic over 16-byte elements, wrapping inside [base, base + bytes). */
uint64_t tb710fu_wifi_mhi_ring_advance(uint64_t pointer, uint64_t base, uint32_t bytes);

/* One 16-byte event record, copied through a byte buffer: the device writes it, the host only reads it. */
typedef struct {
    uint32_t dword[4];
} Tb710fuWifiMhiEvent;

void tb710fu_wifi_mhi_event_read(const uint8_t *entry, Tb710fuWifiMhiEvent *event);

#ifdef __cplusplus
}
#endif

#endif /* TB710FU_WIFI_MHI_H */
