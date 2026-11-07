# Xiaomi Pad 5 Pro Wi-Fi (elish)

A community port for SM8250-AC, maintained by hpadmx.

- [hpadmx/UniXNU-elish](https://github.com/hpadmx/UniXNU-elish)

## State

The board configuration identifies the Wi-Fi model. Board data, staged defconfigs and ADT scripts are present. The maintainer records zero errors and two TODOs from `scripts/check-elish.py`.

Measure userdata geometry and DRAM size, prepare `elishPkg`, and pin the build tree before assembling a boot image.

## Use this port

1. Read the pinned source and validation records below. Prepare the required firmware and system inputs.
2. Clone the device repository and follow its build guide from the checkout:

```sh
git clone --branch master https://github.com/hpadmx/UniXNU-elish.git
cd UniXNU-elish
git checkout a357eae98a0da0796c8efec8519825367da1e3e7
git submodule update --init --recursive
```

3. Complete the build for that revision, follow the [bring-up order](/porting/bring-up), and use [log capture](/porting/log-capture) to record the last confirmed stage.
4. Save results in the [porting record](/porting/records). See [Port a device](/porting/steps) for board structure and component selection.

## Sources

- [README](https://github.com/hpadmx/UniXNU-elish/blob/a357eae98a0da0796c8efec8519825367da1e3e7/README.md)
- [Board configuration](https://github.com/hpadmx/UniXNU-elish/blob/a357eae98a0da0796c8efec8519825367da1e3e7/board.toml)
