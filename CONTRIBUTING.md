# Contributing

## Commit messages

Use Conventional Commits:

```text
<type>(<scope>): <description>

[optional body]

Signed-off-by: Name <email>
```

Common types are `feat`, `fix`, `docs`, `build`, `test`, `refactor`, and `chore`.

## Developer Certificate of Origin

All commits must be signed off under the Developer Certificate of Origin 1.1:

<https://developercertificate.org/>

Use:

```sh
git commit -s
```

The sign-off must match the commit author identity.

## Local commit-message hook

Enable the repository hook once per clone:

```sh
git config core.hooksPath .githooks
```

The hook rejects commits without a valid Conventional Commits subject or DCO
sign-off.
