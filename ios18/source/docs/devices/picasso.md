# Redmi K30 5G (picasso)

A community-maintained port for SM7250. Current results are based on the maintainer records at the revisions linked below.

| Repository | Maintainer |
| --- | --- |
| [UniXNU-picasso](https://github.com/YinCQL/UniXNU-picasso) | [YinCQL](https://github.com/YinCQL) |

## State

UEFI starts the menu and DarwinBoot, and XNU reaches the BSD root-device wait. The kernel loads AMFI, AppleImage4, pthread and the MAC framework, initializes the network core and registers APFS.

UFS root mounting remains the current bring-up task. The source includes USB console work; full userland and GPU support require further device results.

## Sources

- [README: YinCQL/UniXNU-picasso](https://github.com/YinCQL/UniXNU-picasso/blob/11d7ea8e78976e84663ea4b19e656a5564e6c40e/README.md)

## Use this port

1. Read the repository build instructions and pinned records. Prepare its firmware, system images and board inputs.
2. Build with that repository's commands. Use [Build and boot](/building) for common configuration.
3. Follow the [bring-up order](/porting/bring-up) and save results through [log capture](/porting/log-capture).
4. Update the [porting record](/porting/records) with the source revisions and last confirmed stage.
