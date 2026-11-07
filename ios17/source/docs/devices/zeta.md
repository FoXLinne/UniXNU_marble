# Microsoft Surface Duo 2 (zeta)

A community-maintained port for SM8350. Current results are based on the maintainer records at the revisions linked below.

| Repository | Maintainer |
| --- | --- |
| [UniXNU-zeta](https://github.com/none24smu/UniXNU-zeta) | [none24smu](https://github.com/none24smu) |
| [uefi-zeta](https://github.com/none24smu/uefi-zeta) | [none24smu](https://github.com/none24smu) |

## State

The port mounts its APFS root through native UFS. Offline arm64e-to-arm64 rewriting allows the iOS 26.5.2 `launchd` and `dyld` path to reach PID 1 in EL0 and execute shared-cache code.

Execution faults on an unslid pointer. Pager integration remains in progress, and the graphical interface requires further bring-up.

## Sources

- [README: none24smu/UniXNU-zeta](https://github.com/none24smu/UniXNU-zeta/blob/825d75028a3aefc2035164591ba9074837e39979/README.md)

## Use this port

1. Read the repository build instructions and pinned records. Prepare its firmware, system images and board inputs.
2. Build with that repository's commands. Use [Build and boot](/building) for common configuration.
3. Follow the [bring-up order](/porting/bring-up) and save results through [log capture](/porting/log-capture).
4. Update the [porting record](/porting/records) with the source revisions and last confirmed stage.
