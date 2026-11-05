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

Individual source files use the project-wide
`The HPM Zephyr Module Contributors` notice. Project-level copyright
attribution is recorded in `NOTICE`. Preserve copyright notices from HPMicro,
Zephyr, Linaro and other upstream sources.

An additional personal copyright notice is appropriate only for substantial
original work whose copyright is held by that individual. DCO sign-off, when
required, uses the human contributor's legal name and real email and does not
replace file-level copyright attribution.

## Initial-development policy

During initial development, the Agent commits changes directly to this
repository. Local commits use the Zephyr subject format but do not require
Developer Certificate of Origin (DCO) sign-off. Do not add `Signed-off-by`
unless the human maintainer explicitly requests it.

Before upstream submission, squash intermediary development commits, confirm
subsystem names against current Zephyr, and have the human contributor complete
review and DCO sign-off. Run Zephyr's official gitlint, checkpatch, compliance
and Twister checks at that point.
