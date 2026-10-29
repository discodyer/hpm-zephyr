# HPM Zephyr Module

Out-of-tree Zephyr module for HPMicro HPM5361-based boards.

## Layout

```text
boards/hpmicro/hpm5300evk/   HPM5300EVK board definitions
boards/hpmicro/hpm5361icb/   HPM5361ICB board definitions
soc/hpmicro/hpm5300/         HPM5300 SoC family integration
dts/riscv/hpmicro/           HPM5300 devicetree sources and package pinctrl
dts/bindings/                Devicetree bindings
drivers/                     Zephyr drivers backed by HPM SDK
include/                     Public DT bindings and driver headers
modules/hpm_sdk/             HPM SDK integration glue
```

## Supported boards

| Board | Target | Console | Build status | Hardware status |
|---|---|---|---|---|
| HPM5300EVK | `hpm5300evk/hpm5361` | UART0, PA00/PA01, 115200 | RAM and XIP build pass | RevD XIP boot, UART echo, heartbeat, flash runner/GDB and fourteen hardware profiles (52 tests) pass |
| HPM5361ICB | `hpm5361icb/hpm5361` | UART0, PA00/PA01, 115200 | RAM and XIP build pass | Not validated |

Both targets currently provide interrupt routing, machine timer, clock/PLL and
Andes L1 cache startup, UART pinctrl/clock/baud initialization, full pad
pinctrl, GPIO interrupts, and an XPI NOR flash driver. HPM5300EVK also enables
I2C0 and SPI1. The XIP image reserves
the BootROM NOR configuration at `0x80000400`, places the boot header at
`0x80001000`, and starts the application at `0x80003000`. Flash write, erase,
and initialization execute from RAM while XIP is active.

## Compatibility

| Component | Tested revision |
|---|---|
| Zephyr | `f78c4deb4523a58eca057cc59b33532b80c20843` (`v4.5.0-rc2-179-gf78c4deb452`) |
| HPM SDK | `ccc5ec467a047f3ea85cd2fa4565e91db7135a38` (`v1.13.0`) |
| hpm-zephyr | Current `main`; release manifests pin immutable commits |

Build-only validation is complete for both boards, including the Twister
smoke test. EVK RevD XIP startup, basic polling UART echo/heartbeat,
OpenOCD flash/verify and GDB breakpoints/single-step passed. The hardware
acceptance profiles passed 52 tests: `core` (8/8), `dma` (2/2), `crypto` (6/6),
`flash` (1/1), `watchdog` (4/4), `can` (2/2), `motor` (5/5), `gpio` (4/4),
`uart` (4/4), `i2c` (3/3), `spi` (1/1), `analog` (6/6), `front_end` (3/3)
and `pwm` (3/3).
Coverage includes internal CAN/TSU loopback, motor-block math and routing,
GPIO edge interrupts, UART1 DMA/RTS/CTS,
paired I2C DMA/repeated START/SMBus block/PEC transfers, and SPI1 four-mode
GPIO-CS loopback with synchronous and asynchronous linked-block DMA transfers,
plus DAC0-to-ADC0 synchronous/asynchronous, preemption, TRGM, step and buffer
paths, plus OPAMP0 follower voltage and ACMP1 edge, disable/rearm and pending
callback paths. PWM0-to-PWM1 capture covers period, duty, polarity, counter wrap
and constant output levels. Analog accuracy and external waveform timing remain
unvalidated.

Each result covers the paths listed in the
[hardware test record](tests/hardware/README.md). Flash coverage is limited
to erase/program/read/cache behavior in the reserved scratch sector;
JEDEC/geometry, boundaries and latency remain unvalidated. The crypto result
covers synchronous SDP known-answer tests; async completion and stress remain
open. Independent timer accuracy, USB enumeration and class traffic remain
unvalidated. SPI acceptance is limited to MOSI/MISO loopback; external slave
timing, native CS behavior and electrical measurements remain open.
Analog acceptance covers the DAC0.OUT-to-ADC0.IN13 loopback and the OPAMP0/ACMP1
front-end fixture. Accuracy, noise, linearity, external waveform timing and
other instances remain unvalidated.
PWM acceptance uses separate output and capture blocks sharing the AHB clock;
absolute timing accuracy, complementary dead time, faults and high-rate capture
remain unvalidated. Standard capture reserves a block with no previously
configured output during the current boot; measured periods must be shorter
than its full 28-bit counter cycle.

On 2026-10-27, FT2232 reset wiring, NOR boot-option retention and PMP access
to the DM debug ROM were corrected without changing the SDK. The DM region
at `0x30000000..0x300001ff` remains RWX while data execution prevention stays
enabled. The wrapper resets and halts on GDB attach so the XPI probe algorithm
can run before application PMP initialization. Use the minimal
[`samples/bringup`](samples/bringup/README.rst) for startup/debugging checks.
The container's HPM OpenOCD is `/usr/local/bin/openocd`; pass it explicitly to
`west flash/debug` instead of using the Zephyr SDK's OpenOCD.

The module can be validated without adding it to the west manifest:

```sh
ZEPHYR_BASE="$PWD/zephyr" HPM_SDK_DIR="$PWD/hpm_sdk" \
  ./hpm-zephyr/scripts/smoke_build.sh
ZEPHYR_BASE="$PWD/zephyr" HPM_SDK_DIR="$PWD/hpm_sdk" \
  ./hpm-zephyr/scripts/xip_smoke_build.sh
HPM_SDK_ALL_DRIVERS=y ZEPHYR_BASE="$PWD/zephyr" \
  HPM_SDK_DIR="$PWD/hpm_sdk" ./hpm-zephyr/scripts/xip_smoke_build.sh
ZEPHYR_BASE="$PWD/zephyr" HPM_SDK_DIR="$PWD/hpm_sdk" \
  ./hpm-zephyr/scripts/twister_smoke.sh
```

## Driver support

The driver layer is intentionally narrower than the HPM5361 Devicetree. A
binding or a disabled DTS node describes hardware, but does not mean that a
Zephyr driver exists. The current audit of `drivers/`, Kconfig and DTS found:

| Peripheral | Instances | Zephyr API | Driver status | Current scope / missing work |
|---|---:|---|---|---|
| Clock / PLL | 1 PLLCTLV2 | `clock_control` | GPTMR clock recovery hardware-tested | Clock gating and rate queries; full clock-tree coverage and PM/PDGO domains remain separate |
| IOC / PIOC | 2 controllers | `pinctrl` | Implemented (build-only) | Pad mux and pad settings; build-time conflict checks only |
| UART | UART0-7, PUART | `serial` | UART1 DMA and RTS/CTS loopback hardware-tested | Polling, interrupt-driven and async/DMA APIs; 257 B transfer, segmented RX offsets, RTS/CTS loopback and RX disable/re-enable passed; CTS backpressure, continuous dual-buffer RX, RS-485 and address matching remain unvalidated |
| GPIO | GPIOA/B enabled by default; GPIOX/Y, FGPIOA/B/X/Y, PGPIO disabled | `gpio` | PB04/PB05 loopback hardware-tested | Raw levels, set/clear/masked/toggle, rising/falling/both-edge IRQs and disable/rearm passed; level IRQs, other ports and electrical characteristics remain unvalidated; FGPIO has no interrupt output |
| I2C | 4 | `i2c` | Paired DMA and SMBus paths hardware-tested | Controller/target, 7/10-bit addressing, standard/fast/fast-plus, multi-message transfers, DMA, bus recovery and SMBus helpers; paired DMA/repeated START, byte/word/block transfers, valid/corrupt PEC and malformed-count recovery passed; 10-bit, process calls, bus recovery and fault injection remain unvalidated |
| SPI | 4 | `spi` | SPI1 GPIO-CS and linked-block DMA loopback hardware-tested | Four Motorola modes passed synchronous 8-bit scatter/gather and asynchronous 8-bit linked-block DMA loopback with a 256-byte pattern; 16/24/32-bit sync, 16/32-bit DMA, native CS, half-duplex, dual/quad, hold/lock, external slave timing and electrical measurements remain unvalidated |
| XPI NOR / XIP | 1 | `flash` | Basic scratch-sector path hardware-tested | Read, write, erase and page layout through vendor ROM API; JEDEC/geometry discovery and EXIP integration are absent |
| MCAN / CAN-FD | 4 | `can` + custom extension | MCAN0 silent internal loopback hardware-tested | Classic/FD/BRS data and TX/RX ISR checks passed; 64-bit TSU captures matched PTPC0 time bounds across eight slots and two wraps. Shared 32 KiB AHB message RAM is split into 8 KiB windows. Generic frames expose a 16-bit timestamp field; full TSU captures use the `hpm_mcan` helper. External bus behavior and MRAM concurrency remain unvalidated |
| PWM | 2 x 8 channels | `pwm` + HPM extension | PWM0 output / PWM1 capture hardware-tested | 108 period/duty/polarity/type combinations, microsecond/nanosecond conversion, continuous 28-bit counter wrap, disable/rearm, busy rejection, timeout recovery and 0/100% GPIO levels passed; complementary/dead-zone/fault and reference outputs are implemented but not covered by this profile |
| HDMA / DMAMUX | 32 channels | `dma` | Linked-block and suspend/resume paths hardware-tested | Memory-to-memory/peripheral transfers, cyclic mode, linked descriptors, DMAMUX requests, callbacks, channel allocation/release and status/attributes; peripheral-to-peripheral is unsupported by the silicon |
| ADC16 | 2 | `adc` | ADC0 sequence, preemption and TRGM paths hardware-tested | 12-bit synchronous/asynchronous sequence, two-channel read after preemption, 16-bit preemption DMA and external trigger passed; other resolutions, motor forwarding and other instances remain unvalidated |
| DAC12 | 2 | `dac` | DAC0 direct, step and buffer paths hardware-tested | 12-bit direct output, step endpoint and internal buffer measured through ADC0; hardware trigger, waveform timing and other instances remain unvalidated |
| ACMP | 1 block, 2 comparators | `comparator` | ACMP1 edge/callback paths hardware-tested | Single/both edges, callback identity/count/context, disable/rearm and pending read/clear/rebind passed with the internal DAC threshold; ACMP0, filtering, hysteresis, precision, latency and concurrency remain unvalidated; live output level is unavailable |
| OPAMP | 2 | `opamp` | OPAMP0 follower hardware-tested | Four DAC0 levels passed ADC0.IN15 follower readback; other modes, gain settings, OPAMP1 and calibrated accuracy remain unvalidated; no differential mode |
| GPTMR / PTMR | GPTMR0-3, PTMR | `counter` | Core alarm path hardware-tested | GPTMR alarm IRQ and clock recovery passed in the core profile; top/wrap boundaries and other timer channels remain unvalidated |
| Watchdog | EWDG0-1, PEWDG | `watchdog` | EWDG0 IRQ-only paths hardware-tested | Thread/ISR feeding, delayed feeding, repeated timeout, disable/reinstall and 500 ms to 2500 ms reconfiguration passed; SoC reset, window mode and sleep/debug pause remain unvalidated; CPU-core-only reset and multistage watchdog are unsupported |
| RTC | None | N/A | Not applicable | HPM5300/HPM5361 has no independent RTC block; use a board-level external RTC if wall-clock time is required |
| USB HS OTG | 1 | UDC/UHC | Device-only UDC implemented (build-only) | Zephyr UDC driver reuses the unmodified SDK qTD/qDHD engine and covers HS/FS endpoints, SETUP tripwire, reset/suspend/resume, VBUS events, remote wakeup and test modes. qHD/qTD storage is in HPM5361's fixed uncached DLM. UHC/host mode, USB classes and hardware enumeration remain unvalidated; CherryUSB/TinyUSB are not adopted |
| MBX | MBX0A/B | `mbox` | Core path hardware-tested | Bidirectional message ordering passed in the core profile; separate 4-word FIFO, signalling-only mode and cross-core shared-memory IPC remain unvalidated |
| GPIOM | 1 | `hpm_gpiom` | Input visibility path hardware-tested | PA24 FGPIO0 input visibility toggle and restore passed without driving the physical pin; ownership, other controllers, locks and security behavior remain unvalidated |
| TRGM | 1 | `mux` + custom extension | Implemented core (build-only) | One mux control per TRGM output with runtime source selection and readback; input filters, output inversion and edge-pulse conversion, eight DMA request routes, the ADC/DAC/position matrices, and named motor-control routes for SYNT/MMC/SEI/RDC/QEO/PLB inter-IP connections. The route helper validates fixed SDK mux indices at build time and leaves endpoint register configuration to the owning drivers. Interrupt routing is available through output 133/134 plus the existing output API, but no TRGM-specific IRQ callback is claimed because the block has no independent status/clear register |
| QEIV2 | 2 | `sensor` + custom extension | Implemented core (build-only) | ABZ, pulse/direction, up/down, single-phase, UVW and ADC sine/cosine decoding; configurable counts and Z calibration, A/B delay filter, angle, raw position and revolution count; two pulse/duration speed counters with RPM conversion, phase/position comparators, watchdog, HOME/FAULT configuration, event routing to interrupt/DMA/trigger/read and callbacks. ADC sampling and TRGM ADC-matrix setup remain owned by ADC/TRGM; hardware behavior is not validated |
| QEO | 2 | Custom `hpm_qeo` | Cosine debug-value path hardware-tested | Four amplitude/midpoint configurations, four quadrants and three channels passed; ABZ/pulse/direction/PWM outputs, external waveforms and safety routing remain unvalidated |
| SEI | 2 channels | Custom `hpm_sei` | Implemented core, SSI, Tamagawa, BiSS-C, EnDat, HIPERFACE and Nikon consumers (build-only) | Protocol-neutral transceiver, timing-point, command/data-register, instruction, latch/output, engine and W1C event APIs; external/periodic triggers, per-channel interrupt callbacks and whole-block consumer ownership. The SSI, Tamagawa, BiSS-C, EnDat 2.1/2.2, HIPERFACE and Nikon CDF0 master consumers cover position, status/checksum and timestamp capture with periodic polling and watchdog recovery. BiSS-C, EnDat, HIPERFACE and Nikon use fixed receive points because the vendor dynamic-delay calibration requires the E00047-affected RX latch path. HIPERFACE uses the vendor checksum sequence; E00025 requires a hardware parity workaround that is not yet implemented, so the 8E1 path remains hardware-unvalidated |
| MMC | 2 | Custom `hpm_mmc` | Open-loop math and module control hardware-tested | 1 turn/s prediction matched SYNT elapsed time and whole-module disable/resume passed; predictor BR_EN alone did not freeze continuous prediction; closed-loop behavior, external routes and IRQs remain unvalidated |
| RDC | 1 | Custom `hpm_rdc` | Implemented core (build-only) | PWM/DAC excitation configuration, I/Q ADC accumulation input, filtering/scaling, output triggers, accumulated/extrema/delay result access, W1C event status and interrupt callbacks; integer-CORDIC Q16.16 electrical angle conversion without FPU/libm; TRGM/DAC routing is configured through TRGM |
| PLB | 1 | Custom `hpm_plb` | TYPE_B0 slice15 path hardware-tested | LOAD/KEEP behavior passed using internal TRGM comparator observation; SW_INJECT is software-injection readback, not a live counter, so the counter getter returns `-ENOTSUP`; filter/encoder components remain outside this test |
| SYNT | 1 | Custom `hpm_synt` | Counter control hardware-tested | Counter progress, freeze and resume passed; comparator outputs, external routing and debug-stop behavior remain unvalidated |
| CRC | 1 block, 8 channels | `crc` | Core path hardware-tested | Streaming CRC32 known-answer test passed; other widths, polynomials and reflection combinations remain unvalidated |
| TSNS | 1 | `sensor` | Core path hardware-tested | Temperature read and threshold IRQ passed; calibration/accuracy and operating-range coverage remain unvalidated |
| RNG | 1 | `entropy` | Core path hardware-tested | Startup self-test, data reads and nonblocking ISR budget passed; adapter FIFO/reseed waits are bounded, but waits inside the pinned SDK initialization/self-test are not; entropy quality/statistical testing remains unvalidated |
| PTPC | 1 block, 2 timestamp counters | `ptp_clock` + custom extension | Core and MCAN TSU paths partially hardware-tested | Progress, compare IRQ, internal TRGM capture and PTPC0-correlated 64-bit MCAN TSU timestamps passed; absolute precision, external capture, PPS, rate adjustment and the second counter remain unvalidated |
| SDP | 1 | `crypto` | Synchronous KAT paths hardware-tested | AES-128/256 ECB/CBC/CTR/CCM, SHA-1/SHA-256/SM3/CRC32 and SM4 ECB/CBC/CTR through the standard API and HPM extensions, using the unmodified SDK backend. Six KATs including CCM tag rejection passed; true IRQ-driven async completion and stress remain unvalidated |
| SEC / SECMON / KEYM | 1 each | Custom/security | Missing | No Zephyr integration |
| OTP | 1 | `otp` | Read-only path hardware-tested | UUID shadow reads were repeatable; fuse programming, lock/region operations and reload remain unvalidated and irreversible actions stay opt-in |

Hardware acceptance applies only to the paths listed above; other behavior
remains build-only until its corresponding hardware test is completed.
Missing drivers are not built or enabled. The SoC startup, PLIC, PLICSW, machine
timer and cache paths are native Zephyr/SoC integration rather than HPM driver
classes. No third-party module is currently selected for the remaining standard
Zephyr APIs. USB device mode uses Zephyr's native device-next stack plus the
unmodified SDK controller engine. SDP uses the generic Zephyr crypto API and
HPM extensions without an SDK crypto middleware stack. USB host/UHC and
motor-control integration still need separate design decisions before adding
any third-party stack.

The detailed, workspace-level matrix and task queue are maintained in
`docs/agent/tasks.md`.

## HPM5361 packages

HPM5361 has three ordering variants with different package pin exposure. The
common SoC DTSI describes only silicon-level instances; each board must include
exactly one package pinctrl DTSI:

| Package | Ordering code | GPIOs | Analog pins | DTSI |
|---|---|---:|---:|---|
| LQFP100 | HPM5361ICB1 | 56 | 16 | `dts/riscv/hpmicro/hpm5361-icb-pinctrl.dtsi` |
| LQFP64 | HPM5361ICF1 | 36 | 10 | `dts/riscv/hpmicro/hpm5361-icf-pinctrl.dtsi` |
| QFN48 | HPM5361IEG1 | 29 | 8 | `dts/riscv/hpmicro/hpm5361-ieg-pinctrl.dtsi` |

The package DTSIs are generated from the pinned SDK IOMUX headers and expose
one `/omit-if-no-ref/` node per pad/function pair. Boards compose functions
with multiple phandles, for example:

```dts
pinctrl-0 = <&pa00_uart0_txd &pa01_uart0_rxd>;
```

`ngpios` describes the GPIO controller's implemented width and is not a
package pin count. The number of bonded-out pins is recorded in the package
metadata node instead. `scripts/check_pinctrl.py` verifies that enabled
consumers do not reference unbonded pads and do not assign one pad twice in the
same pin state. The RAM and XIP smoke scripts run this check automatically.

The upstream vendor DTS remains untouched. Regenerate the package files after
an SDK update with:

```sh
./hpm-zephyr/scripts/generate_hpm5361_pinctrl.py
./hpm-zephyr/scripts/generate_hpm5361_pinctrl.py --check
```

## HPM SDK integration

The complete, unmodified upstream `hpm_sdk` is kept as a separate west project
and pinned by full commit SHA. Zephyr-specific integration lives in
`modules/hpm_sdk/` and in the Zephyr drivers under `drivers/`. This keeps the
same clean upstream-HAL boundary used by `hal_stm32`, but uses HPM-specific
per-IP selection rather than STM32's SoC-family source lists. Only HAL sources
selected by Zephyr configuration are compiled; SDK middleware, components and
samples stay out of the default build.

`HAS_HPMSDK_*` is selected only by the SoC definition and describes silicon
capabilities. `USE_HPMSDK_*` is selected by the Zephyr integration that needs a
vendor HAL source. Devicetree still controls which instances a board enables.
The optional `CONFIG_HPM_SDK_BUILD_ALL_DRIVERS=y` compiles every HAL source
listed for the selected SoC for build-only regression testing.

The tested Zephyr and HPM SDK revisions, plus hashes of the selected SoC HAL
list and vendor driver source manifest, are recorded in
`modules/hpm_sdk/baseline.env`. Verify a checkout with:

```sh
HPM_SDK_DIR="$PWD/hpm_sdk" ZEPHYR_BASE="$PWD/zephyr" \
  ./hpm-zephyr/scripts/hpm_sdk_baseline.py
```

### HPM SDK upgrade checklist

Do not update only the manifest SHA. Treat an SDK upgrade as an integration
change and perform these steps in order:

1. Check out the candidate tag and resolve it to a full commit SHA. Keep the
   vendor tree unmodified.
2. Compare the candidate `soc/HPM5300/HPM5361/soc_modules.list` and
   `drivers/CMakeLists.txt` with the pinned versions. Add or remove
   `HAS_HPMSDK_*`/`USE_HPMSDK_*` symbols only when the vendor capability or
   source mapping actually changed; update `SOC_HPM5361` capability selects to
   match the new SoC list.
3. Regenerate the package pinctrl DTSIs and run the pinctrl consistency check.
4. Run the normal and all-HAL builds. Review the final map for unexpected
   vendor objects and any duplicate or undefined symbols.
5. Update the HPM SDK SHA in the west manifest and regenerate
   `modules/hpm_sdk/baseline.env` with `--write` only after the review passes.
6. Record the tested Zephyr revision and validation result in
   `docs/agent/tasks.md`, then freeze a release manifest with
   `west manifest --freeze`.

Run the baseline unit tests after changing the checker or its baseline format:

```sh
python3 -m unittest scripts/tests/test_hpm_sdk_baseline.py
```

The baseline check intentionally fails when either the SDK or Zephyr revision
changes. This makes an upgrade review explicit instead of allowing a floating
workspace to pass silently.

The SoC DTSI describes the major HPM5361 instance set, including UART0-7,
I2C0-3, SPI0-3, MCAN0-3, PWM0-1, ADC0-1, DAC0-1, ACMP, OPAMP0-1, GPTMR0-3,
EWDG0-1, HDMA, USB0, GPIOA/B/X/Y, PGPIO, PUART, PTMR and PEWDG. Instances whose
Zephyr drivers are not implemented yet remain disabled. Their bindings define
the hardware contract but do not imply driver support. The DTS inventory is not
yet complete for special-function blocks. HPM5300/HPM5361 has no independent RTC; wall-clock time requires a board-level external RTC.

The workspace-level agent notes record the maintenance assessment, the
per-SoC capability matrix, board interfaces, all-HAL build-only coverage and
upgrade process.

## Development

Peripheral hardware acceptance tests for HPM5300EVK are in
[`tests/hardware`](tests/hardware/README.md). The application provides isolated
Ztest profiles, wiring/fixture requirements, build/flash commands, detailed
results and limits for the eleven verified profiles listed above. Historical
build-only regressions are recorded separately from hardware results.
The original `tests/hpm5300evk` suite remains for build-only coverage.

Use Zephyr's upstream `<subsystem>: <summary>` format for local development:

```sh
git commit -m "soc: hpmicro: add HPM5361 initial support" \
  -m "Add the SoC integration and document the validated build paths."
```

During initial development, Agent-authored commits do not require DCO sign-off
and no local commit-message checker is enforced. Before upstream submission,
squash intermediary commits, run Zephyr's official checks, and have the human
contributor complete review and DCO sign-off.

## License

Apache-2.0. See `LICENSE`.
