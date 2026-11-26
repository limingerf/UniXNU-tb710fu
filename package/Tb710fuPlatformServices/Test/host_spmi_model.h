// SPDX-License-Identifier: BSD-2-Clause-Patent
// host_spmi_model.h - a register-accurate PMK8550 / PMIC-arbiter v7 model.
//
// The model owns the four arbiter windows as plain arrays, so it serves both
// transports the production code offers:
//
//   * host_spmi_io() interprets command writes itself (the callback transport
//     used by the RTC and reset tests);
//   * host_mmio_bridge wraps the real struct tb710fu_spmi_mmio over the same
//     arrays and runs the same command interpreter after the MMIO store (the
//     simulated-MMIO transport).
//
// Nothing outside Test/ consumes this file. The PMIC cells modelled are the
// ones this package owns: the RTC control/counter, SDAM2 0xbc and 0x48.
#ifndef HOST_SPMI_MODEL_H
#define HOST_SPMI_MODEL_H

#include "../Tb710fuPmk8550Rtc.h"
#include "../Tb710fuSpmiMmio.h"

#define HOST_SPMI_APIDS 8u
#define HOST_SPMI_EES 8u
#define HOST_SPMI_WORD_BYTES 4u
#define HOST_SPMI_COUNTER_VALUES 8u
#define HOST_SPMI_CELL_VALUES 8u
#define HOST_SPMI_REASON_VALUES 8u

enum host_spmi_error {
    HOST_SPMI_ERROR_NONE = 0,
    HOST_SPMI_ERROR_FAILURE,
    HOST_SPMI_ERROR_DENIED,
    HOST_SPMI_ERROR_DROPPED,
    HOST_SPMI_ERROR_SILENT /* DONE never becomes set */
};

struct host_spmi_model {
    /* The mapped windows, exactly the sizes the devicetree declares. */
    uint32_t core[TB710FU_SPMI_CORE_BYTES / 4u];
    uint32_t config[TB710FU_SPMI_CONFIG_BYTES / 4u];
    uint32_t observer[TB710FU_SPMI_OBSERVER_BYTES / 4u];
    uint32_t channel[TB710FU_SPMI_CHANNEL_BYTES / 4u];

    unsigned apid_count;

    uint32_t control;
    uint32_t counter_values[HOST_SPMI_COUNTER_VALUES];
    unsigned counter_count;
    unsigned counter_reads;
    uint32_t counter_lsb_values[HOST_SPMI_COUNTER_VALUES];
    unsigned counter_lsb_count;
    unsigned counter_lsb_reads;
    uint32_t last_counter;

    uint32_t cell_values[HOST_SPMI_CELL_VALUES];
    unsigned cell_count;
    unsigned cell_reads;

    uint32_t reason_values[HOST_SPMI_REASON_VALUES];
    unsigned reason_count;
    unsigned reason_reads;

    enum host_spmi_error error;
    int drop_writes;

    unsigned commands;
    unsigned read_commands;
    unsigned write_commands;
    unsigned unexpected_commands;
    unsigned unexpected_writes;
    unsigned pmic_reads;
    unsigned pmic_writes;
    unsigned cell_writes;
    unsigned reason_writes;
};

struct host_mmio_bridge {
    struct tb710fu_spmi_mmio mmio;
    struct tb710fu_spmi_io bridge_io;
    struct host_spmi_model *pmic;
};

void host_spmi_init(struct host_spmi_model *model);
struct tb710fu_spmi_io host_spmi_io(struct host_spmi_model *model);

void host_spmi_map(struct host_spmi_model *model, unsigned apid,
                   unsigned usid);
void host_spmi_unmap(struct host_spmi_model *model, unsigned apid);
void host_spmi_set_owner(struct host_spmi_model *model, unsigned apid,
                         unsigned owner);
void host_spmi_set_error(struct host_spmi_model *model,
                         enum host_spmi_error error);
void host_spmi_set_drop_writes(struct host_spmi_model *model, int drop);
void host_spmi_set_control(struct host_spmi_model *model, uint32_t control);
void host_spmi_set_counter(struct host_spmi_model *model, uint32_t counter);
void host_spmi_set_counter_sequence(struct host_spmi_model *model,
                                    const uint32_t *values, unsigned count);
void host_spmi_set_counter_lsb_sequence(struct host_spmi_model *model,
                                        const uint32_t *values,
                                        unsigned count);
void host_spmi_set_cell(struct host_spmi_model *model, uint32_t cell);
void host_spmi_set_cell_sequence(struct host_spmi_model *model,
                                 const uint32_t *values, unsigned count);
void host_spmi_set_reason(struct host_spmi_model *model, uint8_t reason);
void host_spmi_set_reason_sequence(struct host_spmi_model *model,
                                   const uint32_t *values, unsigned count);

/* The command interpreter. The callback transport calls these directly; the
 * MMIO bridge calls them after the store landed. offset is window-relative. */
void host_spmi_observer_command(struct host_spmi_model *model,
                                unsigned offset, uint32_t command);
void host_spmi_channel_command(struct host_spmi_model *model,
                               unsigned offset, uint32_t command);

/* Simulated MMIO over the model's arrays. */
int host_mmio_bridge_init(struct host_mmio_bridge *bridge,
                          struct host_spmi_model *model);

#endif
