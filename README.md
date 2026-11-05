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
| HPM5300EVK | `hpm5300evk/hpm5361` | UART0, PA00/PA01, 115200 | RAM and XIP build pass | RevD XIP boot, UART echo, heartbeat, flash runner/GDB and 31 hardware configurations (143 tests) pass |
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
| Zephyr | `41d10b59730406bbab4ca9f1fbbe9e96aafb4966` (`v4.5.0`) |
| HPM SDK | `ccc5ec467a047f3ea85cd2fa4565e91db7135a38` (`v1.13.0`) |
| hpm-zephyr | Current `main`; release manifests pin immutable commits |

Build-only validation is complete for both boards, including the Twister
smoke test. EVK RevD XIP startup, basic polling UART echo/heartbeat,
OpenOCD flash/verify and GDB breakpoints/single-step passed. The hardware
acceptance profiles passed 143 tests across 31 configurations: `core` (10/10),
`dma` (2/2), `crypto` (6/6),
`flash` (1/1), `watchdog` (4/4), `can` (2/2), `ptpc` (4/4), `motor` (5/5),
`gpio` (4/4), `uart` (6/6), `uart_address` (1/1), `uart_rs485` (1/1),
`lin` (6/6), `i2c`/`i2c_fast`/`i2c_fast_plus` (16/16 each), `spi` (6/6), `analog` (6/6),
`front_end` (3/3), `pwm` (3/3), `qei` (5/5), `qei_pd` (4/4), `qei_ud` (4/4),
`qei_single` (3/3), `qei_uvw` (3/3), `usb_device` (1/1), `usb_endpoints`
(1/1), `usb_endpoints_fs` (1/1), `usb_host` (1/1), `usb_host_hub` (1/1) and
`usb_host_hid_report` (1/1).
Coverage includes internal CAN/TSU loopback, motor-block math and routing,
GPIO edge interrupts, UART1 DMA/RTS/CTS,
paired I2C DMA/repeated START/SMBus block/PEC transfers at 100 kHz, 400 kHz
and 1 MHz, 10-bit transfers with repeated START, SCL held-low rejection,
stuck-SDA nine-clock recovery and target address/data-phase NACK recovery, and
SPI1 GPIO/native-CS loopback with four-mode 8-bit synchronous/asynchronous
linked-block DMA, 16/24/32-bit synchronous transfers, 16/32-bit linked DMA,
CS hold/release and unsupported-width rejection,
plus DAC0-to-ADC0 synchronous/asynchronous, preemption, TRGM, step and buffer
paths, plus OPAMP0 follower voltage and ACMP1 edge, disable/rearm and pending
callback paths. PWM0-to-PWM1 capture covers period, duty, polarity, counter wrap
and constant output levels. QEI1 covers ABZ, Z calibration, periodic signed RPM,
pulse/direction, up/down, single-phase and UVW sector decoding. PTPC covers both
timestamp channels, rate adjustment, PPS events and shared IRQ dispatch. USB Host
boot/report HID and one HS Single-TT hub with a downstream LS keyboard also
passed. Analog accuracy and external waveform timing remain unvalidated. The
LIN uses the Zephyr standard API over an HPM UART. Its six-case UART1/UART2
TTL fixture passed twice after independent resets; automatic baud
synchronization and bus-conflict detection are intentionally unsupported.
Board-level transceiver and external-peer validation remain separate.

Each result covers the paths listed in the
[hardware test record](tests/hardware/README.md). Flash coverage is limited
to erase/program/read/cache behavior in the reserved scratch sector;
JEDEC/geometry, boundaries and latency remain unvalidated. The crypto result
covers synchronous SDP known-answer tests; async completion and stress remain
open. USB Device enumeration, the endpoint matrix and Host boot/report keyboard
paths are covered by the USB profiles; multi-level/non-TT hubs, non-boot HID,
isochronous traffic, power management and suspend/resume/remote wakeup remain
unvalidated. SPI acceptance covers the single-jumper MOSI/MISO loopback. The
functional native-CS path and GPIO CS hold/release passed, but measured CS
waveforms/setup-hold timing, external-slave compatibility, half-duplex,
dual/quad, `SPI_LOCK_ON` and error recovery remain open.
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

To remove rebuildable build trees while retaining serial and Twister evidence,
run the cleanup helper from the workspace root. It only considers generated
directories with a CMake or Zephyr build marker and never deletes top-level
files:

```sh
python3 hpm-zephyr/scripts/clean_build_artifacts.py
python3 hpm-zephyr/scripts/clean_build_artifacts.py --apply
```

The first command is a dry run. The second removes non-evidence files from
directories containing logs or Twister reports, and removes build directories
without evidence. `*.log`, `twister.json`, `testplan.json`, `twister_report.xml`,
`twister_suite_report.xml` and `twister.xml` are preserved in place. OpenOCD
installation directories are protected by default. Additional file patterns
can be retained with `--preserve`, and complete directories with `--protect`.
Each option may be repeated. This does not use `west build -t pristine` or
Twister cleanup, which only clean a single output directory and do not scan a
workspace for test evidence.

## Driver support

The driver layer is intentionally narrower than the HPM5361 Devicetree. A
binding or a disabled DTS node describes hardware, but does not mean that a
Zephyr driver exists. The current audit of `drivers/`, Kconfig and DTS found:

| Peripheral | Instances | Zephyr API | Driver status | Current scope / missing work |
|---|---:|---|---|---|
| Clock / PLL | 1 PLLCTLV2 | `clock_control` | GPTMR clock recovery hardware-tested | Clock gating and rate queries; full clock-tree coverage and PM/PDGO domains remain separate |
| IOC / PIOC | 2 controllers | `pinctrl` | Implemented (build-only) | Pad mux and pad settings; build-time conflict checks only |
| UART | UART0-7, PUART | `serial` + HPM address-match extension | DMA, dual-buffer RX, CTS backpressure, address matching and RS-485 DE timing hardware-tested | Polling, interrupt-driven and async/DMA APIs; 257 B transfer, segmented offsets, continuous dual-buffer RX, RTS/CTS, real CTS backpressure/recovery, nine-bit address0/address1 filtering and auto-DE coverage of a 64 B TX burst passed; external RS-485 transceiver electrical behavior remains unvalidated |
| LIN | UART-based controller | `lin` | UART1/UART2 TTL commander and responder paths hardware-tested | Commander/responder framing, classic/enhanced checksum, PID filtering, receive timeout and GPIO break/wakeup generation passed twice after independent resets. TX completion uses slot-interrupt streaming and TX-idle detection. Board transceiver timing, external LIN peers, auto-sync and conflict detection remain unvalidated or unsupported |
| GPIO | GPIOA/B enabled by default; GPIOX/Y, FGPIOA/B/X/Y, PGPIO disabled | `gpio` | PB04/PB05 loopback hardware-tested | Raw levels, set/clear/masked/toggle, rising/falling/both-edge IRQs and disable/rearm passed; level IRQs, other ports and electrical characteristics remain unvalidated; FGPIO has no interrupt output |
| I2C | 4 | `i2c` | 100 kHz, 400 kHz and 1 MHz 16-case matrices hardware-tested | Controller/target, 7/10-bit addressing, standard/fast/fast-plus, multi-message transfers, DMA, bus recovery and SMBus helpers; paired DMA/repeated START, 10-bit single read/write and write/restart/read, SCL held-low rejection, nine-clock stuck-SDA recovery, byte/word/block transfers, valid/corrupt PEC, malformed/zero-count recovery, block process call and address/data-phase NACK recovery passed; timeout/fault injection and external timing remain unvalidated |
| SPI | 4 | `spi` | SPI1 GPIO/native-CS and wide-frame linked DMA hardware-tested | Four Motorola modes passed 8-bit synchronous scatter/gather and asynchronous linked-block DMA; 16/24/32-bit synchronous transfers, 16/32-bit linked DMA, GPIO `SPI_HOLD_ON_CS`, native CS and unsupported-width rejection passed on two independent resets. Native-CS waveform/setup-hold timing, half-duplex, dual/quad, `SPI_LOCK_ON`, external-slave timing and electrical measurements remain unvalidated |
| XPI NOR / XIP | 1 | `flash` | Basic scratch-sector path hardware-tested | Read, write, erase and page layout through vendor ROM API; JEDEC/geometry discovery and EXIP integration are absent |
| MCAN / CAN-FD | 4 | `can` + custom extension | MCAN0 silent internal loopback hardware-tested | Classic/FD/BRS data and TX/RX ISR checks passed; 64-bit TSU captures matched PTPC0 time bounds across eight slots and two wraps. Shared 32 KiB AHB message RAM is split into 8 KiB windows. Generic frames expose a 16-bit timestamp field; full TSU captures use the `hpm_mcan` helper. External bus behavior and MRAM concurrency remain unvalidated |
| PWM | 2 x 8 channels | `pwm` + HPM extension | PWM0 output / PWM1 capture hardware-tested | 108 period/duty/polarity/type combinations, microsecond/nanosecond conversion, continuous 28-bit counter wrap, disable/rearm, busy rejection, timeout recovery and 0/100% GPIO levels passed; complementary/dead-zone/fault and reference outputs are implemented but not covered by this profile |
| HDMA / DMAMUX | 32 channels | `dma` | Linked-block and suspend/resume paths hardware-tested | Memory-to-memory/peripheral transfers, cyclic mode, linked descriptors, DMAMUX requests, callbacks, channel allocation/release and status/attributes; peripheral-to-peripheral is unsupported by the silicon |
| ADC16 | 2 | `adc` | ADC0 sequence, preemption and TRGM paths hardware-tested | 12-bit synchronous/asynchronous sequence, two-channel read after preemption, 16-bit preemption DMA and external trigger passed; other resolutions, motor forwarding and other instances remain unvalidated |
| DAC12 | 2 | `dac` | DAC0 direct, step and buffer paths hardware-tested | 12-bit direct output, step endpoint and internal buffer measured through ADC0; hardware trigger, waveform timing and other instances remain unvalidated |
| ACMP | 1 block, 2 comparators | `comparator` | ACMP1 edge/callback paths hardware-tested | Single/both edges, callback identity/count/context, disable/rearm and pending read/clear/rebind passed with the internal DAC threshold; ACMP0, filtering, hysteresis, precision, latency and concurrency remain unvalidated; live output level is unavailable |
| OPAMP | 2 | `opamp` | OPAMP0 follower hardware-tested | Four DAC0 levels passed ADC0.IN15 follower readback; other modes, gain settings, OPAMP1 and calibrated accuracy remain unvalidated; no differential mode |
| GPTMR / PTMR | GPTMR0-3, PTMR | `counter` | Alarm, top/reload flags and 32-bit wrap hardware-tested | GPTMR alarm IRQ, clock recovery, periodic top callback, `DONT_RESET`/`RESET_WHEN_LATE`, active-alarm rejection, exact `set_value` and 32-bit wrap across `UINT32_MAX` passed twice after independent resets. Other timer channels and input-capture/output-compare consumers remain unvalidated |
| Watchdog | EWDG0-1, PEWDG | `watchdog` | EWDG0 IRQ-only paths hardware-tested | Thread/ISR feeding, delayed feeding, repeated timeout, disable/reinstall and 500 ms to 2500 ms reconfiguration passed; SoC reset, window mode and sleep/debug pause remain unvalidated; CPU-core-only reset and multistage watchdog are unsupported |
| RTC | None | N/A | Not applicable | HPM5300/HPM5361 has no independent RTC block; use a board-level external RTC if wall-clock time is required |
| USB HS OTG | 1 | UDC/UHC | Device HS/FS and Host boot/report/keyboard-plus-hub paths hardware-tested | Zephyr UDC reuses the unmodified SDK qTD/qDHD engine; HS CDC enumeration, EP1-15 IN/OUT bulk traffic and the address-before-status path passed, as did forced FS. Zephyr UHC passed boot- and report-protocol wired-keyboard enumeration, HID control and one HS Single-TT hub with an LS keyboard. qHD/qTD storage is in HPM5361 fixed uncached DLM. Multi-level/non-TT hubs, non-boot HID, isochronous traffic, Host PM, suspend/resume/remote wakeup and runtime OTG role switching remain unvalidated; CherryUSB/TinyUSB are not adopted |
| MBX | MBX0A/B | `mbox` | Core path hardware-tested | Bidirectional message ordering passed in the core profile; separate 4-word FIFO, signalling-only mode and cross-core shared-memory IPC remain unvalidated |
| GPIOM | 1 | `hpm_gpiom` | Input visibility path hardware-tested | PA24 FGPIO0 input visibility toggle and restore passed without driving the physical pin; ownership, other controllers, locks and security behavior remain unvalidated |
| TRGM | 1 | `mux` + custom extension | Implemented core (build-only) | One mux control per TRGM output with runtime source selection and readback; input filters, output inversion and edge-pulse conversion, eight DMA request routes, the ADC/DAC/position matrices, and named motor-control routes for SYNT/MMC/SEI/RDC/QEO/PLB inter-IP connections. The route helper validates fixed SDK mux indices at build time and leaves endpoint register configuration to the owning drivers. Interrupt routing is available through output 133/134 plus the existing output API, but no TRGM-specific IRQ callback is claimed because the block has no independent status/clear register |
| QEIV2 | 2 | `sensor` + custom extension | QEI1 ABZ, PD, UD, single-phase and UVW paths hardware-tested | ABZ, pulse/direction, up/down, single-phase, UVW and ADC sine/cosine decoding; configurable counts and Z calibration, A/B delay filter, angle, raw position and revolution count; two pulse/duration speed counters with RPM conversion, phase/position comparators, watchdog, HOME/FAULT configuration, event routing to interrupt/DMA/trigger/read and callbacks. ADC sampling and TRGM ADC-matrix setup remain owned by ADC/TRGM. QEI1 ABZ/Z/speed, PD, UD, single-phase and UVW paths passed hardware acceptance; sine/cosine accuracy, external high-rate timing and other instances remain unvalidated |
| QEO | 2 | Custom `hpm_qeo` | Cosine debug-value path hardware-tested | Four amplitude/midpoint configurations, four quadrants and three channels passed; ABZ/pulse/direction/PWM outputs, external waveforms and safety routing remain unvalidated |
| SEI | 2 channels | Custom `hpm_sei` | Implemented core, SSI, Tamagawa, BiSS-C, EnDat, HIPERFACE and Nikon consumers (build-only) | Protocol-neutral transceiver, timing-point, command/data-register, instruction, latch/output, engine and W1C event APIs; external/periodic triggers, per-channel interrupt callbacks and whole-block consumer ownership. The SSI, Tamagawa, BiSS-C, EnDat 2.1/2.2, HIPERFACE and Nikon CDF0 master consumers cover position, status/checksum and timestamp capture with periodic polling and watchdog recovery. BiSS-C, EnDat, HIPERFACE and Nikon use fixed receive points because the vendor dynamic-delay calibration requires the E00047-affected RX latch path. HIPERFACE uses the vendor checksum sequence; E00025 requires a hardware parity workaround that is not yet implemented, so the 8E1 path remains hardware-unvalidated |
| MMC | 2 | Custom `hpm_mmc` | Open-loop math and module control hardware-tested | 1 turn/s prediction matched SYNT elapsed time and whole-module disable/resume passed; predictor BR_EN alone did not freeze continuous prediction; closed-loop behavior, external routes and IRQs remain unvalidated |
| RDC | 1 | Custom `hpm_rdc` | Implemented core (build-only) | PWM/DAC excitation configuration, I/Q ADC accumulation input, filtering/scaling, output triggers, accumulated/extrema/delay result access, W1C event status and interrupt callbacks; integer-CORDIC Q16.16 electrical angle conversion without FPU/libm; TRGM/DAC routing is configured through TRGM |
| PLB | 1 | Custom `hpm_plb` | TYPE_B0 slice15 path hardware-tested | LOAD/KEEP behavior passed using internal TRGM comparator observation; SW_INJECT is software-injection readback, not a live counter, so the counter getter returns `-ENOTSUP`; filter/encoder components remain outside this test |
| SYNT | 1 | Custom `hpm_synt` | Counter control hardware-tested | Counter progress, freeze and resume passed; comparator outputs, external routing and debug-stop behavior remain unvalidated |
| CRC | 1 block, 8 channels | `crc` | Core path hardware-tested | Streaming CRC32 known-answer test passed; other widths, polynomials and reflection combinations remain unvalidated |
| TSNS | 1 | `sensor` | Core path hardware-tested | Temperature read and threshold IRQ passed; calibration/accuracy and operating-range coverage remain unvalidated |
| RNG | 1 | `entropy` | Core path hardware-tested | Startup self-test, data reads and nonblocking ISR budget passed; adapter FIFO/reseed waits are bounded, but waits inside the pinned SDK initialization/self-test are not; entropy quality/statistical testing remains unvalidated |
| PTPC | 1 block, 2 timestamp counters | `ptp_clock` + custom extension | Two-channel time/PPS/rate and MCAN TSU paths hardware-tested | Both timestamp channels, positive/negative time adjustment, 1.05/0.95 rate adjustment, PPS controls 0/1/2/4/15, shared IRQ dispatch, internal TRGM capture and MCAN TSU timestamps passed; external PPS electrical measurements, sustained high-rate IRQ load and broader CAN routes remain unvalidated |
| SDP | 1 | `crypto` | Synchronous KAT paths hardware-tested | AES-128/256 ECB/CBC/CTR/CCM, SHA-1/SHA-256/SM3/CRC32 and SM4 ECB/CBC/CTR through the standard API and HPM extensions, using the unmodified SDK backend. Six KATs including CCM tag rejection passed; true IRQ-driven async completion and stress remain unvalidated |
| SEC / SECMON / KEYM | 1 each | No matching API | Not implemented by policy | SEC/PSEC lifecycle, SECMON/PMON monitoring and KEYM management require custom APIs; no such vendor-only security API is added under the current scope |
| OTP | 1 | `otp` | Read-only path hardware-tested | UUID shadow reads were repeatable. The driver is permanently read-only: programming, lock, reload and region reconfiguration are not implemented; `otp_program()` returns `-ENOSYS` when the Zephyr option is enabled |

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
yet complete for special-function blocks. HPM5300/HPM5361 has no independent
RTC; wall-clock time requires a board-level external RTC.

The workspace-level agent notes record the maintenance assessment, the
per-SoC capability matrix, board interfaces, all-HAL build-only coverage and
upgrade process.

## Development

Peripheral hardware acceptance tests for HPM5300EVK are in
[`tests/hardware`](tests/hardware/README.md). The application provides isolated
Ztest profiles, wiring/fixture requirements, build/flash commands, detailed
results and limits for the verified profiles listed above. Historical
build-only regressions are recorded separately from hardware results. Against
Zephyr `v4.5.0`, the latest full build-only regression rebuilt all 31 hardware
configurations (143 tests), 17 historical EVK/ICB scenarios (26 configurations
after platform filtering), 10 native-sim motor-math tests, and the bringup
sample in both XIP and RAM modes. These checks did not repeat hardware
acceptance. The latest hardware-profile build-only report is
`build/twister-hardware-spi-extended-final/twister.json`. Compatibility
scenarios use semicolon-separated configuration lists so the common
`peripherals.conf` is not overwritten by a later `EXTRA_CONF_FILE` argument.
The SPI extension is now integrated and its feature-scoped stash has been
removed. Unvalidated RDC, SEI, and USB suspend/wakeup drafts remain outside the
worktree in feature-scoped local stashes. The hardware test record contains
their stable stash SHA values and recovery commands.
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

Apache-2.0. See `LICENSE` and `NOTICE`. Individual source files retain the
project-wide `The HPM Zephyr Module Contributors` notice, while project-level
copyright attribution is recorded in `NOTICE`.
