#ifndef TB710FU_CDC_TRANSPORT_GUARD_H
#define TB710FU_CDC_TRANSPORT_GUARD_H

#include <stdint.h>

#define TB710FU_CDC_DWC3_STREAM_ID 0x40u
#define TB710FU_CDC_DMA_BYTES 0x4000u
#define TB710FU_CDC_DMA_ALIGNMENT 0x4000u

enum tb710fu_cdc_usb_role {
    TB710FU_CDC_USB_ROLE_UNKNOWN,
    TB710FU_CDC_USB_ROLE_HOST,
    TB710FU_CDC_USB_ROLE_DEVICE,
    TB710FU_CDC_USB_ROLE_OTG_HOST,
    TB710FU_CDC_USB_ROLE_OTG_PERIPHERAL
};

enum tb710fu_cdc_smmu_route {
    TB710FU_CDC_SMMU_ROUTE_UNKNOWN,
    TB710FU_CDC_SMMU_ROUTE_IDENTITY,
    TB710FU_CDC_SMMU_ROUTE_TRANSLATED
};

enum tb710fu_cdc_preflight_result {
    TB710FU_CDC_PREFLIGHT_READY,
    TB710FU_CDC_PREFLIGHT_DISABLED,
    TB710FU_CDC_PREFLIGHT_BAD_BOOT_ARG,
    TB710FU_CDC_PREFLIGHT_BAD_SNAPSHOT,
    TB710FU_CDC_PREFLIGHT_WRONG_USB_ROLE,
    TB710FU_CDC_PREFLIGHT_CLOCKS_UNVERIFIED,
    TB710FU_CDC_PREFLIGHT_CLOCKS_OFF,
    TB710FU_CDC_PREFLIGHT_PHY_UNVERIFIED,
    TB710FU_CDC_PREFLIGHT_PHY_NOT_READY,
    TB710FU_CDC_PREFLIGHT_SMMU_UNVERIFIED,
    TB710FU_CDC_PREFLIGHT_SMMU_ROUTE,
    TB710FU_CDC_PREFLIGHT_DMA_NOT_CONTIGUOUS,
    TB710FU_CDC_PREFLIGHT_DMA_ALIGNMENT,
    TB710FU_CDC_PREFLIGHT_DMA_SIZE,
    TB710FU_CDC_PREFLIGHT_DMA_ADDRESS_WIDTH
};

// The hardware reader must set the verified fields only after bounded read-only checks.
struct tb710fu_cdc_transport_snapshot {
    enum tb710fu_cdc_usb_role usb_role;
    uint8_t clocks_verified;
    uint8_t required_clocks_enabled;
    uint8_t phy_verified;
    uint8_t phy_ready;
    uint8_t smmu_verified;
    uint8_t smmu_identity;
    uint16_t smmu_stream_id;
    uint16_t matching_streams;
    uint8_t dma_contiguous;
    uint8_t dma_address_bits;
    uint64_t dma_physical;
    uint64_t dma_bytes;
};

enum tb710fu_cdc_preflight_result tb710fu_cdc_preflight(
    int boot_arg_value,
    const struct tb710fu_cdc_transport_snapshot *snapshot);

#endif
