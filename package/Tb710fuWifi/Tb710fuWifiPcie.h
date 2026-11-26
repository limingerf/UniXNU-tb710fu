/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Stage 1 of the TB710FU Wi-Fi bring-up: board power, PCIe0 root complex, PHY, link training and the endpoint's
 * configuration space. All addresses come from Tb710fuWifiRegs.h (DTS provenance there); the write order follows the
 * hardware-verified sequence in UniXNU-sheng's WifiProbe.cpp, re-checked against SM8650's own GCC/PARF offsets.
 * The caller owns the stage decision: this file only reports how far it got. */
#ifndef TB710FU_WIFI_PCIE_H
#define TB710FU_WIFI_PCIE_H

#include <stdint.h>

#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOMemoryMap.h>

struct Tb710fuWifiPcie {
    IOMemoryMap *gccMap, *tlmmMap, *tcsrMap, *parfMap, *dbiMap, *atuMap, *cfgMap, *phyMap, *barMap;
    volatile uint32_t *gcc, *tlmm, *tcsr, *parf, *dbi, *atu, *cfg, *phy, *bar;

    uint32_t gdsc_word, gdsc_phy_word, branch_halts;
    uint32_t ltssm, port_debug0, port_debug1, link_control;
    uint32_t endpoint_id;      /* vendor | device << 16, as configuration space returns it */
    uint32_t bar0_mask;
    uint32_t bar0_size;
    uint64_t bar0;             /* CPU address of the endpoint's BAR0 (identity-mapped PCI memory window) */
    uint8_t link_up;
    uint8_t config_window;     /* outbound iATU region 0 points at bus 1 */
    uint8_t memory_window;     /* outbound iATU region 1 identity-maps the DTS MMIO window */
    uint8_t used_clkref;
};

/* Negative returns name the step that failed; the struct keeps the last values read for the boot log. */
enum {
    TB710FU_WIFI_PCIE_OK = 0,
    TB710FU_WIFI_PCIE_ERR_MAP = -1,
    TB710FU_WIFI_PCIE_ERR_PINS = -2,
    TB710FU_WIFI_PCIE_ERR_GDSC = -3,
    TB710FU_WIFI_PCIE_ERR_CLOCKS = -4,
    TB710FU_WIFI_PCIE_ERR_CLKREF = -5,
    TB710FU_WIFI_PCIE_ERR_RESET = -6,
    TB710FU_WIFI_PCIE_ERR_PARF = -7,
    TB710FU_WIFI_PCIE_ERR_PHY = -8,
    TB710FU_WIFI_PCIE_ERR_SID = -9,
    TB710FU_WIFI_PCIE_ERR_LINK = -10,
    TB710FU_WIFI_PCIE_ERR_CONFIG = -11,
    TB710FU_WIFI_PCIE_ERR_ENDPOINT = -12,
    TB710FU_WIFI_PCIE_ERR_BAR = -13,
    TB710FU_WIFI_PCIE_ERR_MSI = -14,
};

int tb710fu_wifi_pcie_power(struct Tb710fuWifiPcie *pci);
int tb710fu_wifi_pcie_config_window(struct Tb710fuWifiPcie *pci);
int tb710fu_wifi_pcie_claim_bar(struct Tb710fuWifiPcie *pci);
int tb710fu_wifi_pcie_msi_sink(struct Tb710fuWifiPcie *pci, uint64_t sink);
int tb710fu_wifi_pcie_map_bar(struct Tb710fuWifiPcie *pci);
void tb710fu_wifi_pcie_release(struct Tb710fuWifiPcie *pci);

uint32_t tb710fu_wifi_pcie_cfg_read(struct Tb710fuWifiPcie *pci, uint32_t offset);
void tb710fu_wifi_pcie_cfg_write(struct Tb710fuWifiPcie *pci, uint32_t offset, uint32_t value);

#endif /* TB710FU_WIFI_PCIE_H */
