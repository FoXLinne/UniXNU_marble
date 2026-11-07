# OnePlus Ace 5 Pro (hummer)

A community-maintained port for SM8750. Current results are based on the maintainer records at the revisions linked below.

| Repository | Maintainer |
| --- | --- |
| [UniXNU-hummer](https://github.com/2426120557/UniXNU-hummer) | [2426120557](https://github.com/2426120557) |

## State

The device reaches Hello through UEFI, XNU, six CPUs and the physical root volume. The 160 MiB payload includes OS/SystemOS extraction, UFS policies and RAM Data.

Persistent Data, GPU acceleration and physical touch remain pending. The source contains S3910 touch work that needs its own device validation.

## Sources

- [README: 2426120557/UniXNU-hummer](https://github.com/2426120557/UniXNU-hummer/blob/3d3d8ccd7e9544aefa5fb14ba8baad977828b435/README.md)

## Use this port

1. Read the repository build instructions and pinned records. Prepare its firmware, system images and board inputs.
2. Build with that repository's commands. Use [Build and boot](/building) for common configuration.
3. Follow the [bring-up order](/porting/bring-up) and save results through [log capture](/porting/log-capture).
4. Update the [porting record](/porting/records) with the source revisions and last confirmed stage.
