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

## Initial-development policy

During initial development, the Agent commits changes directly to this
repository. Local commits use the Zephyr subject format but do not require
Developer Certificate of Origin (DCO) sign-off. Do not add `Signed-off-by`
unless the human maintainer explicitly requests it.

Before upstream submission, squash intermediary development commits, confirm
subsystem names against current Zephyr, and have the human contributor complete
review and DCO sign-off. Run Zephyr's official gitlint, checkpatch, compliance
and Twister checks at that point.
