# Security Policy

This is a portfolio / educational bootloader, not a product with deployed
devices. Even so, if you spot a security flaw in the design or the code, a
report is welcome.

## Reporting a vulnerability

Please use GitHub's **private vulnerability reporting** on this repository
(the **Security** tab → **Report a vulnerability**) rather than opening a public
issue, so the details can be discussed before they are public.

Useful things to include: which component (Boot Manager, FBL, a transport, the
crypto, or the UDS layer), what an attacker could achieve, and how to reproduce.

## Scope and known limits

The threat model and its deliberate limits on this MCU (flash read-out on the
STM32F103, the symmetric key baked into the FBL, WRP vs RDP) are documented in
the [Security model](README.md#security-model) section of the README. Findings
that restate those known limits are still welcome, but that section is the
starting point.

The trust anchor is the off-device Ed25519 private signing key: the worst a
physical attacker gains is a board running their own code, not the ability to
forge firmware for other devices.
