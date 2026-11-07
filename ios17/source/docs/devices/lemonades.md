# OnePlus 9R (lemonades)

A community port for SM8250-AC, maintained by soooink.

- [soooink/UniXNU-lemonades](https://github.com/soooink/UniXNU-lemonades)

## State

The port contains board configuration, defconfigs, an ADT generator and packaging tools. Linux LLVM 19 builds produce a 3 MiB firmware volume and a header-v2 boot image.

Hardware firmware checks, kernel startup and root integration remain pending. Measure the target userdata geometry and complete the Synaptics S3908 touch integration. Follow `source-lock.json` and the repository build guide for dependencies.

## Use this port

1. Read the pinned source and validation records below. Prepare the required firmware and system inputs.
2. Clone the device repository and follow its build guide from the checkout:

```sh
git clone --branch main https://github.com/soooink/UniXNU-lemonades.git
cd UniXNU-lemonades
git checkout 5f52da00907a0cec41e73a81269549bc11991bff
git submodule update --init --recursive
```

3. Complete the build for that revision, follow the [bring-up order](/porting/bring-up), and use [log capture](/porting/log-capture) to record the last confirmed stage.
4. Save results in the [porting record](/porting/records). See [Port a device](/porting/steps) for board structure and component selection.

## Sources

- [Board configuration: soooink/UniXNU-lemonades](https://github.com/soooink/UniXNU-lemonades/blob/b4bf1f0c2141564fcb65a63e0de2d3fefee053f2/board.toml)
