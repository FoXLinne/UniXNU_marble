# OnePlus Pad Pro (caihong)

A community-maintained port for SM8650. Current results are based on the maintainer records at the revisions linked below.

| Repository | Maintainer |
| --- | --- |
| [UniXNU-caihong](https://github.com/Axiang-dev/UniXNU-caihong) | [Axiang-dev](https://github.com/Axiang-dev) |

## State

The APFS restore RAM disk mounts as `md0`. `/sbin/launchd` executes and the text console works. IOFramebuffer integration, input and the full system root remain in progress.

## Sources

- [README: Axiang-dev/UniXNU-caihong](https://github.com/Axiang-dev/UniXNU-caihong/blob/b57b7656b09133979975d74679e1d37e2b6c5d37/README.md)

## Use this port

1. Read the repository build instructions and pinned records. Prepare its firmware, system images and board inputs.
2. Build with that repository's commands. Use [Build and boot](/building) for common configuration.
3. Follow the [bring-up order](/porting/bring-up) and save results through [log capture](/porting/log-capture).
4. Update the [porting record](/porting/records) with the source revisions and last confirmed stage.
