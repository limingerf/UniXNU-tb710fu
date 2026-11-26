// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuSpmiArbiter.h - SPMI PMIC-arbiter version 7 core for TB710FU.
//
// Register facts, all from Linux mainline v6.12 (drivers/spmi/spmi-pmic-arb.c)
// and the board devicetree (arch/arm64/boot/dts/qcom/sm8650.dtsi):
//
//   PMIC_ARB_VERSION        0x0000, version 7 starts at 0x70000000   L23-27
//   PMIC_ARB_FEATURES       0x0004, APID count is GENMASK(10, 0)     L30-31
//   PMIC_ARB_FEATURES1      0x0008, second bus (not used here)       L33
//   PMIC_ARB_CMD            0x00, CONFIG 0x04, STATUS 0x08,
//   PMIC_ARB_WDATA0         0x10, RDATA0 0x18                        L36-41
//   status: DONE bit 0, FAILURE bit 1, DENIED bit 2, DROPPED bit 3   L63-66
//   opcodes: EXT_WRITEL 0, EXT_READL 1                               L74-75
//   APID map v7: core + 0x2000 + 4*apid                             L1555-1557
//   APID owner v7: cnfg + 4*(apid - base_apid), base_apid 0 for bus 0
//                                                                   L1572-1575
//   observer channel: 0x8000*ee + 0x20*apid                         L1411-1413
//   read/write channel: 0x1000*apid                                 L1414-1416
//   command word v2: (opc << 27) | (addr << 4) | (bc & 7), no SID    L1432-1435
//   the bus is addressed by PPID = sid << 8 | addr >> 8              L1404-1410
//   a write is refused unless this EE owns the APID                  L1415-1420
//
// sm8650.dtsi declares spmi@c400000 with reg-names core (0x0c400000,
// 0x3000), chnls (0x0c500000, 0x400000), obsrvr (0x0c440000, 0x80000),
// intr (0x0c4c0000, 0x20000), cnfg (0x0c42d000, 0x4000), qcom,ee = <0>,
// qcom,channel = <0>, qcom,bus-id = <0>; the sizes are exactly the full
// 1024-APID tables (4*1024 for the map, 0x20*1024 for the observer,
// 0x1000*1024 for the channel window).
//
// The core never maps memory: struct tb710fu_spmi_io forwards aligned 32-bit
// window accesses, so the same code runs on a host model and behind
// ml_io_map. Tb710fuSpmiMmio.c is the real MMIO transport.
//
// WRITE POLICY. Reads are unrestricted within the APID range: an observer
// channel exists for every execution environment, and reading a PMIC register
// cannot change the device. Writes are deny-by-default and only two cells of
// the PMK8550's second SDAM are writable:
//
//   SID 0, PPID 0x071, byte 0x48, 1 byte  the reboot-reason cell (bits 7:1)
//   SID 0, PPID 0x071, byte 0xbc, 4 bytes the RTC offset cell (if policy opts in)
//
// Everything else is read-only, in particular the whole of SID 7 (PM8550B,
// the board's charger/analog slave: pm8550b.dtsi@v6.12 declares its
// temperature-alarm at 0xa00, its GPIOs at 0x8800 and the eUSB2 repeater at
// 0xfd00, and nothing in that slave may be written by this port), the RTC and
// alarm blocks of SID 0 (PPID 0x061/0x062), the PON block (PPID 0x013), and
// the SDAM2 control bytes. See tb710fu_spmi_write_target() for the single
// table both the direct and the APID-level write path consult.
#ifndef TB710FU_SPMI_ARBITER_H
#define TB710FU_SPMI_ARBITER_H

#include <stdint.h>

/* PMIC identities used by this port. USID = SID << 8 | PPID. */
#define TB710FU_PMK8550_SID 0u
#define TB710FU_PMK8550_RTC_PPID 0x061u
#define TB710FU_PMK8550_RTC_ALARM_PPID 0x062u
#define TB710FU_PMK8550_PON_PPID 0x013u
#define TB710FU_PMK8550_SDAM2_PPID 0x071u
#define TB710FU_PMK8550_GPIO_PPID 0x0b8u
#define TB710FU_PM8550B_SID 7u

#define TB710FU_SPMI_USID(sid, ppid) ((((unsigned)(sid)) << 8) | ((unsigned)(ppid)))
#define TB710FU_PMK8550_RTC_USID \
    TB710FU_SPMI_USID(TB710FU_PMK8550_SID, TB710FU_PMK8550_RTC_PPID)
#define TB710FU_PMK8550_SDAM2_USID \
    TB710FU_SPMI_USID(TB710FU_PMK8550_SID, TB710FU_PMK8550_SDAM2_PPID)

/* SDAM2 cells this port knows. 0x48 is the board's reboot-reason cell
 * (pmk8550.dtsi: reboot-reason@48, bits <1 7>). 0xbc is the four-byte RTC
 * offset cell of the stage-1 note; mainline rtc-pm8xxx.c takes such an offset
 * from an nvmem cell instead, and the TB710FU devicetree does not declare
 * one, so 0xbc stays unverified on this board and is read-only unless policy
 * explicitly opts in. */
#define TB710FU_PMK8550_SDAM2_REBOOT_REASON 0x48u
#define TB710FU_PMK8550_SDAM2_RTC_OFFSET 0xbcu

/* PMIC-arbiter v7 register layout, mainline spmi-pmic-arb.c. */
#define TB710FU_SPMI_VERSION 0x0000u
#define TB710FU_SPMI_VERSION_V7_MIN 0x70000000u
#define TB710FU_SPMI_VERSION_V8_MIN 0x80000000u
#define TB710FU_SPMI_FEATURES 0x0004u
#define TB710FU_SPMI_FEATURES_PERIPH_MASK 0x7ffu
#define TB710FU_SPMI_FEATURES1 0x0008u
#define TB710FU_SPMI_MAX_APIDS 1024u
#define TB710FU_SPMI_MAX_EE 7u

#define TB710FU_SPMI_APID_MAP_BASE 0x2000u
#define TB710FU_SPMI_APID_OWNER_BASE 0x0000u
#define TB710FU_SPMI_OBSERVER_EE_STRIDE 0x8000u
#define TB710FU_SPMI_OBSERVER_APID_STRIDE 0x0020u
#define TB710FU_SPMI_CHANNEL_APID_STRIDE 0x1000u

#define TB710FU_SPMI_CMD 0x00u
#define TB710FU_SPMI_CONFIG 0x04u
#define TB710FU_SPMI_STATUS 0x08u
#define TB710FU_SPMI_WDATA0 0x10u
#define TB710FU_SPMI_RDATA0 0x18u

#define TB710FU_SPMI_STATUS_DONE (1u << 0)
#define TB710FU_SPMI_STATUS_FAILURE (1u << 1)
#define TB710FU_SPMI_STATUS_DENIED (1u << 2)
#define TB710FU_SPMI_STATUS_DROPPED (1u << 3)
#define TB710FU_SPMI_STATUS_ERRORS \
    (TB710FU_SPMI_STATUS_FAILURE | TB710FU_SPMI_STATUS_DENIED | \
     TB710FU_SPMI_STATUS_DROPPED)

#define TB710FU_SPMI_OP_EXT_WRITEL 0u
#define TB710FU_SPMI_OP_EXT_READL 1u

/* One data register holds four bytes; mainline supports eight with RDATA1 and
 * WDATA1, which no cell this port owns needs. */
#define TB710FU_SPMI_MAX_BYTES 4u
#define TB710FU_SPMI_MAX_APID 1023u
#define TB710FU_SPMI_POLL_LIMIT 100000u

/* Window sizes the devicetree provides and the tables require. */
#define TB710FU_SPMI_CORE_BYTES 0x3000u
#define TB710FU_SPMI_CONFIG_BYTES 0x4000u
#define TB710FU_SPMI_OBSERVER_BYTES 0x80000u
#define TB710FU_SPMI_CHANNEL_BYTES 0x400000u
#define TB710FU_SPMI_INT_BYTES 0x20000u

/* Physical windows of spmi@c400000 (sm8650.dtsi). */
#define TB710FU_SPMI_CORE_PHYS 0x0c400000u
#define TB710FU_SPMI_CHANNEL_PHYS 0x0c500000u
#define TB710FU_SPMI_OBSERVER_PHYS 0x0c440000u
#define TB710FU_SPMI_INTERRUPT_PHYS 0x0c4c0000u
#define TB710FU_SPMI_CONFIG_PHYS 0x0c42d000u

/* The board's execution environment and bus. */
#define TB710FU_SPMI_EE 0u
#define TB710FU_SPMI_BUS 0u

enum tb710fu_spmi_window {
    TB710FU_SPMI_WINDOW_CORE,
    TB710FU_SPMI_WINDOW_CONFIG,
    TB710FU_SPMI_WINDOW_OBSERVER,
    TB710FU_SPMI_WINDOW_CHANNEL
};

/* The kernel wrapper maps each window once and forwards aligned 32-bit
 * accesses; byte_offset is relative to the window base. */
struct tb710fu_spmi_io {
    void *context;
    uint32_t (*read32)(void *context, enum tb710fu_spmi_window window,
                       unsigned byte_offset);
    void (*write32)(void *context, enum tb710fu_spmi_window window,
                    unsigned byte_offset, uint32_t value);
};

struct tb710fu_spmi_bus {
    uint32_t version;
    unsigned base_apid;
    unsigned apid_count;
};

struct tb710fu_spmi_peripheral {
    unsigned apid;
    unsigned owner; /* execution environment from cnfg+4*apid, bits 2:0 */
};

enum tb710fu_spmi_result {
    TB710FU_SPMI_OK = 0,
    TB710FU_SPMI_FAILED,   /* the arbiter reported a failed transfer */
    TB710FU_SPMI_DENIED,   /* the arbiter reported a denied transfer */
    TB710FU_SPMI_DROPPED,  /* the arbiter reported a dropped transfer */
    TB710FU_SPMI_TIMEOUT,  /* the arbiter never reported DONE */
    TB710FU_SPMI_NO_DEVICE,/* no APID maps this USID, or the layout is foreign */
    TB710FU_SPMI_BAD_APID, /* the APID is outside the bus's APID count */
    TB710FU_SPMI_NOT_OWNER,/* this EE does not own the APID for a write */
    TB710FU_SPMI_READ_ONLY,/* the write target is not on the whitelist */
    TB710FU_SPMI_ARGUMENT  /* a null or out-of-range argument */
};

/* The two writable cells, and why anything else is refused. */
enum tb710fu_spmi_write_target {
    TB710FU_SPMI_WRITE_TARGET_NONE = 0,
    TB710FU_SPMI_WRITE_TARGET_SDAM_REBOOT_REASON,
    TB710FU_SPMI_WRITE_TARGET_SDAM_RTC_OFFSET
};

enum tb710fu_spmi_write_denial {
    TB710FU_SPMI_DENY_NONE = 0,
    TB710FU_SPMI_DENY_CHARGER_SLAVE,  /* SID 7, the PM8550B analog slave */
    TB710FU_SPMI_DENY_FOREIGN_SLAVE,  /* any other SID but 0 */
    TB710FU_SPMI_DENY_RTC_BLOCK,      /* PMK8550 RTC/alarm, PPID 0x061/0x062 */
    TB710FU_SPMI_DENY_UNLISTED_CELL   /* SID 0, but not a whitelisted cell */
};

/* The command word for 1..4 bytes. The encoding is the arbiter's (v2 format,
 * no SID field): (op << 27) | (byte_addr << 4) | (bytes - 1). */
uint32_t tb710fu_spmi_command(unsigned operation, unsigned byte_addr,
                              unsigned bytes);

/* Version and APID count of the primary bus. Fails closed on a layout this
 * core does not know rather than guessing a different table base. */
int tb710fu_spmi_probe(const struct tb710fu_spmi_io *io,
                       struct tb710fu_spmi_bus *bus);

/* Find the APID for a USID. As mainline pmic_arb_read_apid_map_v5() does it,
 * the first mapping wins unless a later mapping belongs to this execution
 * environment; duplicate PPIDs are legal in v7. */
int tb710fu_spmi_find(const struct tb710fu_spmi_io *io, unsigned ee,
                      unsigned usid, struct tb710fu_spmi_peripheral *out);

/* Recover the USID an APID is mapped to, by scanning the same table. Returns
 * TB710FU_SPMI_NO_DEVICE when the APID is empty. Used by the write gate so an
 * APID-level write cannot bypass the USID whitelist. */
int tb710fu_spmi_lookup_apid(const struct tb710fu_spmi_io *io, unsigned apid,
                             unsigned *usid);

/* The execution environment that owns an APID for writes, from cnfg+4*apid
 * (mainline pmic_arb_apid_owner_v7). */
int tb710fu_spmi_ownership(const struct tb710fu_spmi_io *io, unsigned apid,
                           unsigned *owner);

/* One transfer of 1..4 bytes through the observer (read) or read/write
 * channel. bytes is the byte count, not the command's count-minus-one field.
 * Reads are not gated by ownership; writes are. polls, when non-null,
 * receives the number of status words read. */
int tb710fu_spmi_read(const struct tb710fu_spmi_io *io, unsigned ee,
                      unsigned apid, uint8_t byte_addr, unsigned bytes,
                      uint32_t *value, unsigned *polls);

int tb710fu_spmi_write_apid(const struct tb710fu_spmi_io *io, unsigned ee,
                            unsigned apid, uint8_t byte_addr, unsigned bytes,
                            uint32_t value, unsigned *polls);

/* Write by USID, resolving the APID first. Both paths end in the same gate. */
int tb710fu_spmi_write(const struct tb710fu_spmi_io *io, unsigned ee,
                       unsigned usid, uint8_t byte_addr, unsigned bytes,
                       uint32_t value, unsigned *polls);

/* Pure policy: which cell a (usid, byte_addr, bytes) write names, and why a
 * refusal happens. No I/O. */
enum tb710fu_spmi_write_target tb710fu_spmi_write_target(unsigned usid,
                                                         uint8_t byte_addr,
                                                         unsigned bytes);
enum tb710fu_spmi_write_denial tb710fu_spmi_write_denial(unsigned usid,
                                                         uint8_t byte_addr,
                                                         unsigned bytes);

/* Names for logs and tests; never NULL. */
const char *tb710fu_spmi_result_name(int result);
const char *tb710fu_spmi_denial_name(int denial);

#endif
