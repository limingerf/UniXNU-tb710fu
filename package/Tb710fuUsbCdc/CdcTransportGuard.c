#include "CdcTransportGuard.h"

static int has_device_role(enum tb710fu_cdc_usb_role role)
{
    return role == TB710FU_CDC_USB_ROLE_DEVICE ||
           role == TB710FU_CDC_USB_ROLE_OTG_PERIPHERAL;
}

enum tb710fu_cdc_preflight_result tb710fu_cdc_preflight(
    int boot_arg_value,
    const struct tb710fu_cdc_transport_snapshot *snapshot)
{
    if (boot_arg_value == 0) {
        return TB710FU_CDC_PREFLIGHT_DISABLED;
    }
    if (boot_arg_value != 1) {
        return TB710FU_CDC_PREFLIGHT_BAD_BOOT_ARG;
    }
    if (snapshot == 0) {
        return TB710FU_CDC_PREFLIGHT_BAD_SNAPSHOT;
    }
    if (!has_device_role(snapshot->usb_role)) {
        return TB710FU_CDC_PREFLIGHT_WRONG_USB_ROLE;
    }
    if (!snapshot->clocks_verified) {
        return TB710FU_CDC_PREFLIGHT_CLOCKS_UNVERIFIED;
    }
    if (!snapshot->required_clocks_enabled) {
        return TB710FU_CDC_PREFLIGHT_CLOCKS_OFF;
    }
    if (!snapshot->phy_verified) {
        return TB710FU_CDC_PREFLIGHT_PHY_UNVERIFIED;
    }
    if (!snapshot->phy_ready) {
        return TB710FU_CDC_PREFLIGHT_PHY_NOT_READY;
    }
    if (!snapshot->smmu_verified) {
        return TB710FU_CDC_PREFLIGHT_SMMU_UNVERIFIED;
    }
    if (!snapshot->smmu_identity ||
        snapshot->smmu_stream_id != TB710FU_CDC_DWC3_STREAM_ID ||
        snapshot->matching_streams != 1) {
        return TB710FU_CDC_PREFLIGHT_SMMU_ROUTE;
    }
    if (!snapshot->dma_contiguous) {
        return TB710FU_CDC_PREFLIGHT_DMA_NOT_CONTIGUOUS;
    }
    if (snapshot->dma_bytes != TB710FU_CDC_DMA_BYTES) {
        return TB710FU_CDC_PREFLIGHT_DMA_SIZE;
    }
    if ((snapshot->dma_physical & (TB710FU_CDC_DMA_ALIGNMENT - 1)) != 0) {
        return TB710FU_CDC_PREFLIGHT_DMA_ALIGNMENT;
    }
    if (snapshot->dma_address_bits < 32 || snapshot->dma_address_bits > 64) {
        return TB710FU_CDC_PREFLIGHT_DMA_ADDRESS_WIDTH;
    }
    if (snapshot->dma_address_bits < 64) {
        const uint64_t limit = 1ULL << snapshot->dma_address_bits;
        if (snapshot->dma_physical >= limit ||
            snapshot->dma_bytes > limit - snapshot->dma_physical) {
            return TB710FU_CDC_PREFLIGHT_DMA_ADDRESS_WIDTH;
        }
    }
    return TB710FU_CDC_PREFLIGHT_READY;
}
