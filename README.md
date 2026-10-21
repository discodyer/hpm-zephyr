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

Use Conventional Commits and sign off every commit:

```sh
git commit -s
```

Example:

```sh
git commit -s -m "feat(soc): add HPM5361 initial support"
```

## License

Apache-2.0. See `LICENSE`.
