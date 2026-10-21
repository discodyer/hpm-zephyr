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
