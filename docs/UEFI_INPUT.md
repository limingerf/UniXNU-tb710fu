# TB710FU UEFI input path

The clean firmware now carries `Tb710fuKeyInputDxe`, a source-only DXE that
publishes an `EFI_SIMPLE_TEXT_INPUT_PROTOCOL` for the tablet's physical keys.
It is derived from the pinned open implementation
`UniXNU/uefi-sheng@66421cb7bb5694843aec8de1ca3d1de4c20cca38` and is kept in the
board overlay so the fetched source checkout remains immutable.

The wiring is taken from the same-device Linux DTS at
`SpendyYT/tb710fu-linux@4d04410798b538fbe4b50664e3a66d9500a2ed31`:

- PMK8550 SID 0 PON status, peripheral `0x13` (`0x1300`): bit 7 is power and
  bit 6 is the resin/volume-down key.
- PM8550 SID 1 GPIO 6, peripheral `0x88 + 6 - 1 = 0x8d`: bit 0 is active-low
  volume-up.
- The SM8650 SPMI arbiter declares core `0x0c400000`, observer `0x0c440000`
  and execution environment 0. APIDs are discovered from the arbiter map; the
  driver accepts only version-7 layouts and performs observer reads only.

The driver debounces samples, repeats held volume keys, maps volume-up to
`SCAN_UP`, volume-down to `SCAN_DOWN`, and power to `Enter`, then appends its
vendor device path to `ConIn` before BDS connects the console. It stops polling
at `ExitBootServices` and never writes PMIC configuration registers. Missing or incompatible
SPMI state fails closed and leaves the normal firmware input path intact.

`Test/tb710fu_keys_host.c` exercises APID discovery, key bit decoding, read
errors, debounce and repeat behaviour against a software arbiter model. The
UEFI cloud workflow compiles and runs this test before the firmware build.

The host test and cloud build prove source integration and the key translation
contract. Only a device run can prove that this particular ABL handoff leaves
the arbiter mapped and that physical keys generate events on the rebuilt FD.

## Source review and verification

Exact source commits, paths and SHA-256 digests are recorded in
[the source manifest](evidence/uefi-input-sources.json). The board DTS supplies
wiring and MMIO windows, the PMIC DTS files supply SID/peripheral bases, and
pinned Linux spmi-pmic-arb.c supplies APID ownership, observer offsets, command
format and status bits. pm8941-pwrkey.c and pinctrl-spmi-gpio.c supply key bits.
The ownership window is 0x0c42d000; duplicate PPID mappings prefer this EE.

The adapted upstream implementation now uses IoLib MMIO and memory barriers.
Observer transactions are bounded to 100000 status reads. It writes observer
command registers to request reads but never writes PMIC configuration.
Fifty consecutive failed samples stop polling. A failed initial read installs
no console, and a power key held through entry must be released before it can
confirm. ConIn uses validated DevicePathLib multi-instance operations. Entry
failure closes created events and restores the previous ConIn value if needed.
Successful installation logs: Tb710fuKeyInput: ConIn installed.

The host model is deterministic and covers duplicate owner mapping, boundaries,
repeated reads of fresh data, timeout/error bits, debounce, simultaneous keys and
70000-poll holds. It does not test UEFI service mocks or physical hardware.
The cloud workflow runs this test with AddressSanitizer/UndefinedBehaviorSanitizer
before calling the unchanged official firmware build entry; the real compressed
FV is checked for all ten required module GUIDs including this driver.

Local verification on 2026-11-15: GCC and Clang strict C11 builds passed;
GCC ASan/UBSan passed. Board preflight still has 40 regions with semantic SHA-256
3033991185c76994ca5d4cfe46aaad1bca255bb450c9a7e031a5b5057ea8acbe.
Cloud run/FD hashes are recorded separately. This is UEFI SimpleTextIn only;
USB host, touch and XNU HID support remain separate work.
