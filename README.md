# HPM Zephyr Module

Out-of-tree Zephyr module for HPMicro HPM5361 and HPM5300EVK support.

## Layout

```text
boards/hpmicro/hpm5300evk/   HPM5300EVK board definitions
soc/hpmicro/HPM5300/         HPM5300 SoC family integration
dts/riscv/hpmicro/           HPM5300 devicetree sources
dts/bindings/                Devicetree bindings
drivers/                     Zephyr drivers backed by HPM SDK
include/                     Public DT bindings and driver headers
modules/hpm_sdk/             HPM SDK integration glue
```

## Status

The module configures and builds a minimal HPM5361 application against Zephyr
4.5.0-rc2 in RAM mode. The current smoke test exercises Devicetree, Kconfig,
SoC startup, machine timer, UART, pinctrl, and GPIO build integration. Hardware
validation and XIP/flash bring-up are still required before declaring support.

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
