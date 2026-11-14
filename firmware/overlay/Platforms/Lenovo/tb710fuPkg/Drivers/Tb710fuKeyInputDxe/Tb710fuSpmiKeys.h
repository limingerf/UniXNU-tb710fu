/** @file
  Lenovo TB710FU PMIC keys through the SPMI arbiter (host-testable).

  Derived from UniXNU/uefi-sheng@66421cb7bb5694843aec8de1ca3d1de4c20cca38,
  Platforms/Xiaomi/shengPkg/Drivers/ShengKeyInputDxe/ShengSpmiKeys.h.
  TB710FU wiring and MMIO windows: SpendyYT/tb710fu-linux at
  4d04410798b538fbe4b50664e3a66d9500a2ed31, sm8650-lenovo-tb710fu.dts,
  sm8650.dtsi, pmk8550.dtsi and pm8550.dtsi. See docs/UEFI_INPUT.md.

  Facts, all from the board devicetree and mainline Linux (drivers/input/misc/pm8941-pwrkey.c, drivers/pinctrl/qcom/pinctrl-spmi-gpio.c):
    power key   PMIC SID 0, PON peripheral PID 0x13 (reg 0x1300), PON_RT_STS at +0x10, PON_GEN3_KPDPWR_N_SET = bit 7, set = pressed
    volume down PMIC SID 0, same register, PON_GEN3_RESIN_N_SET = bit 6, set = pressed (the board's linux,code 114)
    volume up   PM8550 (SID 1) GPIO6, peripheral PID 0x88 + 5 = 0x8d, RT_STS at +0x10 bit 0; gpio-keys flags 1 = active low
  The board DTS declares core 0xc400000, observer 0xc440000 and EE 0. Only
  the inherited version-7 layout is accepted at runtime; another version fails
  closed. APIDs are discovered by PPID (SID << 8 | PID), never hardcoded.
  Observer commands read PMIC status; no PMIC configuration register is written.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef TB710FU_SPMI_KEYS_H_
#define TB710FU_SPMI_KEYS_H_

#include <stdint.h>

#define SK_VERSION_V7_MIN 0x70000000u
#define SK_VERSION_V8_MIN 0x80000000u

enum {
  SK_FEATURES = 0x04, SK_FEATURES_APIDS_MASK = 0x7ffu,
  SK_APID_MAP = 0x2000,
  SK_CNFG_OWNER = 0x00, SK_MAX_APIDS = 1024, SK_MAX_EE = 7,
  SK_OBS_EE_STRIDE = 0x8000, SK_OBS_APID_STRIDE = 0x20,
  SK_CMD = 0x00, SK_STATUS = 0x08, SK_RDATA0 = 0x18,
  SK_OP_EXT_READL = 1,
  SK_STATUS_DONE = 1u << 0, SK_STATUS_ERRORS = 0xeu,
  SK_POLLS = 100000,
  SK_PPID_PON = 0x013, SK_PPID_GPIO6 = 0x18d, SK_REG_RT_STS = 0x10,
  SK_PON_RESIN = 1u << 6, SK_PON_KPDPWR = 1u << 7,
  SK_KEY_POWER = 1u << 0, SK_KEY_VOLDOWN = 1u << 1, SK_KEY_VOLUP = 1u << 2
};

struct sk_arbiter {
  volatile uint32_t *core;   // 0x3000 bytes
  volatile uint32_t *obs;    // observer channels of this EE and above (EE 0 starts at the base)
  volatile uint32_t *cnfg;   // primary bus: owner table at cnfg + 4*APID
  uint8_t ee;
};

// Production supplies IoLib accessors and MemoryFence; the host supplies a
// deterministic observer model. Never silently fall back to plain RAM accesses.
#if !defined(TB710FU_SPMI_READ32) || !defined(TB710FU_SPMI_WRITE32) || !defined(TB710FU_SPMI_BARRIER)
#error Define TB710FU_SPMI_READ32, TB710FU_SPMI_WRITE32 and TB710FU_SPMI_BARRIER
#endif

// Primary-bus v7 layout only. Store the bounded APID count on success.
static inline int sk_layout (const struct sk_arbiter *a, uint32_t *apids)
{
  uint32_t version;
  if (!a || !a->core || !a->obs || !a->cnfg || !apids || a->ee > SK_MAX_EE) return 4;
  version = TB710FU_SPMI_READ32 (a->core);
  if (version < SK_VERSION_V7_MIN || version >= SK_VERSION_V8_MIN) return 1;
  *apids = TB710FU_SPMI_READ32 (a->core + SK_FEATURES / 4) & SK_FEATURES_APIDS_MASK;
  if (!*apids || *apids > SK_MAX_APIDS) return 2;
  return 0;
}

// 0 and *apid on success. 1 unknown layout, 2 bad APID count, 3 PPID absent,
// 4 invalid arguments. Match Linux's first mapping, replaced by one this EE
// owns: duplicate PPID mappings are valid in arbiter v7. Ownership registers
// are cnfg + 4*APID on primary bus 0, not the older v2 +0x700 layout.
static inline int sk_find_apid (const struct sk_arbiter *a, uint32_t ppid, int *apid)
{
  uint32_t apids = 0;
  int rc;
  if (!apid) return 4;
  *apid = -1;
  if (ppid > 0xfff) return 4;
  rc = sk_layout (a, &apids);
  if (rc) return rc;
  for (uint32_t i = 0; i < apids; ++i) {
    uint32_t map = TB710FU_SPMI_READ32 (a->core + (SK_APID_MAP + 4 * i) / 4);
    if (map && ((map >> 8) & 0xfff) == ppid) {
      uint32_t owner = TB710FU_SPMI_READ32 (a->cnfg + (SK_CNFG_OWNER + 4 * i) / 4) & 7u;
      if (*apid < 0 || owner == a->ee) *apid = (int) i;
    }
  }
  return *apid < 0 ? 3 : 0;
}

// One register byte through the observer channel. 0 ok, 1 timeout, 2 hardware
// error, 4 invalid arguments/layout. Preserve *value on all failures. The
// command re-arms STATUS in hardware, including repeated reads of one register.
static inline int sk_read8 (const struct sk_arbiter *a, int apid, uint8_t reg, uint8_t *value)
{
  uint32_t apids = 0;
  if (!value || sk_layout (a, &apids) || apid < 0 || (uint32_t) apid >= apids) return 4;
  volatile uint32_t *ch = a->obs + (SK_OBS_EE_STRIDE * a->ee + SK_OBS_APID_STRIDE * (uint32_t) apid) / 4;
  TB710FU_SPMI_WRITE32 (ch + SK_CMD / 4, ((uint32_t) SK_OP_EXT_READL << 27) | ((uint32_t) reg << 4));
  TB710FU_SPMI_BARRIER ();
  for (unsigned n = 0; n < SK_POLLS; ++n) {
    uint32_t s = TB710FU_SPMI_READ32 (ch + SK_STATUS / 4);
    if (s & SK_STATUS_DONE) {
      if (s & SK_STATUS_ERRORS) return 2;
      TB710FU_SPMI_BARRIER ();
      *value = (uint8_t) TB710FU_SPMI_READ32 (ch + SK_RDATA0 / 4);
      return 0;
    }
  }
  return 1;
}

struct sk_keys { int pon, gpio; };   // APIDs, -1 = not available

static inline int sk_open (const struct sk_arbiter *a, struct sk_keys *k)
{
  if (!k) return 4;
  k->pon = k->gpio = -1;
  int rc = sk_find_apid (a, SK_PPID_PON, &k->pon);
  if (rc) return rc;
  if (sk_find_apid (a, SK_PPID_GPIO6, &k->gpio)) k->gpio = -1;   // the power and volume-down keys still work without it
  return 0;
}

// Pressed keys as SK_KEY_* bits. 0 ok; non-zero when a read failed (nothing is reported pressed then).
static inline int sk_sample (const struct sk_arbiter *a, const struct sk_keys *k, uint8_t *pressed)
{
  uint8_t v = 0;
  if (!pressed || !k) return 4;
  *pressed = 0;
  int rc = sk_read8 (a, k->pon, SK_REG_RT_STS, &v);
  if (rc) return rc;
  if (v & SK_PON_KPDPWR) *pressed |= SK_KEY_POWER;
  if (v & SK_PON_RESIN) *pressed |= SK_KEY_VOLDOWN;
  if (k->gpio >= 0) {
    rc = sk_read8 (a, k->gpio, SK_REG_RT_STS, &v);
    if (rc) { *pressed = 0; return rc; }
    if (!(v & 1)) *pressed |= SK_KEY_VOLUP;       // active low
  }
  return 0;
}

// Debounce and auto-repeat. A key counts as pressed after two identical samples. Volume keys repeat while held: first after
// `delay` polls, then every `period` polls. step() returns the keys to report now (new presses plus repeats).
struct sk_filter { uint8_t last, stable; uint16_t held[3], repeat[3]; };

static inline uint8_t sk_step (struct sk_filter *f, uint8_t sample, uint16_t delay, uint16_t period)
{
  uint8_t out = 0;
  if (sample == f->last) {
    uint8_t fresh = (uint8_t) (sample & ~f->stable);
    f->stable = sample;
    out = fresh;
    for (unsigned i = 0; i < 3; ++i) {
      uint8_t bit = (uint8_t) (1u << i);
      if (!(sample & bit) || (fresh & bit)) { f->held[i] = f->repeat[i] = 0; continue; }
      if (bit == SK_KEY_POWER || !delay || !period) continue;
      // Saturate the initial delay; keep a separate bounded repeat counter so
      // a long hold never wraps and never divides by a zero period.
      if (f->held[i] < delay) {
        if (++f->held[i] == delay) { out |= bit; f->repeat[i] = 0; }
      } else {
        f->held[i] = delay;
        if (f->repeat[i] >= period - 1u) { out |= bit; f->repeat[i] = 0; }
        else ++f->repeat[i];
      }
    }
  }
  f->last = sample;
  return out;
}

#endif
