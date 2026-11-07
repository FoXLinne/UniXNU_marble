# Xiaomi Mi 10S (thyme)

A community-maintained port for SM8250-AC. Current results are based on the maintainer records at the revisions linked below.

| Repository | Maintainer |
| --- | --- |
| [UniXNU-thyme](https://github.com/tobyfox1640/UniXNU-thyme) | [tobyfox1640](https://github.com/tobyfox1640) |
| [UniXNU-thyme](https://github.com/Coryyi/UniXNU-thyme) | [Coryyi](https://github.com/Coryyi) |

## State

The Coryyi board file identifies Mi 10S and records measured SM8250-family board inputs. Kernel configuration and early trace work are present. The tobyfox1640 repository provides another board, defconfig and ADT implementation. Target-specific boot and userland results remain to be published.

## Sources

- [Board configuration: tobyfox1640/UniXNU-thyme](https://github.com/tobyfox1640/UniXNU-thyme/blob/8efba539ce54b34e576e71799498d287c6eba894/board.toml)
- [Board configuration: Coryyi/UniXNU-thyme](https://github.com/Coryyi/UniXNU-thyme/blob/8728dc4666759998a7873966bd220702af638fec/board.toml)

## Use this port

1. Read the repository build instructions and pinned records. Prepare its firmware, system images and board inputs.
2. Build with that repository's commands. Use [Build and boot](/building) for common configuration.
3. Follow the [bring-up order](/porting/bring-up) and save results through [log capture](/porting/log-capture).
4. Update the [porting record](/porting/records) with the source revisions and last confirmed stage.
