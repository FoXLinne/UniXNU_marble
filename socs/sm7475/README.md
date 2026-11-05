# SM7475 profile

This profile records the CPU affinities, GIC, timer and UFS register windows used by marble. The source values are preserved in `scripts/make-marble-adt.py`, `package/MarbleCpu.cpp` and `package/MarbleUfs.cpp` in the device repository. Board RAM, reservations, panel and peripheral wiring belong to `dts/marble.dts`.

The profile is published as a pinned `socs/sm7475` subtree of FoXLinne/UniXNU_marble. Its feed names that commit and path. A future shared SoC repository can retain these files and their source history.

Read the UniXNU SoC support and device-tree guides before changing a resource. Validate the board DTS with dtc; record hardware observations in the device porting record.
