# HPM Zephyr Module

Out-of-tree Zephyr module for HPMicro HPM5361-based boards.

## Layout

```text
boards/hpmicro/hpm5300evk/   HPM5300EVK board definitions
boards/hpmicro/hpm5361icb/   HPM5361ICB board definitions
soc/hpmicro/hpm5300/         HPM5300 SoC family integration
dts/riscv/hpmicro/           HPM5300 devicetree sources
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
pinctrl, GPIO interrupts, and an XPI NOR flash driver. The XIP image reserves
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

## HPM SDK integration

The complete, unmodified upstream `hpm_sdk` is kept as a separate west project
and pinned by full commit SHA. Zephyr-specific integration lives in
`modules/hpm_sdk/` and in the Zephyr drivers under `drivers/`, following the
`hal_stm32` model. Only HAL sources selected by Zephyr configuration are
compiled; SDK middleware, components and samples stay out of the default build.

`HAS_HPMSDK_*` is selected only by the SoC definition and describes silicon
capabilities. `USE_HPMSDK_*` is selected by the Zephyr integration that needs a
vendor HAL source. Devicetree still controls which instances a board enables.
The optional `CONFIG_HPM_SDK_BUILD_ALL_DRIVERS=y` compiles every HAL source
listed for the selected SoC for build-only regression testing.

The workspace-level agent notes record the maintenance assessment, the per-SoC
capability matrix, board interfaces, all-HAL build-only coverage and upgrade
process.

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
