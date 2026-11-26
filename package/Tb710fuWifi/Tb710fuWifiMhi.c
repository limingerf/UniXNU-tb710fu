/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Tb710fuWifiMhi.h"

static uint32_t rd(Tb710fuWifiMhi *m, uint32_t reg) { return m->io.read(m->io.ctx, reg); }
static void wr(Tb710fuWifiMhi *m, uint32_t reg, uint32_t value) { m->io.write(m->io.ctx, reg, value); }

int tb710fu_wifi_mhi_open(Tb710fuWifiMhi *m, const Tb710fuWifiMhiIo *io)
{
    if (!m || !io || !io->read || !io->write) return -1;
    for (unsigned i = 0; i < sizeof *m; ++i) ((uint8_t *)m)[i] = 0;
    m->io = *io;
    m->reglen = io->read(io->ctx, TB710FU_WIFI_MHI_REGLEN);
    m->bhi = rd(m, TB710FU_WIFI_MHI_BHIOFF);
    m->bhie = rd(m, TB710FU_WIFI_MHI_BHIEOFF);
    const int good = m->reglen >= 0x100u && m->reglen <= 0x100000u &&
                     m->bhi >= 0x100u && m->bhi + TB710FU_WIFI_MHI_BHI_ERRCODE + 4u <= m->reglen &&
                     m->bhie >= 0x100u && m->bhie + TB710FU_WIFI_MHI_BHIE_TXVECSTATUS + 4u <= m->reglen &&
                     !(m->bhi & 3u) && !(m->bhie & 3u);
    if (!good) {
        m->phase = TB710FU_WIFI_MHI_FAILED;
        m->error = TB710FU_WIFI_MHI_ERR_LAYOUT;
        return -1;
    }
    m->phase = TB710FU_WIFI_MHI_IDLE;
    return 0;
}

uint32_t tb710fu_wifi_mhi_ee(Tb710fuWifiMhi *m) { return m->ee = rd(m, m->bhi + TB710FU_WIFI_MHI_BHI_EXECENV); }
uint32_t tb710fu_wifi_mhi_state(Tb710fuWifiMhi *m)
{
    return m->state = (rd(m, TB710FU_WIFI_MHI_STATUS) >> TB710FU_WIFI_MHI_STATE_SHIFT) & 0xffu;
}

static uint32_t fail(Tb710fuWifiMhi *m, uint32_t error)
{
    m->failed_phase = m->phase;
    m->error = error;
    m->phase = TB710FU_WIFI_MHI_FAILED;
    return m->phase;
}

static uint32_t start_sbl(Tb710fuWifiMhi *m)
{
    wr(m, m->bhi + TB710FU_WIFI_MHI_BHI_INTVEC, m->msi_vector); /* a reset clears it */
    wr(m, m->bhi + TB710FU_WIFI_MHI_BHI_IMGADDR_HIGH, (uint32_t)(m->sbl_addr >> 32));
    wr(m, m->bhi + TB710FU_WIFI_MHI_BHI_IMGADDR_LOW, (uint32_t)m->sbl_addr);
    wr(m, m->bhi + TB710FU_WIFI_MHI_BHI_IMGSIZE, m->sbl_size);
    wr(m, m->bhi + TB710FU_WIFI_MHI_BHI_IMGTXDB, m->seq);
    m->phase = TB710FU_WIFI_MHI_SBL_LOADING;
    return m->phase;
}

uint32_t tb710fu_wifi_mhi_begin(Tb710fuWifiMhi *m, uint32_t msi_vector, uint64_t sbl_addr, uint32_t sbl_size,
                                uint64_t table_addr, uint32_t table_size, uint32_t seq)
{
    if (m->phase != TB710FU_WIFI_MHI_IDLE) return fail(m, TB710FU_WIFI_MHI_ERR_STATE);
    m->msi_vector = msi_vector;
    m->sbl_addr = sbl_addr;
    m->sbl_size = sbl_size;
    m->table_addr = table_addr;
    m->table_size = table_size;
    m->seq = seq & TB710FU_WIFI_MHI_SEQ_MASK;
    if (!m->seq) m->seq = 1;
    const uint32_t ee = tb710fu_wifi_mhi_ee(m);
    if (ee != TB710FU_WIFI_EE_PBL && ee != TB710FU_WIFI_EE_EDL) return fail(m, TB710FU_WIFI_MHI_ERR_NOT_PBL);
    const uint32_t status = rd(m, TB710FU_WIFI_MHI_STATUS);
    m->state = (status >> TB710FU_WIFI_MHI_STATE_SHIFT) & 0xffu;
    wr(m, m->bhi + TB710FU_WIFI_MHI_BHI_INTVEC, msi_vector);
    if ((status & TB710FU_WIFI_MHI_STATUS_SYSERR) || m->state == TB710FU_WIFI_MHI_STATE_SYSERR) {
        wr(m, TB710FU_WIFI_MHI_CTRL, TB710FU_WIFI_MHI_CTRL_RESET);
        m->phase = TB710FU_WIFI_MHI_RESETTING;
        return m->phase;
    }
    return start_sbl(m);
}

static uint32_t start_image(Tb710fuWifiMhi *m)
{
    m->seq = (m->seq + 1) & TB710FU_WIFI_MHI_SEQ_MASK;
    if (!m->seq) m->seq = 1;
    wr(m, m->bhie + TB710FU_WIFI_MHI_BHIE_TXVECADDR_HIGH, (uint32_t)(m->table_addr >> 32));
    wr(m, m->bhie + TB710FU_WIFI_MHI_BHIE_TXVECADDR_LOW, (uint32_t)m->table_addr);
    wr(m, m->bhie + TB710FU_WIFI_MHI_BHIE_TXVECSIZE, m->table_size);
    wr(m, m->bhie + TB710FU_WIFI_MHI_BHIE_TXVECDB, m->seq);
    m->phase = TB710FU_WIFI_MHI_IMAGE_LOADING;
    return m->phase;
}

uint32_t tb710fu_wifi_mhi_step(Tb710fuWifiMhi *m)
{
    if (m->phase == TB710FU_WIFI_MHI_IDLE || m->phase == TB710FU_WIFI_MHI_DONE ||
        m->phase == TB710FU_WIFI_MHI_FAILED || m->phase == TB710FU_WIFI_MHI_NEED_CONTEXTS) return m->phase;
    const uint32_t status = rd(m, TB710FU_WIFI_MHI_STATUS);
    if (status == 0xffffffffu) return m->phase; /* the link is retraining while the boot loader starts */
    m->state = (status >> TB710FU_WIFI_MHI_STATE_SHIFT) & 0xffu;
    if (m->phase == TB710FU_WIFI_MHI_RESETTING) {
        if (rd(m, TB710FU_WIFI_MHI_CTRL) & TB710FU_WIFI_MHI_CTRL_RESET) return m->phase;
        return start_sbl(m);
    }
    const uint32_t ee = tb710fu_wifi_mhi_ee(m);
    if (ee == TB710FU_WIFI_EE_RDDM) return fail(m, TB710FU_WIFI_MHI_ERR_RDDM);
    if ((status & TB710FU_WIFI_MHI_STATUS_SYSERR) || m->state == TB710FU_WIFI_MHI_STATE_SYSERR)
        return fail(m, TB710FU_WIFI_MHI_ERR_SYSERR);
    switch (m->phase) {
    case TB710FU_WIFI_MHI_SBL_LOADING: {
        const uint32_t bhi = rd(m, m->bhi + TB710FU_WIFI_MHI_BHI_STATUS);
        m->last_status = bhi;
        if (bhi == 0xffffffffu) return m->phase;
        if (bhi >> 30 == TB710FU_WIFI_MHI_XFER_ERROR) {
            m->errcode = rd(m, m->bhi + TB710FU_WIFI_MHI_BHI_ERRCODE);
            return fail(m, TB710FU_WIFI_MHI_ERR_BHI);
        }
        if (bhi >> 30 != TB710FU_WIFI_MHI_XFER_SUCCESS) return m->phase;
        m->phase = TB710FU_WIFI_MHI_WAIT_READY;
        return tb710fu_wifi_mhi_step(m);
    }
    case TB710FU_WIFI_MHI_WAIT_READY:
        if ((rd(m, TB710FU_WIFI_MHI_CTRL) & TB710FU_WIFI_MHI_CTRL_RESET) ||
            !(status & TB710FU_WIFI_MHI_STATUS_READY)) return m->phase;
        m->phase = TB710FU_WIFI_MHI_NEED_CONTEXTS;
        return m->phase;
    case TB710FU_WIFI_MHI_WAIT_SBL:
        if (ee == TB710FU_WIFI_EE_AMSS || ee == TB710FU_WIFI_EE_WFW) {
            m->phase = TB710FU_WIFI_MHI_DONE; /* the device already had the full image */
            return m->phase;
        }
        if (ee != TB710FU_WIFI_EE_SBL) return m->phase;
        return start_image(m);
    case TB710FU_WIFI_MHI_IMAGE_LOADING: {
        const uint32_t tx = rd(m, m->bhie + TB710FU_WIFI_MHI_BHIE_TXVECSTATUS);
        m->last_status = tx;
        if ((tx & TB710FU_WIFI_MHI_SEQ_MASK) != m->seq) return m->phase;
        if (tx >> 30 == TB710FU_WIFI_MHI_XFER_ERROR) return fail(m, TB710FU_WIFI_MHI_ERR_BHIE);
        if (tx >> 30 != TB710FU_WIFI_MHI_XFER_SUCCESS) return m->phase;
        m->phase = TB710FU_WIFI_MHI_WAIT_MISSION;
        return tb710fu_wifi_mhi_step(m);
    }
    case TB710FU_WIFI_MHI_WAIT_MISSION:
        if (ee == TB710FU_WIFI_EE_AMSS || ee == TB710FU_WIFI_EE_WFW) m->phase = TB710FU_WIFI_MHI_DONE;
        return m->phase;
    default:
        return m->phase;
    }
}

uint32_t tb710fu_wifi_mhi_contexts_done(Tb710fuWifiMhi *m)
{
    if (m->phase != TB710FU_WIFI_MHI_NEED_CONTEXTS) return fail(m, TB710FU_WIFI_MHI_ERR_STATE);
    wr(m, TB710FU_WIFI_MHI_CTRL,
       (rd(m, TB710FU_WIFI_MHI_CTRL) & ~(0xffu << TB710FU_WIFI_MHI_STATE_SHIFT) & ~TB710FU_WIFI_MHI_CTRL_RESET) |
           (uint32_t)TB710FU_WIFI_MHI_STATE_M0 << TB710FU_WIFI_MHI_STATE_SHIFT);
    m->phase = TB710FU_WIFI_MHI_WAIT_SBL;
    return tb710fu_wifi_mhi_step(m);
}

uint32_t tb710fu_wifi_mhi_timeout(Tb710fuWifiMhi *m)
{
    if (m->phase == TB710FU_WIFI_MHI_IDLE || m->phase == TB710FU_WIFI_MHI_DONE ||
        m->phase == TB710FU_WIFI_MHI_FAILED) return m->phase;
    if (m->phase == TB710FU_WIFI_MHI_SBL_LOADING) m->errcode = rd(m, m->bhi + TB710FU_WIFI_MHI_BHI_ERRCODE);
    return fail(m, TB710FU_WIFI_MHI_ERR_TIMEOUT);
}

uint32_t tb710fu_wifi_bhie_table(Tb710fuWifiBhieEntry *table, uint32_t count, const uint64_t *addresses,
                                 uint32_t image_size, uint32_t segment_size)
{
    if (!table || !addresses || !segment_size || !image_size) return 0;
    const uint32_t needed = (image_size + segment_size - 1u) / segment_size;
    if (needed > count) return 0;
    for (uint32_t i = 0; i < needed; ++i) {
        const uint32_t left = image_size - i * segment_size;
        table[i].address = addresses[i];
        table[i].size = left < segment_size ? left : segment_size;
    }
    return needed;
}

/* ---- context array and rings ---- */

int tb710fu_wifi_mhi_plan_make(uint64_t iova, Tb710fuWifiMhiPlan *plan)
{
    if (!plan) return 0;
    for (unsigned i = 0; i < sizeof *plan; ++i) ((uint8_t *)plan)[i] = 0;
    const uint32_t ch_off = 0;
    const uint32_t ev_off = ch_off + TB710FU_WIFI_MHI_CHANNELS * TB710FU_WIFI_MHI_CTX_BYTES;
    const uint32_t cmd_off = ev_off + TB710FU_WIFI_MHI_EVENTS * TB710FU_WIFI_MHI_CTX_BYTES;
    uint32_t ring = (cmd_off + TB710FU_WIFI_MHI_CTX_BYTES + 4095u) & ~4095u;
    plan->ch_off = ch_off;
    plan->ev_off = ev_off;
    plan->cmd_off = cmd_off;
    plan->ring_off = ring;
    for (unsigned i = 0; i < TB710FU_WIFI_MHI_EVENTS; ++i) {
        const uint32_t elems = i == 0 ? TB710FU_WIFI_MHI_EV0_ELEMS : TB710FU_WIFI_MHI_EV1_ELEMS;
        plan->ev_bytes[i] = elems * TB710FU_WIFI_MHI_ELEM_BYTES;
        plan->ev_base[i] = iova + ring;
        ring += plan->ev_bytes[i];
    }
    plan->cmd_base = iova + ring;
    plan->cmd_bytes = TB710FU_WIFI_MHI_CMD_ELEMS * TB710FU_WIFI_MHI_ELEM_BYTES;
    ring += plan->cmd_bytes;
    plan->total_bytes = (ring + 4095u) & ~4095u;
    return 1;
}

static void put32(uint8_t *buffer, uint32_t at, uint32_t value)
{
    buffer[at + 0] = (uint8_t)value;
    buffer[at + 1] = (uint8_t)(value >> 8);
    buffer[at + 2] = (uint8_t)(value >> 16);
    buffer[at + 3] = (uint8_t)(value >> 24);
}

static void put64(uint8_t *buffer, uint32_t at, uint64_t value)
{
    put32(buffer, at, (uint32_t)value);
    put32(buffer, at + 4, (uint32_t)(value >> 32));
}

int tb710fu_wifi_mhi_contexts(uint8_t *buffer, uint32_t bytes, uint64_t iova, uint32_t msi_vector,
                              const Tb710fuWifiMhiPlan *plan)
{
    if (!buffer || !plan || bytes < plan->total_bytes) return 0;
    for (unsigned i = 0; i < bytes; ++i) buffer[i] = 0;
    for (unsigned i = 0; i < TB710FU_WIFI_MHI_EVENTS; ++i) {
        const uint32_t at = plan->ev_off + i * TB710FU_WIFI_MHI_CTX_BYTES;
        put32(buffer, at + TB710FU_WIFI_MHI_CTX_MODERATION, i == 0 ? 0u : (1u << 16));
        put32(buffer, at + TB710FU_WIFI_MHI_CTX_TYPE, TB710FU_WIFI_MHI_ER_TYPE_VALID);
        put32(buffer, at + TB710FU_WIFI_MHI_CTX_MSIVEC, msi_vector + i);
        put64(buffer, at + TB710FU_WIFI_MHI_CTX_BASE, plan->ev_base[i]);
        put64(buffer, at + TB710FU_WIFI_MHI_CTX_LEN, plan->ev_bytes[i]);
        put64(buffer, at + TB710FU_WIFI_MHI_CTX_RP, plan->ev_base[i]);
        put64(buffer, at + TB710FU_WIFI_MHI_CTX_WP + 0, plan->ev_base[i] + plan->ev_bytes[i] - TB710FU_WIFI_MHI_ELEM_BYTES);
    }
    const uint32_t cmd = plan->cmd_off;
    put64(buffer, cmd + TB710FU_WIFI_MHI_CTX_BASE, plan->cmd_base);
    put64(buffer, cmd + TB710FU_WIFI_MHI_CTX_LEN, plan->cmd_bytes);
    put64(buffer, cmd + TB710FU_WIFI_MHI_CTX_RP, plan->cmd_base);
    put64(buffer, cmd + TB710FU_WIFI_MHI_CTX_WP, plan->cmd_base);
    (void)iova; /* the plan already carries device addresses; kept for symmetry with the register builder */
    return 1;
}

int tb710fu_wifi_mhi_mmio(const Tb710fuWifiMhiPlan *plan, uint64_t iova, Tb710fuWifiMhiMmio *mmio)
{
    if (!plan || !mmio) return 0;
    const uint64_t ch = iova + plan->ch_off;
    const uint64_t ev = iova + plan->ev_off;
    const uint64_t cmd = iova + plan->cmd_off;
    mmio->words[0][0] = TB710FU_WIFI_MHI_CTX_CH_BASE;  mmio->words[0][1] = (uint32_t)ch;
    mmio->words[1][0] = TB710FU_WIFI_MHI_CTX_EV_BASE;  mmio->words[1][1] = (uint32_t)ev;
    mmio->words[2][0] = TB710FU_WIFI_MHI_CTX_CMD_BASE; mmio->words[2][1] = (uint32_t)cmd;
    mmio->words[3][0] = TB710FU_WIFI_MHI_CTRL_BASE;    mmio->words[3][1] = 0;
    mmio->words[4][0] = TB710FU_WIFI_MHI_CTRL_LIMIT;   mmio->words[4][1] = 0xffffffffu;
    mmio->words[5][0] = TB710FU_WIFI_MHI_DATA_BASE;    mmio->words[5][1] = 0;
    mmio->words[6][0] = TB710FU_WIFI_MHI_DATA_LIMIT;   mmio->words[6][1] = 0xffffffffu;
    mmio->cfg_mask = TB710FU_WIFI_MHI_CFG_NER_MASK << TB710FU_WIFI_MHI_CFG_NER_SHIFT;
    mmio->cfg_value = TB710FU_WIFI_MHI_EVENTS << TB710FU_WIFI_MHI_CFG_NER_SHIFT;
    return 1;
}

uint64_t tb710fu_wifi_mhi_ring_advance(uint64_t pointer, uint64_t base, uint32_t bytes)
{
    if (!bytes || bytes < TB710FU_WIFI_MHI_ELEM_BYTES) return base;
    pointer += TB710FU_WIFI_MHI_ELEM_BYTES;
    if (pointer >= base + bytes) pointer = base;
    return pointer;
}

void tb710fu_wifi_mhi_event_read(const uint8_t *entry, Tb710fuWifiMhiEvent *event)
{
    if (!entry || !event) return;
    for (unsigned i = 0; i < 4; ++i) {
        event->dword[i] = (uint32_t)entry[4 * i] | ((uint32_t)entry[4 * i + 1] << 8) |
                          ((uint32_t)entry[4 * i + 2] << 16) | ((uint32_t)entry[4 * i + 3] << 24);
    }
}
