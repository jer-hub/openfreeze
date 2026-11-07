# Security

## Supported use

OpenFreeze is a **prototype** KMDF volume filter. Lab and test builds use
**test-signed** drivers and typically require Secure Boot **off**.

Do **not** load `OfFilter.sys` on a daily-driver or Hyper-V build host. Develop
on the host; test only in an isolated VM or a disposable lab image.

Production / Secure Boot deployments require Microsoft attestation (or WHQL)
signing — see [docs/production-signing.md](docs/production-signing.md).

## Reporting a vulnerability

Open an issue on this repository with a clear title prefix `[security]`, or
contact the maintainers privately if disclosure would put systems at risk.
Please include:

- Affected component (driver / service / installer)
- Windows version and whether testsigning / Secure Boot was in use
- Steps to reproduce (no need to attach secrets or host passwords)

## Secrets

Never commit lab passwords, EV certificate private keys, or files such as
`devadmin_password.txt`. Keep those outside the repo (local lab paths only).
