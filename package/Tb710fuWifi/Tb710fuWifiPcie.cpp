/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Stage 1: SM8650 PCIe0 host controller + TB710FU board power for the WCN7850.
 * Sequence provenance: UniXNU-sheng package/WifiProbe.cpp chipPowerUp()/bringUp() (hardware-verified on SM8550, the same
 * PCIe IP the SM8650 DTS lists as "qcom,pcie-sm8550"), with every register offset below re-derived from SM8650's own
 * DTS nodes and upstream gcc-sm8650.c/pcie-qcom.c. GPIOs 16/17/94/96 are this board's (sm8650-lenovo-tb710fu.dts). */

#include "Tb710fuWifiPcie.h"
#include "Tb710fuWifiRegs.h"
#include "Tb710fuWifiBoard.h"
#include "Tb710fuWifiPcieTables.h"

#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>
#include <string.h>

namespace {

constexpr uint32_t kGccSize = 0x1f4200;
constexpr uint32_t kTlmmSize = 0xa0000; /* covers GPIO 96 at 0x96 * 0x1000; the DTS node is 0x300000 */
constexpr uint32_t kParfSize = TB710FU_WIFI_PCIE_PARF_BYTES;
constexpr uint32_t kDbiSize = TB710FU_WIFI_PCIE_DBI_BYTES;
constexpr uint32_t kAtuSize = TB710FU_WIFI_PCIE_ATU_BYTES;
constexpr uint32_t kCfgMapSize = TB710FU_WIFI_PCIE_CFG_BYTES;
constexpr uint32_t kCfgWindow = 0x100000; /* DTS reg[4] length: bus 0..255 config space, we use bus 1 only */
constexpr uint32_t kPhySize = TB710FU_WIFI_PCIE_PHY_BYTES;

inline uint32_t regRead(volatile uint32_t *base, uint32_t offset)
{
    return base[offset >> 2];
}

inline void regWrite(volatile uint32_t *base, uint32_t offset, uint32_t value)
{
    base[offset >> 2] = value;
}

inline void ioBarrier(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

IOMemoryMap *mapPhysical(uint64_t address, uint32_t bytes)
{
    IOMemoryDescriptor *desc = IOMemoryDescriptor::withPhysicalAddress(address, bytes, kIODirectionInOut);
    if (!desc) {
        return nullptr;
    }
    IOMemoryMap *map = desc->map(kIOMapAnywhere);
    desc->release();
    return map;
}

bool pollValue(volatile uint32_t *base, uint32_t offset, uint32_t mask, uint32_t want, unsigned us)
{
    for (unsigned i = 0; i < us; ++i) {
        if ((regRead(base, offset) & mask) == want) {
            return true;
        }
        IODelay(1);
    }
    return false;
}

void pulseReset(volatile uint32_t *gcc, uint32_t bcr)
{
    regWrite(gcc, bcr, regRead(gcc, bcr) | 1u);
    ioBarrier();
    IODelay(1000);
    regWrite(gcc, bcr, regRead(gcc, bcr) & ~1u);
    ioBarrier();
    IODelay(1000);
}

/* One TLMM pad: cfg word then, if level >= 0, the IO word (2 high, 0 low). */
void padWrite(volatile uint32_t *tlmm, uint32_t pin, uint32_t cfg, int level)
{
    const uint32_t cfgOffset = pin * TB710FU_WIFI_TLMM_STRIDE;
    regWrite(tlmm, cfgOffset, cfg);
    ioBarrier();
    if (level >= 0) {
        regWrite(tlmm, cfgOffset + TB710FU_WIFI_TLMM_IO_OFFSET, tb710fu_wifi_tlmm_level(level));
        ioBarrier();
    }
}

uint8_t crc8Bdf(uint16_t bdf)
{
    uint8_t crc = 0;
    const uint8_t bytes[2] = { (uint8_t)(bdf >> 8), (uint8_t)bdf };
    for (unsigned i = 0; i < 2; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 7) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

} /* namespace */

uint32_t tb710fu_wifi_pcie_cfg_read(struct Tb710fuWifiPcie *pci, uint32_t offset)
{
    if (!pci || !pci->cfg || offset >= kCfgMapSize) {
        return 0xffffffffu;
    }
    return regRead(pci->cfg, offset);
}

void tb710fu_wifi_pcie_cfg_write(struct Tb710fuWifiPcie *pci, uint32_t offset, uint32_t value)
{
    if (!pci || !pci->cfg || offset >= kCfgMapSize) {
        return;
    }
    regWrite(pci->cfg, offset, value);
    ioBarrier();
}

int tb710fu_wifi_pcie_power(struct Tb710fuWifiPcie *pci)
{
    if (!pci) {
        return TB710FU_WIFI_PCIE_ERR_MAP;
    }
    memset(pci, 0, sizeof(*pci));

    pci->gccMap = mapPhysical(TB710FU_WIFI_GCC_BASE, kGccSize);
    pci->tlmmMap = mapPhysical(TB710FU_WIFI_TLMM_BASE, kTlmmSize);
    pci->tcsrMap = mapPhysical(TB710FU_WIFI_TCSR_BASE, TB710FU_WIFI_TCSR_BYTES);
    pci->parfMap = mapPhysical(TB710FU_WIFI_PCIE_PARF_BASE, kParfSize);
    pci->dbiMap = mapPhysical(TB710FU_WIFI_PCIE_DBI_BASE, kDbiSize);
    if (!pci->gccMap || !pci->tlmmMap || !pci->tcsrMap || !pci->parfMap || !pci->dbiMap) {
        return TB710FU_WIFI_PCIE_ERR_MAP;
    }
    pci->gcc = (volatile uint32_t *)pci->gccMap->getVirtualAddress();
    pci->tlmm = (volatile uint32_t *)pci->tlmmMap->getVirtualAddress();
    pci->tcsr = (volatile uint32_t *)pci->tcsrMap->getVirtualAddress();
    pci->parf = (volatile uint32_t *)pci->parfMap->getVirtualAddress();
    pci->dbi = (volatile uint32_t *)pci->dbiMap->getVirtualAddress();
    if (!pci->gcc || !pci->tlmm || !pci->tcsr || !pci->parf || !pci->dbi) {
        return TB710FU_WIFI_PCIE_ERR_MAP;
    }

    /* WLAN_EN stays low while the PMIC rails settle; PERST# is held asserted across the whole power-up (the 100 ms
     * minimum is covered by the 150 ms below). */
    struct tb710fu_wifi_pin_plan pins;
    tb710fu_wifi_pin_plan(&pins);
    padWrite(pci->tlmm, TB710FU_WIFI_GPIO_PERST, pins.perst_cfg, 0);
    padWrite(pci->tlmm, TB710FU_WIFI_GPIO_WLAN_EN, pins.wlan_cfg, 0);
    padWrite(pci->tlmm, TB710FU_WIFI_GPIO_BT_EN, pins.bt_cfg, 0);
    padWrite(pci->tlmm, TB710FU_WIFI_GPIO_WAKE, pins.wake_cfg, -1);
    if ((regRead(pci->tlmm, TB710FU_WIFI_GPIO_PERST * TB710FU_WIFI_TLMM_STRIDE) & 0x3ffu) !=
        (pins.perst_cfg & 0x3ffu)) {
        return TB710FU_WIFI_PCIE_ERR_PINS;
    }
    IOSleep(5);
    padWrite(pci->tlmm, TB710FU_WIFI_GPIO_WLAN_EN, pins.wlan_cfg, 1);
    IOSleep(150);

    /* pcie_0_gdsc / pcie_0_phy_gdsc (gcc-sm8650.c 0x6b004 / 0x6c000, POLL_CFG_GDSCR|RETAIN_FF|VOTABLE): drop the
     * collapse bits, then wait for GDSC_POWER_UP_COMPLETE (bit16 of CFG_GDSCR at +4). */
    struct tb710fu_wifi_gcc_plan gcc;
    tb710fu_wifi_gcc_plan(&gcc);
    regWrite(pci->gcc, TB710FU_WIFI_GCC_COLLAPSE,
             regRead(pci->gcc, TB710FU_WIFI_GCC_COLLAPSE) & ~gcc.collapse_clear);
    ioBarrier();
    pci->gdsc_word = regRead(pci->gcc, TB710FU_WIFI_GCC_PCIE0_GDSC + TB710FU_WIFI_GCC_GDSC_CFG_DELTA);
    pci->gdsc_phy_word = regRead(pci->gcc, TB710FU_WIFI_GCC_PCIE0_PHY_GDSC + TB710FU_WIFI_GCC_GDSC_CFG_DELTA);
    if (!pollValue(pci->gcc, TB710FU_WIFI_GCC_PCIE0_GDSC + TB710FU_WIFI_GCC_GDSC_CFG_DELTA, 1u << 16, 1u << 16, 100)) {
        return TB710FU_WIFI_PCIE_ERR_GDSC;
    }
    if (!pollValue(pci->gcc, TB710FU_WIFI_GCC_PCIE0_PHY_GDSC + TB710FU_WIFI_GCC_GDSC_CFG_DELTA, 1u << 16, 1u << 16, 100)) {
        return TB710FU_WIFI_PCIE_ERR_GDSC;
    }

    /* AUX clock source: TCXO/1 (cmd_rcgr 0x6b074, mnd_width 16), so the +4 MND word is zero; rchng: GPLL0_EVEN/3 = 100
     * MHz (0x6b058, M=6 N=5). Writing 1 into the CFG word starts the branch and the bit self-clears. */
    regWrite(pci->gcc, TB710FU_WIFI_GCC_RCG_AUX + 4, 0);
    regWrite(pci->gcc, TB710FU_WIFI_GCC_RCG_AUX, regRead(pci->gcc, TB710FU_WIFI_GCC_RCG_AUX) | 1u);
    regWrite(pci->gcc, TB710FU_WIFI_GCC_RCG_RCHNG + 4, (6u << 8) | 5u);
    regWrite(pci->gcc, TB710FU_WIFI_GCC_RCG_RCHNG, regRead(pci->gcc, TB710FU_WIFI_GCC_RCG_RCHNG) | 1u);
    ioBarrier();
    if (!pollValue(pci->gcc, TB710FU_WIFI_GCC_RCG_AUX, 1u, 0u, 100)) {
        return TB710FU_WIFI_PCIE_ERR_CLOCKS;
    }
    if (!pollValue(pci->gcc, TB710FU_WIFI_GCC_RCG_RCHNG, 1u, 0u, 100)) {
        return TB710FU_WIFI_PCIE_ERR_CLOCKS;
    }
    /* Pipe mux: 2 = TCXO before the PHY can drive it; later the PHY step clears it to select the PHY's pipe. */
    regWrite(pci->gcc, TB710FU_WIFI_GCC_PIPE_MUX,
             (regRead(pci->gcc, TB710FU_WIFI_GCC_PIPE_MUX) & ~3u) | 2u);

    regWrite(pci->gcc, TB710FU_WIFI_GCC_VOTE8, regRead(pci->gcc, TB710FU_WIFI_GCC_VOTE8) | gcc.vote8_mask);
    regWrite(pci->gcc, TB710FU_WIFI_GCC_VOTE0, regRead(pci->gcc, TB710FU_WIFI_GCC_VOTE0) | gcc.vote0_mask);
    ioBarrier();
    IODelay(200);
    pci->branch_halts = regRead(pci->gcc, TB710FU_WIFI_GCC_VOTE0) & 0x7fu;
    if (tb710fu_wifi_branch_halted(regRead(pci->gcc, TB710FU_WIFI_GCC_HALT_SLV)) ||
        tb710fu_wifi_branch_halted(regRead(pci->gcc, TB710FU_WIFI_GCC_HALT_MSTR)) ||
        tb710fu_wifi_branch_halted(regRead(pci->gcc, TB710FU_WIFI_GCC_HALT_CFG)) ||
        tb710fu_wifi_branch_halted(regRead(pci->gcc, TB710FU_WIFI_GCC_HALT_AUX))) {
        return TB710FU_WIFI_PCIE_ERR_CLOCKS;
    }

    /* TCSR PCIE_0_CLKREF_EN (tcsrcc-sm8650.c 0x31100 bit0; TCSR base from sm8650.dtsi tcsr@1fc0000). */
    if (!(regRead(pci->tcsr, TB710FU_WIFI_TCSR_CLKREF_OFF) & 1u)) {
        regWrite(pci->tcsr, TB710FU_WIFI_TCSR_CLKREF_OFF,
                 regRead(pci->tcsr, TB710FU_WIFI_TCSR_CLKREF_OFF) | TB710FU_WIFI_TCSR_CLKREF_EN);
        ioBarrier();
    }
    pci->used_clkref = (regRead(pci->tcsr, TB710FU_WIFI_TCSR_CLKREF_OFF) & 1u) ? 1 : 0;
    if (!pci->used_clkref) {
        return TB710FU_WIFI_PCIE_ERR_CLKREF;
    }

    pulseReset(pci->gcc, TB710FU_WIFI_GCC_PCIE0_BCR);
    pulseReset(pci->gcc, TB710FU_WIFI_GCC_PCIE0_PHY_BCR);
    if ((regRead(pci->gcc, TB710FU_WIFI_GCC_PCIE0_BCR) & 1u) ||
        (regRead(pci->gcc, TB710FU_WIFI_GCC_PCIE0_PHY_BCR) & 1u)) {
        return TB710FU_WIFI_PCIE_ERR_RESET;
    }

    /* PARF (pcie@1c00000 + 0x3000): device type 4 = root complex, DBI/ATU CPU base addresses, slave space aperture. */
    regWrite(pci->parf, TB710FU_WIFI_PARF_DEVICE_TYPE, TB710FU_WIFI_PARF_DEVICE_TYPE_RC);
    regWrite(pci->parf, TB710FU_WIFI_PARF_PHY_CTRL,
             regRead(pci->parf, TB710FU_WIFI_PARF_PHY_CTRL) & ~TB710FU_WIFI_PARF_PHY_TEST_PWR_DOWN);
    regWrite(pci->parf, TB710FU_WIFI_PARF_DBI_BASE_V2, (uint32_t)TB710FU_WIFI_PCIE_DBI_BASE);
    regWrite(pci->parf, TB710FU_WIFI_PARF_DBI_BASE_V2_HI, (uint32_t)(TB710FU_WIFI_PCIE_DBI_BASE >> 32));
    regWrite(pci->parf, TB710FU_WIFI_PARF_ATU_BASE, (uint32_t)TB710FU_WIFI_PCIE_ATU_BASE);
    regWrite(pci->parf, TB710FU_WIFI_PARF_ATU_BASE_HI, (uint32_t)(TB710FU_WIFI_PCIE_ATU_BASE >> 32));
    regWrite(pci->parf, TB710FU_WIFI_PARF_SLV_SPACE_V2, 0);
    regWrite(pci->parf, TB710FU_WIFI_PARF_SLV_SPACE_V2_HI, 0x80000000u);
    regWrite(pci->parf, TB710FU_WIFI_PARF_SYS_CTRL,
             regRead(pci->parf, TB710FU_WIFI_PARF_SYS_CTRL) & ~(1u << 29));
    regWrite(pci->parf, TB710FU_WIFI_PARF_MHI_CLOCK_RESET_CTRL,
             regRead(pci->parf, TB710FU_WIFI_PARF_MHI_CLOCK_RESET_CTRL) | TB710FU_WIFI_PARF_MHI_AHB_CLK_EN);
    regWrite(pci->parf, TB710FU_WIFI_PARF_PM_CTRL,
             regRead(pci->parf, TB710FU_WIFI_PARF_PM_CTRL) & ~(1u << 5));
    regWrite(pci->parf, TB710FU_WIFI_PARF_AXI_MSTR_WR_ADDR_HALT_V2,
             regRead(pci->parf, TB710FU_WIFI_PARF_AXI_MSTR_WR_ADDR_HALT_V2) | (1u << 31));
    ioBarrier();

    /* PHY: qmp_pcie_power_on order (power down, serdes, both lanes' tx/rx, pcs, pcs_misc, pipe mux to the PHY, out of
     * sw reset, start, poll PHYSTATUS). Tables are the v5 configuration reused below in Tb710fuWifiPcieTables.h. */
    pci->phyMap = mapPhysical(TB710FU_WIFI_PCIE_PHY_BASE, kPhySize);
    if (!pci->phyMap) {
        return TB710FU_WIFI_PCIE_ERR_PHY;
    }
    pci->phy = (volatile uint32_t *)pci->phyMap->getVirtualAddress();
    if (!pci->phy) {
        return TB710FU_WIFI_PCIE_ERR_PHY;
    }
    volatile uint32_t *ph = pci->phy;
    regWrite(ph, TB710FU_WIFI_PHY_POWER_DOWN, regRead(ph, TB710FU_WIFI_PHY_POWER_DOWN) | 3u);
    for (unsigned i = 0; i < sizeof(tb710fu_wifi_phy_serdes) / sizeof(tb710fu_wifi_phy_serdes[0]); ++i) {
        regWrite(ph, tb710fu_wifi_phy_serdes[i].offset, tb710fu_wifi_phy_serdes[i].value);
    }
    for (unsigned i = 0; i < sizeof(tb710fu_wifi_phy_tx) / sizeof(tb710fu_wifi_phy_tx[0]); ++i) {
        regWrite(ph, tb710fu_wifi_phy_tx[i].offset, tb710fu_wifi_phy_tx[i].value);
        regWrite(ph, tb710fu_wifi_phy_tx[i].offset + TB710FU_WIFI_PHY_TX2_DELTA, tb710fu_wifi_phy_tx[i].value);
    }
    for (unsigned i = 0; i < sizeof(tb710fu_wifi_phy_rx) / sizeof(tb710fu_wifi_phy_rx[0]); ++i) {
        regWrite(ph, tb710fu_wifi_phy_rx[i].offset, tb710fu_wifi_phy_rx[i].value);
        regWrite(ph, tb710fu_wifi_phy_rx[i].offset + TB710FU_WIFI_PHY_RX2_DELTA, tb710fu_wifi_phy_rx[i].value);
    }
    for (unsigned i = 0; i < sizeof(tb710fu_wifi_phy_pcs) / sizeof(tb710fu_wifi_phy_pcs[0]); ++i) {
        regWrite(ph, tb710fu_wifi_phy_pcs[i].offset, tb710fu_wifi_phy_pcs[i].value);
    }
    for (unsigned i = 0; i < sizeof(tb710fu_wifi_phy_pcs_misc) / sizeof(tb710fu_wifi_phy_pcs_misc[0]); ++i) {
        regWrite(ph, tb710fu_wifi_phy_pcs_misc[i].offset, tb710fu_wifi_phy_pcs_misc[i].value);
    }
    regWrite(pci->gcc, TB710FU_WIFI_GCC_PIPE_MUX, regRead(pci->gcc, TB710FU_WIFI_GCC_PIPE_MUX) & ~3u);
    regWrite(ph, TB710FU_WIFI_PHY_SW_RESET, regRead(ph, TB710FU_WIFI_PHY_SW_RESET) & ~1u);
    regWrite(ph, TB710FU_WIFI_PHY_START_CONTROL, regRead(ph, TB710FU_WIFI_PHY_START_CONTROL) | 3u);
    ioBarrier();
    IODelay(1000);
    if (!pollValue(ph, TB710FU_WIFI_PHY_PCS_STATUS1, TB710FU_WIFI_PHY_STATUS_BIT, 0u, 100)) {
        return TB710FU_WIFI_PCIE_ERR_PHY;
    }

    /* qcom_pcie_config_sid_1_9_0: 256-entry requester-ID hash (BDF << 16 | stream offset << 8 | next), CRC-8 poly 7
     * over the requester's two bytes high-first. iommu-map: bus 1 requester 0x100 -> SID 0x1401. */
    for (unsigned i = 0; i < 256; ++i) {
        regWrite(pci->parf, TB710FU_WIFI_PARF_BDF_TO_SID + 4 * i, 0);
    }
    regWrite(pci->parf, TB710FU_WIFI_PARF_BDF_TO_SID_CFG,
             regRead(pci->parf, TB710FU_WIFI_PARF_BDF_TO_SID_CFG) & ~TB710FU_WIFI_PARF_BDF_TO_SID_BYPASS);
    const uint8_t hash = crc8Bdf((uint16_t)((TB710FU_WIFI_PCI_BUS << 8) |
                                            (TB710FU_WIFI_PCI_DEV << 3) | TB710FU_WIFI_PCI_FN));
    regWrite(pci->parf, TB710FU_WIFI_PARF_BDF_TO_SID + 4 * hash, (0x100u << 16) | (1u << 8));
    ioBarrier();

    /* Root port DBI: primary 0, secondary 1, subordinate 1; memory decode and bus master on. */
    regWrite(pci->dbi, TB710FU_WIFI_PCI_BUS_NUMBERS, 0x00010100u);
    regWrite(pci->dbi, TB710FU_WIFI_PCI_COMMAND,
             regRead(pci->dbi, TB710FU_WIFI_PCI_COMMAND) | TB710FU_WIFI_PCI_COMMAND_MEM_EN |
             TB710FU_WIFI_PCI_COMMAND_BUS_MASTER);
    ioBarrier();

    /* qcom_ep_reset_deassert: PERST# high, 1 ms, then LTSSM enable; wait for L0 (port_debug0 5:0 = 0x11, debug1 bit4). */
    padWrite(pci->tlmm, TB710FU_WIFI_GPIO_PERST, pins.perst_cfg, 1);
    IODelay(1000);
    regWrite(pci->parf, TB710FU_WIFI_PARF_LTSSM,
             regRead(pci->parf, TB710FU_WIFI_PARF_LTSSM) | TB710FU_WIFI_PARF_LTSSM_EN);
    ioBarrier();
    pci->ltssm = regRead(pci->parf, TB710FU_WIFI_PARF_LTSSM);
    for (unsigned ms = 0; ms < 1000; ++ms) {
        pci->port_debug0 = regRead(pci->dbi, TB710FU_WIFI_PCI_PORT_DEBUG0);
        pci->port_debug1 = regRead(pci->dbi, TB710FU_WIFI_PCI_PORT_DEBUG1);
        if ((pci->port_debug1 & (1u << 4)) &&
            ((pci->port_debug0 & TB710FU_WIFI_PARF_LTSSM_STATE_MASK) == 0x11u)) {
            pci->link_up = 1;
            break;
        }
        IOSleep(1);
    }
    pci->link_control = regRead(pci->dbi, TB710FU_WIFI_PCI_LINK_CONTROL);
    if (!pci->link_up) {
        return TB710FU_WIFI_PCIE_ERR_LINK;
    }
    return TB710FU_WIFI_PCIE_OK;
}

int tb710fu_wifi_pcie_config_window(struct Tb710fuWifiPcie *pci)
{
    if (!pci) {
        return TB710FU_WIFI_PCIE_ERR_MAP;
    }
    if (!pci->atu || !pci->cfg) {
        pci->atuMap = mapPhysical(TB710FU_WIFI_PCIE_ATU_BASE, kAtuSize);
        if (pci->atuMap) {
            pci->atu = (volatile uint32_t *)pci->atuMap->getVirtualAddress();
        }
        pci->cfgMap = mapPhysical(TB710FU_WIFI_PCIE_CFG_BASE, kCfgMapSize);
        if (pci->cfgMap) {
            pci->cfg = (volatile uint32_t *)pci->cfgMap->getVirtualAddress();
        }
    }
    if (!pci->atu || !pci->cfg) {
        return TB710FU_WIFI_PCIE_ERR_MAP;
    }

    /* Region 0: 1 MiB of config space at CPU 0x60100000 becomes type-0 config cycles to bus 1, device 0. */
    struct tb710fu_wifi_atu_region r0;
    tb710fu_wifi_atu_config(&r0, TB710FU_WIFI_PCIE_CFG_BASE, kCfgWindow, TB710FU_WIFI_PCI_BUS);
    volatile uint32_t *a = pci->atu;
    regWrite(a, TB710FU_WIFI_ATU_CTRL1, r0.ctrl1);
    regWrite(a, TB710FU_WIFI_ATU_CTRL2, 0);
    regWrite(a, TB710FU_WIFI_ATU_LOWER_BASE, (uint32_t)r0.base);
    regWrite(a, TB710FU_WIFI_ATU_UPPER_BASE, (uint32_t)(r0.base >> 32));
    regWrite(a, TB710FU_WIFI_ATU_LOWER_LIMIT, (uint32_t)r0.limit);
    regWrite(a, TB710FU_WIFI_ATU_UPPER_LIMIT, (uint32_t)(r0.limit >> 32));
    regWrite(a, TB710FU_WIFI_ATU_LOWER_TARGET, (uint32_t)r0.target);
    regWrite(a, TB710FU_WIFI_ATU_UPPER_TARGET, (uint32_t)(r0.target >> 32));
    regWrite(a, TB710FU_WIFI_ATU_CTRL2, r0.ctrl2);
    ioBarrier();
    if (!pollValue(a, TB710FU_WIFI_ATU_CTRL2, TB710FU_WIFI_ATU_CTRL2_ENABLE,
                   TB710FU_WIFI_ATU_CTRL2_ENABLE, 100)) {
        return TB710FU_WIFI_PCIE_ERR_CONFIG;
    }
    pci->config_window = 1;

    /* Region 1: identity-map the DTS memory window (ranges: 0x60300000 + 0x3d00000), so a BAR placed there is
     * reachable at the CPU address the DTS gives it, which is also its PCI address. */
    struct tb710fu_wifi_atu_region r1;
    tb710fu_wifi_atu_memory(&r1, TB710FU_WIFI_PCIE_MMIO_BASE, TB710FU_WIFI_PCIE_MMIO_BYTES,
                            TB710FU_WIFI_PCIE_MMIO_BASE);
    volatile uint32_t *a1 = (volatile uint32_t *)((uintptr_t)a + TB710FU_WIFI_ATU_REGION_STRIDE);
    regWrite(a1, TB710FU_WIFI_ATU_CTRL1, r1.ctrl1);
    regWrite(a1, TB710FU_WIFI_ATU_CTRL2, 0);
    regWrite(a1, TB710FU_WIFI_ATU_LOWER_BASE, (uint32_t)r1.base);
    regWrite(a1, TB710FU_WIFI_ATU_UPPER_BASE, (uint32_t)(r1.base >> 32));
    regWrite(a1, TB710FU_WIFI_ATU_LOWER_LIMIT, (uint32_t)r1.limit);
    regWrite(a1, TB710FU_WIFI_ATU_UPPER_LIMIT, (uint32_t)(r1.limit >> 32));
    regWrite(a1, TB710FU_WIFI_ATU_LOWER_TARGET, (uint32_t)r1.target);
    regWrite(a1, TB710FU_WIFI_ATU_UPPER_TARGET, (uint32_t)(r1.target >> 32));
    regWrite(a1, TB710FU_WIFI_ATU_CTRL2, r1.ctrl2);
    ioBarrier();
    if (!pollValue(a1, TB710FU_WIFI_ATU_CTRL2, TB710FU_WIFI_ATU_CTRL2_ENABLE,
                   TB710FU_WIFI_ATU_CTRL2_ENABLE, 100)) {
        return TB710FU_WIFI_PCIE_ERR_CONFIG;
    }
    pci->memory_window = 1;

    pci->endpoint_id = tb710fu_wifi_pcie_cfg_read(pci, 0);
    if ((pci->endpoint_id & 0xffffu) != TB710FU_WIFI_PCI_VENDOR ||
        ((pci->endpoint_id >> 16) & 0xffffu) != TB710FU_WIFI_PCI_DEVICE) {
        return TB710FU_WIFI_PCIE_ERR_ENDPOINT;
    }
    return TB710FU_WIFI_PCIE_OK;
}

int tb710fu_wifi_pcie_claim_bar(struct Tb710fuWifiPcie *pci)
{
    if (!pci || !pci->cfg || !pci->config_window) {
        return TB710FU_WIFI_PCIE_ERR_CONFIG;
    }
    uint32_t command = tb710fu_wifi_pcie_cfg_read(pci, TB710FU_WIFI_PCI_COMMAND);
    tb710fu_wifi_pcie_cfg_write(pci, TB710FU_WIFI_PCI_COMMAND, command & ~TB710FU_WIFI_PCI_COMMAND_MEM_EN);
    tb710fu_wifi_pcie_cfg_write(pci, TB710FU_WIFI_PCI_BAR0, 0xffffffffu);
    uint32_t mask = tb710fu_wifi_pcie_cfg_read(pci, TB710FU_WIFI_PCI_BAR0);
    tb710fu_wifi_pcie_cfg_write(pci, TB710FU_WIFI_PCI_BAR0, 0);
    if (mask == 0 || mask == 0xffffffffu) {
        return TB710FU_WIFI_PCIE_ERR_BAR;
    }
    pci->bar0_mask = mask;
    pci->bar0_size = (uint32_t)tb710fu_wifi_bar_size(mask);
    uint64_t address = 0;
    if (!pci->bar0_size ||
        !tb710fu_wifi_bar_place(pci->bar0_size, &address, TB710FU_WIFI_PCIE_MMIO_BASE,
                                TB710FU_WIFI_PCIE_MMIO_BYTES)) {
        return TB710FU_WIFI_PCIE_ERR_BAR;
    }
    pci->bar0 = address;
    tb710fu_wifi_pcie_cfg_write(pci, TB710FU_WIFI_PCI_BAR0, (uint32_t)pci->bar0);
    if (mask & 0x4u) { /* 64-bit BAR: upper half must be zero, the window is below 4 GiB */
        tb710fu_wifi_pcie_cfg_write(pci, TB710FU_WIFI_PCI_BAR0 + 4, (uint32_t)(pci->bar0 >> 32));
    }
    tb710fu_wifi_pcie_cfg_write(pci, TB710FU_WIFI_PCI_COMMAND,
                                tb710fu_wifi_pcie_cfg_read(pci, TB710FU_WIFI_PCI_COMMAND) |
                                TB710FU_WIFI_PCI_COMMAND_MEM_EN | TB710FU_WIFI_PCI_COMMAND_BUS_MASTER);
    ioBarrier();
    uint32_t readback = tb710fu_wifi_pcie_cfg_read(pci, TB710FU_WIFI_PCI_BAR0);
    if ((readback & ~0xfu) != ((uint32_t)pci->bar0 & ~0xfu)) {
        return TB710FU_WIFI_PCIE_ERR_BAR;
    }
    return TB710FU_WIFI_PCIE_OK;
}

int tb710fu_wifi_pcie_msi_sink(struct Tb710fuWifiPcie *pci, uint64_t sink)
{
    if (!pci || !pci->cfg || !sink || (sink >> 32)) {
        return TB710FU_WIFI_PCIE_ERR_MSI;
    }
    uint32_t cap = tb710fu_wifi_pcie_cfg_read(pci, TB710FU_WIFI_PCI_CAP_PTR) & 0xffu;
    for (unsigned guard = 0; guard < 48 && cap >= 0x40u && cap < 0x100u; ++guard) {
        uint32_t id = tb710fu_wifi_pcie_cfg_read(pci, cap) & 0xffu;
        uint32_t next = (tb710fu_wifi_pcie_cfg_read(pci, cap) >> 8) & 0xffu;
        if (id == 0x05u) { /* MSI: one message, 32-bit address, no masking, enabled */
            uint32_t control = tb710fu_wifi_pcie_cfg_read(pci, cap + 2) & ~0x70u;
            tb710fu_wifi_pcie_cfg_write(pci, cap + 4, (uint32_t)sink);
            tb710fu_wifi_pcie_cfg_write(pci, cap + 8, 0u);
            tb710fu_wifi_pcie_cfg_write(pci, cap + 2, control | 0x1u);
            ioBarrier();
            return TB710FU_WIFI_PCIE_OK;
        }
        if (id == 0x10u || id == 0x11u) { /* PCIe / MSI-X: keep MSI, no INTx on this platform */
            break;
        }
        cap = next;
    }
    return TB710FU_WIFI_PCIE_ERR_MSI;
}

int tb710fu_wifi_pcie_map_bar(struct Tb710fuWifiPcie *pci)
{
    if (!pci || !pci->bar0 || !pci->bar0_size) {
        return TB710FU_WIFI_PCIE_ERR_BAR;
    }
    pci->barMap = mapPhysical(pci->bar0, pci->bar0_size);
    if (!pci->barMap) {
        return TB710FU_WIFI_PCIE_ERR_MAP;
    }
    pci->bar = (volatile uint32_t *)pci->barMap->getVirtualAddress();
    return pci->bar ? TB710FU_WIFI_PCIE_OK : TB710FU_WIFI_PCIE_ERR_MAP;
}

void tb710fu_wifi_pcie_release(struct Tb710fuWifiPcie *pci)
{
    if (!pci) {
        return;
    }
    IOMemoryMap *maps[] = { pci->barMap, pci->phyMap, pci->cfgMap, pci->atuMap, pci->dbiMap,
                            pci->parfMap, pci->tcsrMap, pci->tlmmMap, pci->gccMap };
    for (unsigned i = 0; i < sizeof(maps) / sizeof(maps[0]); ++i) {
        if (maps[i]) {
            maps[i]->release();
        }
    }
    pci->barMap = pci->phyMap = pci->cfgMap = pci->atuMap = pci->dbiMap = nullptr;
    pci->parfMap = pci->tcsrMap = pci->tlmmMap = pci->gccMap = nullptr;
    pci->bar = pci->phy = pci->cfg = pci->atu = pci->dbi = pci->parf = pci->tcsr = pci->tlmm = pci->gcc = nullptr;
}
