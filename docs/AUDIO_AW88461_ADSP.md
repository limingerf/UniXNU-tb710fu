# Audio: Awinic AW88461 speakers and the ADSP's playback and capture graphs

Status: the audio subsystem is **host-verified and compile-ready**, not device-verified. Nothing in this package has
run on the tablet yet; `audio=0` is the default and touches no hardware. What each stage does, what a green stage
does *not* prove, and the two facts that can only be settled on hardware (the AW88461 wire byte order and the GPR
domain numbers) are below.

| Piece | State |
|---|---|
| SM8650 GENI I2C master on `i2c_hub_4` | implemented (polled, aligned 32-bit MMIO), host framing tests pass |
| Awinic AW88461 control: probe, ACF replay, mute/volume | implemented, host tests replay the real 3,504-byte ACF |
| GPR codec over the existing `adsp_apps` GLINK client | implemented, host tests cover header, routing, refusals |
| AudioReach APM graph (speaker TDM playback + DMIC capture) | implemented, host tests cover every builder and the 19-step sequence |
| macOS service (`IOService` + `IOUserClient`) | implemented, **no IOAudioFamily link symbol** (static audit + link assertion) |
| CoreAudio HAL plugin | **not in this repository**; the ABI it would speak is `Tb710fuAudioAbi.h` |
| Sound on the device | stage `audio=4` on hardware, after the ADSP and the ACF file are in place |

## Where every number comes from

Only these files were used for addresses, buses, GPIOs, IRQs and compatibles: the board DTS
`sm8650-lenovo-tb710fu.dts` and the SoC `sm8650.dtsi` from `d:\unixnu-tb710fu-clean\.local\input-sources`. No
SM8550/sheng address is carried over.

| Fact | Value | Source node |
|---|---|---|
| I2C controller for the four amps | `i2c_hub_4: i2c@990000`, reg `<0 0x00990000 0 0x4000>`, IRQ `GIC_SPI 468`, compatible `qcom,geni-i2c-master-hub`, clocks `GCC_QUPV3_I2C_S4_CLK` + `GCC_QUPV3_I2C_CORE_CLK` | `sm8650.dtsi` `i2c_hub_4` |
| Pin control (reset lines are read here) | `tlmm: pinctrl@f100000`, reg `<0 0x0f100000 0 0x300000>` | `sm8650.dtsi` `tlmm` |
| GPR transport | `compatible = "qcom,gpr"`, `qcom,glink-channels = "adsp_apps"`, `qcom,intents = <512 20>`; `q6apm: service@1` (`GPR_APM_MODULE_IID`), `q6prm: service@2` | `sm8650.dtsi` `qcom,gpr` |
| The four amplifiers | `aw88461_1@34`..`aw88461_4@37` under `&i2c_hub_4`, `compatible = "awinic,aw88461"`, `awinic,audio-channel = <0..3>`, `awinic,sync-flag`, `sound-name-prefix = "AMP0".."AMP3"`, `firmware-name = "awinic/aw88461_tb710fu.bin"` | the board DTS `&i2c_hub_4` |
| Amp reset lines | `awinic_1` `&tlmm 9`, `awinic_2` `&tlmm 8`, `awinic_3` `&tlmm 2`, `awinic_4` `&tlmm 60`, all `GPIO_ACTIVE_HIGH` | same nodes |
| Amp 1 radio coexistence lines | `wifi-mode-gpio = <&tlmm 99 GPIO_ACTIVE_LOW>`, `wifi-en-gpio = <&tlmm 98 GPIO_ACTIVE_LOW>` (not driven by this driver) | `aw88461_1@34` |
| Sound card | `sound`, `compatible = "lenovo,tb710fu-sndcard", "qcom,sm8650-sndcard", "qcom,sm8450-sndcard"`, `model = "Lenovo-TB710FU"`, `pinctrl-0 = <&tdm1_default>` | the board DTS `sound` |
| Playback link | `link-name = "Secondary TDM0 Playback"`, cpu `sound-dai = <&q6apmbedai SECONDARY_TDM_RX_0>`, `dai-tdm-slot-num = <4>`, `dai-tdm-slot-width = <32>`; codec `<&awinic_1 &awinic_2 &awinic_3 &awinic_4>` with `dai-tdm-slot-rx-mask = <1 1 1 1>`; platform `<&q6apm>` | `sound/secondary-tdm-dai-link` |
| Capture link | `link-name = "WCD Capture"`, cpu `sound-dai = <&q6apmbedai TX_CODEC_DMA_TX_3>`, codec `<&wcd939x 1>, <&swr2 0>, <&lpass_txmacro 0>` | `sound/wcd-capture-dai-link` |
| Microphone routing | `wcd_tx: codec@0,3` on `&swr2`: `qcom,tx-port-mapping = <2 2 3 4>`, with the DT comment "WCD9395 TX Port 3 (DMIC0,1,2,3 & MBHC) <=> SWR2 Port 3 (TX SWR_INPUT 4,5,6,7)" | the board DTS `&swr2` |
| Codec | `wcd939x: audio-codec`, `compatible = "qcom,wcd9395-codec", "qcom,wcd9390-codec"`, `reset-gpios = <&tlmm 107 GPIO_ACTIVE_LOW>`, supplies `vreg_l15b_1p8` / `vreg_bob1` | the board DTS `wcd939x` |
| Audio pin muxing for the TDM bus | `pinctrl-0 = <&tdm1_default>` on `sound` (the pins themselves are muxed by the boot chain, not by this driver) | `sound` |

Two consequences worth stating plainly:

- The board DTS has **no WSA (smart-amp speaker) node and no LPASS VA-macro node**. Speaker audio leaves the SoC
  on the LPASS I2S/TDM bus to the four AW88461s; microphone audio enters through the **WCD9395 codec's DMIC
  inputs**, comes out on SWR2 port 3, and reaches the DSP as `TX_CODEC_DMA_TX_3`. This driver therefore does not
  program a VA macro, and it does not program the codec: the WCD9395's own driver (reset line 107, supplies
  `vreg_l15b_1p8`/`vreg_bob1`) is a separate subsystem.
- `GPR_DOMAIN_ID_ADSP` is a macro from `dt-bindings/soc/qcom,gpr.h`, and that header is not in this input snapshot
  (the snapshot is the four DTS files plus firmware). The domain numbers therefore default to the values the
  hardware-verified sheng stack uses (`APPS=3`, `ADSP=2`) and can be corrected without a rebuild with
  `audio-gpr-src=`/`audio-gpr-dst=`.

## Speaker topology

One LPASS TDM bus, four slots, one smart amplifier per slot:

```
q6apm (APM) ── SECONDARY_TDM_RX_0 ── I2S/TDM, 4 slots × 32 bit ──┬── 0x34 awinic_1 ch0 AMP0
                                                                ├── 0x35 awinic_2 ch1 AMP1
                                                                ├── 0x36 awinic_3 ch2 AMP2
                                                                └── 0x37 awinic_4 ch3 AMP3
```

- `dai-tdm-slot-num = <4>`, `dai-tdm-slot-width = <32>` on both sides, `dai-tdm-slot-rx-mask = <1 1 1 1>` on the
  codec side: each amplifier takes exactly one 32-bit slot, so the playback stream is 4 channels × 32-bit.
- The driver's defaults mirror that: `audio-speaker-channels=4`, `audio-speaker-bits=32`, and the APM's I2S
  interface selectors default to (interface 1 = secondary, SD line 0, WS source 1 = internal), derived from the
  link name "Secondary TDM0".
- All four amplifiers share one I2C controller, one ACF file and one `awinic,sync-flag` group. There is no per-amp
  TDM slot property in the DTS: slot order follows `awinic,audio-channel`.

## Capture topology

```
DMIC0..3 ── wcd939x (WCD9395 TX port 3) ── SWR2 port 3 (TX SWR_INPUT 4..7) ── lpass_txmacro ── TX_CODEC_DMA_TX_3 ── q6apm
```

- The capture endpoint is the codec-DMA transmitter `TX_CODEC_DMA_TX_3`; in the q6 numbering the sheng stack uses
  that is codec-DMA index **4**, which is this driver's default (`audio-capture-channels=2`, `channel_mask 0x3`
  covering DMIC0/1; raise `audio-capture-channels` for more).
- Which DMIC pair is live, and whether the codec needs its own clock/mic-bias enable, is the codec driver's
  business. Our graph opens the DSP side of the path; if capture is silent while playback works, the next thing to
  check is the WCD9395 driver, not this package.

## The ACF profile file

`awinic/aw88461_tb710fu.bin` in the stock firmware (MD5SUMS entry `7ba6173fedee5eaea1a1e41a2164bb88`, byte-identical
to `aw882xx_acf.bin`; 3,504 bytes). The layout below was reverse-documented from the shipped file itself and every
offset is asserted by `tests/tb710fu_audio_test.c` against the real file:

| Offset | Size | Meaning in the stock file |
|---|---|---|
| `0x00` | 4 | magic `0x0A15F908` |
| `0x04` | 4 | project tag `"P805"` |
| `0x1C` | 16 | vendor `"awinic"` |
| `0x2C` | 4 | format `0x01000000` |
| `0x30` | 4 | image id `0x000002B2` |
| `0x34` | 4 | record table bytes `0x200` |
| `0x38` | 4 | record count `8` |
| `0x3C` | 4 | record table offset `0x50` |
| `0x40` | 4 | profile container count `1` |
| `0x50` | 8 × 64 | record table |
| `0x250` | 8 × 364 | eight profile blocks, back to back (end of file `0xDB0` = 3,504) |

Record (64 bytes): `+0x00` u32 type (`2` = register profile, the only type used), `+0x04` char model[16]
`"aw88461"`, `+0x14` u16 channel (0..3), `+0x16`/`+0x18` u16 `0xFFFF`, `+0x1A` u16 rate tag (`0` = the 8..48 kHz
tuning, `10` = the 96 kHz tuning), `+0x1C` u16 constant `4`, `+0x20` u32 stride `0x16C`, `+0x24` u32 absolute
profile offset, `+0x28` u32 constant `0x34`.

Profile (364 bytes): `+0x00` u32 identity word, `+0x18` char model[8] `"AW88461"`, `+0x3C` u32 pair count `75`,
`+0x40` 75 × (u16 register, u16 value) little endian; `0x40 + 4 × 75 = 0x16C` is exactly the stride. The eight
identities are channel 0 `0x4A38`/`0x47B5`, channel 1 `0x4A4A`/`0x47C7`, channel 2 `0x4A68`/`0x47E5`, channel 3
`0x4A7A`/`0x47F7` (rate tag `0` then `10`). The replay starts `0x0003 <- 0xFFFF` and continues through
`0x0004..0x0056`.

What is **verified**: the container layout, the pair count and the exact byte values of all eight profiles (host
test parses the shipped file). What is **not**: the three semantics below, which need a data sheet or a hardware
probe:

1. **Wire byte order.** The file stores 16-bit values little endian. The driver puts *most-significant byte first*
   on I2C for both the register address and the value (the AW88xx family convention). If `audio=1` reads garbage
   from all four parts, this is the first thing to flip.
2. **Amplifier mute word.** `0x0003` bit 15 is treated as HIZ/standby: `0x7FFF` = muted, `0xFFFF` = active. The
   ACF itself writes `0x0003 <- 0xFFFF` first, so the bit's meaning is an inference from that and from the AW88xx
   family shape.
3. **Amplifier volume field.** `0x0004` bits 13:8 are treated as a 6-bit attenuation, `0x3F` = 0 dB, 1.5 dB per
   step down, with bits 15:14 and 7:0 preserved.

Because all three are inferences, **the amplifier's own mute and volume registers are off by default**. Mute and
volume are applied where the arithmetic is provable: the service's fader on the copy into the DSP buffer
(`Tb710fuAudioGain.c`, a dB→ppm table plus saturating 16-bit scaling). `audio-amp-mute=1` and
`audio-amp-volume=1` opt into the register paths once a hardware run has confirmed them.

## GPR and AudioReach

The transport is the already hardware-verified ADSP stack (`package/Tb710fuAdsp`) reached through
`Tb710fuPowerLink.h`:

```c
Tb710fuPowerLink *tb710fu_power_link_acquire("adsp_apps", kMessage, 4, &client, this);
int tb710fu_power_link_send(link, packet, bytes);
/* client: { state(context, up), receive(context, data, length) } — receive runs on the transport thread */
```

`kMessage` is `TB710FU_GPR_MAX_PACKET` (560 bytes, the ADSP stack's compacted message size). All three entry points
are weak-imported: with `adsp=0` the subsystem still links and simply reports "no ADSP transport".

**Channel budget.** `Tb710fuAdsp.cpp` currently creates `kClients = 2` GLINK clients and the battery subsystem
already holds one. Audio takes the second and last one. Any third client (a future subsystem) needs `kClients`
raised, otherwise `tb710fu_power_link_acquire` returns `null` and audio reports `Tb710fuAudioErrorLink`.

Packet header (24 bytes, little endian):

```
word0  version 3:0 | header words 7:4 | packet bytes 31:8
word1  dst domain 7:0 | src domain 15:8
word2  src port      word3  dst port
word4  token         word5  opcode
```

Defaults: `src = APPS (3)`, `dst = ADSP (2)`, `src port = 1`, APM port `1`, PRM port `2`. Each command's token
increments; the engine accepts an answer only when its token matches the command in flight, and treats anything
else as foreign traffic (a data completion with a stale token bumps `write_done`/`read_done`, nothing else).

The graph the driver builds, in the order the engine sends it (19 steps with the buffer mapped, 18 without):

| # | Step | Opcode | Target |
|---|---|---|---|
| 1 | `SpfProbe` | `GPR_OP_APM_GET_SPF_STATE` (0x01000006) | APM |
| 2 | `SpeakerOpen` | `AR_OP_GRAPH_OPEN` (0x01000002) | APM, subgraph 0x4005 + container + I2S sink module |
| 3 | `PlaybackOpen` | `AR_OP_GRAPH_OPEN` | APM, subgraph 0x4001 + shmem→PCM dec→converter chain |
| 4 | `SpeakerCfg` | `AR_OP_SET_CFG` | subgraph 0x4005: PCM output format, I2S interface 0x08001019, hw-endpoint media format, (optional TDM parameter) |
| 5 | `PlaybackCfg` | `AR_OP_SET_CFG` | subgraph 0x4001: PCM output format |
| 6 | `ShmemCfg` | `AR_OP_SET_CFG` | instance 0x6001 (playback shmem endpoint): media format |
| 7-8 | `SpeakerPrepare`, `PlaybackPrepare` | `AR_OP_GRAPH_PREPARE` (0x01000005) | subgraphs 0x4005, 0x4001 |
| 9-10 | `SpeakerStart`, `PlaybackStart` | `AR_OP_GRAPH_START` (0x01000006) | subgraphs 0x4005, 0x4001 |
| 11 | `MicOpen` | `AR_OP_GRAPH_OPEN` | APM, subgraph 0x4007 + codec-DMA source module |
| 12 | `CaptureOpen` | `AR_OP_GRAPH_OPEN` | APM, subgraph 0x4003 + shmem writer chain |
| 13 | `MicCfg` | `AR_OP_SET_CFG` | subgraph 0x4007: codec-DMA interface 0x08001063, channel mask |
| 14 | `CaptureCfg` | `AR_OP_SET_CFG` | subgraph 0x4003 |
| 15-16 | `MicPrepare`, `CapturePrepare` | `AR_OP_GRAPH_PREPARE` | subgraphs 0x4007, 0x4003 |
| 17-18 | `MicStart`, `CaptureStart` | `AR_OP_GRAPH_START` | subgraphs 0x4007, 0x4003 |
| 19 | `Map` | `AR_OP_MEM_MAP` (0x01000007) | APM: shared region, one 4 KiB-aligned region |

Each step has a 250 ms deadline and two retries; the third deadline fails the sequence and records
`Tb710fuAudioErrorTimeout`. A refusal (status ≠ 0 in the basic result) fails at that step and keeps the DSP's status
in `native_tb710fu_audio_stats[10]`, so `audio=3` bisects by step number alone.

The map command is the shape the DSP's own client builds: the GPR header, then a bare region description
(`u16 3` shared/8-byte-aligned/4 KiB pool, `u16 1` region, `u32 0` flags, `u64 address`, `u32 bytes`) and the
trailing 16-byte command space — 60 bytes total, no parameter blocks. Our host test asserts that number, so a
future refactor cannot silently reshape it.

## The macOS service and its ABI

The service is a plain `IOService` (`Tb710fuAudioDevice`) with an `IOUserClient` (`Tb710fuAudioUserClient`), not an
IOAudioFamily driver. On macOS 27 the family exists as a kext but is closed to new drivers; every family symbol
would be an undefined link symbol for `libTb710fuAudio.a`. This package names the family exactly once, inside a
string literal, for an `OSMetaClass::getMetaClassWithName("IOAudioDevice")` lookup whose answer becomes
`Tb710fuAudioFlagIOAudioFamily`. `scripts/build-tb710fu-audio.py` asserts with `nm -u` that no `IOAudio*` symbol is
undefined in the archive, and `scripts/check-tb710fu-audio-host.py` fails the build if a family identifier is
named in code.

The ABI (`Tb710fuAudioAbi.h`):

- IORegistry property `"tb710fu-audio"` = magic `0x54413130` ("TA10"), version `3`; provider class
  `Tb710fuAudioDevice`, user client class `Tb710fuAudioUserClient`.
- `clientMemoryForType(0)` returns one physically contiguous region of
  `64 (header) + 4 × 4096` bytes; the header (`Tb710fuAudioSharedHeader`, exactly 64 bytes, asserted) carries
  magic/version/sizes and four counters (`produced`, `consumed`, `captured`, `fetched`).
- Chunks are 4,096 bytes = 512 frames of 4 channels × 16 bit; the DSP side is 32-bit TDM, so the service's fader
  and the DSP's converter are what bridge 16-bit client audio to the 32-bit slots.
- Selectors: 0 `GetStatus` (`Tb710fuAudioStatus` out), 1 `SetVolume` (dB, 0..-96), 2 `SetMute`, 3 `Start`
  (1 playback, 2 capture, 3 both), 4 `Stop`, 5 `CommitPlayback` (chunk index: applies gain, copies into the DSP
  buffer, sends a WRITE), 6 `FetchCapture`, 7 `GetText` (the last bring-up line, 256 bytes).

A CoreAudio HAL plugin is not part of this repository. It would match on the provider class, open the user client,
`IOConnectCallStructMethod` selectors 0/5/6, and map the shared region through `IOConnectMapMemory64`. The ABI is
fixed-width and byte-order-explicit so such a plugin cannot drift from the kext without failing the host test.

## Boot arguments

Default `audio=0`: nothing is mapped, nothing is written, no GLINK client is taken.

| Argument | Default | Meaning |
|---|---|---|
| `audio` | `0` | ceiling: 1 I2C + amp probe (read-only), 2 + ACF replay, 3 + graphs open/prepared/started, 4 + buffer mapped and streaming |
| `audio-rate` | `48000` | sample rate for both endpoints (8000..192000) |
| `audio-speaker-bits` | `32` | TDM slot width (16 or 32) |
| `audio-speaker-channels` | `4` | slots used = amplifiers driven |
| `audio-capture-bits` | `16` | capture sample width |
| `audio-capture-channels` | `2` | DMIC channels requested (`channel_mask` 0x3 for 2) |
| `audio-amp-mute` | `0` | 1 = also use the amplifier's own `0x0003` mute word (unverified) |
| `audio-amp-volume` | `0` | 1 = also use the amplifier's own `0x0004` attenuation field (unverified) |
| `audio-tdm-intf` / `-sd` / `-ws` | `1` / `0` / `1` | I2S interface selectors: secondary / SD0 / internal WS |
| `audio-tdm-param` | `0` | 1 = also send the unverified TDM parameter `0x0800101A` |
| `audio-gpr-src` / `audio-gpr-dst` | `3` / `2` | GPR domains (APPS / ADSP) |
| `audio-acf` | `/private/var/firmware/awinic/aw88461_tb710fu.bin` | ACF path on the Data volume |

The ACF file must be staged on the Data volume (the firmware is never committed): copy
`awinic/aw88461_tb710fu.bin` from the stock firmware set to that path, or point `audio-acf=` at it.

## Bring-up stages and what each proves

`audio=1` — maps `i2c_hub_4`, checks `proto == 3` (I2C) and that `GENI_IF_DISABLE` is clear, records FIFO depths,
then probes each `0x34..0x37` with read-only reads of `0x0000..0x0003`. **Proves**: the bus is out of reset and the
parts answer. **Does not prove**: anything about the audio path. It writes nothing. The four reset lines are only
*snapshotted* (`native_tb710fu_audio_pins[i]` = the TLMM `GPIO_CFG` word); this driver never drives them, because a
wrong polarity there costs a boot. If no part answers, compare that snapshot with the DTS's
`reset-gpio = <&tlmm 9/8/2/60 GPIO_ACTIVE_HIGH>` and check the boot chain's pin muxing.

`audio=2` — opens the ACF file through `tb710fu_fw_open` (the ADSP stack's reader: Data volume, then the boot
addon), validates the container, and replays each channel's 75 pairs in order with a read-back every eighth pair.
**Proves**: the parts accept the vendor tuning. **Does not prove**: audible output — nothing is unmuted, no graph
exists.

`audio=3` — acquires the `adsp_apps` GLINK client and runs the 19-step sequence. **Proves**: the ADSP's APM accepts
this board's graph description and starts both endpoints. **Does not prove**: that the DSP's TDM frame matches the
four amplifiers' expectations, or that the codec's DMIC path is clocked.

`audio=4` — maps the contiguous region, sends it to the DSP, and accepts chunks from the user client. **Proves**:
the whole path from a HAL-style client to the DSP buffer. Sound at the speakers additionally needs the WCD9395
codec (for capture) and the amplifiers' own unmute (`audio-amp-mute=1`) — or the fader path with a graph that is
already running.

## What is not verified, and how to settle it

| Open item | How it is settled |
|---|---|
| AW88461 wire byte order (address and value MSB-first) | `audio=1`: if all four probes read `0000`/`FFFF` nonsense, flip to LSB-first |
| `0x0003` bit 15 = HIZ/standby | `audio=2` + `audio-amp-mute=1`: does the speaker go silent and come back |
| `0x0004` bits 13:8 = 6-bit attenuation, 1.5 dB/step | `audio=2` + `audio-amp-volume=1`: audible level change across `-0..-96 dB` |
| GPR domains 3→2 | `audio=3`: a wrong domain shows up as `gpr_answers == 0` and every step timing out; override with `audio-gpr-src`/`-dst` |
| TDM selectors (interface 1, SD 0, WS internal) | `audio=3` answers, but silence means the DSP's I2S module wants different selectors; override with `audio-tdm-intf/-sd/-ws` |
| The TDM parameter `0x0800101A` | off by default; `audio-tdm-param=1` only after the selectors are confirmed |
| Capture channel count / which DMIC pair | `audio-capture-channels=2` default; raise it and fix the codec side separately |

`native_tb710fu_audio_stats[]` is the bisect surface: `[0]` stage, `[1]` requested stage, `[2]` amps present,
`[3]` amps loaded, `[4]` amp writes, `[5]` write errors | read-back failures << 16, `[6]` GPR packets sent,
`[7]` answers, `[8]` timeouts, `[9]` graph step, `[10]` last status, `[11]` I2C ok, `[12]` I2C failures,
`[13]` write completions, `[14]` read completions, `[15]` last error | flags << 16. `native_tb710fu_audio_text`
carries the last one-line status for the user client's `GetText`.

## Wiring this into the build (final merge)

Nothing outside `package/Tb710fuAudio/` was modified by this branch. The four edits below belong to the merge:

1. `board.toml`, `archives` list: add `"build-tb710fu-audio.py"` after `"build-tb710fu-adsp.py"`.
2. `configs/tb710fu_macos_defconfig`, `CONFIG_XNU_EXTRA_LD_ARCHIVES`: append
   `-Wl,-force_load,{tree}/build/tb710fu-audio/libTb710fuAudio.a`.
3. `configs/tb710fu_macos_defconfig`, `CONFIG_XNU_EXTRA_CFLAGS`: append `-DXNU_TB710FU_AUDIO=1`.
4. `package/Macos27Services.cpp`: declare the two entry points weak, install the metaclass before
   `postModLoad` next to the battery's, and start the service after the ADSP (the GLINK client name only exists
   once `adsp=2` created the transport; `audio=0` makes the call a no-op):

   ```cpp
   extern "C" void tb710fu_audio_meta_init(void) __asm__("__GLOBAL__sub_I_Tb710fuAudioService.cpp")
       __attribute__((weak_import));
   extern "C" void tb710fu_audio_start(void) __attribute__((weak_import));

   /* before postModLoad, beside tb710fu_battery_meta_init() */
   if (tb710fu_audio_meta_init) tb710fu_audio_meta_init();

   /* in the start section, after tb710fu_adsp_probe() and tb710fu_battery_start() */
   if (tb710fu_audio_start) tb710fu_audio_start();
   ```

   Keep the ordering: ADSP first (transport), then audio (takes the second and last GLINK client slot).

`package/Kit.toml` needs no change: it enumerates the archives' scripts, not per-subsystem files. The ACF file is
firmware, not source: it is staged on the Data volume by the image job (the same way `adsp.mbn` is), never committed.

## How this is checked

```sh
python3 scripts/check-tb710fu-audio-host.py     # host C checks + C++ static audit; writes build/tb710fu-audio-host/report.json
TB710FU_ACF_FILE=/path/aw88461_tb710fu.bin python3 scripts/check-tb710fu-audio-host.py
python3 scripts/build-tb710fu-audio.py          # on the macOS kernel runner, after the minimal XNU build
```

The host check compiles the seven portable C modules and `tests/tb710fu_audio_test.c` with
`-std=c11 -Wall -Wextra -Werror` plus AddressSanitizer/UndefinedBehaviorSanitizer, then runs 324 assertions: ACF
container (including the real 3,504-byte file), GENI framing and NACK/cancel paths, amplifier probe/load/mute/volume
against a fake register file, GPR header and refusals, every AudioReach builder (including the exact map size), the
19-step engine against a fake APM (including a refusal, two retries then a timeout, and chunk writes), and the ABI
layout and fader arithmetic. It also audits the two C++ files it cannot compile on this host: declared members
versus definitions, brace balance, no bulk memory call on mapped memory, no increment of a `volatile` lvalue, the
platform header where `getPlatform()` appears, and no IOAudioFamily identifier.

`scripts/build-tb710fu-audio.py` compiles with the real XNU/IOKit compile records (arm64e-apple-ios27.0), defers the
service's metaclass constructor so `Macos27Services.cpp` installs it before `postModLoad`, writes
`libTb710fuAudio.a`, and then asserts with `nm` that no `IOAudio*` symbol is undefined and that
`tb710fu_audio_start`/`tb710fu_audio_meta_init` are defined. A green build is a compile — not a prelinked kernel, not
a boot, and not a tone.
