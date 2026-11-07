# OnePlus 10 Pro 5G (negroni)

Community ports for OnePlus 10 Pro 5G (negroni, SM8450). The pawlickerr record uses NE2213; the independent iOSPlus10 workspace uses NE2211.

| Repository | Maintainer |
| --- | --- |
| [UniXNU-negroni](https://github.com/pawlickerr/UniXNU-negroni) | [pawlickerr](https://github.com/pawlickerr) |
| [Mu-Silicum-negroni](https://github.com/pawlickerr/Mu-Silicum-negroni) | [pawlickerr](https://github.com/pawlickerr) |
| [iOSPlus10](https://github.com/akshaynexus/iOSPlus10) | [akshaynexus](https://github.com/akshaynexus) |

## State

The pawlickerr port records iOS 16.7.16 SpringBoard with touch and CPU rendering.

The independent iOSPlus10 workspace records Metal composition in `backboardd`: a 150-second device run completes without a crash, with 4.5 to 8 ms command buffers and up to 83 GPU presentations/s. These measurements apply to that workspace's selected configuration. The QCA6490 PCIe link and device response are verified; Wi-Fi networking, audio playback and charging changes require further device results.

## Maintenance

The maintainer owns the repository and its release decisions. UniXNU documentation lists the port and its state; it does not take over maintenance.

## Sources

- [Port README: pawlickerr/UniXNU-negroni](https://github.com/pawlickerr/UniXNU-negroni/blob/f812002ae3e6b861d23007efdfd9f1c394934ba9/README.md)
- [Workspace status: akshaynexus/iOSPlus10](https://github.com/akshaynexus/iOSPlus10/blob/c0a451fbe774b44a47ce91795d1f8b5b1c4ac351/docs/plan.md)
