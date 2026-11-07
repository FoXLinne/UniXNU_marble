# OnePlus 8T (kebab)

A community-maintained port for SM8250. Current results are based on the maintainer records at the revisions linked below.

| Repository | Maintainer |
| --- | --- |
| [UniXNU-kebab](https://github.com/GFander/UniXNU-kebab) | [GFander](https://github.com/GFander) |
| [UniXNU-kebab](https://github.com/stevedang67889/UniXNU-kebab) | [stevedang67889](https://github.com/stevedang67889) |
| [KebabRamDisk](https://github.com/stevedang67889/KebabRamDisk) | [stevedang67889](https://github.com/stevedang67889) |

## State

The GFander port uses 16 KiB pages and targets iOS 16.7.16. Hardware logs confirm UFS partition publication, an HFS root and execution of a static test init in EL0. The file named `/sbin/launchd` in that test root is the custom test program. Console output, full Apple userland, additional CPUs and USB integration remain in progress.

The stevedang67889 port separately records a working firmware menu, buttons and panel logs. Its kernel, ADT and payload are built; firmware storage integration remains pending.

## Sources

- [Boot record: GFander/UniXNU-kebab](https://github.com/GFander/UniXNU-kebab/blob/786f31707708dbde0c4ae9b95784f543113b0314/docs/BRING-UP-STATE.md)
- [README: stevedang67889/UniXNU-kebab](https://github.com/stevedang67889/UniXNU-kebab/blob/848b04fea469062b48e0eb3799d656bc111b8ce0/README.md)

## Use this port

1. Read the repository build instructions and pinned records. Prepare its firmware, system images and board inputs.
2. Build with that repository's commands. Use [Build and boot](/building) for common configuration.
3. Follow the [bring-up order](/porting/bring-up) and save results through [log capture](/porting/log-capture).
4. Update the [porting record](/porting/records) with the source revisions and last confirmed stage.
