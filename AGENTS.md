# Working rules

- This is the clean TB710FU SM8650Q-AB macOS 27 port. Preserve the previous workspace as evidence only.
- Follow the pinned official UniXNU schema, component recipes and macOS documentation. Do not edit fetched source caches or silently replace official scripts.
- Keep device configuration, SoC facts, OS compatibility fixes and UEFI integration separate. Every nontrivial patch needs its source, reason and verification.
- Never commit credentials, .env files, signing keys, full firmware backups or IPSW members. GitHub credentials remain in the existing local credential file and are passed only through process environment or HTTP headers.
- No device reboot, flash, signing, partitioning or filesystem changes are included in the clean cloud-build task.
- A successful native-only build is not a prelinked kernel, a successful prelink is not hardware boot, and a menu is not macOS userspace. Report the exact stage.
- First build a minimal kernel on the supported macOS runner. Do not add GPU, audio, wireless or unrelated hardware to fix a compiler/linker error.
- Disable optional providers coherently; never add empty security, crypto or hardware stubs to hide missing dependencies.
- Use one bounded GitHub run at a time. Diagnose the first actual failure before dispatching another run.
