# Contributing

## Commit messages

Use Zephyr's upstream commit format for both local and upstream development:

```text
<subsystem>: <summary>

[non-empty body]
```

Examples:

```text
soc: hpmicro: add HPM5361 initial support
boards: hpmicro: add HPM5300EVK support
drivers: serial: hpmicro: initialize UART baud rate
```

This is a development convention only during the initial port. No local
commit-message hook or commit-lint workflow is required at this stage.

## Copyright notices

Project-owned files use the default copyright line
`Copyright (c) 2026 The HPM Zephyr Module Contributors`. Preserve copyright
notices only in files directly copied, imported, or derived from HPMicro,
Zephyr, Linaro, or another upstream source. Do not add vendor copyright lines
to normally developed project code. When adding original code or an original
project modification, add the personal notice
`Copyright (c) 2026 Cody Gu <gujiaqi@iscas.ac.cn>` after the project-wide line.
Project-level attribution is also recorded in `NOTICE`.

DCO sign-off, when required, uses the human contributor's legal name and real
email and does not replace file-level copyright attribution.

## Initial-development policy

During initial development, the Agent commits changes directly to this
repository. Local commits use the Zephyr subject format but do not require
Developer Certificate of Origin (DCO) sign-off. Do not add `Signed-off-by`
unless the human maintainer explicitly requests it.

Before upstream submission, squash intermediary development commits, confirm
subsystem names against current Zephyr, and have the human contributor complete
review and DCO sign-off. Run Zephyr's official gitlint, checkpatch, compliance
and Twister checks at that point.
