# Google Pixel 9 Pro XL (komodo)

A community-maintained port for Tensor G4. Current results are based on the maintainer records at the revisions linked below.

| Repository | Maintainer |
| --- | --- |
| [UniXNU-komodo](https://github.com/Trijal08/UniXNU-komodo) | [Trijal08](https://github.com/Trijal08) |

## State

Hardware tests reach the firmware menu, test payload and entry probes using a prebuilt uke XNU image. The post-MMU reset probe is recorded. Native Tensor platform support and root integration remain pending.

## Sources

- [Board configuration: Trijal08/UniXNU-komodo](https://github.com/Trijal08/UniXNU-komodo/blob/5a4cfddef602f11c1e5a1836a089e83f1794504c/board.toml)

## Use this port

1. Read the repository build instructions and pinned records. Prepare its firmware, system images and board inputs.
2. Build with that repository's commands. Use [Build and boot](/building) for common configuration.
3. Follow the [bring-up order](/porting/bring-up) and save results through [log capture](/porting/log-capture).
4. Update the [porting record](/porting/records) with the source revisions and last confirmed stage.
