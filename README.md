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
| HPM5300EVK | `hpm5300evk/hpm5361` | UART0, PA00/PA01, 115200 | RAM and XIP build pass | Not validated |
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
smoke test. No console, timer, GPIO, flash erase/program, boot, or runner
behavior has been validated on real hardware.

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
| Clock / PLL | 1 PLLCTLV2 | `clock_control` | Implemented (build-only) | Clock gating and rate queries; PM/PDGO domains are separate |
| IOC / PIOC | 2 controllers | `pinctrl` | Implemented (build-only) | Pad mux and pad settings; build-time conflict checks only |
| UART | UART0-7, PUART | `serial` | Implemented core (build-only) | Polling and interrupt-driven APIs, FIFO fill/read, runtime IRQ control, callbacks, line-status errors; async/DMA, wide data, flow control and RS-485 not exposed |
| GPIO | GPIOA/B enabled by default; GPIOX/Y, FGPIOA/B/X/Y, PGPIO disabled | `gpio` | Implemented core (build-only) | Basic I/O, port operations, interrupts and callbacks; FGPIO has no interrupt output; current tests exercise GPIOA and FGPIOA |
| I2C | 4 | `i2c` | Partial | Controller, 7-bit, standard/fast/fast-plus, arbitrary multi-message transactions and bus recovery; no target, 10-bit, SMBus helpers or DMA |
| SPI | 4 | `spi` | Partial | Controller, Motorola, single-line, native CS, scatter/gather TX/RX buffers with 8/16/24/32-bit frames and unequal TX/RX lengths; no GPIO CS, half-duplex, TI frame, multi-line, hold/lock or asynchronous API |
| XPI NOR / XIP | 1 | `flash` | Implemented (build-only) | Read, write, erase and page layout through vendor ROM API; no EXIP integration |
| MCAN / CAN-FD | 4 | `can` + custom extension | Implemented (build-only) | Zephyr `can_mcan` backend, classic CAN and CAN-FD timing, shared 32 KiB AHB message RAM split into 8 KiB windows; HPM TSU configuration/readback, PTPC external timebase selection, RX timestamp-slot decode and per-filter sync-capture control through `hpm_mcan`. Generic CAN frames still expose only the 16-bit timestamp field, so TSU captures are decoded with the HPM helper |
| PWM | 2 x 8 channels | `pwm` | Implemented (build-only) | Edge-aligned `set_cycles`, polarity, 0-100% duty; complementary/dead-zone/fault/capture modes not exposed yet |
| HDMA / DMAMUX | 32 channels | `dma` | Implemented core (build-only) | Single-block mem2mem/mem2periph/periph2mem, cyclic, DMAMUX requests, callbacks, channel allocation/release and status/attributes; linked-list gather/scatter, suspend/resume and multi-block transfer lists not exposed |
| ADC16 | 2 | `adc` | Implemented (build-only) | Synchronous single-ended reads, 8/10/12/16-bit resolution, sample-cycle acquisition, PLL/ANA clock selection; no async, DMA sequence or hardware trigger |
| DAC12 | 2 | `dac` | Implemented (build-only) | Direct-mode 12-bit channel setup and synchronous value writes; buffer, step and trigger modes not exposed |
| ACMP | 1 block, 2 comparators | `comparator` | Implemented (build-only) | Per-channel comparator devices, input selection, hysteresis, filtering, internal DAC threshold, edge IRQ/callback; live output level is not exposed by hardware |
| OPAMP | 2 | `opamp` | Implemented (build-only) | Follower, inverting, non-inverting and custom modes; internal/external/DAC references, initial and runtime gain, input selection, Miller and phase-margin capacitors; no differential mode |
| GPTMR / PTMR | GPTMR0-3, PTMR | `counter` | Implemented (build-only) | Four hardware alarm channels, top/reload callback, absolute and relative alarms, guard period; capture and output compare are separate consumers |
| Watchdog | EWDG0-1, PEWDG | `watchdog` | Implemented (build-only) | Single timeout/channel, 32 kHz counter clock, SoC reset, early timeout callback, sleep/debug pause options; window and CPU-core reset unsupported |
| RTC | None | N/A | Not applicable | HPM5300/HPM5361 has no independent RTC block; use a board-level external RTC if wall-clock time is required |
| USB HS OTG | 1 | UDC/UHC | Missing | Native Zephyr UDC/UHC driver not implemented; CherryUSB not adopted |
| MBX | MBX0A/B | `mbox` | Implemented (build-only) | One 32-bit word-message channel per endpoint, interrupt-driven receive callbacks and clock gating; separate 4-word FIFO, signalling-only mode and shared-memory IPC protocol not exposed |
| GPIOM | 1 | `hpm_gpiom` | Implemented core (build-only) | GPIO0/FGPIO0 ownership, per-controller input visibility, lock and lock-state queries for GPIOA/B/X/Y; permission/security controls and migration of existing board pin ownership are not automated |
| TRGM | 1 | `mux` + custom extension | Implemented core (build-only) | One mux control per TRGM output with runtime source selection and readback; input filters, output inversion and edge-pulse conversion, eight DMA request routes, and the ADC/DAC/position routing matrices through `hpm_trgm`. Interrupt routing is available through output 133/134 plus the existing output API, but no TRGM-specific IRQ callback is claimed because the block has no independent status/clear register |
| QEIV2 | 2 | `sensor` + custom extension | Implemented core (build-only) | ABZ, pulse/direction, up/down and UVW decoding; configurable counts and Z calibration, A/B delay filter, angle, raw position and revolution count; two pulse/duration speed counters, phase/position comparators, event status and interrupt callbacks. Sincos ADC modes, speed conversion helpers and TRGM/DMA routing are not exposed |
| QEO | 2 | Custom `hpm_qeo` | Implemented core (build-only) | ABZ, pulse/direction, up/down and three-phase output; three-channel cosine/saddle/rectified-cosine/saw WAVE output with phase, amplitude, midpoint, limits and deadzone; 12-step positive/negative PWM FORCE tables and safety outputs; hardware/software position source, injection and status. VD/VQ injection and TRGM/MMC position routing are not exposed |
| SEI | 2 channels | Custom `hpm_sei` | Implemented core (build-only) | Protocol-neutral transceiver, instruction, latch, SAMPLE/UPDATE, engine and data-register access; encoder protocols (SSI, Tamagawa, BiSS, EnDat, etc.), periodic/external trigger routing and interrupt callbacks are not built in |
| MMC | 2 | Custom `hpm_mmc` | Implemented core (build-only) | Track and two-predictor configuration, position/delta input, coefficients, position/speed triggers, result/status access and module control; interrupt callbacks and TRGM position routing are not exposed |
| RDC | 1 | Custom `hpm_rdc` | Implemented core (build-only) | PWM/DAC excitation configuration, I/Q ADC accumulation input, filtering/scaling, output triggers and accumulated/extrema/delay result access; angular conversion, interrupts and TRGM/DAC routing are not exposed |
| PLB | 1 | Custom `hpm_plb` | Implemented core (build-only) | Four TYPE_A truth tables with software injection; four TYPE_B channels with 16 selectable slices, comparator modes and counter inject/read; TRGM input/output routing and SDK filter/encoder components are not exposed |
| SYNT | 1 | Custom `hpm_synt` | Implemented core (build-only) | Motor-system synchronization counter, four comparator outputs, debug-stop control and the shared timestamp/set/increment/decrement/capture interface; TRGM routing and userspace wrappers are not exposed |
| CRC | 1 block, 8 channels | `crc` | Implemented (build-only) | Streaming CRC API, byte input, 8/16/32-bit polynomials, input/output reflection, IEEE/Castagnoli/MPEG-2 final XOR handling |
| TSNS | 1 | `sensor` | Implemented (build-only) | Continuous die-temperature measurement, configurable averaging/speed/interval, exact fixed-point conversion to sensor_value; limits and interrupts not exposed |
| RNG | 1 | `entropy` | Implemented (build-only) | Hardware TRNG with automatic reseeding, optional startup self-test and blocking thread-mode entropy reads; no ISR or asynchronous API |
| PTPC | 1 block, 2 timestamp counters | `ptp_clock` + custom extension | Implemented (build-only) | 32-bit seconds plus nanosecond counters, set/get, signed time adjustment and ADDEND rate adjustment; input capture, compare targets, PPS output, event status and shared-IRQ callbacks through `hpm_ptpc`. CAN0-3 time-source selection and the four MCAN TSU input routes are exposed. Hardware timestamp capture behavior remains unvalidated |
| SDP | 1 | `crypto` candidate | Missing | API and algorithm mapping not designed |
| SEC / SECMON / KEYM | 1 each | Custom/security | Missing | No Zephyr integration |
| OTP | 1 | `otp` | Implemented (build-only) | Read-only shadow access by default, byte-addressed API, optional irreversible fuse programming behind CONFIG_OTP_PROGRAM; lock/region helpers and reload not exposed |

Implemented drivers are build-only until hardware validation is completed;
missing drivers are not built or enabled. The SoC startup, PLIC, PLICSW, machine
timer and cache paths are native Zephyr/SoC integration rather than HPM driver
classes. No third-party module is currently selected for the remaining standard
Zephyr APIs; USB and motor-control features still need a separate design
decision before adding any third-party stack.

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

Use Zephyr's upstream `<subsystem>: <summary>` format for local development:

```sh
git commit -m "soc: hpmicro: add HPM5361 initial support"
```

During initial development, Agent-authored commits do not require DCO sign-off
and no local commit-message checker is enforced. Before upstream submission,
squash intermediary commits, run Zephyr's official checks, and have the human
contributor complete review and DCO sign-off.

## License

Apache-2.0. See `LICENSE`.
